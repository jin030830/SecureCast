// =============================================================================
// securecast-filter.cpp — SecureCast 필터의 lifecycle 콜백 + dispatch table
//
// 역할:
//   OBS가 우리 필터 인스턴스의 생명주기 이벤트(생성/매 프레임 tick/매 프레임
//   render/소멸) 마다 호출하는 콜백들을 모아두는 곳. 이 파일 하단의
//   securecast_filter_info 구조체가 모든 콜백을 묶은 dispatch table이며,
//   plugin-main.cpp가 이걸 obs_register_source()로 OBS에 등록한다.
//
// 콜백 호출 시점:
//   securecast_get_name      : OBS 메뉴에 표시할 이름 — 등록 직후/메뉴 열 때
//   securecast_create        : 사용자가 필터를 소스에 추가하는 순간 1회
//   securecast_destroy       : 필터 제거/씬 종료/OBS 종료 시
//   securecast_video_tick    : 매 프레임 (60fps), 화면 그리기 직전 — 느린 작업용
//   securecast_video_render  : 매 프레임 (60fps), 실제 픽셀 렌더링 단계
// =============================================================================

#include "securecast-filter.h"
#include "plugin-support.h"   // obs_log
#include "window_tracker.h"   // sc_tracker_tick (Role A: 블랙리스트 앱 좌표 수집)
#include <stdlib.h>

// ---------------------------------------------------------
// Filter Lifecycle Callbacks
// ---------------------------------------------------------

// OBS가 필터 메뉴/관리 UI에 표시할 이름.
// 인자 type_data는 obs_source_info::type_data 필드 — 우리는 안 씀.
static const char* securecast_get_name(void* type_data)
{
    (void)type_data;
    return "SecureCast Privacy Masking";
}

// 사용자가 어떤 비디오 소스에 SecureCast 필터를 추가할 때 호출.
// settings는 OBS Properties UI에서 사용자가 입력한 값 (현재 미사용),
// context는 OBS가 만든 이 필터의 source 핸들.
//
// 반환값은 OBS가 보관하다가 이후 모든 콜백의 data 인자로 다시 넘겨준다.
static void* securecast_create(obs_data_t* settings, obs_source_t* context)
{
    // bzalloc은 OBS가 제공하는 zero-fill 할당자. 해제는 반드시 bfree로.
    SecureCastFilter* filter = (SecureCastFilter*)bzalloc(sizeof(SecureCastFilter));
    filter->context = context;
    filter->isActive = true;
    filter->isGameMode = false;
    filter->currentState = SecurityState::SAFE;
    filter->trackerAccumulator = 0.0f;  // window_tracker tick throttle 누산기

    obs_log(LOG_INFO, "[SecureCast] Filter created.");

    // TODO: Role C - Initialize N-Frame Delay Queue here
    // TODO: Role B - Initialize AI/OCR Engine here
    // TODO: Role A - Initialize Window Tracking Subsystem here (HLSL effect 컴파일 등)

    return filter;
}

// 필터 인스턴스 정리. OBS는 이 시점 이후 동일 data 포인터를 다시 안 넘긴다.
static void securecast_destroy(void* data)
{
    SecureCastFilter* filter = (SecureCastFilter*)data;

    obs_log(LOG_INFO, "[SecureCast] Filter destroyed. Cleaning up resources...");

    // TODO: Release GPU textures, stop AI threads, free memory
    bfree(filter);
}

// ---------------------------------------------------------
// Tick (Slow-Path) — 매 프레임 호출되지만 윈도우 추적은 0.15초마다
// ---------------------------------------------------------
//
// OBS 렌더링 파이프라인은 60fps라 video_tick도 60Hz로 들어온다.
// 그러나 EnumWindows는 무거운 호출이라 매 tick 돌리면 CPU 낭비가 크다.
// → window_tracker.cpp의 sc_tracker_tick이 trackerAccumulator를 누산해
//   임계 (0.15초) 를 넘을 때만 실제 스캔을 수행한다.
//
// seconds: 직전 tick과의 경과 시간(초). 60fps면 약 0.0167.
static void securecast_video_tick(void* data, float seconds)
{
    SecureCastFilter* filter = (SecureCastFilter*)data;
    if (!filter || !filter->isActive)
        return;

    sc_tracker_tick(seconds, &filter->trackerAccumulator);
}

// ---------------------------------------------------------
// Fast-Path Render Loop (60 FPS)
// ---------------------------------------------------------
//
// OBS가 이 필터가 부착된 소스를 화면에 그릴 때 호출.
// 여기서 우리가 픽셀에 손대거나 다른 텍스처로 교체할 수 있다.
//
// 현재는 작업이 없어 obs_source_skip_video_filter로 원본을 통과시킨다(bypass).
// Role A의 HLSL 블러 셰이더 + Role C의 N-Frame Delay Queue 가 들어올 자리.
static void securecast_video_render(void* data, gs_effect_t* effect)
{
    SecureCastFilter* filter = (SecureCastFilter*)data;

    // TODO (Role C):
    // 1. 현재 화면의 프레임 버퍼를 N-Frame Queue 뒤에 넣기 (Push)
    // 2. Queue 앞에서 이미 분석(AI Thread)이 끝난 N번째 과거 프레임을 꺼내오기 (Pop)

    // 현재 단계: 필터 조작 없이 직통(Bypass) 렌더링.
    obs_source_skip_video_filter(filter->context);

    // TODO (Role A):
    // 꺼내온 N번째 프레임 위에 HLSL Pixel Shader를 써서 Blur 처리 덮어쓰기
}

// ---------------------------------------------------------
// Source Info Dispatch Table
// ---------------------------------------------------------
//
// 위 콜백들을 묶어 OBS에 등록할 obs_source_info 구조체.
// lambda-IIFE 패턴 ([]{...}()) 으로 정적 초기화하면서 필드를 채운다.
// plugin-main.cpp의 obs_register_source(&securecast_filter_info) 가 이걸 등록.
//
// 핵심 필드:
//   id           : 내부 식별자 (씬/scene-collection 저장 시 키로 사용됨)
//   type         : OBS_SOURCE_TYPE_FILTER → 입력/트랜지션이 아닌 "필터"
//   output_flags : OBS_SOURCE_VIDEO → 비디오 필터 (오디오 아님)
//                  → 이 플래그 덕에 OBS가 "효과 필터" 메뉴에 노출시킴
struct obs_source_info securecast_filter_info = []() {
    struct obs_source_info info = {};
    info.id = "securecast_filter";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO;
    info.get_name = securecast_get_name;
    info.create = securecast_create;
    info.destroy = securecast_destroy;
    info.video_tick = securecast_video_tick;
    info.video_render = securecast_video_render;
    return info;
}();
