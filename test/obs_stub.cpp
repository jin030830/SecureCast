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
