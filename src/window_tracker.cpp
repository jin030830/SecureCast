// =============================================================================
// window_tracker.cpp — Role A 1주차 골격: 블랙리스트 앱 좌표 스캐너
//
// 동작 흐름:
//   securecast_video_tick (매 프레임, 60fps)
//     └─ trackerAccumulator 누산 (0.15초 미만이면 즉시 리턴) [C-1: 인라인 직접 처리]
//        └─ sc_scan_blacklisted_windows
//             └─ EnumWindows(enum_proc) — 모든 최상위 창 순회
//                  └─ enum_proc: 보이는 창 → DWM 좌표 → PID → exe명 → 블랙리스트 매칭
//                       └─ 매칭되면 TrackedWindowList에 슬롯 추가
//        └─ 결과를 blacklistMask에 저장 (mutex 보호)
//
// Win32 API 선택 이유:
//   - DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS): GetWindowRect는 창
//     그림자까지 포함된 좌표를 줘서 마스킹 영역이 더 넓어진다. DWM 쪽이 정확.
//   - PROCESS_QUERY_LIMITED_INFORMATION: 보호된 프로세스(시스템 등)에도 권한 부여
//     없이 PID→exe명 조회 가능. 일반 _INFORMATION보다 권한이 낮아 안전.
//   - QueryFullProcessImageNameW: 32/64bit 프로세스 양쪽 모두에서 동작.
// =============================================================================

#include "window_tracker.h"
#include "plugin-support.h"   // obs_log

#include <obs.h>

#include <windows.h>
#include <dwmapi.h>     // DwmGetWindowAttribute
#include <psapi.h>      // QueryFullProcessImageNameW (psapi 또는 kernel32 양쪽 노출)
#include <wctype.h>     // towlower

namespace {

// 너무 작은 창은 시각적으로 노출 가능성이 낮고, 트레이 아이콘 같은 노이즈가 많다.
constexpr int MIN_WINDOW_DIMENSION = 100;

// EnumWindows 1회의 비용을 60fps 매 tick 떠안기엔 부담. 사람의 창 이동 인지
// 시간보다 짧으면 충분하므로 0.15초 (≈6.7Hz) 로 둔다.
constexpr float SCAN_INTERVAL_SEC = 0.15f;

// 보호 대상 앱 목록. 향후 OBS Properties UI에서 사용자가 편집할 수 있게 확장 예정.
const wchar_t *const kBlacklist[] = {
	L"KakaoTalk.exe",
	L"Discord.exe",
	L"Slack.exe",
};

// UWP (Microsoft Store) 앱은 모두 ApplicationFrameHost.exe라는 단일 호스트
// 프로세스로 보고된다. 실제 앱 식별은 자식 윈도우의 PID를 다시 봐야 가능하므로
// 1주차에선 일괄 스킵하고 후속 단계에서 EnumChildWindows로 보강한다.
const wchar_t *const kUwpHost = L"ApplicationFrameHost.exe";

// 대소문자 무시 wide string 비교. 윈도우 파일시스템이 case-insensitive라
// "kakaotalk.exe"가 들어와도 매칭되어야 한다.
bool iequals(const wchar_t *a, const wchar_t *b)
{
	while (*a && *b) {
		if (towlower(*a) != towlower(*b))
			return false;
		++a;
		++b;
	}
	return *a == 0 && *b == 0;
}

// "C:\Path\To\App.exe" → "App.exe" 만 추출.
// 표준 라이브러리 PathFindFileNameW도 있지만 Shlwapi 의존 추가하기 싫어서 직접 구현.
void path_basename(const wchar_t *full_path, wchar_t *out, size_t out_cap)
{
	const wchar_t *last_sep = full_path;
	for (const wchar_t *p = full_path; *p; ++p) {
		if (*p == L'\\' || *p == L'/')
			last_sep = p + 1;
	}
	size_t i = 0;
	while (last_sep[i] && i + 1 < out_cap) {
		out[i] = last_sep[i];
		++i;
	}
	out[i] = 0;
}

bool is_blacklisted(const wchar_t *exe_name)
{
	for (const wchar_t *entry : kBlacklist) {
		if (iequals(entry, exe_name))
			return true;
	}
	return false;
}

// EnumWindows의 콜백. 시스템의 모든 최상위 윈도우에 대해 한 번씩 호출됨.
// 반환:
//   TRUE  → 다음 윈도우로 계속 순회
//   FALSE → 즉시 순회 중단 (슬롯 소진 시 사용)
BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lparam)
{
	auto *out = reinterpret_cast<TrackedWindowList *>(lparam);
	if (out->count >= SC_MAX_TRACKED_WINDOWS)
		return FALSE; // 슬롯 소진 — 순회 중단

	// 최소화·숨김 창은 화면에 안 그려지므로 보호 대상 아님.
	if (!IsWindowVisible(hwnd))
		return TRUE;

	// DWM 기반 정확한 화면 좌표 (그림자 영역 제외).
	RECT rect{};
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))))
		return TRUE;

	const int width = rect.right - rect.left;
	const int height = rect.bottom - rect.top;
	if (width < MIN_WINDOW_DIMENSION || height < MIN_WINDOW_DIMENSION)
		return TRUE;

	// 윈도우의 소유 프로세스 ID 획득. 0이면 system 윈도우 (실패).
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == 0)
		return TRUE;

	// 최소 권한으로 프로세스 핸들 열기. 실패해도 무시 (보호된 프로세스 등).
	HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc)
		return TRUE;

	// 프로세스 실행 파일 전체 경로 조회.
	wchar_t exe_path[MAX_PATH] = {};
	DWORD path_size = MAX_PATH;
	bool ok = QueryFullProcessImageNameW(proc, 0, exe_path, &path_size) != 0;
	CloseHandle(proc);
	if (!ok)
		return TRUE;

	// 풀 경로에서 베이스네임만 뽑아서 블랙리스트와 비교.
	wchar_t exe_name[64] = {};
	path_basename(exe_path, exe_name, sizeof(exe_name) / sizeof(exe_name[0]));

	// UWP 앱은 ApplicationFrameHost.exe로 잡힘 → 자식 윈도우 PID로 재시도가 필요하지만
	// 1주차 골격에서는 일단 무시하고 다음 단계에서 EnumChildWindows로 보강한다.
	if (iequals(exe_name, kUwpHost))
		return TRUE;

	if (!is_blacklisted(exe_name))
		return TRUE;

	// 매칭 성공 → out 슬롯에 정보 복사.
	auto &slot = out->items[out->count++];
	slot.hwnd = hwnd;
	slot.bounds = rect;
	for (size_t i = 0; i < sizeof(slot.exe_name) / sizeof(slot.exe_name[0]); ++i) {
		slot.exe_name[i] = exe_name[i];
		if (!exe_name[i])
			break;
	}

	return TRUE;
}

} // namespace

// 호출자는 OBS의 video_tick / video_render 같은 렌더 스레드 컨텍스트에서만 호출할 것.
// (DWM/Win32 윈도우 핸들 조회는 caller 스레드의 메시지 큐에 의존)
extern "C" void sc_scan_blacklisted_windows(TrackedWindowList *out)
{
	if (!out)
		return;
	out->count = 0;
	EnumWindows(enum_proc, reinterpret_cast<LPARAM>(out));
}


