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

// ============================================================
// [Resize 가드] maximize/restore/사용자 드래그 리사이즈 시 owner 창
// 트랜지션을 감지해 sticky expansion 신호로 활용.
//
// 신호 소스 (둘 다 같은 g_resizeMap 갱신):
//   1. WinEvent EVENT_SYSTEM_MOVESIZESTART/END  — 사용자 드래그 리사이즈
//   2. GetWindowPlacement.showCmd 폴링 변화      — 버튼/단축키 maximize
//
// minimize tracker와 동일한 refcount 패턴.
// ============================================================
void sc_resize_tracker_init();
void sc_resize_tracker_shutdown();

// 주어진 HWND가 현재 리사이즈/maximize 트랜지션 중인지 검사. visual-tracker가
// snapshot_for_push에서 호출해 sticky expansion을 켜는 신호로 사용.
// graceMs 동안 트랜지션 종료 후에도 true 반환 (송출 지연 보호).
bool sc_is_window_resizing(HWND hwnd, uint64_t graceMs);

// 주어진 exe 이름이 사용자 블랙리스트에 있는지 검사 (game mode dialog 제외 판단용).
bool sc_is_blacklisted_exe(const wchar_t *exe_name);

// 게임 모드 추가 블랙리스트 검사 (일반 + game-mode-extra 모두 통과 시 true).
bool sc_is_blacklisted_exe_game_mode(const wchar_t *exe_name);

// 일반 모드 블랙리스트에만 있는지 (game-mode-extra 제외). dialog 분기에서 사용.
bool sc_is_blacklisted_exe_normal_only(const wchar_t *exe_name);

// 사용자 설정 → 동적 블랙리스트 갱신. exes는 wchar_t* 배열 (count개).
// settings update 콜백에서 1회 호출.
void sc_set_user_blacklist_normal(const wchar_t *const *exes, int count);
void sc_set_user_blacklist_game_mode(const wchar_t *const *exes, int count);

// 어느 필터든 게임 모드면 true → sc_is_blacklisted_exe가 자동으로
// game-mode-extra까지 검사. 필터의 game mode enter/exit에서 호출.
// 다중 필터 인스턴스 안전: 내부적으로 refcount(atomic int)로 관리되며,
// active=true는 fetch_add(+1), false는 fetch_sub(-1) + underflow 가드.
// 같은 필터가 ON/OFF를 짝지어 호출해야 하며, 마지막 OFF가 들어와야
// 글로벌이 비활성화된다.
void sc_set_global_game_mode(bool active);

// 어느 필터든 게임 모드 활성 상태인지 즉시 조회. refcount > 0 이면 true.
// OCR 워커 등 다른 모듈이 게임 모드 동안 무거운 작업을 skip할 때 사용.
bool sc_any_filter_in_game_mode();

// =============================================================================
// Tier 1/3 게임 식별 (Game mode v2 — T03)
//
// 게임 모드 진입 trigger와 fg 자동 가림 제외 판정에 함께 사용:
//   - sc_is_known_game             : 빌트인 정적 리스트(known_games.h) 검사
//   - sc_is_known_game_or_user_game: 빌트인 + 사용자 등록(Tier 3) 통합 검사
//   - sc_set_user_game_list        : 사용자가 OBS 설정에서 등록한 게임 exe 갱신
//
// 매칭은 모두 case-insensitive (iequals). exe basename 비교.
// =============================================================================
bool sc_is_known_game(const wchar_t *exe_name);
bool sc_is_known_game_or_user_game(const wchar_t *exe_name);
void sc_set_user_game_list(const wchar_t *const *exes, int count);

// 빌트인 친화명 매핑(known_games.h::kKnownExeNames)에서 exe 검색.
// 매칭되면 out_buf에 친화명 복사 후 true. 미매칭이면 false / out_buf 미변경.
// 시스템 enum이 잡지 못하는 백그라운드 서비스(vgc.exe = Riot Vanguard 등)
// 친화명을 즉시 제공하기 위함.
bool sc_lookup_friendly_name(const wchar_t *exe_name, wchar_t *out_buf,
                              size_t out_cap);

// 주어진 HWND의 owner process exe 이름을 base name으로 추출.
// 성공 시 true 반환. out 버퍼는 wchar 단위 크기.
bool sc_get_hwnd_exe_name(HWND hwnd, wchar_t *out, size_t out_cap);

// =============================================================================
// [실시간 경로 매칭] 게임 스토어 설치 폴더 기반 게임 판정
//
//   sc_set_game_dirs   : 게임 스토어 설치 폴더 prefix 목록 갱신(autodetect가 호출).
//   sc_is_game_by_path : exe 전체 경로가 그 폴더들 아래면 true.
//   sc_hwnd_is_game    : 창의 owner exe를 1회 조회해 "목록 OR 경로"로 게임 판정.
//                        out_exe(null 허용)에 exe basename을 채움(진입 캡처용).
//
// 목록(빌트인/사용자)에 없어도, Steam 등 스토어 설치 폴더에서 실행되면 게임으로
// 인지한다 → 신작·마이너 게임을 수동 등록 없이 실시간 인지.
// =============================================================================
void sc_set_game_dirs(const wchar_t *const *dirs, int count);
bool sc_is_game_by_path(const wchar_t *full_exe_path);
bool sc_hwnd_is_game(HWND hwnd, wchar_t *out_exe, size_t out_cap);

// [사용자 추가 제외] OBS 설정 "게임 아님 폴더" 목록 갱신. 경로 매칭에서 이 폴더
// 아래 실행파일은 게임으로 보지 않는다(Wallpaper Engine 등 빌트인 제외 보강).
void sc_set_user_path_excludes(const wchar_t *const *names, int count);

// 폴링 진입점: 호출자(visual-tracker)가 owner HWND 별로 showCmd 변화를 감지해
// 위 맵에 트랜지션을 기록한다. graceMs 동안 sc_is_window_resizing이 true.
void sc_notify_showcmd_change(HWND hwnd);

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

// 살아있는 모든 블랙리스트 프로세스의 사용자 노출 가능 top-level HWND를 즉시 enum해서
// out에 채움. minimized(iconic) 창은 통과(peek 시 잠시 화면에 뜨므로), Electron류 앱이
// 다수 만드는 hidden helper hwnd(IsWindowVisible=false 또는 non-iconic & <100x100)는
// 거른다 — 그렇지 않으면 hover 가드가 실제 가시 창보다 큰 블러 박스를 그린다.
// 각 항목의 bounds: iconic이면 GetWindowPlacement.rcNormalPosition을 화면 좌표로
// 변환한 값(=복원 시 표시될 위치), 아니면 DWM EXTENDED_FRAME_BOUNDS.
// 반환값: out->count에 채워진 개수.
void sc_find_all_alive_blacklist_windows(TrackedWindowList *out);

// 작업표시줄 hover 식별 결과 (3-state).
// 호출자는 BL/OTHER 둘 다 hysteresis로 추적해 가장 최근 신호로 가드를 결정하고,
// UNKNOWN은 직전 신호를 보존(둘 다 hysteresis 밖이면 fail-safe로 가드 ON).
typedef enum ScTaskbarHoverResult {
  SC_TB_HOVER_BLACKLIST,  // 마우스 아래 버튼이 블랙리스트 앱
  SC_TB_HOVER_OTHER,      // 마우스 아래 버튼이 다른 앱
  SC_TB_HOVER_UNKNOWN     // 식별 불가(작업표시줄 밖, 빈 영역, UIA 실패 등)
} ScTaskbarHoverResult;

// hover 식별의 매칭 정보. BLACKLIST 결과일 때만 `exe`가 채워진다(예: "KakaoTalk.exe").
// 호출자는 이 exe로 lingering 등록 대상을 좁혀, 한 BL 앱을 hover했을 때 다른
// 살아있는 BL 앱들까지 함께 가려지는 회귀를 막는다.
typedef struct ScHoverInfo {
  ScTaskbarHoverResult result;
  wchar_t exe[64];
} ScHoverInfo;

// 살아있는 블랙리스트 앱의 작업표시줄 버튼 위치를 사전 매핑(UIA 1회 enum, ~5초 TTL
// 캐시)한 뒤, 현재 마우스 좌표가 그 버튼 사각형 중 하나에 들어있는지 확인.
//
// 식별 전략:
//   1. 살아있는 BL 앱 목록을 먼저 수집 — 0개면 hover 어디든 BL일 수 없으므로
//      UIA enum을 생략하고 OTHER 즉시 반환.
//   2. 주/보조 작업표시줄(Shell_TrayWnd / Shell_SecondaryTrayWnd)에 대해
//      ElementFromHandle로 UIA root element 획득
//   3. TreeScope_Descendants + ControlType=Button 조건으로 FindAll
//   4. 각 버튼의 PID→exe로 직접 매칭 시도; Win11 작업표시줄은 모든 버튼이
//      explorer.exe로 보고되므로 사실상 살아있는 BL exe에 매핑된 다국어
//      displayName으로 Name 매칭이 주된 경로 — 핀-only(예: '카카오톡 고정됨')
//      false positive는 alive set 교차매칭으로 차단된다.
//   5. 매칭된 버튼의 BoundingRectangle을 캐시에 저장
//
// 캐시는 약 5초 TTL — 작업표시줄 재배치/앱 추가-제거 대응. UIA FindAll은 30~80ms
// 비용이라 매 video_tick 호출은 부담이므로 캐시 갱신은 hit 시 자동(만료된 경우만).
//
// 반환값: ScHoverInfo. result는 BLACKLIST/OTHER/UNKNOWN. result==BLACKLIST일 때
//        exe에 매칭된 BL exe(예: "KakaoTalk.exe")가 채워진다. 그 외엔 exe[0]=0.
ScHoverInfo sc_taskbar_hover_blacklist_btn();

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
