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
#include "win_event_listener.h"
#include "plugin-support.h"   // obs_log

#include <obs.h>

#include <windows.h>
#include <dwmapi.h>     // DwmGetWindowAttribute
#include <psapi.h>      // QueryFullProcessImageNameW (psapi 또는 kernel32 양쪽 노출)
#include <wctype.h>     // towlower

namespace {

constexpr int MIN_WINDOW_DIMENSION = 100;

// 기본 스캔 주기 참조값 (일반 모드). 실제 스캔 주기는 sc_tracker_tick의 interval 인자로 전달된다.
// 게임 모드에서는 호출자(securecast-filter.cpp)가 SCAN_INTERVAL_GAME(0.5초)을 전달한다.
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

// Win+Tab(Task View)나 Alt+Tab(앱 전환기)인지 클래스명으로 판별.
// 이 창이 앞에 있을 때는 뒤에 있는 민감 앱을 추적 해제하지 않는다.
// 이유: Task View/Alt+Tab은 전환 UI이므로, 그 뒤의 민감 앱은 "선택 중인 상태"일 수 있고
//       전환 애니메이션(~500ms) 동안도 마스킹을 유지해야 즉시 가려진다.
bool is_task_switcher_window(HWND hwnd)
{
	wchar_t cls[128] = {};
	GetClassNameW(hwnd, cls, 128);
	// Win+Tab Task View (Windows 10 / 11)
	if (wcsstr(cls, L"MultitaskingView") != nullptr)
		return true;
	// Alt+Tab 앱 전환기 (버전마다 클래스명 다름)
	if (iequals(cls, L"TaskSwitcherWnd") || iequals(cls, L"XamlExplorerHostIslandWindow"))
		return true;
	return false;
}

// 창이 다른 앱에 완전히 가려져 있는지 확인 (뒤로 보내기 감지).
// 중앙 1점이 아니라 5점(중앙 + 4코너 25% 안쪽)을 샘플링해,
// 하나라도 hwnd가 최상위이면 true 반환.
// 완전히 다른 앱에 덮여야(5점 모두 다른 창) 추적 해제.
bool is_window_top_at_center(HWND hwnd, const RECT &rect)
{
	const LONG w4 = (rect.right  - rect.left) / 4;
	const LONG h4 = (rect.bottom - rect.top)  / 4;
	const POINT samples[5] = {
		{ (rect.left + rect.right) / 2,  (rect.top + rect.bottom) / 2 }, // center
		{ rect.left  + w4,               rect.top  + h4               }, // top-left
		{ rect.right - w4,               rect.top  + h4               }, // top-right
		{ rect.left  + w4,               rect.bottom - h4             }, // bottom-left
		{ rect.right - w4,               rect.bottom - h4             }, // bottom-right
	};
	for (const POINT &pt : samples) {
		HWND topAt = WindowFromPoint(pt);
		if (!topAt)
			return true;
		HWND topRoot = GetAncestor(topAt, GA_ROOT);
		if (topRoot == hwnd)
			return true;
		if (is_task_switcher_window(topRoot))
			return true;
	}
	return false; // 5개 샘플점 모두 다른 앱에 가려짐 → 뒤로 보내기로 간주
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
	// WinEvent 세트 우선 확인 — Aero Peek 중에 IsIconic이 FALSE를 반환하는 경우 대비.
	if (!IsWindowVisible(hwnd) || WinEventListener::isMinimized(hwnd))
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

	// Z-order: 창 중앙이 다른 앱에 가려져 있으면 (뒤로 보내기 상태) 추적 대상 제외.
	// 카톡이 OBS 뒤로 가도 IsWindowVisible=true라 이 체크 없이는 계속 추적된다.
	if (!is_window_top_at_center(hwnd, rect))
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

extern "C" void sc_update_tracked_bounds(TrackedWindowList *list)
{
	if (!list || list->count == 0)
		return;

	// 역순으로 순회해야 swap-and-pop 시 인덱스가 안 틀린다.
	for (int i = list->count - 1; i >= 0; --i) {
		HWND hwnd = list->items[i].hwnd;

		// 창이 닫혔거나 최소화됐으면 슬롯 제거 (마지막 원소와 swap-and-pop).
		if (!IsWindowVisible(hwnd) || WinEventListener::isMinimized(hwnd)) {
			list->items[i] = list->items[--list->count];
			continue;
		}

		RECT rect{};
		if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
		                                    &rect, sizeof(rect))))
			list->items[i].bounds = rect;

		// 다른 앱 창이 앞으로 와서 이 창을 가리면 즉시 추적 해제.
		// 뒤로 보내기는 IsWindowVisible=true를 유지하므로 위의 체크만으로는 감지 불가.
		if (!is_window_top_at_center(hwnd, list->items[i].bounds)) {
			list->items[i] = list->items[--list->count];
			continue;
		}
	}
}

// 호출자는 OBS의 video_tick / video_render 같은 렌더 스레드 컨텍스트에서만 호출할 것.
// (DWM/Win32 윈도우 핸들 조회는 caller 스레드의 메시지 큐에 의존)
extern "C" void sc_scan_blacklisted_windows(TrackedWindowList *out)
{
	if (!out)
		return;
	out->count = 0;
	EnumWindows(enum_proc, reinterpret_cast<LPARAM>(out));
}

// 60fps tick에서 매번 호출되어도 실제 무거운 EnumWindows는 0.15초마다 1회만 실행.
// 매칭된 창은 일단 obs_log로만 출력 — 후속 단계에서 BlurRect로 변환 후 셰이더에 전달.
extern "C" void sc_tracker_tick(float seconds, float *accumulator, TrackedWindowList *out, float interval)
{
	if (!accumulator)
		return;

	*accumulator += seconds;
	if (*accumulator < interval)
		return;
	*accumulator = 0.0f;

	TrackedWindowList list{};
	sc_scan_blacklisted_windows(&list);

	// 스캔 결과를 호출자에게 전달 (창이 0개여도 업데이트 — 닫힌 창 반영).
	if (out)
		*out = list;

	if (list.count == 0)
		return;

	for (int i = 0; i < list.count; ++i) {
		const auto &w = list.items[i];
		obs_log(LOG_INFO, "[tracker] %ls @ (%ld,%ld)-(%ld,%ld) %ldx%ld", w.exe_name,
			w.bounds.left, w.bounds.top, w.bounds.right, w.bounds.bottom,
			w.bounds.right - w.bounds.left, w.bounds.bottom - w.bounds.top);
	}
}
