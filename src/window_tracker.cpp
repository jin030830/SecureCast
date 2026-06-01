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
#include "known_games.h"      // Tier 1 빌트인 게임 exe 리스트 (T03)

#include <obs.h>
#include <util/platform.h>    // os_gettime_ns

#include <windows.h>
#include <dwmapi.h>     // DwmGetWindowAttribute
#include <psapi.h>      // QueryFullProcessImageNameW (psapi 또는 kernel32 양쪽 노출)
#include <wctype.h>     // towlower
#include <objbase.h>    // CoInitializeEx / CoCreateInstance
#include <uiautomation.h> // IUIAutomation, ElementFromHandle, FindAll

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

namespace {

constexpr int MIN_WINDOW_DIMENSION = 100;

// 기본 스캔 주기 참조값 (일반 모드). 실제 스캔 주기는 sc_tracker_tick의 interval 인자로 전달된다.
// 게임 모드에서는 호출자(securecast-filter.cpp)가 SCAN_INTERVAL_GAME(0.5초)을 전달한다.
constexpr float SCAN_INTERVAL_SEC = 0.15f;

// 보호 대상 앱 기본 목록 (일반 모드).
const wchar_t *const kBlacklist[] = {
	L"KakaoTalk.exe",
	L"Discord.exe",
	L"Slack.exe",
};

// 게임 모드 추가 기본 목록. 일반 모드에 안 들어가 있어도 게임 모드에선 자동
// 블러 (브라우저/메모장 등 게임 중 노출되기 쉬운 민감 가능 앱).
const wchar_t *const kGameModeExtraBlacklist[] = {
	L"chrome.exe",
	L"firefox.exe",
	L"msedge.exe",
	L"whale.exe",  // Naver Whale
	L"notepad.exe",
	L"WINWORD.EXE",
	L"EXCEL.EXE",
	L"POWERPNT.EXE",
};

// 사용자가 OBS settings에서 추가한 동적 블랙리스트 (일반/게임 모드 별도).
// mutex 보호 — settings update와 scan thread 사이 race 방지.
std::mutex g_userBlacklistMutex;
std::vector<std::wstring> g_userBlacklistNormal;
std::vector<std::wstring> g_userBlacklistGameMode;

// [T03] Tier 3: 사용자가 OBS 설정에서 "이건 게임"으로 등록한 exe 목록.
// 빌트인(Tier 1)이 놓치는 신작/마이너 게임을 사용자가 직접 보강하는 경로.
// 블랙리스트와 무관 — 게임 모드 진입/제외 trigger에 사용된다.
std::mutex g_userGameListMutex;
std::vector<std::wstring> g_userGameList;

// 게임 모드 글로벌 refcount — 어느 필터든 게임 모드면 > 0. is_blacklisted가
// 자동으로 game-mode-extra 리스트도 검사하도록 함. enum_proc 등 모든 scan
// 경로가 자동으로 게임 모드 블랙리스트 적용.
//
// 다중 인스턴스 안전: 각 필터의 게임 모드 진입/탈출이 +1/-1 짝으로 일어나,
// 한 쪽이 OFF 되어도 다른 쪽이 여전히 게임 모드면 글로벌이 켜진 상태를 유지.
// 마지막 필터가 OFF로 내려와야 글로벌도 비활성화. 동일 필터에서 OFF가 두 번
// 호출되어도 음수로 빠지지 않도록 fetch_sub 후 underflow 가드.
std::atomic<int> g_gameMode_RefCount{0};

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
	// 1. 하드코딩 기본 (일반 모드)
	for (const wchar_t *entry : kBlacklist) {
		if (iequals(entry, exe_name))
			return true;
	}
	// 2. 사용자 추가 (일반 모드)
	{
		std::lock_guard<std::mutex> lock(g_userBlacklistMutex);
		for (const auto &entry : g_userBlacklistNormal) {
			if (iequals(entry.c_str(), exe_name))
				return true;
		}
	}
	// 3. 어느 필터든 게임 모드면 game-mode-extra도 자동 적용
	if (g_gameMode_RefCount.load(std::memory_order_acquire) > 0) {
		for (const wchar_t *entry : kGameModeExtraBlacklist) {
			if (iequals(entry, exe_name))
				return true;
		}
		std::lock_guard<std::mutex> lock(g_userBlacklistMutex);
		for (const auto &entry : g_userBlacklistGameMode) {
			if (iequals(entry.c_str(), exe_name))
				return true;
		}
	}
	return false;
}

// 일반 모드 블랙리스트에만 있는지 검사 (game-mode-extra 제외).
// dialog 토글 분기에서 "일반 블랙리스트는 항상 자동, 게임 블랙리스트만 토글" 처리에 사용.
bool is_blacklisted_normal_only(const wchar_t *exe_name)
{
	for (const wchar_t *entry : kBlacklist) {
		if (iequals(entry, exe_name))
			return true;
	}
	std::lock_guard<std::mutex> lock(g_userBlacklistMutex);
	for (const auto &entry : g_userBlacklistNormal) {
		if (iequals(entry.c_str(), exe_name))
			return true;
	}
	return false;
}

// 명시적 game-mode 체크 — 게임 모드 flag와 무관하게 game-mode-extra까지 검사.
bool is_blacklisted_game_mode(const wchar_t *exe_name)
{
	for (const wchar_t *entry : kBlacklist) {
		if (iequals(entry, exe_name))
			return true;
	}
	for (const wchar_t *entry : kGameModeExtraBlacklist) {
		if (iequals(entry, exe_name))
			return true;
	}
	std::lock_guard<std::mutex> lock(g_userBlacklistMutex);
	for (const auto &entry : g_userBlacklistNormal) {
		if (iequals(entry.c_str(), exe_name))
			return true;
	}
	for (const auto &entry : g_userBlacklistGameMode) {
		if (iequals(entry.c_str(), exe_name))
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

// 차집합 알고리즘 작업 버퍼 크기. 차감을 거듭하면 이론상 4^N까지 늘 수 있으나
// 실제 데스크탑 시나리오에서는 대부분 16 이내. 64면 5~6개 앞 창이 복잡하게
// 겹쳐도 안전하게 표현 가능.
constexpr int VISIBILITY_WORK_CAP = 64;

// 사각형 A에서 사각형 B를 뺀 결과를 out에 최대 maxOut개까지 채워 반환.
// 결과는 최대 4개의 disjoint 사각형 (top / bottom 띠 + left / right 가운데 띠).
// 반환값: 채운 개수. 교차 없으면 A를 그대로 1개 반환. B가 A를 완전 덮으면 0.
int rect_subtract(const RECT &A, const RECT &B, RECT *out, int maxOut)
{
	RECT I;
	I.left   = (A.left   > B.left)   ? A.left   : B.left;
	I.top    = (A.top    > B.top)    ? A.top    : B.top;
	I.right  = (A.right  < B.right)  ? A.right  : B.right;
	I.bottom = (A.bottom < B.bottom) ? A.bottom : B.bottom;
	if (I.left >= I.right || I.top >= I.bottom) {
		if (maxOut < 1)
			return 0;
		out[0] = A;
		return 1;
	}
	int n = 0;
	if (A.top    < I.top    && n < maxOut) out[n++] = {A.left, A.top,    A.right, I.top};
	if (A.bottom > I.bottom && n < maxOut) out[n++] = {A.left, I.bottom, A.right, A.bottom};
	if (A.left   < I.left   && n < maxOut) out[n++] = {A.left,  I.top,   I.left,  I.bottom};
	if (A.right  > I.right  && n < maxOut) out[n++] = {I.right, I.top,   A.right, I.bottom};
	return n;
}

// rects[]의 bounding box를 단일 사각형으로 반환 (union 폴백용).
RECT rect_union_all(const RECT *rects, int n)
{
	RECT u = rects[0];
	for (int i = 1; i < n; ++i) {
		if (rects[i].left   < u.left)   u.left   = rects[i].left;
		if (rects[i].top    < u.top)    u.top    = rects[i].top;
		if (rects[i].right  > u.right)  u.right  = rects[i].right;
		if (rects[i].bottom > u.bottom) u.bottom = rects[i].bottom;
	}
	return u;
}

} // namespace

// =============================================================================
// sc_compute_visible_subrects
//
// target의 bounds에서, target보다 z-order가 위인 모든 top-level 창의 rect를
// 차례로 빼서, 화면에 실제로 노출된 disjoint 사각형들을 구한다.
//
// 핵심:
//   - GetWindow(w, GW_HWNDPREV)는 z-order에서 한 칸 "위"의 형제 창을 반환.
//     top-level 창들은 desktop의 형제이므로, target에서 GW_HWNDPREV로 거슬러
//     올라가며 nullptr를 만날 때까지 순회하면 위에 있는 모든 top-level 창을 enum.
//   - WindowFromPoint 5점 샘플링보다 훨씬 정확. 작은 가시 영역(가장자리 띠)도
//     올바르게 산출되어 그 영역만 블러할 수 있다.
//   - Task switcher / Alt+Tab 같은 일시적 전환 UI는 차감에서 제외 — 전환 중에도
//     아래 민감 앱이 가려진 것으로 간주하면 마스킹이 잠시 사라져 노출 발생.
// =============================================================================
extern "C" int sc_compute_visible_subrects(HWND target, RECT target_bounds,
                                            RECT *out, int maxOut)
{
	if (!out || maxOut <= 0)
		return 0;
	if (target_bounds.right <= target_bounds.left ||
	    target_bounds.bottom <= target_bounds.top)
		return 0;

	RECT working[VISIBILITY_WORK_CAP];
	int wc = 0;
	working[wc++] = target_bounds;

	for (HWND w = GetWindow(target, GW_HWNDPREV); w != nullptr;
	     w = GetWindow(w, GW_HWNDPREV)) {
		if (!IsWindowVisible(w))
			continue;
		HWND wroot = GetAncestor(w, GA_ROOT);
		if (wroot == target)
			continue; // target 자신은 스킵
		// Alt+Tab/Task View는 전환 중 일시적 — 가린 것으로 보지 않음.
		if (is_task_switcher_window(wroot))
			continue;

		// DWM cloak 검사 — Aero Peek 등에서 시각적으로 투명 처리된 창은 z-order
		// 위에 있어도 픽셀상 가리지 않으므로 차감 대상에서 제외해야 한다.
		// 0=일반, 그 외(CLOAKED_APP/SHELL/INHERITED)는 보이지 않음.
		DWORD cloaked = 0;
		if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked,
		                                     sizeof(cloaked))) &&
		    cloaked != 0)
			continue;

		RECT cov{};
		if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS,
		                                  &cov, sizeof(cov)))) {
			if (!GetWindowRect(w, &cov))
				continue;
		}
		// 너무 작은 창(툴팁/인디케이터)은 차감 안 함 — 사용자 인식상 "가린" 게
		// 아니므로 마스킹 유지가 더 안전.
		if (cov.right - cov.left < 8 || cov.bottom - cov.top < 8)
			continue;

		RECT next[VISIBILITY_WORK_CAP];
		int nc = 0;
		bool overflow = false;
		for (int i = 0; i < wc; ++i) {
			if (nc + 4 > VISIBILITY_WORK_CAP) {
				overflow = true;
				break;
			}
			nc += rect_subtract(working[i], cov, &next[nc],
			                     VISIBILITY_WORK_CAP - nc);
		}
		if (overflow) {
			// 폴백: 현재 작업 사각형들을 한 박스로 union해서 다음 차감 계속.
			// 시각적 정밀도는 떨어지지만 노출 누락은 없음.
			RECT u = rect_union_all(working, wc);
			wc = rect_subtract(u, cov, working, VISIBILITY_WORK_CAP);
		} else {
			for (int i = 0; i < nc; ++i)
				working[i] = next[i];
			wc = nc;
		}
		if (wc == 0)
			return 0;
	}

	// maxOut을 초과하면 union 폴백으로 단일 사각형 반환.
	if (wc > maxOut) {
		out[0] = rect_union_all(working, wc);
		return 1;
	}
	for (int i = 0; i < wc; ++i)
		out[i] = working[i];
	return wc;
}

namespace {

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

	// Z-order: z-order 위 창들로 잘라낸 실제 가시영역을 산출.
	// 완전히 가려져 있으면(visible == 0) 추적 대상 제외.
	// 일부만 가려져 있으면 그 띠/패치만 visibleRects에 담는다 — 다운스트림 렌더는
	// 이를 순회해 노출된 부분만 블러한다.
	RECT visRects[SC_MAX_VISIBLE_SUBRECTS];
	int visCount = sc_compute_visible_subrects(hwnd, rect, visRects,
	                                            SC_MAX_VISIBLE_SUBRECTS);
	if (visCount == 0)
		return TRUE;

	// 매칭 성공 → out 슬롯에 정보 복사.
	auto &slot = out->items[out->count++];
	slot.hwnd = hwnd;
	slot.bounds = rect;
	slot.visibleCount = visCount;
	for (int v = 0; v < visCount; ++v)
		slot.visibleRects[v] = visRects[v];
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
		if (!IsWindowVisible(hwnd)) {
			list->items[i] = list->items[--list->count];
			continue;
		}

		RECT rect{};
		if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
		                                    &rect, sizeof(rect))))
			list->items[i].bounds = rect;

		// z-order 위 창들로 잘라낸 가시영역을 매 프레임 재계산.
		// 완전히 가려져 있으면(0개) 즉시 추적 해제 — 뒤로 보내기/다른 앱 덮음 둘 다 커버.
		RECT visRects[SC_MAX_VISIBLE_SUBRECTS];
		int visCount = sc_compute_visible_subrects(hwnd, list->items[i].bounds,
		                                            visRects,
		                                            SC_MAX_VISIBLE_SUBRECTS);
		if (visCount == 0) {
			list->items[i] = list->items[--list->count];
			continue;
		}
		list->items[i].visibleCount = visCount;
		for (int v = 0; v < visCount; ++v)
			list->items[i].visibleRects[v] = visRects[v];
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

	// [Alt+Tab / Win+Tab Task View 가드]
	// Task Switcher / Multitasking View 창은 라이브 썸네일을 렌더하므로 블랙리스트
	// 앱이 최소화·뒷창인 상태에서도 노출될 수 있다. foreground가 스위처 클래스면
	// 마스크에 추가해 썸네일 영역을 통째로 가린다.
	//
	// foreground 기준을 쓰는 이유: 스위처는 키 입력을 받아야 하므로 정의상
	// foreground다(Alt+Tab은 Alt 잡혀있는 동안, Win+Tab은 사용자가 닫기 전까지).
	// 같은 XamlExplorerHostIslandWindow 클래스를 쓰는 Snap Layouts/Assist 오버레이는
	// 드래그 중인 사용자 창이 foreground라 자동 제외 — 평상시 false positive 방지.
	HWND fg = GetForegroundWindow();
	if (fg && is_task_switcher_window(fg) && out->count < SC_MAX_TRACKED_WINDOWS) {
		bool alreadyIn = false;
		for (int i = 0; i < out->count; ++i) {
			if (out->items[i].hwnd == fg) {
				alreadyIn = true;
				break;
			}
		}
		if (!alreadyIn) {
			RECT fgRect{};
			if (SUCCEEDED(DwmGetWindowAttribute(
			        fg, DWMWA_EXTENDED_FRAME_BOUNDS, &fgRect, sizeof(fgRect)))) {
				const int w = fgRect.right - fgRect.left;
				const int h = fgRect.bottom - fgRect.top;
				if (w >= MIN_WINDOW_DIMENSION && h >= MIN_WINDOW_DIMENSION) {
					auto &slot = out->items[out->count++];
					slot.hwnd = fg;
					slot.bounds = fgRect;
					slot.visibleCount = 1;
					slot.visibleRects[0] = fgRect;
					const wchar_t kMarker[] = L"__taskSw";
					for (size_t i = 0;
					     i < sizeof(slot.exe_name) / sizeof(slot.exe_name[0]); ++i) {
						slot.exe_name[i] = (i < sizeof(kMarker) / sizeof(kMarker[0]))
						                       ? kMarker[i]
						                       : L'\0';
						if (!slot.exe_name[i])
							break;
					}
				}
			}
		}
	}
}

namespace {

struct EnumAllCtx {
	TrackedWindowList *out;
	DWORD selfPid;
};

// 가시 top-level 창을 모두 캡처. 자기 자신(OBS) 프로세스는 제외.
// EnumWindows는 Z-order 위→아래 순으로 호출하므로 out도 자연스레 위쪽이 앞.
static BOOL CALLBACK enum_all_proc(HWND hwnd, LPARAM lparam)
{
	auto *ctx = reinterpret_cast<EnumAllCtx *>(lparam);
	if (ctx->out->count >= SC_MAX_TRACKED_WINDOWS)
		return FALSE; // 슬롯 가득

	if (!IsWindowVisible(hwnd))
		return TRUE;

	// 자기 프로세스 필터 (OBS preview 등이 PII 위에 잠깐 떠도 owner로 잡지 않게)
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == ctx->selfPid)
		return TRUE;

	RECT bounds{};
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
	                                 &bounds, sizeof(bounds))))
		return TRUE;

	const long w = bounds.right - bounds.left;
	const long h = bounds.bottom - bounds.top;
	if (w < MIN_WINDOW_DIMENSION || h < MIN_WINDOW_DIMENSION)
		return TRUE; // 너무 작은 창은 무시

	auto &item = ctx->out->items[ctx->out->count++];
	item.hwnd = hwnd;
	item.bounds = bounds;
	item.exe_name[0] = L'\0'; // owner 매칭에 불필요
	// allWindows는 owner 매칭(bounds 사용)에만 쓰여 visible 계산이 불필요.
	// 다만 struct 멤버가 unset이면 후속 코드가 잘못 읽을 수 있어 zero-init.
	item.visibleCount = 0;
	return TRUE;
}

} // namespace

extern "C" void sc_enum_all_visible_windows(TrackedWindowList *out)
{
	if (!out)
		return;
	out->count = 0;
	EnumAllCtx ctx{out, GetCurrentProcessId()};
	EnumWindows(enum_all_proc, reinterpret_cast<LPARAM>(&ctx));
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

// =============================================================================
// 최소화 애니메이션 가드 — WinEvent 훅 구현
//
// SetWinEventHook(WINEVENT_OUTOFCONTEXT)은 별도 시스템 스레드에서 콜백을
// 호출하므로 모든 글로벌 상태는 mutex로 보호한다. 콜백 진입 시점이 애니메이션
// "직전"이므로 DwmGetWindowAttribute로 받는 bounds가 pre-minimize 좌표다.
//
// 여러 SecureCast filter 인스턴스가 동시에 살아있을 수 있으므로 init/shutdown은
// atomic refcount 기반. ref 0→1로 올라갈 때만 SetWinEventHook, 1→0으로 내려갈
// 때만 UnhookWinEvent.
// =============================================================================
namespace {

struct MinimizeState {
	uint64_t startTick;   // GetTickCount64 ms — age-based eviction
	uint64_t endNs;       // os_gettime_ns at MINIMIZEEND. 0 = animation 진행 중.
	uint64_t endTick;     // GetTickCount64 at MINIMIZEEND. grace-period 계산용.
	RECT preBounds;
};

constexpr uint64_t kMinimizeEndGraceMs = 1500; // MINIMIZEEND 후 entry 유지 시간
                                                // (ring buffer 지연 + 여유)

std::mutex g_minimizeMutex;
std::unordered_map<HWND, MinimizeState> g_minimizingWindows;
HWINEVENTHOOK g_minimizeHook = nullptr;
std::atomic<int> g_minimizeRefCount{0};

// 콜백 시점에는 이미 minimize 애니메이션이 시작돼 DWM bounds가 mid-shrink일 수
// 있다. 따라서 (1) DWM EXTENDED_FRAME_BOUNDS, (2) GetWindowRect,
// (3) GetWindowPlacement.rcNormalPosition 세 값을 모두 받아 면적이 가장 큰 것을
// pre-minimize bounds로 채택. 면적이 0/음수면 polling 시점에 filter가 caller
// windowList에서 보강해주도록 빈 RECT로 등록(폴링 측에서 detection 가능).
RECT pick_pre_minimize_bounds(HWND hwnd)
{
	RECT dwm{}, rc{}, normal{};
	bool dwmOK = SUCCEEDED(DwmGetWindowAttribute(
		hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &dwm, sizeof(dwm)));
	bool rcOK = GetWindowRect(hwnd, &rc) != 0;

	WINDOWPLACEMENT wp{};
	wp.length = sizeof(wp);
	bool wpOK = GetWindowPlacement(hwnd, &wp) != 0;
	if (wpOK) {
		// rcNormalPosition은 workspace(작업영역) 좌표 — 작업표시줄 등을 보정한
		// 모니터 origin을 더해야 화면 좌표가 된다.
		HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi{};
		mi.cbSize = sizeof(mi);
		if (mon && GetMonitorInfo(mon, &mi)) {
			const LONG dx = mi.rcWork.left - mi.rcMonitor.left;
			const LONG dy = mi.rcWork.top - mi.rcMonitor.top;
			normal.left = wp.rcNormalPosition.left + mi.rcMonitor.left + dx;
			normal.top = wp.rcNormalPosition.top + mi.rcMonitor.top + dy;
			normal.right = wp.rcNormalPosition.right + mi.rcMonitor.left + dx;
			normal.bottom = wp.rcNormalPosition.bottom + mi.rcMonitor.top + dy;
		} else {
			wpOK = false;
		}
	}

	auto area = [](const RECT &r) -> LONGLONG {
		LONGLONG w = r.right - r.left;
		LONGLONG h = r.bottom - r.top;
		return (w > 0 && h > 0) ? (w * h) : 0;
	};

	const LONGLONG aDwm = dwmOK ? area(dwm) : 0;
	const LONGLONG aRc = rcOK ? area(rc) : 0;
	const LONGLONG aWp = wpOK ? area(normal) : 0;

	RECT best{};
	LONGLONG bestA = 0;
	if (aDwm > bestA) { best = dwm; bestA = aDwm; }
	if (aRc > bestA)  { best = rc;  bestA = aRc; }
	if (aWp > bestA)  { best = normal; bestA = aWp; }
	return best;
}

void CALLBACK MinimizeEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                 LONG idObject, LONG idChild, DWORD, DWORD)
{
	// 창 자체에 대한 이벤트만 (자식 컨트롤 제외).
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd)
		return;

	if (event == EVENT_SYSTEM_MINIMIZESTART) {
		RECT pre = pick_pre_minimize_bounds(hwnd);
		const bool valid = pre.right > pre.left && pre.bottom > pre.top;
		if (!valid)
			return;
		std::lock_guard<std::mutex> lock(g_minimizeMutex);
		g_minimizingWindows[hwnd] = {GetTickCount64(), 0, 0, pre};
	} else if (event == EVENT_SYSTEM_MINIMIZEEND) {
		// erase 대신 endNs를 기록 — 종료 후에도 filter가 grace 기간 동안 polling으로
		// endNs를 받아 lingering 컷오프 처리에 사용. grace 만료 시 evict.
		const uint64_t nowNs = os_gettime_ns();
		const uint64_t nowTick = GetTickCount64();
		std::lock_guard<std::mutex> lock(g_minimizeMutex);
		auto it = g_minimizingWindows.find(hwnd);
		if (it != g_minimizingWindows.end()) {
			it->second.endNs = nowNs;
			it->second.endTick = nowTick;
		}
	}
}

} // namespace

extern "C" void sc_minimize_tracker_init()
{
	// refcount는 shutdown과 1:1로 페어링되는 "훅을 원하는 필터 수"다(항상 +1).
	// 여기서 refcount를 롤백하면 shutdown의 -1과 짝이 안 맞아 언더플로가 난다.
	g_minimizeRefCount.fetch_add(1, std::memory_order_acq_rel);
	// [Hook-fix] 훅이 아직 없으면(첫 참조이거나 이전 SetWinEventHook 실패) (재)시도.
	// 이전엔 첫 참조에서 실패하면 refcount만 오르고 영영 재시도하지 않아, 살아있는
	// 필터가 있는 동안 최소화 추적이 비활성이었다. null일 때만 거는 것으로 교정.
	if (!g_minimizeHook)
		g_minimizeHook = SetWinEventHook(
			EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
			nullptr, MinimizeEventProc, 0, 0,
			WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

extern "C" void sc_minimize_tracker_shutdown()
{
	if (g_minimizeRefCount.fetch_sub(1, std::memory_order_acq_rel) != 1)
		return; // 아직 다른 참조가 있음
	if (g_minimizeHook) {
		UnhookWinEvent(g_minimizeHook);
		g_minimizeHook = nullptr;
	}
	std::lock_guard<std::mutex> lock(g_minimizeMutex);
	g_minimizingWindows.clear();
}

extern "C" int sc_poll_minimizing_windows(ScMinimizingEntry *out, int maxOut,
                                          uint64_t maxAgeMs)
{
	if (!out || maxOut <= 0)
		return 0;
	const uint64_t now = GetTickCount64();
	std::lock_guard<std::mutex> lock(g_minimizeMutex);
	int count = 0;
	for (auto it = g_minimizingWindows.begin();
	     it != g_minimizingWindows.end();) {
		const auto &st = it->second;
		// (1) 종료된 entry는 grace 만료 시 evict.
		// (2) 종료되지 않은 entry는 startTick 기준 age 만료 시 evict
		//     (MINIMIZEEND가 누락되는 경우 누적 방지).
		const bool ended = st.endNs != 0;
		const bool expired =
			ended ? (now - st.endTick > kMinimizeEndGraceMs)
			      : (now - st.startTick > maxAgeMs);
		if (expired) {
			it = g_minimizingWindows.erase(it);
			continue;
		}
		if (count < maxOut) {
			out[count].hwnd = it->first;
			out[count].preBounds = st.preBounds;
			out[count].endNs = st.endNs;
			++count;
		}
		++it;
	}
	return count;
}

extern "C" uint64_t sc_get_minimize_end_ns(HWND hwnd)
{
	std::lock_guard<std::mutex> lock(g_minimizeMutex);
	auto it = g_minimizingWindows.find(hwnd);
	if (it == g_minimizingWindows.end())
		return 0;
	return it->second.endNs;
}

// =============================================================================
// [Resize 가드] WinEvent MOVESIZESTART/END + showCmd 폴링 신호
//
// minimize tracker와 같은 패턴 — refcount 기반 init/shutdown.
// 두 신호 소스(WinEvent 드래그 리사이즈, showCmd 변화 maximize/restore)는
// 모두 g_resizeMap의 endTick을 갱신해 동일 grace 윈도우로 통합.
// =============================================================================
namespace {

struct ResizeState {
	uint64_t startTick; // GetTickCount64 ms — 트랜지션 시작 또는 마지막 갱신
	uint64_t endTick;   // GetTickCount64 ms — MOVESIZEEND/showCmd 안정화 시점
};

std::mutex g_resizeMutex;
std::unordered_map<HWND, ResizeState> g_resizingWindows;
HWINEVENTHOOK g_resizeHook = nullptr;
std::atomic<int> g_resizeRefCount{0};

// showCmd 폴링 캐시 (사용자 드래그 외 maximize 버튼/단축키 트랜지션 감지용).
std::unordered_map<HWND, UINT> g_lastShowCmd;

void CALLBACK ResizeEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                              LONG idObject, LONG idChild, DWORD, DWORD)
{
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd)
		return;
	const uint64_t nowTick = GetTickCount64();
	std::lock_guard<std::mutex> lock(g_resizeMutex);
	if (event == EVENT_SYSTEM_MOVESIZESTART) {
		g_resizingWindows[hwnd] = {nowTick, 0};
	} else if (event == EVENT_SYSTEM_MOVESIZEEND) {
		auto it = g_resizingWindows.find(hwnd);
		if (it != g_resizingWindows.end())
			it->second.endTick = nowTick;
		else
			g_resizingWindows[hwnd] = {nowTick, nowTick};
	}
}

} // namespace

extern "C" void sc_resize_tracker_init()
{
	// refcount는 shutdown과 페어링되므로 항상 +1 (롤백하면 언더플로).
	g_resizeRefCount.fetch_add(1, std::memory_order_acq_rel);
	// [Hook-fix] 훅이 null일 때만 (재)시도 (minimize와 동일 — 영구 비활성 방지).
	if (!g_resizeHook)
		g_resizeHook = SetWinEventHook(
			EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, nullptr,
			ResizeEventProc, 0, 0,
			WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

extern "C" void sc_resize_tracker_shutdown()
{
	if (g_resizeRefCount.fetch_sub(1, std::memory_order_acq_rel) != 1)
		return;
	if (g_resizeHook) {
		UnhookWinEvent(g_resizeHook);
		g_resizeHook = nullptr;
	}
	std::lock_guard<std::mutex> lock(g_resizeMutex);
	g_resizingWindows.clear();
	g_lastShowCmd.clear();
}

extern "C" void sc_notify_showcmd_change(HWND hwnd)
{
	if (!hwnd)
		return;
	WINDOWPLACEMENT wp{};
	wp.length = sizeof(wp);
	if (!GetWindowPlacement(hwnd, &wp))
		return;
	const UINT cur = wp.showCmd;
	const uint64_t nowTick = GetTickCount64();
	std::lock_guard<std::mutex> lock(g_resizeMutex);
	auto it = g_lastShowCmd.find(hwnd);
	if (it == g_lastShowCmd.end()) {
		g_lastShowCmd[hwnd] = cur;
		return;
	}
	if (it->second != cur) {
		const UINT prev = it->second;
		it->second = cur;
		// minimize 관련 트랜지션(SW_SHOWMINIMIZED/MINIMIZE/SHOWMINNOACTIVE)은
		// 신호 발생 안 함 — minimize는 owner가 숨겨지는 거라 expansion이 필요
		// 없고, 신호 발생 시 sticky가 켜져 전체 화면 블러 부작용 발생.
		// NORMAL ↔ MAXIMIZED 같은 visible-state 간 트랜지션만 신호.
		auto isMinimizedState = [](UINT s) {
			return s == SW_SHOWMINIMIZED || s == SW_MINIMIZE ||
			       s == SW_SHOWMINNOACTIVE;
		};
		if (isMinimizedState(prev) || isMinimizedState(cur))
			return;
		// 진짜 트랜지션 (maximize/restore) — endTick 갱신해 grace 카운트 시작.
		auto rit = g_resizingWindows.find(hwnd);
		if (rit != g_resizingWindows.end())
			rit->second.endTick = nowTick;
		else
			g_resizingWindows[hwnd] = {nowTick, nowTick};
	}
}

extern "C" bool sc_is_blacklisted_exe(const wchar_t *exe_name)
{
	return is_blacklisted(exe_name);
}

extern "C" bool sc_is_blacklisted_exe_game_mode(const wchar_t *exe_name)
{
	return is_blacklisted_game_mode(exe_name);
}

extern "C" bool sc_is_blacklisted_exe_normal_only(const wchar_t *exe_name)
{
	return is_blacklisted_normal_only(exe_name);
}

// 사용자 설정 → 동적 블랙리스트 갱신. 줄바꿈으로 구분된 텍스트 파싱.
// settings update 호출 시 1회 호출.
extern "C" void sc_set_user_blacklist_normal(const wchar_t *const *exes,
                                              int count)
{
	std::lock_guard<std::mutex> lock(g_userBlacklistMutex);
	g_userBlacklistNormal.clear();
	for (int i = 0; i < count; ++i) {
		if (exes[i] && exes[i][0])
			g_userBlacklistNormal.emplace_back(exes[i]);
	}
}

extern "C" void sc_set_user_blacklist_game_mode(const wchar_t *const *exes,
                                                 int count)
{
	std::lock_guard<std::mutex> lock(g_userBlacklistMutex);
	g_userBlacklistGameMode.clear();
	for (int i = 0; i < count; ++i) {
		if (exes[i] && exes[i][0])
			g_userBlacklistGameMode.emplace_back(exes[i]);
	}
}

// 어느 필터든 게임 모드면 true로 설정 — is_blacklisted가 자동으로
// game-mode-extra 적용. enum_proc 등 모든 scan 경로 무수정 적용.
//
// 다중 필터 인스턴스 안전: refcount 기반. active=true는 +1, false는 -1.
// 같은 필터에서 OFF가 중복 호출되어도 음수로 빠지지 않도록 underflow 가드.
extern "C" void sc_set_global_game_mode(bool active)
{
	if (active) {
		g_gameMode_RefCount.fetch_add(1, std::memory_order_acq_rel);
	} else {
		int prev = g_gameMode_RefCount.fetch_sub(1, std::memory_order_acq_rel);
		if (prev <= 0) {
			// 중복 OFF — 음수가 되지 않도록 되돌림.
			g_gameMode_RefCount.fetch_add(1, std::memory_order_acq_rel);
		}
	}
}

// refcount > 0 이면 어떤 필터든 게임 모드. OCR 워커 등에서 무거운 작업
// skip 분기에 사용. lock-free atomic load — 매 프레임 호출 안전.
extern "C" bool sc_any_filter_in_game_mode()
{
	return g_gameMode_RefCount.load(std::memory_order_acquire) > 0;
}

// [T03] Tier 1 빌트인 게임 리스트 검사. case-insensitive basename 매칭.
// nullptr/빈 문자열 안전.
extern "C" bool sc_is_known_game(const wchar_t *exe_name)
{
	if (!exe_name || !exe_name[0])
		return false;
	for (size_t i = 0; i < securecast::kKnownGameExesCount; ++i) {
		if (iequals(securecast::kKnownGameExes[i], exe_name))
			return true;
	}
	return false;
}

// [T03] Tier 1 + Tier 3 통합 검사. 게임 모드 trigger / fg 제외 판정에 사용.
extern "C" bool sc_is_known_game_or_user_game(const wchar_t *exe_name)
{
	if (!exe_name || !exe_name[0])
		return false;
	if (sc_is_known_game(exe_name))
		return true;
	std::lock_guard<std::mutex> lock(g_userGameListMutex);
	for (const auto &entry : g_userGameList) {
		if (iequals(entry.c_str(), exe_name))
			return true;
	}
	return false;
}

// [T03] Tier 3 사용자 등록 게임 리스트 갱신. settings update 콜백에서 1회 호출.
// nullptr/빈 entry는 자동 skip.
extern "C" void sc_set_user_game_list(const wchar_t *const *exes, int count)
{
	std::lock_guard<std::mutex> lock(g_userGameListMutex);
	g_userGameList.clear();
	if (!exes || count <= 0)
		return;
	g_userGameList.reserve(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i) {
		if (exes[i] && exes[i][0])
			g_userGameList.emplace_back(exes[i]);
	}
}

// ============================================================
// [실시간 경로 매칭] 게임 스토어 설치 폴더 기반 게임 판정.
//   g_gameDirs: 정규화(소문자, 끝에 '\')된 게임 설치 폴더 prefix 목록.
//   활성창 exe의 전체 경로가 이 중 하나로 시작하면 "게임"으로 인지한다.
//   autodetect(주기/수동)가 스토어 설치 경로를 모아 sc_set_game_dirs로 갱신.
// ============================================================
std::mutex g_gameDirMutex;
std::vector<std::wstring> g_gameDirs;

// [사용자 추가 제외 — "게임 제외 목록"] OBS 설정에서 사용자가 입력한 항목.
//   - exe 이름(.exe로 끝남)  → g_userExcludeExes (basename 정확 일치로 제외)
//   - 그 외(폴더 이름)        → g_userExcludeSegs (\name\ 경로 세그먼트로 제외)
// 둘 다 소문자. 제외에 걸리면 목록/경로/CPU와 무관하게 "게임 아님"으로 판정.
std::mutex g_pathExcludeMutex;
std::vector<std::wstring> g_userExcludeSegs;
std::vector<std::wstring> g_userExcludeExes;

static std::wstring sc_lower_ws(const wchar_t *s)
{
	std::wstring r;
	if (!s)
		return r;
	for (; *s; ++s)
		r.push_back(static_cast<wchar_t>(towlower(*s)));
	return r;
}

extern "C" void sc_set_game_dirs(const wchar_t *const *dirs, int count)
{
	std::lock_guard<std::mutex> lock(g_gameDirMutex);
	g_gameDirs.clear();
	if (!dirs || count <= 0)
		return;
	g_gameDirs.reserve(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i) {
		if (!dirs[i] || !dirs[i][0])
			continue;
		std::wstring d = sc_lower_ws(dirs[i]);
		if (d.empty())
			continue;
		if (d.back() != L'\\')
			d.push_back(L'\\');  // prefix 매칭 시 폴더 경계 보장
		g_gameDirs.push_back(std::move(d));
	}
}

// 게임 스토어 폴더(특히 ...\steamapps\common) 아래에 있지만 게임이 아닌 항목들.
// 경로 매칭에서 제외해 OCR이 잘못 꺼지는 것을 막는다(PII 노출 위험 회피).
// 경로 세그먼트(앞뒤 '\')로 매칭 → 폴더 이름 전체가 일치해야 함. 전부 소문자.
// 필요 시 여기에 폴더명을 추가하면 됨(예: 새로운 비-게임 Steam 앱).
static const wchar_t *const kPathExcludeSegments[] = {
	L"\\steamworks common redistributables\\", // Steam 재배포 패키지
	L"\\steamvr\\",                            // SteamVR 런타임
	L"\\wallpaper_engine\\",                   // Wallpaper Engine (폴더명)
	L"\\wallpaper engine\\",                   // (혹시 모를 변형)
	L"\\spacewar\\",                           // Steam 테스트 앱
	L"\\steam linux runtime\\",                // Linux 런타임
	L"\\proton\\",                             // Proton(Linux 호환)
};

// [게임 제외 목록] OBS 설정 항목 갱신. update 콜백에서 호출.
//   "Wallpaper Engine"(폴더) → "\wallpaper engine\" 세그먼트
//   "wallpaper64.exe"(exe)   → "wallpaper64.exe" exe 이름
extern "C" void sc_set_user_path_excludes(const wchar_t *const *names, int count)
{
	std::lock_guard<std::mutex> lock(g_pathExcludeMutex);
	g_userExcludeSegs.clear();
	g_userExcludeExes.clear();
	if (!names || count <= 0)
		return;
	for (int i = 0; i < count; ++i) {
		if (!names[i] || !names[i][0])
			continue;
		std::wstring s = sc_lower_ws(names[i]);
		// 앞뒤 공백/슬래시 제거.
		size_t b = s.find_first_not_of(L" \t\\/");
		size_t e = s.find_last_not_of(L" \t\\/");
		if (b == std::wstring::npos)
			continue;
		s = s.substr(b, e - b + 1);
		if (s.empty())
			continue;
		// ".exe"로 끝나면 exe 이름 제외, 아니면 폴더 세그먼트 제외.
		if (s.size() >= 4 && s.compare(s.size() - 4, 4, L".exe") == 0)
			g_userExcludeExes.push_back(std::move(s));
		else
			g_userExcludeSegs.push_back(L"\\" + s + L"\\");
	}
}

// [게임 제외 판정] 빌트인 폴더 + 사용자 폴더/exe 제외에 걸리면 true(=게임 아님).
//   path_lower: 소문자 전체 경로, base_lower: 소문자 exe basename.
static bool sc_is_excluded_from_game(const std::wstring &path_lower,
                                     const std::wstring &base_lower)
{
	for (const wchar_t *seg : kPathExcludeSegments) {
		if (path_lower.find(seg) != std::wstring::npos)
			return true;
	}
	std::lock_guard<std::mutex> lock(g_pathExcludeMutex);
	for (const auto &seg : g_userExcludeSegs) {
		if (path_lower.find(seg) != std::wstring::npos)
			return true;
	}
	for (const auto &exe : g_userExcludeExes) {
		if (base_lower == exe)
			return true;
	}
	return false;
}

// 경로 기반 게임 판정(순수). 제외 처리는 sc_hwnd_is_game에서 별도로 수행한다.
extern "C" bool sc_is_game_by_path(const wchar_t *full_exe_path)
{
	if (!full_exe_path || !full_exe_path[0])
		return false;
	const std::wstring p = sc_lower_ws(full_exe_path);
	std::lock_guard<std::mutex> lock(g_gameDirMutex);
	for (const auto &d : g_gameDirs) {
		if (d.size() <= p.size() && p.compare(0, d.size(), d) == 0)
			return true;
	}
	return false;
}

extern "C" bool sc_hwnd_is_game(HWND hwnd, wchar_t *out_exe, size_t out_cap)
{
	if (out_exe && out_cap)
		out_exe[0] = 0;
	if (!hwnd)
		return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == 0)
		return false;
	HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc)
		return false;
	wchar_t exe_path[MAX_PATH] = {};
	DWORD path_size = MAX_PATH;
	bool ok = QueryFullProcessImageNameW(proc, 0, exe_path, &path_size) != 0;
	CloseHandle(proc);
	if (!ok)
		return false;
	wchar_t base[128] = {};
	path_basename(exe_path, base, 128);
	if (out_exe && out_cap)
		lstrcpynW(out_exe, base, static_cast<int>(out_cap));
	// [게임 제외 목록] 제외에 걸리면 목록/경로와 무관하게 "게임 아님".
	const std::wstring path_lower = sc_lower_ws(exe_path);
	const std::wstring base_lower = sc_lower_ws(base);
	if (sc_is_excluded_from_game(path_lower, base_lower))
		return false;
	// 목록(빌트인/사용자) 또는 실시간 경로(스토어 설치 폴더) 중 하나라도 맞으면 게임.
	if (sc_is_known_game_or_user_game(base))
		return true;
	if (sc_is_game_by_path(exe_path))
		return true;
	return false;
}

// 빌트인 친화명 매핑에서 exe 검색. 매칭되면 out_buf에 복사 후 true.
extern "C" bool sc_lookup_friendly_name(const wchar_t *exe_name,
                                          wchar_t *out_buf, size_t out_cap)
{
	if (!exe_name || !exe_name[0] || !out_buf || out_cap == 0)
		return false;
	for (size_t i = 0; i < securecast::kKnownExeNamesCount; ++i) {
		const auto &entry = securecast::kKnownExeNames[i];
		if (iequals(entry.exe, exe_name)) {
			size_t n = 0;
			while (entry.name[n] && n + 1 < out_cap) {
				out_buf[n] = entry.name[n];
				++n;
			}
			out_buf[n] = 0;
			return true;
		}
	}
	return false;
}

extern "C" bool sc_get_hwnd_exe_name(HWND hwnd, wchar_t *out, size_t out_cap)
{
	if (!hwnd || !out || out_cap == 0)
		return false;
	out[0] = 0;
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == 0)
		return false;
	HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc)
		return false;
	wchar_t exe_path[MAX_PATH] = {};
	DWORD path_size = MAX_PATH;
	bool ok = QueryFullProcessImageNameW(proc, 0, exe_path, &path_size) != 0;
	CloseHandle(proc);
	if (!ok)
		return false;
	path_basename(exe_path, out, out_cap);
	return out[0] != 0;
}

extern "C" bool sc_is_window_resizing(HWND hwnd, uint64_t graceMs)
{
	if (!hwnd)
		return false;
	const uint64_t nowTick = GetTickCount64();
	std::lock_guard<std::mutex> lock(g_resizeMutex);
	auto it = g_resizingWindows.find(hwnd);
	if (it == g_resizingWindows.end())
		return false;
	const auto &st = it->second;
	if (st.endTick == 0)
		return true; // MOVESIZESTART 후 END 미수신 — 진행 중
	// END 수신 후 graceMs 동안 true 유지 (송출 N프레임 지연 흡수).
	if (nowTick - st.endTick > graceMs) {
		g_resizingWindows.erase(it); // grace 만료 → evict
		return false;
	}
	return true;
}

namespace {

// EnumWindows callback for sc_find_all_alive_blacklist_windows.
// 살아있는 BL exe의 HWND 중 사용자에게 노출 가능한 창만 통과시킨다:
//   - IsWindowVisible=false → 일반적으로 Electron/Chromium 앱의 hidden helper
//     hwnd (Discord/Slack 등에서 다수 존재). 사용자에게 안 보이므로 lingering
//     등록 시 실제 가시 창보다 큰 박스가 생기는 원인. 거른다.
//   - non-iconic 창 중 100x100 미만 → 마찬가지로 helper hwnd 의심. 거른다.
//   - iconic(minimized) 창 → peek 시 잠시 화면에 뜨므로 통과. bounds는 아래에서
//     rcNormalPosition으로 산출한다(GetWindowRect는 화면 밖 좌표라 무의미).
BOOL CALLBACK enum_all_blacklist_proc(HWND hwnd, LPARAM lparam)
{
	auto *out = reinterpret_cast<TrackedWindowList *>(lparam);
	if (out->count >= SC_MAX_TRACKED_WINDOWS)
		return FALSE;

	// 닫힌 창은 패스 (EnumWindows는 보통 살아있는 것만 주지만 방어적).
	if (!IsWindow(hwnd))
		return TRUE;

	// hidden helper 거르기 — minimized는 visible=true 유지하므로 통과한다.
	if (!IsWindowVisible(hwnd))
		return TRUE;

	// 프로세스 → exe 매칭.
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == 0)
		return TRUE;
	HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc)
		return TRUE;
	wchar_t exe_path[MAX_PATH] = {};
	DWORD path_size = MAX_PATH;
	bool ok = QueryFullProcessImageNameW(proc, 0, exe_path, &path_size) != 0;
	CloseHandle(proc);
	if (!ok)
		return TRUE;
	wchar_t exe_name[64] = {};
	path_basename(exe_path, exe_name, sizeof(exe_name) / sizeof(exe_name[0]));
	if (iequals(exe_name, kUwpHost))
		return TRUE;
	if (!is_blacklisted(exe_name))
		return TRUE;

	// non-iconic 가시 창 중 100x100 미만은 helper hwnd로 보고 거른다.
	// iconic이면 GetWindowRect가 화면 밖 좌표라 의미 없으므로 size check를 건너뛴다.
	const bool iconic = IsIconic(hwnd) != 0;
	if (!iconic) {
		RECT wr{};
		if (GetWindowRect(hwnd, &wr)) {
			const LONG w = wr.right - wr.left;
			const LONG h = wr.bottom - wr.top;
			if (w < MIN_WINDOW_DIMENSION || h < MIN_WINDOW_DIMENSION)
				return TRUE;
		}
	}

	// Bounds 산출:
	//   iconic이면 GetWindowPlacement.rcNormalPosition (workspace 좌표)을 모니터
	//   origin 보정해서 화면 좌표로 변환.
	//   non-iconic이면 DWM EXTENDED_FRAME_BOUNDS 우선, 실패하면 GetWindowRect.
	RECT bounds{};
	bool gotBounds = false;
	if (iconic) {
		WINDOWPLACEMENT wp{};
		wp.length = sizeof(wp);
		if (GetWindowPlacement(hwnd, &wp)) {
			HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
			MONITORINFO mi{};
			mi.cbSize = sizeof(mi);
			if (mon && GetMonitorInfo(mon, &mi)) {
				const LONG dx = mi.rcWork.left - mi.rcMonitor.left;
				const LONG dy = mi.rcWork.top - mi.rcMonitor.top;
				bounds.left = wp.rcNormalPosition.left + mi.rcMonitor.left + dx;
				bounds.top = wp.rcNormalPosition.top + mi.rcMonitor.top + dy;
				bounds.right = wp.rcNormalPosition.right + mi.rcMonitor.left + dx;
				bounds.bottom = wp.rcNormalPosition.bottom + mi.rcMonitor.top + dy;
				gotBounds = true;
			}
		}
	}
	if (!gotBounds) {
		if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
		                                  &bounds, sizeof(bounds)))) {
			if (!GetWindowRect(hwnd, &bounds))
				return TRUE;
		}
	}
	if (bounds.right <= bounds.left || bounds.bottom <= bounds.top)
		return TRUE;

	auto &slot = out->items[out->count++];
	slot.hwnd = hwnd;
	slot.bounds = bounds;
	slot.visibleCount = 1;
	slot.visibleRects[0] = bounds;
	for (size_t i = 0; i < sizeof(slot.exe_name) / sizeof(slot.exe_name[0]); ++i) {
		slot.exe_name[i] = exe_name[i];
		if (!exe_name[i])
			break;
	}
	return TRUE;
}

} // namespace

extern "C" void sc_find_all_alive_blacklist_windows(TrackedWindowList *out)
{
	if (!out)
		return;
	out->count = 0;
	EnumWindows(enum_all_blacklist_proc, reinterpret_cast<LPARAM>(out));
}

extern "C" bool sc_mouse_over_taskbar()
{
	POINT pt{};
	if (!GetCursorPos(&pt))
		return false;

	// 주 모니터 작업표시줄.
	HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
	if (tray) {
		RECT r{};
		if (GetWindowRect(tray, &r) && PtInRect(&r, pt))
			return true;
	}

	// 보조 모니터들의 작업표시줄 (멀티모니터).
	HWND tray2 = nullptr;
	while ((tray2 = FindWindowExW(nullptr, tray2, L"Shell_SecondaryTrayWnd",
	                               nullptr)) != nullptr) {
		RECT r{};
		if (GetWindowRect(tray2, &r) && PtInRect(&r, pt))
			return true;
	}
	return false;
}

namespace {

// 작업표시줄의 화면 안쪽 방향 인접 영역(thumbnail 영역) RECT 산출.
// 작업표시줄이 모니터 어느 변에 붙어있든 자동 판정해 zoneDepth만큼 안쪽 띠 반환.
// 작업표시줄 자체 영역은 제외(= peek 검출은 thumbnail 영역에서만 발동).
bool tray_thumbnail_zone(HWND tray, int zoneDepth, RECT *outZone)
{
	if (!tray || !outZone)
		return false;
	RECT trayRect{};
	if (!GetWindowRect(tray, &trayRect))
		return false;
	HMONITOR mon = MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{};
	mi.cbSize = sizeof(mi);
	if (!mon || !GetMonitorInfo(mon, &mi))
		return false;

	constexpr int kEdgeTol = 12; // 모니터 경계 매칭 허용 오차 (DPI/그림자 보정)

	auto clamp = [](LONG v, LONG lo, LONG hi) {
		return v < lo ? lo : (v > hi ? hi : v);
	};

	if (trayRect.bottom >= mi.rcMonitor.bottom - kEdgeTol) {
		// 하단 작업표시줄 → 위쪽 띠
		outZone->left = trayRect.left;
		outZone->right = trayRect.right;
		outZone->bottom = trayRect.top;
		outZone->top = clamp(trayRect.top - zoneDepth, mi.rcMonitor.top,
		                      mi.rcMonitor.bottom);
	} else if (trayRect.top <= mi.rcMonitor.top + kEdgeTol) {
		// 상단 작업표시줄 → 아래쪽 띠
		outZone->left = trayRect.left;
		outZone->right = trayRect.right;
		outZone->top = trayRect.bottom;
		outZone->bottom = clamp(trayRect.bottom + zoneDepth, mi.rcMonitor.top,
		                         mi.rcMonitor.bottom);
	} else if (trayRect.left <= mi.rcMonitor.left + kEdgeTol) {
		// 좌측 작업표시줄 → 오른쪽 띠
		outZone->top = trayRect.top;
		outZone->bottom = trayRect.bottom;
		outZone->left = trayRect.right;
		outZone->right = clamp(trayRect.right + zoneDepth, mi.rcMonitor.left,
		                        mi.rcMonitor.right);
	} else {
		// 우측 작업표시줄 → 왼쪽 띠
		outZone->top = trayRect.top;
		outZone->bottom = trayRect.bottom;
		outZone->right = trayRect.left;
		outZone->left = clamp(trayRect.left - zoneDepth, mi.rcMonitor.left,
		                       mi.rcMonitor.right);
	}
	return outZone->right > outZone->left && outZone->bottom > outZone->top;
}

} // namespace

extern "C" bool sc_mouse_over_thumbnail_zone()
{
	POINT pt{};
	if (!GetCursorPos(&pt))
		return false;
	constexpr int kZoneDepth = 400; // 작업표시줄에서 화면 안쪽으로 픽셀

	HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
	if (tray) {
		RECT zone{};
		if (tray_thumbnail_zone(tray, kZoneDepth, &zone) && PtInRect(&zone, pt))
			return true;
	}
	HWND tray2 = nullptr;
	while ((tray2 = FindWindowExW(nullptr, tray2, L"Shell_SecondaryTrayWnd",
	                               nullptr)) != nullptr) {
		RECT zone{};
		if (tray_thumbnail_zone(tray2, kZoneDepth, &zone) && PtInRect(&zone, pt))
			return true;
	}
	return false;
}

// ────────────────────────────────────────────────────────────
// UI Automation 기반 작업표시줄 블랙리스트 버튼 위치 매핑
//
// 접근: 마우스 아래 element를 식별하는 대신, 살아있는 블랙리스트 앱의 작업표시줄
// 버튼 BoundingRectangle을 사전에 enum해서 캐시. 마우스 위치 검사 시 캐시 hit만
// 확인 — 사용자 환경의 UIA Name 표기에 무관하게 exe 이름 기반으로 매칭.
// ────────────────────────────────────────────────────────────
namespace {

bool point_over_any_taskbar(POINT pt)
{
	HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
	if (tray) {
		RECT r{};
		if (GetWindowRect(tray, &r) && PtInRect(&r, pt))
			return true;
	}
	HWND tray2 = nullptr;
	while ((tray2 = FindWindowExW(nullptr, tray2, L"Shell_SecondaryTrayWnd",
	                               nullptr)) != nullptr) {
		RECT r{};
		if (GetWindowRect(tray2, &r) && PtInRect(&r, pt))
			return true;
	}
	return false;
}

bool pid_to_exe_name(DWORD pid, wchar_t *exe_name, size_t cap)
{
	if (pid == 0 || cap == 0)
		return false;
	HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc)
		return false;
	wchar_t exe_path[MAX_PATH] = {};
	DWORD path_size = MAX_PATH;
	bool ok = QueryFullProcessImageNameW(proc, 0, exe_path, &path_size) != 0;
	CloseHandle(proc);
	if (!ok)
		return false;
	path_basename(exe_path, exe_name, cap);
	return true;
}

// Name 속성 보조 매칭 사전 — UIA가 작업표시줄 버튼에 대해 PID를 노출하지 않을 때
// fallback. Win11의 새 작업표시줄은 모든 버튼이 explorer.exe로 보고되므로 사실상
// 이 Name 매칭이 주된 식별 경로.
// exe별로 displayName 후보를 묶어, "살아있는 BL 앱의 exe"에 매핑된 표기만 매칭
// 활성화한다. 카톡이 핀만 되어 있고 종료 상태면 'KakaoTalk' Name도 매칭에서 제외
// → '카카오톡 고정됨' false positive 차단.
struct BlacklistAppNames {
	const wchar_t *exe;            // basename (예: L"KakaoTalk.exe"), iequals 비교
	const wchar_t *displayNames[4]; // null-terminated, 다국어 표기
};

const BlacklistAppNames kBlacklistApps[] = {
	{L"KakaoTalk.exe", {L"KakaoTalk", L"카카오톡", nullptr, nullptr}},
	{L"Discord.exe",   {L"Discord", nullptr, nullptr, nullptr}},
	{L"Slack.exe",     {L"Slack", nullptr, nullptr, nullptr}},
};

bool wcsistr_contains(const wchar_t *hay, const wchar_t *needle)
{
	if (!hay || !needle || !*needle)
		return false;
	for (const wchar_t *h = hay; *h; ++h) {
		const wchar_t *a = h;
		const wchar_t *b = needle;
		while (*a && *b && towlower(*a) == towlower(*b)) {
			++a;
			++b;
		}
		if (!*b)
			return true;
	}
	return false;
}

// 살아있는 BL 앱 목록(`alive` — TrackedWindowList의 exe_name들)에 매핑된 displayName
// 중 하나라도 Name에 포함되면 true. 매칭된 exe(`kBlacklistApps[].exe`)는 out_exe로
// 반환되어 호출자가 lingering 등록 대상을 좁히는 데 사용한다.
// alive가 비어있으면 항상 false (B안: 살아있어야만 가드 대상).
bool name_matches_alive_blacklist(const wchar_t *name,
                                  const TrackedWindowList *alive,
                                  wchar_t *out_exe, size_t out_cap)
{
	if (out_exe && out_cap > 0)
		out_exe[0] = 0;
	if (!name || !*name || !alive || alive->count == 0)
		return false;
	for (int i = 0; i < alive->count; ++i) {
		const wchar_t *aliveExe = alive->items[i].exe_name;
		for (const auto &app : kBlacklistApps) {
			if (!iequals(app.exe, aliveExe))
				continue;
			for (const wchar_t *display : app.displayNames) {
				if (!display)
					break;
				if (wcsistr_contains(name, display)) {
					if (out_exe && out_cap > 0) {
						size_t i2 = 0;
						while (app.exe[i2] && i2 + 1 < out_cap) {
							out_exe[i2] = app.exe[i2];
							++i2;
						}
						out_exe[i2] = 0;
					}
					return true;
				}
			}
		}
	}
	return false;
}

// UIA COM 상태 (호출 스레드 기준 lazy init).
std::atomic<bool> g_uiaInitTried{false};
std::atomic<bool> g_uiaCoInitOk{false};
IUIAutomation *g_uiaPtr = nullptr;
std::mutex g_uiaInitMtx;

IUIAutomation *get_or_create_uia()
{
	if (g_uiaPtr)
		return g_uiaPtr;
	std::lock_guard<std::mutex> lock(g_uiaInitMtx);
	if (g_uiaPtr)
		return g_uiaPtr;
	if (!g_uiaInitTried.exchange(true)) {
		HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		g_uiaCoInitOk.store(SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE);
	}
	if (!g_uiaCoInitOk.load())
		return nullptr;
	IUIAutomation *uia = nullptr;
	HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr,
	                              CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
	                              reinterpret_cast<void **>(&uia));
	if (FAILED(hr) || !uia)
		return nullptr;
	g_uiaPtr = uia;
	return g_uiaPtr;
}

// 한 버튼 element가 블랙리스트 앱인지 다단계로 판정. 매칭된 BL exe는 out_exe로
// 반환되어 호출자가 lingering 등록 대상을 좁히는 데 사용된다.
// 1) NativeWindowHandle → 그 hwnd의 PID → exe
// 2) 안 되면 element ProcessId 직접
// 3) 둘 다 fail이면 alive BL exe set에 매핑된 displayName으로 Name 매칭
// explorer.exe인 경우는 작업표시줄 자체 element라 1)/2)에서는 false로 떨어지지만
// Win11 작업표시줄은 모든 버튼이 explorer.exe라 사실상 3) Name 매칭이 주된 경로.
// alive list가 비어있으면 3)도 항상 false → BL 결정 불가능 = false 반환.
bool element_is_blacklist_btn(IUIAutomationElement *btn,
                              const TrackedWindowList *alive,
                              wchar_t *out_exe, size_t out_cap)
{
	if (out_exe && out_cap > 0)
		out_exe[0] = 0;
	if (!btn)
		return false;

	DWORD pid = 0;
	UIA_HWND uhwnd = nullptr;
	if (SUCCEEDED(btn->get_CurrentNativeWindowHandle(&uhwnd)) && uhwnd) {
		GetWindowThreadProcessId(reinterpret_cast<HWND>(uhwnd), &pid);
	}
	if (pid == 0) {
		int ipid = 0;
		if (SUCCEEDED(btn->get_CurrentProcessId(&ipid)) && ipid > 0)
			pid = static_cast<DWORD>(ipid);
	}

	if (pid != 0) {
		wchar_t exe_name[64] = {};
		if (pid_to_exe_name(pid, exe_name,
		                    sizeof(exe_name) / sizeof(exe_name[0]))) {
			if (iequals(exe_name, L"explorer.exe"))
				; // pass — Name으로 보조 매칭 한 번 더 시도
			else if (is_blacklisted(exe_name)) {
				if (out_exe && out_cap > 0) {
					size_t i = 0;
					while (exe_name[i] && i + 1 < out_cap) {
						out_exe[i] = exe_name[i];
						++i;
					}
					out_exe[i] = 0;
				}
				return true;
			} else
				return false; // 다른 앱 확정
		}
	}

	// Name 속성 보조 매칭 (alive BL exe set과 교차매칭)
	BSTR bstrName = nullptr;
	bool nameHit = false;
	if (SUCCEEDED(btn->get_CurrentName(&bstrName)) && bstrName) {
		nameHit = name_matches_alive_blacklist(bstrName, alive, out_exe,
		                                        out_cap);
		SysFreeString(bstrName);
	}
	return nameHit;
}

// 캐시된 버튼 하나. rect는 화면 좌표, exe는 어떤 BL 앱에 매핑됐는지(`KakaoTalk.exe`
// 등). hover hit 시 이 exe를 ScHoverInfo로 반환해 호출자가 lingering 등록을 그 exe로
// 좁힐 수 있게 한다.
struct CachedBtn {
	RECT rect;
	wchar_t exe[64];
};

struct BtnCache {
	std::vector<CachedBtn> btns;
	uint64_t lastUpdateMs = 0;      // 마지막 rebuild 시각
	bool everSucceeded = false;     // 한 번이라도 UIA가 작업표시줄 element를 잡았는지
	std::mutex mtx;
};
BtnCache g_btnCache;

constexpr uint64_t kBtnCacheTtlMs = 5000;

// 캐시 rebuild. caller가 g_btnCache.mtx를 lock한 상태로 호출.
// `alive`는 현재 살아있는 BL 앱 목록 — Name 매칭 시 그 exe에 매핑된 displayName만
// 통과시켜 '카카오톡 고정됨' 같은 핀-only false positive를 차단한다.
// 호출자는 alive->count > 0 일 때만 이 함수를 호출해야 한다.
void rebuild_btn_cache_locked(const TrackedWindowList *alive)
{
	g_btnCache.btns.clear();

	IUIAutomation *uia = get_or_create_uia();
	if (!uia) {
		g_btnCache.lastUpdateMs = GetTickCount64();
		return;
	}

	// 주/보조 작업표시줄 hwnd 수집
	HWND trays[8] = {};
	int trayCount = 0;
	HWND tray0 = FindWindowW(L"Shell_TrayWnd", nullptr);
	if (tray0)
		trays[trayCount++] = tray0;
	HWND t2 = nullptr;
	while ((t2 = FindWindowExW(nullptr, t2, L"Shell_SecondaryTrayWnd",
	                            nullptr)) != nullptr &&
	       trayCount < 8) {
		trays[trayCount++] = t2;
	}

	// ControlType=Button 조건
	VARIANT v{};
	v.vt = VT_I4;
	v.lVal = UIA_ButtonControlTypeId;
	IUIAutomationCondition *cond = nullptr;
	uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond);
	if (!cond) {
		g_btnCache.lastUpdateMs = GetTickCount64();
		return;
	}

	bool anyTrayElementOk = false;

	for (int t = 0; t < trayCount; ++t) {
		IUIAutomationElement *trayEl = nullptr;
		if (FAILED(uia->ElementFromHandle(trays[t], &trayEl)) || !trayEl)
			continue;
		anyTrayElementOk = true;

		IUIAutomationElementArray *buttons = nullptr;
		if (SUCCEEDED(trayEl->FindAll(TreeScope_Descendants, cond,
		                              &buttons)) &&
		    buttons) {
			int count = 0;
			buttons->get_Length(&count);
			for (int i = 0; i < count; ++i) {
				IUIAutomationElement *btn = nullptr;
				if (FAILED(buttons->GetElement(i, &btn)) || !btn)
					continue;

				CachedBtn cb{};
				if (element_is_blacklist_btn(
				        btn, alive, cb.exe,
				        sizeof(cb.exe) / sizeof(cb.exe[0]))) {
					if (SUCCEEDED(btn->get_CurrentBoundingRectangle(
					        &cb.rect)) &&
					    cb.rect.right > cb.rect.left &&
					    cb.rect.bottom > cb.rect.top) {
						g_btnCache.btns.push_back(cb);
					}
				}
				btn->Release();
			}
			buttons->Release();
		}
		trayEl->Release();
	}

	cond->Release();
	if (anyTrayElementOk)
		g_btnCache.everSucceeded = true;
	g_btnCache.lastUpdateMs = GetTickCount64();
}

} // namespace

extern "C" ScHoverInfo sc_taskbar_hover_blacklist_btn()
{
	ScHoverInfo info{};
	info.result = SC_TB_HOVER_UNKNOWN;

	POINT pt{};
	if (!GetCursorPos(&pt))
		return info;
	if (!point_over_any_taskbar(pt))
		return info;

	// [B안] 살아있는 BL 앱이 0개면 hover 어디든 BL일 수 없음 → OTHER 확정.
	// EnumWindows ~수 ms 비용이지만 매 tick 호출자가 이미 같은 함수를 호출하므로
	// 캐시 효과(프로세스 핸들 등)로 추가 부담은 미미.
	TrackedWindowList alive{};
	sc_find_all_alive_blacklist_windows(&alive);
	if (alive.count == 0) {
		std::lock_guard<std::mutex> lock(g_btnCache.mtx);
		g_btnCache.btns.clear();
		g_btnCache.lastUpdateMs = GetTickCount64();
		info.result = SC_TB_HOVER_OTHER;
		return info;
	}

	std::lock_guard<std::mutex> lock(g_btnCache.mtx);
	const uint64_t nowMs = GetTickCount64();
	if (g_btnCache.lastUpdateMs == 0 ||
	    (nowMs - g_btnCache.lastUpdateMs) > kBtnCacheTtlMs) {
		rebuild_btn_cache_locked(&alive);
	}

	// UIA가 작업표시줄 element를 한 번도 못 잡았으면 식별 자체 불가 → UNKNOWN
	// (호출자는 본래 동작으로 fallback).
	if (!g_btnCache.everSucceeded)
		return info;

	for (const CachedBtn &cb : g_btnCache.btns) {
		if (PtInRect(&cb.rect, pt)) {
			info.result = SC_TB_HOVER_BLACKLIST;
			size_t i = 0;
			const size_t cap = sizeof(info.exe) / sizeof(info.exe[0]);
			while (cb.exe[i] && i + 1 < cap) {
				info.exe[i] = cb.exe[i];
				++i;
			}
			info.exe[i] = 0;
			return info;
		}
	}
	info.result = SC_TB_HOVER_OTHER;
	return info;
}
