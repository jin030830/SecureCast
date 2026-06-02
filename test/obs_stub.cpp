// =============================================================================
// obs_stub.cpp — 단위 테스트 전용 OBS 런타임 스텁
//
// test_pii / test_tracker 는 ocr-engine.cpp · visual-tracker.cpp 를 플러그인과
// 분리해 단독 컴파일한다. 두 소스는 OBS 로깅 함수 blog() 를 호출하므로 링크
// 시점에 심볼이 필요하지만, 단위 테스트에 obs.dll 전체 런타임(+ 의존 DLL)을
// 끌어오는 것은 과하다.
//
// 여기서 blog() 를 stderr 출력으로 대체해, OBS 런타임 의존 없이 테스트를
// 빌드/실행할 수 있게 한다. (헤더 경로만 있으면 됨 — CMakeLists 참고)
// =============================================================================
#include <util/base.h>

#include <cstdarg>
#include <cstdio>

extern "C" EXPORT void blog(int log_level, const char *format, ...)
{
	const char *tag = "LOG";
	switch (log_level) {
	case LOG_ERROR:   tag = "ERROR";   break;
	case LOG_WARNING: tag = "WARNING"; break;
	case LOG_INFO:    tag = "INFO";    break;
	case LOG_DEBUG:   tag = "DEBUG";   break;
	default: break;
	}

	std::fprintf(stderr, "[%s] ", tag);

	va_list args;
	va_start(args, format);
	std::vfprintf(stderr, format, args);
	va_end(args);

	std::fputc('\n', stderr);
}

// =============================================================================
// [test_tracker 전용] visual-tracker.cpp 가 참조하는 환경 신호 스텁
//
// visual-tracker.cpp 는 창 가시영역(z-order 차감)·리사이즈·스크롤 모션 같은
// "주변 환경" 신호를 window_tracker / visible_subrects_cache / scroll_motion_hook
// 에서 받아온다. 이들을 진짜로 링크하면 게임모드 enum(game_sources/*, steam,
// win_event_listener…) 서브시스템 전체가 딸려와 단위 테스트가 비대해진다.
//
// test_tracker 가 검증하는 것은 NCC 박스 매칭 로직 자체이고, 위 신호들은 실제
// 창이 없는 헤드리스 테스트에선 어차피 "무활동"이 정답이다. 그래서 여기서
// 중립값으로 스텁한다 (blog() 와 동일한 분리 원칙).
//   - sc_compute_visible_subrects : 박스 전체가 보임(가림 없음). 0을 반환하면
//     visual-tracker 가 "완전히 가려짐 → 숨김"으로 처리하므로 반드시 1+전체박스.
//   - sc_is_window_resizing       : 리사이즈 중 아님(false)
//   - sc_notify_showcmd_change    : no-op
//   - last_scroll_motion_time_ms  : 스크롤 입력 없음(0)
// =============================================================================
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>

extern "C" void sc_notify_showcmd_change(HWND) {}

extern "C" bool sc_is_window_resizing(HWND, uint64_t) { return false; }

extern "C" int sc_compute_visible_subrects(HWND, RECT target_bounds, RECT *out,
                                           int maxOut)
{
	if (out && maxOut >= 1) {
		out[0] = target_bounds; // 가림 없음 = 박스 전체가 가시영역
		return 1;
	}
	return 0;
}

namespace securecast {
int64_t last_scroll_motion_time_ms() { return 0; }
} // namespace securecast
#endif
