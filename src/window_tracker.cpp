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
#include <util/platform.h>    // os_gettime_ns

#include <windows.h>
#include <dwmapi.h>     // DwmGetWindowAttribute
#include <psapi.h>      // QueryFullProcessImageNameW (psapi 또는 kernel32 양쪽 노출)
#include <wctype.h>     // towlower

#include <atomic>
#include <mutex>
#include <unordered_map>

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
	if (g_minimizeRefCount.fetch_add(1, std::memory_order_acq_rel) != 0)
		return; // 이미 등록됨
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
