// =============================================================================
// plugin-main.cpp — DLL의 OBS 진입점 (Module Entry Point)
//
// 역할:
//   OBS Studio가 securecast.dll을 LoadLibrary로 로드한 직후, 이 파일에 정의된
//   obs_module_load()를 호출한다. 우리는 그 시점에 SecureCast 필터 타입을
//   OBS에 등록(register)한다. 등록되면 사용자가 OBS의 "효과 필터" 메뉴에서
//   "SecureCast Privacy Masking"을 선택할 수 있게 된다.
//
// 호출 순서 (OBS startup):
//   1. OBS가 %PROGRAMDATA%\obs-studio\plugins\securecast\bin\64bit\securecast.dll 로드
//   2. C++ 정적 초기자가 securecast_filter_info 구조체를 채움
//      (securecast-filter.cpp 하단의 lambda-IIFE)
//   3. OBS가 obs_module_load() 호출 → 아래 함수가 실행됨
//   4. obs_register_source()로 필터 등록 → OBS Effect Filters 메뉴에 노출
//   5. 사용자가 OBS 종료 시 obs_module_unload() 호출
// =============================================================================

#include <obs-module.h>

#include "plugin-support.h"   // obs_log 매크로/함수, PLUGIN_NAME 등

// securecast-filter.cpp에 정의된 obs_source_info 인스턴스를 가져온다.
// (필터의 lifecycle 콜백을 묶은 dispatch table)
extern struct obs_source_info securecast_filter_info;

// libobs는 모듈의 함수들을 C 링크 규약으로 찾는다.
// obs_module_load / obs_module_unload는 반드시 C symbol이어야 한다.
extern "C" {

// OBS_DECLARE_MODULE: libobs가 이 DLL을 OBS 플러그인으로 인식하도록
// 필요한 export 함수들 (obs_module_set_pointer, obs_current_module 등)을 자동 생성.
OBS_DECLARE_MODULE()

// OBS_MODULE_USE_DEFAULT_LOCALE: data/locale/<lang>.ini 파일을 OBS가 자동으로 읽도록 지정.
// 첫 인자는 모듈 이름 ("securecast"), 두 번째는 fallback 언어.
// → data/locale/en-US.ini의 키들이 obs_module_text("키")로 접근 가능해진다.
OBS_MODULE_USE_DEFAULT_LOCALE("securecast", "en-US")

// 플러그인 로드 시 OBS가 한 번 호출. 등록 작업은 여기서.
/*
 * [개발 프로세스 안내]
 * 1. Role A (렌더링): obs_video_render() 과정에서 마스킹 셰이더를 적용합니다.
 * 2. Role B (AI): 분석 엔진을 통해 화면의 개인정보 영역을 탐지하고 MaskPayload를 생성합니다.
 * 3. Role C (파이프라인): FrameRingBuffer를 통해 프레임 지연을 관리하고, AtomicMaskChannel로 데이터를 중계합니다.
 * 4. Role D (설정/UI): 사용자 설정값을 관리하며 전체 모듈의 상태(SecurityState)를 제어합니다.
 */

// Module entry point
bool obs_module_load(void)
{
    // [로그 정책] 모든 플러그인 메시지는 [SecureCast] 접두사를 사용하여 Role별 식별을 돕습니다.
    blog(LOG_INFO, "[SecureCast] Initializing SecureCast v5.7 plugin (Single-Process Native C++)");

    // SecureCast 필터 타입을 OBS의 filter_types 배열에 등록.
    // 이후 사용자가 어떤 비디오 소스에 필터 추가하면 OBS가 securecast_filter_info
    // 의 create 콜백 (securecast_create) 을 호출해 필터 인스턴스를 만든다.
    // Register our main filter
    // [Role C/A 협업] 여기서 등록된 소스가 필터링 파이프라인의 시작점이 됩니다.
    obs_register_source(&securecast_filter_info);

    return true;  // false 반환 시 OBS가 모듈을 unload함
}

// 플러그인 unload 시 OBS가 호출. 정적 리소스 정리 위치.
// (필터 인스턴스별 정리는 securecast_destroy에서 처리)
void obs_module_unload(void)
{
    // [자원 정리] Role C가 할당한 버퍼 및 Role B의 스레드를 여기서 안전하게 종료해야 합니다.
    blog(LOG_INFO, "[SecureCast] Unloading plugin");
}

} // extern "C"
