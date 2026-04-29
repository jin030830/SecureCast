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
#include <chrono>
#include <stdlib.h>
#include <string.h>
#include "securecast-filter.h"

// ================================================================
// [Role C] FrameRingBuffer 구현부
// ================================================================

bool FrameRingBuffer::initialize(uint32_t width, uint32_t height)
{
    if (m_initialized)
        return true;

    m_width  = width;
    m_height = height;

    for (auto& slot : m_slots) {
        slot.texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
        if (!slot.texrender) {
            blog(LOG_ERROR, "Failed to allocate gs_texrender slot.");
            destroy();
            return false;
        }
        slot.timestamp = 0;
    }

    m_initialized = true;
    blog(LOG_INFO, "FrameRingBuffer initialized: %dx%d, %d slots (gs_texrender).",
            width, height, SC_RING_BUFFER_SLOTS);
    return true;
}

void FrameRingBuffer::destroy()
{
    if (!m_initialized)
        return;

    obs_enter_graphics();
    for (auto& slot : m_slots) {
        if (slot.texrender) {
            gs_texrender_destroy(slot.texrender);
            slot.texrender = nullptr;
        }
        slot.timestamp = 0;
    }
    obs_leave_graphics();

    m_initialized = false;
    m_head        = 0;
    m_frameCount  = 0;
    blog(LOG_INFO, "FrameRingBuffer destroyed.");
}

void FrameRingBuffer::pushFrame(obs_source_t* source, uint64_t timestamp)
{
    if (!m_initialized)
        return;

    gs_texrender_t* tr = m_slots[m_head].texrender;
    gs_texrender_reset(tr);

    // gs_texrender_begin/end가 내부적으로 렌더 타겟, 뷰포트, 투영 행렬을
    // 안전하게 저장/복원해 주므로 OBS 렌더 상태가 오염되지 않는다.
    if (gs_texrender_begin(tr, m_width, m_height)) {
        struct vec4 clearColor;
        vec4_zero(&clearColor);
        gs_clear(GS_CLEAR_COLOR, &clearColor, 1.0f, 0);

        gs_ortho(0.0f, (float)m_width, 0.0f, (float)m_height, -100.0f, 100.0f);
        obs_source_video_render(source);

        gs_texrender_end(tr);
    }

    m_slots[m_head].timestamp = timestamp;

    // 다음 HEAD로 순환
    m_head = (m_head + 1) % SC_RING_BUFFER_SLOTS;
    if (m_frameCount < SC_RING_BUFFER_SLOTS)
        m_frameCount++;
}

const FrameRingBuffer::Slot* FrameRingBuffer::peekDelayedSlot() const
{
    if (m_frameCount < SC_RING_BUFFER_SLOTS)
        return nullptr;

    int tail = (m_head - SC_RING_BUFFER_SLOTS + SC_RING_BUFFER_SLOTS) % SC_RING_BUFFER_SLOTS;
    return &m_slots[tail];
}

// ================================================================
// [Role C] MockAIWorker 구현부
// ================================================================

void MockAIWorker::start(uint32_t frameWidth, uint32_t frameHeight, ResultCallback callback)
{
    if (m_running.load())
        return;

    m_frameWidth  = frameWidth;
    m_frameHeight = frameHeight;
    m_callback    = std::move(callback);
    m_running.store(true);

    m_thread = std::thread([this]() { workerLoop(); });
    blog(LOG_INFO, "MockAIWorker started (frame: %dx%d).",
            m_frameWidth, m_frameHeight);
}

void MockAIWorker::stop()
{
    if (!m_running.load())
        return;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running.store(false);
    }
    m_cv.notify_all();

    if (m_thread.joinable())
        m_thread.join();

    blog(LOG_INFO, "MockAIWorker stopped.");
}

void MockAIWorker::workerLoop()
{
    while (m_running.load()) {
        // AI 처리 시간 시뮬레이션 (실제 OCR 처리 예상 지연: ~40~60ms)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(50),
                          [this]() { return !m_running.load(); });
        }

        if (!m_running.load())
            break;

        // 가짜 마스킹 결과 생성: 화면 정중앙에 200x100 픽셀의 블랙아웃 박스 하나
        MaskPayload payload{};
        payload.rectCount = 1;
        payload.rects[0].x     = static_cast<int>(m_frameWidth  / 2) - 100;
        payload.rects[0].y     = static_cast<int>(m_frameHeight / 2) - 50;
        payload.rects[0].width  = 200;
        payload.rects[0].height = 100;
        payload.rects[0].type   = 1; // Blackout

        if (m_callback)
            m_callback(payload);
    }
}

// ================================================================
// [Role C] AtomicMaskChannel 구현부
//
// [P0 수정] m_pending은 비원자적 구조체이므로 뮤텍스로 보호한다.
// 이전 코드는 memory_order만으로 data race를 방지하려 했으나,
// m_pending 쓰기 중에 Render Thread가 읽으면 torn read가 발생한다.
// ================================================================

void AtomicMaskChannel::publish(const MaskPayload& payload)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending = payload;
    m_ready.store(true, std::memory_order_release);
}

bool AtomicMaskChannel::consume(MaskPayload& out)
{
    if (!m_ready.load(std::memory_order_acquire))
        return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready.exchange(false, std::memory_order_acq_rel))
        return false; // 다른 consumer가 먼저 가져간 경우 (현재는 단일이지만 방어적 처리)
    out = m_pending;
    return true;
}

// ================================================================
// Filter Lifecycle Callbacks
// ================================================================

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
    (void)settings;

    SecureCastFilter* filter = new SecureCastFilter();
    filter->context      = context;
    filter->isActive     = true;
    filter->isGameMode   = false;
    filter->currentState = SecurityState::SAFE;
    filter->trackerAccumulator = 0.0f;  // window_tracker tick throttle 누산기

    obs_log(LOG_INFO, "[SecureCast] Filter created.");

    // TODO: Role C - Initialize N-Frame Delay Queue here
    // TODO: Role B - Initialize AI/OCR Engine here
    // TODO: Role A - Initialize Window Tracking Subsystem here (HLSL effect 컴파일 등)
    // FrameRingBuffer는 첫 video_render 호출 시 지연 초기화 (OBS gs context 필요)
    // MockAIWorker도 렌더러가 실제 해상도를 알게 되는 시점(video_render)에 맞춰 시작하도록 연기합니다.

    blog(LOG_INFO, "Filter created (Role C: Mock Pipeline Active).");
    return filter;
}

// 필터 인스턴스 정리. OBS는 이 시점 이후 동일 data 포인터를 다시 안 넘긴다.
static void securecast_destroy(void* data)
{
    SecureCastFilter* filter = static_cast<SecureCastFilter*>(data);

    blog(LOG_INFO, "Destroying filter...");

    // AI 워커 먼저 중지 (콜백이 ring buffer에 접근하지 않도록)
    filter->mockWorker.stop();

    // GPU 텍스처 해제 (내부에서 obs_enter/leave_graphics 처리)
    filter->ringBuffer.destroy();

    delete filter;
    blog(LOG_INFO, "Filter destroyed.");
}

// ================================================================
// [Role C] 핵심 렌더 루프 (60 FPS)
//
// 흐름도:
//
//   ┌─────────────────────────────────────────────────┐
//   │ video_render() [Render Thread, ~16ms 주기]       │
//   │                                                   │
//   │  1. 링 버퍼 지연 초기화 (첫 프레임 한 번만)        │
//   │  2. 현재 프레임 → Ring Buffer HEAD 에 Push        │
//   │  3. AtomicMaskChannel에서 최신 AI 결과 Consume    │
//   │  4. Ring Buffer TAIL(N프레임 전) 꺼내기           │
//   │  5a. 버퍼 미충족 → 블랙 홀드 프레임 출력           │
//   │  5b. 버퍼 충족  → 지연 프레임 + 마스킹 박스 출력   │
//   └─────────────────────────────────────────────────┘
// ================================================================
static void securecast_video_render(void* data, gs_effect_t* effect)
{
    (void)effect;

    SecureCastFilter* filter = static_cast<SecureCastFilter*>(data);
    if (!filter->isActive) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    // 상위 소스의 실제 해상도 가져오기
    obs_source_t* parent = obs_filter_get_parent(filter->context);
    if (!parent) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    uint32_t w = obs_source_get_width(parent);
    uint32_t h = obs_source_get_height(parent);
    if (w == 0 || h == 0) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    // --- Step 1: 링 버퍼 지연 초기화 또는 해상도 변경 대응 ---
    if (!filter->ringBuffer.isInitialized()) {
        if (!filter->ringBuffer.initialize(w, h)) {
            obs_source_skip_video_filter(filter->context);
            return;
        }
        // 첫 초기화 시 AI 워커도 실제 해상도로 시작
        filter->mockWorker.start(w, h, [filter](const MaskPayload& payload) {
            filter->maskChannel.publish(payload);
        });
    } else if (filter->ringBuffer.getWidth() != w || filter->ringBuffer.getHeight() != h) {
        // [P1 수정] 소스 해상도가 바뀌면 텍스처를 재생성해야 화면 깨짐 및 크래시 방지
        blog(LOG_INFO, "Resolution changed (%dx%d -> %dx%d). Reinitializing ring buffer.",
                filter->ringBuffer.getWidth(), filter->ringBuffer.getHeight(), w, h);
        
        // 1. AI 워커 중지 (진행 중인 텍스처 읽기/분석 취소)
        filter->mockWorker.stop();

        // 2. 링 버퍼 파괴 및 재할당
        filter->ringBuffer.destroy();
        if (!filter->ringBuffer.initialize(w, h)) {
            obs_source_skip_video_filter(filter->context);
            return;
        }

        // 3. 이전 해상도에서 만들어진 쓸모없는 마스킹 큐 비우기
        MaskPayload dummy;
        while (filter->maskChannel.consume(dummy)) {}
        filter->lastMask = MaskPayload{};

        // 4. 새 해상도로 AI 워커 재시작
        filter->mockWorker.start(w, h, [filter](const MaskPayload& payload) {
            filter->maskChannel.publish(payload);
        });
    }

    // --- Step 2: 현재 프레임을 Ring Buffer HEAD에 Push ---
    uint64_t ts = obs_get_video_frame_time();
    filter->ringBuffer.pushFrame(parent, ts);

    // --- Step 3: AI 결과 채널에서 최신 마스킹 페이로드 Consume ---
    MaskPayload newMask{};
    if (filter->maskChannel.consume(newMask))
        filter->lastMask = newMask;

    // --- Step 4~5: N프레임 지연된 슬롯 꺼내기 ---
    const FrameRingBuffer::Slot* delayedSlot = filter->ringBuffer.peekDelayedSlot();

    if (!delayedSlot || !delayedSlot->getTexture()) {
        // 버퍼가 아직 충분히 안 쌓임 → 검정 홀드 프레임 (Bounded Exposure: 노출 0)
        gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
        gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xFF000000);
        while (gs_effect_loop(solid, "Solid"))
            gs_draw_sprite(nullptr, 0, w, h);
        return;
    }

    // --- Step 5b: 지연 프레임 그리기 ---
    gs_texture_t* delayedTex = delayedSlot->getTexture();
    gs_effect_t* draw = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_effect_set_texture(gs_effect_get_param_by_name(draw, "image"), delayedTex);
    while (gs_effect_loop(draw, "Draw"))
        gs_draw_sprite(delayedTex, 0, w, h);

    // --- 마스킹 오버레이: BlurRect 영역에 검정 박스 덮어쓰기 (Mock 시각화) ---
    // TODO (Role A): 이 부분을 HLSL Pixel Blur Shader 호출로 교체
    if (filter->lastMask.rectCount > 0) {
        gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
        gs_eparam_t* colorParam = gs_effect_get_param_by_name(solid, "color");
        gs_effect_set_color(colorParam, 0xFF000000); // 불투명 검정

        gs_matrix_push();
        while (gs_effect_loop(solid, "Solid")) {
            for (int i = 0; i < filter->lastMask.rectCount; i++) {
                const BlurRect& r = filter->lastMask.rects[i];
                gs_matrix_identity();
                gs_matrix_translate3f((float)r.x, (float)r.y, 0.0f);
                gs_draw_sprite(nullptr, 0, (uint32_t)r.width, (uint32_t)r.height);
            }
        }
        gs_matrix_pop();
    }

    // --- [Role D] 보안 상태 테두리 오버레이 ---
    // currentState에 따라 화면 가장자리에 색상 테두리를 그린다.
    // SAFE: 초록 / PARTIAL: 노랑 / RISK: 빨강
    {
        uint32_t borderColor = 0xFF00FF00; // 기본: SAFE (초록, 0xAARRGGBB)
        if (filter->currentState == SecurityState::PARTIAL)
            borderColor = 0xFFFFFF00; // 노랑 (0xAARRGGBB)
        else if (filter->currentState == SecurityState::RISK)
            borderColor = 0xFFFF0000; // 빨강 (0xAARRGGBB)

        const int BORDER = 6; // 테두리 두께 (픽셀)

        gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
        gs_eparam_t* colorParam = gs_effect_get_param_by_name(solid, "color");
        gs_effect_set_color(colorParam, borderColor);

        gs_matrix_push();
        while (gs_effect_loop(solid, "Solid")) {
            // 위
            gs_matrix_identity();
            gs_matrix_translate3f(0.0f, 0.0f, 0.0f);
            gs_draw_sprite(nullptr, 0, w, (uint32_t)BORDER);
            // 아래
            gs_matrix_identity();
            gs_matrix_translate3f(0.0f, (float)(h - BORDER), 0.0f);
            gs_draw_sprite(nullptr, 0, w, (uint32_t)BORDER);
            // 왼쪽
            gs_matrix_identity();
            gs_matrix_translate3f(0.0f, 0.0f, 0.0f);
            gs_draw_sprite(nullptr, 0, (uint32_t)BORDER, h);
            // 오른쪽
            gs_matrix_identity();
            gs_matrix_translate3f((float)(w - BORDER), 0.0f, 0.0f);
            gs_draw_sprite(nullptr, 0, (uint32_t)BORDER, h);
        }
        gs_matrix_pop();
    }
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
  
// ================================================================
// [Role D] Properties UI
// ================================================================

#define SC_SETTING_BLACKLIST      "sc_blacklist"
#define SC_SETTING_BLUR_INTENSITY "sc_blur_intensity"
#define SC_SETTING_GAME_MODE      "sc_game_mode"
#define SC_SETTING_SENSITIVITY    "sc_sensitivity"

// OBS 필터 패널에 표시할 기본값 설정
static void securecast_get_defaults(obs_data_t* settings)
{
    obs_data_set_default_string(settings, SC_SETTING_BLACKLIST,      "");
    obs_data_set_default_double(settings, SC_SETTING_BLUR_INTENSITY, 5.0);
    obs_data_set_default_bool  (settings, SC_SETTING_GAME_MODE,      false);
    obs_data_set_default_double(settings, SC_SETTING_SENSITIVITY,    0.5);
}

// OBS 필터 패널 UI 컨트롤 구성
static obs_properties_t* securecast_get_properties(void* data)
{
    (void)data;
    obs_properties_t* props = obs_properties_create();

    obs_properties_add_text(props, SC_SETTING_BLACKLIST,
        "Blacklist Apps (one per line)", OBS_TEXT_MULTILINE);

    obs_properties_add_float_slider(props, SC_SETTING_BLUR_INTENSITY,
        "Blur Intensity", 1.0, 10.0, 0.5);

    obs_properties_add_bool(props, SC_SETTING_GAME_MODE,
        "Game Mode");

    obs_properties_add_float_slider(props, SC_SETTING_SENSITIVITY,
        "Detection Sensitivity", 0.0, 1.0, 0.05);

    return props;
}

// 사용자가 패널에서 값을 변경할 때마다 호출 — 필터 인스턴스에 반영
static void securecast_update(void* data, obs_data_t* settings)
{
    SecureCastFilter* filter = static_cast<SecureCastFilter*>(data);

    filter->blacklistApps = obs_data_get_string(settings, SC_SETTING_BLACKLIST);
    filter->blurIntensity = (float)obs_data_get_double(settings, SC_SETTING_BLUR_INTENSITY);
    filter->isGameMode    = obs_data_get_bool(settings, SC_SETTING_GAME_MODE);
    filter->sensitivity   = (float)obs_data_get_double(settings, SC_SETTING_SENSITIVITY);

    blog(LOG_INFO, "[SecureCast][D] Settings updated — blur=%.1f game=%d sensitivity=%.2f",
         filter->blurIntensity, (int)filter->isGameMode, filter->sensitivity);
}

// ================================================================
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
    info.id           = "securecast_filter";
    info.type         = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO;
    info.get_name = securecast_get_name;
    info.create = securecast_create;
    info.destroy = securecast_destroy;
    info.video_tick = securecast_video_tick;
    info.video_render = securecast_video_render;
    info.get_properties = securecast_get_properties;
    info.get_defaults   = securecast_get_defaults;
    info.update         = securecast_update;
    return info;
}();
