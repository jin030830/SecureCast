// =============================================================================
// plugin-support.h — 모듈 메타정보 + 로그 헬퍼 선언
//
// 역할:
//   이 파일에서 선언한 PLUGIN_NAME / PLUGIN_VERSION / obs_log() 는 같은 폴더의
//   plugin-support.c.in (CMake가 .c로 변환) 에서 정의된다. 우리 코드 어디서든
//   #include "plugin-support.h" 한 줄로 사용 가능.
//
// 빌드 시점:
//   cmake/common/helpers_common.cmake가 plugin-support.c.in의 @CMAKE_PROJECT_NAME@,
//   @CMAKE_PROJECT_VERSION@ 토큰을 실제 값으로 치환해 .c 파일을 생성하고,
//   "plugin-support" 라는 이름의 정적 라이브러리로 빌드해 securecast.dll에 링크.
// =============================================================================

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// CMake에서 주입된 모듈 식별 문자열. 디버그 로그/UI 표시 등에 활용 가능.
extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;

// 모든 로그 출력은 이 함수를 통해 OBS 로그창으로 흘려보낸다.
// 내부적으로 "[<PLUGIN_NAME>] <format>" 형식으로 prefix를 붙인 뒤 blogva 호출.
// log_level은 obs.h의 LOG_INFO / LOG_WARNING / LOG_ERROR / LOG_DEBUG 중 하나.
void obs_log(int log_level, const char *format, ...);

#ifdef __cplusplus
}
#endif
