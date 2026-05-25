// =============================================================================
// window_tracker.h — Role A의 1차 보호망: 블랙리스트 앱 화면 좌표 추적기
//
// 역할:
//   화면 캡처에 노출되면 위험한 앱 (KakaoTalk / Discord / Slack 등) 의 창이
//   현재 어디에 있는지 좌표를 주기적으로 알아내는 모듈.
//   향후 이 좌표는 securecast-filter.cpp의 video_render에서 HLSL 블러 셰이더
//   에 넘겨 해당 영역만 가려버리는 데 사용된다.
//
// 어디서 사용:
//   - securecast-filter.cpp::securecast_video_tick
//     trackerAccumulator를 누산하여 0.15초마다 sc_scan_blacklisted_windows 직접 호출
//
// 왜 별도 파일:
//   Win32 API (EnumWindows / DwmGetWindowAttribute / OpenProcess) 의존이
//   securecast-filter의 OBS 로직과 분리되어 있어야 Role A가 단독으로
//   교체/확장 가능 (예: macOS 포팅 시 이 파일만 갈아끼움).
// =============================================================================

#pragma once

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 한 창의 가시영역(visible sub-rectangles)을 표현할 수 있는 최대 슬롯 수.
// 단일 차감이면 최대 4개(top/bottom/left/right 띠)면 충분하지만, 작업표시줄,
// 시스템 알림, 트레이 등 z-order 위 다른 창들이 누적 차감되면 분할이 더 잘게
// 쪼개진다. 16이면 일반 데스크탑 환경(2~3개 앞 창)까지 안전하게 표현 가능.
// 16을 초과하면 union 폴백으로 bounding box 1개 반환 (과블러).
#define SC_MAX_VISIBLE_SUBRECTS 16

// 블랙리스트 매칭에 성공한 창 1개의 정보.
// hwnd          : Win32 윈도우 핸들 (이후 EnumChildWindows로 자식 탐색 시 사용)
// exe_name      : 실행 파일 베이스네임 (예: "KakaoTalk.exe") — 디버그/로그용
// bounds        : DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS) 로 얻은 화면 좌표.
//                 GetWindowRect와 달리 윈도우 그림자/DPI 보정값을 제외한 "보이는" 영역.
// visibleRects  : 앞 창(z-order 위)들에 가려진 영역을 뺀 실제 화면에 노출된 사각형들.
//                 visibleCount == 0 이면 완전히 가려진 상태(슬롯에서 제거 대상).
//                 1 이상이면 그 N개 사각형만 블러하면 시각적으로 정확.
// visibleCount  : visibleRects의 유효 개수.
struct TrackedWindow {
	HWND hwnd;
	wchar_t exe_name[64];
	RECT bounds;
	RECT visibleRects[SC_MAX_VISIBLE_SUBRECTS];
	int visibleCount;
};

// 한 번의 스캔에서 잡힐 수 있는 최대 창 개수.
// 한 앱이 여러 창을 띄울 수 있어 (KakaoTalk이 메인 + 미니챗 등) 16개로 둠.
#define SC_MAX_TRACKED_WINDOWS 16

struct TrackedWindowList {
	TrackedWindow items[SC_MAX_TRACKED_WINDOWS];
	int count;
};

// EnumWindows 한 번 + 블랙리스트 매칭 + DwmGetWindowAttribute 좌표 추출을 동기로 수행.
// 필터링: 100x100 미만 / 보이지 않음(IsWindowVisible=false) / UWP host 프로세스 등.
//
// 결과는 out에 채워 반환. count 멤버에 매칭된 창 수가 들어온다.
// 호출자는 video_tick / video_render에서만 부를 것 — 다른 스레드에서 부르면 안 됨
// (DwmGetWindowAttribute는 caller 스레드 컨텍스트로 동작).
void sc_scan_blacklisted_windows(TrackedWindowList *out);

// [Window anchor v6] 캡처 시점의 모든 가시 top-level 창을 enum.
// OBS 자기 자신 프로세스는 PID로 필터링 (preview 창 등 OCR owner 오인 방지).
// 결과는 Z-order 위쪽부터 채워짐 — 같은 source 위치에 여러 창 겹칠 때 매칭은
// 첫 contains를 우선해 z-order 최상단 선택.
// 호출자는 video_render(렌더 스레드)에서만 호출할 것 — Win32 DWM 의존.
void sc_enum_all_visible_windows(TrackedWindowList *out);

// ============================================================
// 최소화 애니메이션 가드 (Role A 부속)
//
// 창을 최소화할 때 ~200~300ms의 shrink 애니메이션이 일어나는 동안 DWM
// bounds가 축소되어 블러 박스가 함께 줄어들고, OCR 트래커 박스는 원래
// 위치에 고정돼 텍스트 가장자리가 잠깐 노출되는 현상을 막기 위한 모듈.
//
// 동작: SetWinEventHook으로 EVENT_SYSTEM_MINIMIZESTART/END를 받아,
// 최소화 직전 bounds를 캡처해 글로벌 맵에 보관한다. filter는 매 fast
// 업데이트마다 sc_poll_minimizing_windows로 이벤트를 폴링해 해당 HWND에
// pre-bounds 기반 lingering 블러를 등록.
//
// init/shutdown은 refcount 기반 — 여러 filter 인스턴스가 동시에 사용 가능.
// ============================================================
struct ScMinimizingEntry {
	HWND hwnd;
	RECT preBounds;
	// MINIMIZEEND가 발화된 시각 (os_gettime_ns 기준 나노초). 0이면 아직
	// 애니메이션 진행 중. > 0이면 종료된 상태 — lingering은 slot.timestamp가
	// 이 값보다 큰(=종료 이후 캡처) 프레임에는 적용하지 말아야 잔상이 사라진다.
	uint64_t endNs;
};

// 글로벌 훅을 획득/해제. filter create/destroy에서 1:1로 호출.
void sc_minimize_tracker_init();
void sc_minimize_tracker_shutdown();

// 현재 활성화된 minimize 이벤트(최근 maxAgeMs 이내)를 out에 채워 반환.
// 반환값: out에 채운 개수. 호출이 entry를 제거하지는 않으므로 매 프레임
// 호출해 lingering을 refresh할 수 있다. ageMs 초과 entry는 자동 evict.
// MINIMIZEEND 발화된 entry도 short grace period 동안 함께 반환되어 호출자가
// cutoff 처리에 사용할 수 있다.
int sc_poll_minimizing_windows(ScMinimizingEntry *out, int maxOut,
                                uint64_t maxAgeMs);

// 주어진 HWND에 대해 마지막으로 관측된 MINIMIZEEND의 OBS 시각(나노초).
// 발화되지 않았거나 grace period가 지나 evict됐으면 0 반환. lingering 렌더
// 루프에서 slot.timestamp와 비교해 ghost blur를 잘라내는 데 사용.
uint64_t sc_get_minimize_end_ns(HWND hwnd);

// 현재 마우스 커서가 작업표시줄 영역 (주 + 보조 모니터) 위에 있는지 검사.
// Aero Peek은 TaskListThumbnailWnd가 안 보일 수도 있고 DWM 썸네일로 직접 렌더
// 되어 HWND 추적이 안 되는 경우도 있어, 마우스 위치를 peek 상태의 보조 신호로
// 사용. 검출되는 동안 recentlySeenList lingering을 등록해 안전하게 가린다.
bool sc_mouse_over_taskbar();

// 마우스 커서가 작업표시줄 바로 옆 "썸네일 영역"(작업표시줄에서 화면 안쪽으로
// ~400px 띠) 안에 있는지 검사. 단순히 작업표시줄 위에 있는 상태(소형 썸네일만
// 떠 있는 단계)와 구분해, 사용자가 실제로 썸네일/peek 영역으로 마우스를 옮긴
// 시점만 peek 발동 신호로 사용. 작업표시줄이 화면 어느 가장자리에 붙어있든
// 자동 보정.
bool sc_mouse_over_thumbnail_zone();

// 살아있는 모든 블랙리스트 프로세스의 top-level HWND를 즉시 enum해서 out에 채움.
// sc_scan_blacklisted_windows와 달리 가시성/크기/visibleRects 필터 없음 —
// 최소화 상태(iconic) 또는 cloaked 상태인 창도 포함. peek가 그런 창을 잠시
// 화면에 띄울 때 lingering 등록 대상으로 쓰기 위함.
// 각 항목의 bounds: iconic이면 GetWindowPlacement.rcNormalPosition을 화면 좌표로
// 변환한 값(=복원 시 표시될 위치), 아니면 DWM EXTENDED_FRAME_BOUNDS.
// 반환값: out->count에 채워진 개수.
void sc_find_all_alive_blacklist_windows(TrackedWindowList *out);

// `target` 창의 bounds를 z-order 위의 다른 top-level 창들로 잘라서 실제
// 화면에 노출된 disjoint 사각형들을 out에 채워 반환한다.
// 반환값: out에 채워진 사각형 개수 (0 = 완전히 가려짐).
// maxOut 슬롯이 부족할 만큼 잘게 쪼개지면 안전한 폴백으로 잔여 사각형들의
// union(bounding box)을 1개 사각형으로 반환한다 — 시각적으로 약간 과블러될 수
// 있으나 노출 누락은 발생하지 않는다.
// 호출자는 video_tick / video_render 컨텍스트에서만 호출할 것.
int sc_compute_visible_subrects(HWND target, RECT target_bounds, RECT *out,
                                int maxOut);

// Fast-path 좌표 갱신: 이미 list에 들어있는 HWND들에 대해 DWM으로 bounds만 재조회.
// EnumWindows / OpenProcess 없이 DWM query만 수행하므로 매 프레임 호출 가능.
//
// 창이 닫혀 IsWindowVisible이 false가 되면 해당 슬롯을 list에서 즉시 제거한다.
// (slow scan은 새 창 발견용, fast-path는 이미 알고 있는 창의 위치 추적용)
void sc_update_tracked_bounds(TrackedWindowList *list);

// video_tick의 throttle 헬퍼.
// seconds     : 직전 tick과의 경과 시간 (60fps면 약 0.0167)
// accumulator : 누산값 보관 위치 (보통 SecureCastFilter::trackerAccumulator)
// out         : 스캔이 실행된 경우에만 결과로 채워짐; throttle로 스킵되면 불변.
//               null 허용 (결과가 필요 없으면 null 전달).
//
// 동작:
//   1. *accumulator += seconds
//   2. interval 미만이면 즉시 리턴 (대부분의 호출은 여기서 끝)
//   3. 임계 도달 시 sc_scan_blacklisted_windows 1회 + *out 업데이트 + obs_log 출력
//   4. *accumulator = 0
// interval: 실제 스캔 주기(초). 기본 0.15초, 게임 모드 시 0.5초 전달.
void sc_tracker_tick(float seconds, float *accumulator, TrackedWindowList *out, float interval);

#ifdef __cplusplus
}
#endif
