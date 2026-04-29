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
//   - securecast-filter.cpp::securecast_video_tick → sc_tracker_tick 호출
//   - sc_tracker_tick 내부에서 sc_scan_blacklisted_windows 호출 (150ms마다 1회)
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

// 블랙리스트 매칭에 성공한 창 1개의 정보.
// hwnd       : Win32 윈도우 핸들 (이후 EnumChildWindows로 자식 탐색 시 사용)
// exe_name   : 실행 파일 베이스네임 (예: "KakaoTalk.exe") — 디버그/로그용
// bounds     : DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS) 로 얻은 화면 좌표.
//              GetWindowRect와 달리 윈도우 그림자/DPI 보정값을 제외한 "보이는" 영역.
struct TrackedWindow {
	HWND hwnd;
	wchar_t exe_name[64];
	RECT bounds;
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

// video_tick의 throttle 헬퍼.
// seconds : 직전 tick과의 경과 시간 (60fps면 약 0.0167)
// accumulator : 누산값 보관 위치 (보통 SecureCastFilter::trackerAccumulator)
//
// 동작:
//   1. *accumulator += seconds
//   2. 0.15초 미만이면 즉시 리턴 (대부분의 호출은 여기서 끝)
//   3. 임계 도달 시 sc_scan_blacklisted_windows 1회 + 결과를 obs_log로 출력
//   4. *accumulator = 0
//
// 0.15초인 이유: 사람이 창 이동 후 마우스를 떼는 데 걸리는 시간보다 짧으면서,
// 60fps tick 매번(0.0167) 호출하는 것보다는 ~9배 적게 EnumWindows 호출.
void sc_tracker_tick(float seconds, float *accumulator);

#ifdef __cplusplus
}
#endif
