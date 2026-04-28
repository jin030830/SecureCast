#include <obs-module.h>

// Forward declaration from securecast-filter.cpp
extern struct obs_source_info securecast_filter_info;

OBS_DECLARE_MODULE()

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

    // Register our main filter
    // [Role C/A 협업] 여기서 등록된 소스가 필터링 파이프라인의 시작점이 됩니다.
    obs_register_source(&securecast_filter_info);

    return true;
}

// Module cleanup
void obs_module_unload(void)
{
    // [자원 정리] Role C가 할당한 버퍼 및 Role B의 스레드를 여기서 안전하게 종료해야 합니다.
    blog(LOG_INFO, "[SecureCast] Unloading plugin");
}
