#include "securecast-filter.h"
#include <stdlib.h>

// ---------------------------------------------------------
// Filter Lifecycle Callbacks
// ---------------------------------------------------------

static const char* securecast_get_name(void* type_data)
{
    (void)type_data;
    return "SecureCast Privacy Masking";
}

static void* securecast_create(obs_data_t* settings, obs_source_t* context)
{
    SecureCastFilter* filter = (SecureCastFilter*)bzalloc(sizeof(SecureCastFilter));
    filter->context = context;
    filter->isActive = true;
    filter->isGameMode = false;
    filter->currentState = SecurityState::SAFE;

    obs_log(LOG_INFO, "[SecureCast] Filter created.");
    
    // TODO: Role C - Initialize N-Frame Delay Queue here
    // TODO: Role B - Initialize AI/OCR Engine here
    // TODO: Role A - Initialize Window Tracking Subsystem here

    return filter;
}

static void securecast_destroy(void* data)
{
    SecureCastFilter* filter = (SecureCastFilter*)data;

    obs_log(LOG_INFO, "[SecureCast] Filter destroyed. Cleaning up resources...");

    // TODO: Release GPU textures, stop AI threads, free memory
    bfree(filter);
}

// ---------------------------------------------------------
// Fast-Path Render Loop (60 FPS)
// ---------------------------------------------------------

static void securecast_video_render(void* data, gs_effect_t* effect)
{
    SecureCastFilter* filter = (SecureCastFilter*)data;

    // TODO (Role C): 
    // 1. 현재 화면의 프레임 버퍼를 N-Frame Queue 뒤에 넣기 (Push)
    // 2. Queue 앞에서 이미 분석(AI Thread)이 끝난 N번째 과거 프레임을 꺼내오기 (Pop)
    
    // 이 단계에서는 필터 조작 없이 직통(Bypass) 렌더링을 수행합니다.
    obs_source_skip_video_filter(filter->context); 
    
    // TODO (Role A): 
    // 꺼내온 N번째 프레임 위에 HLSL Pixel Shader를 써서 Blur 처리 덮어쓰기
}

// ---------------------------------------------------------
// Source Info Dispatch Table
// ---------------------------------------------------------

struct obs_source_info securecast_filter_info = []() {
    struct obs_source_info info = {};
    info.id = "securecast_filter";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO;
    info.get_name = securecast_get_name;
    info.create = securecast_create;
    info.destroy = securecast_destroy;
    info.video_render = securecast_video_render;
    return info;
}();
