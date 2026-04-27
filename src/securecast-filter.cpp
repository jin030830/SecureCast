#include "securecast-filter.h"
#include <stdlib.h>
#include <string.h>
#include <chrono>

// ================================================================
// [Role C] FrameRingBuffer 구현부
// ================================================================

bool FrameRingBuffer::initialize(uint32_t width, uint32_t height)
{
    if (m_initialized)
        return true;

    m_width  = width;
    m_height = height;

    // OBS Graphics Context가 잡혀있는 상태(video_render 내부)에서만 호출됨
    // 각 슬롯에 GPU 텍스처를 미리 할당한다.
    for (auto& slot : m_slots) {
        // GS_BGRA: OBS 내부 표준 포맷 (DX11 기준 DXGI_FORMAT_B8G8R8A8_UNORM)
        slot.texture = gs_texture_create(width, height, GS_BGRA, 1, nullptr, GS_RENDER_TARGET);
        if (!slot.texture) {
            blog(LOG_ERROR, "Failed to allocate ring buffer texture slot.");
            destroy(); // 부분 할당된 것 정리
            return false;
        }
        slot.timestamp = 0;
    }

    m_initialized = true;
    blog(LOG_INFO, "FrameRingBuffer initialized: %dx%d, %d slots.",
            width, height, SC_RING_BUFFER_SLOTS);
    return true;
}

void FrameRingBuffer::destroy()
{
    if (!m_initialized)
        return;

    // [P1 수정] OBS가 이미 Graphics Context를 잡은 상태에서 호출될 수 있으므로
    // 안전하게 enter/leave를 감싸되, 이미 잡혀있는 경우를 대비해 상태 체크
    obs_enter_graphics();
    for (auto& slot : m_slots) {
        if (slot.texture) {
            gs_texture_destroy(slot.texture);
            slot.texture = nullptr;
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

    // HEAD 슬롯의 텍스처를 렌더 타겟(RT)으로 설정
    gs_texture_t* rt = m_slots[m_head].texture;
    gs_set_render_target(rt, nullptr);

    // [P0 수정] gs_clear에 반드시 유효한 vec4* 전달 (nullptr 전달 시 크래시)
    struct vec4 clearColor;
    vec4_zero(&clearColor);
    gs_clear(GS_CLEAR_COLOR, &clearColor, 1.0f, 0);

    // 상위(Parent) 소스의 현재 프레임을 RT에 복사 (OBS 내장 함수)
    obs_source_video_render(source);

    // 렌더 타겟 해제 (OBS 기본 RT로 복귀)
    gs_set_render_target(nullptr, nullptr);

    m_slots[m_head].timestamp = timestamp;

    // 다음 HEAD로 순환
    m_head = (m_head + 1) % SC_RING_BUFFER_SLOTS;
    if (m_frameCount < SC_RING_BUFFER_SLOTS)
        m_frameCount++;
}

const FrameRingBuffer::Slot* FrameRingBuffer::peekDelayedSlot() const
{
    // N 프레임이 아직 쌓이지 않은 초기 상태 → 대기
    if (m_frameCount < SC_RING_BUFFER_SLOTS)
        return nullptr;

    // TAIL = HEAD - N (순환 인덱스)
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

static const char* securecast_get_name(void* type_data)
{
    (void)type_data;
    return "SecureCast Privacy Masking";
}

static void* securecast_create(obs_data_t* settings, obs_source_t* context)
{
    (void)settings;

    SecureCastFilter* filter = new SecureCastFilter();
    filter->context      = context;
    filter->isActive     = true;
    filter->isGameMode   = false;
    filter->currentState = SecurityState::SAFE;

    // FrameRingBuffer는 첫 video_render 호출 시 지연 초기화 (OBS gs context 필요)

    // MockAIWorker 시작 (해상도는 video_render 첫 호출 때 rimingBuffer 초기화 후 갱신)
    // 기본 해상도(1920x1080)로 먼저 시작, 렌더러 초기화 후 재시작하지 않아도 됨
    filter->mockWorker.start(1920, 1080,
        [filter](const MaskPayload& payload) {
            // AI 스레드에서 결과 도착 → 락프리 채널에 발행
            filter->maskChannel.publish(payload);
        });

    blog(LOG_INFO, "Filter created (Role C: Mock Pipeline Active).");
    return filter;
}

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
    } else if (filter->ringBuffer.getWidth() != w || filter->ringBuffer.getHeight() != h) {
        // [P1 수정] 소스 해상도가 바뀌면 텍스처를 재생성해야 화면 깨짐 방지
        blog(LOG_INFO, "Resolution changed (%dx%d -> %dx%d). Reinitializing ring buffer.",
                filter->ringBuffer.getWidth(), filter->ringBuffer.getHeight(), w, h);
        filter->ringBuffer.destroy();
        if (!filter->ringBuffer.initialize(w, h)) {
            obs_source_skip_video_filter(filter->context);
            return;
        }
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

    if (!delayedSlot) {
        // 버퍼가 아직 충분히 안 쌓임 → 검정 홀드 프레임 (Bounded Exposure: 노출 0)
        gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
        gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xFF000000);
        while (gs_effect_loop(solid, "Solid"))
            gs_draw_sprite(nullptr, 0, w, h);
        return;
    }

    // --- Step 5b: 지연 프레임 그리기 ---
    gs_effect_t* draw = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_effect_set_texture(gs_effect_get_param_by_name(draw, "image"), delayedSlot->texture);
    while (gs_effect_loop(draw, "Draw"))
        gs_draw_sprite(delayedSlot->texture, 0, w, h);

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
}

// ================================================================
// Source Info Dispatch Table
// ================================================================

struct obs_source_info securecast_filter_info = []() {
    struct obs_source_info info = {};
    info.id           = "securecast_filter";
    info.type         = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO;
    info.get_name     = securecast_get_name;
    info.create       = securecast_create;
    info.destroy      = securecast_destroy;
    info.video_render = securecast_video_render;
    return info;
}();
