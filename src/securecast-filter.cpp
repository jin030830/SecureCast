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
#include "ocr-engine.h"       // [Role B] OCR engine

#include <chrono>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <utility>
#include <vector>

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

    if (gs_texrender_begin(tr, m_width, m_height)) {
        struct vec4 clearColor;
        vec4_zero(&clearColor);
        gs_clear(GS_CLEAR_COLOR, &clearColor, 1.0f, 0);

        gs_ortho(0.0f, (float)m_width, 0.0f, (float)m_height, -100.0f, 100.0f);
        obs_source_video_render(source);

        gs_texrender_end(tr);
    }

    m_slots[m_head].timestamp = timestamp;

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
// [Role B/C] GPU texture -> CPU BGRA pixels
// ================================================================

static bool read_texture_bgra_to_cpu(
    gs_texture_t* texture,
    uint32_t width,
    uint32_t height,
    std::vector<uint8_t>& outPixels,
    int& outStride)
{
    if (!texture || width == 0 || height == 0)
        return false;

    gs_stagesurf_t* stage = gs_stagesurface_create(width, height, GS_BGRA);
    if (!stage) {
        blog(LOG_ERROR, "[securecast][ocr] Failed to create staging surface.");
        return false;
    }

    gs_stage_texture(stage, texture);

    uint8_t* mappedData = nullptr;
    uint32_t mappedStride = 0;

    if (!gs_stagesurface_map(stage, &mappedData, &mappedStride)) {
        blog(LOG_WARNING, "[securecast][ocr] Failed to map staging surface.");
        gs_stagesurface_destroy(stage);
        return false;
    }

    const uint32_t tightStride = width * 4;
    outPixels.resize((size_t)tightStride * height);

    for (uint32_t y = 0; y < height; y++) {
        memcpy(
            outPixels.data() + (size_t)y * tightStride,
            mappedData + (size_t)y * mappedStride,
            tightStride
        );
    }

    gs_stagesurface_unmap(stage);
    gs_stagesurface_destroy(stage);

    outStride = (int)tightStride;
    return true;
}

// ================================================================
// [Role C] MockAIWorker 구현부
//
// 注意：这个类现在保留，但 securecast_video_render() 里不再 start 它。
// 这样可以先切到真实 OCR pipeline。
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
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(50),
                          [this]() { return !m_running.load(); });
        }

        if (!m_running.load())
            break;

        // Mock 逻辑保留备用，但当前真实 OCR pipeline 不会启动这个 worker。
        MaskPayload payload{};
        payload.rectCount = 1;
        payload.rects[0].x      = static_cast<int>(m_frameWidth  / 2) - 100;
        payload.rects[0].y      = static_cast<int>(m_frameHeight / 2) - 50;
        payload.rects[0].width  = 200;
        payload.rects[0].height = 100;
        payload.rects[0].type   = 1; // Blackout

        if (m_callback)
            m_callback(payload);
    }
}

// ================================================================
// [Role C] AtomicMaskChannel 구현부
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
        return false;

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
    filter->trackerAccumulator = 0.0f;

    obs_log(LOG_INFO, "[SecureCast] Filter created.");

    // [Role B] OCR Engine 초기화
    filter->ocrEngine.init();

    blog(LOG_INFO, "Filter created (Role B/C: OCR Pipeline Active).");
    return filter;
}

static void securecast_destroy(void* data)
{
    SecureCastFilter* filter = static_cast<SecureCastFilter*>(data);

    blog(LOG_INFO, "Destroying filter...");

    // 当前不再启动 MockAIWorker，但 stop() 保留是安全的。
    filter->mockWorker.stop();

    filter->ringBuffer.destroy();

    delete filter;
    blog(LOG_INFO, "Filter destroyed.");
}

// ================================================================
// [Role C] 핵심 렌더 루프
// ================================================================

static void securecast_video_render(void* data, gs_effect_t* effect)
{
    (void)effect;

    SecureCastFilter* filter = static_cast<SecureCastFilter*>(data);
    if (!filter->isActive) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

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

    // --- Step 1: Ring buffer init / resize ---
    if (!filter->ringBuffer.isInitialized()) {
        if (!filter->ringBuffer.initialize(w, h)) {
            obs_source_skip_video_filter(filter->context);
            return;
        }

        // 不再启动 MockAIWorker。
        blog(LOG_INFO, "[securecast][ocr] Real OCR pipeline waiting for delayed frames.");
    } else if (filter->ringBuffer.getWidth() != w || filter->ringBuffer.getHeight() != h) {
        blog(LOG_INFO, "Resolution changed (%dx%d -> %dx%d). Reinitializing ring buffer.",
                filter->ringBuffer.getWidth(), filter->ringBuffer.getHeight(), w, h);

        filter->mockWorker.stop();

        filter->ringBuffer.destroy();
        if (!filter->ringBuffer.initialize(w, h)) {
            obs_source_skip_video_filter(filter->context);
            return;
        }

        MaskPayload dummy;
        while (filter->maskChannel.consume(dummy)) {}
        filter->lastMask = MaskPayload{};

        blog(LOG_INFO, "[securecast][ocr] Real OCR pipeline restarted after resize.");
    }

    // --- Step 2: Push current frame to Ring Buffer ---
    uint64_t ts = obs_get_video_frame_time();
    filter->ringBuffer.pushFrame(parent, ts);

    // --- Step 3: Get delayed frame ---
    const FrameRingBuffer::Slot* delayedSlot = filter->ringBuffer.peekDelayedSlot();

    if (!delayedSlot || !delayedSlot->getTexture()) {
        gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
        gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xFF000000);
        while (gs_effect_loop(solid, "Solid"))
            gs_draw_sprite(nullptr, 0, w, h);
        return;
    }

    // =====================================================
    // [Role B] Real OCR analysis from delayed frame
    // =====================================================
    if (filter->ocrEngine.available()) {
        static int ocrFrameCounter = 0;
        ocrFrameCounter++;

        // 不要每帧 OCR。30fps 下约 0.5 秒识别一次。
        if (ocrFrameCounter >= 15) {
            ocrFrameCounter = 0;

            std::vector<uint8_t> bgraPixels;
            int stride = 0;

            gs_texture_t* ocrTex = delayedSlot->getTexture();

            if (read_texture_bgra_to_cpu(ocrTex, w, h, bgraPixels, stride)) {
                auto ocrBoxes = filter->ocrEngine.analyze_bgra_frame(
                    bgraPixels.data(),
                    (int)w,
                    (int)h,
                    stride
                );

                MaskPayload payload{};
                payload.rectCount = 0;

                const int maxRects = (int)(sizeof(payload.rects) / sizeof(payload.rects[0]));

for (const auto& box : ocrBoxes) {
    if (payload.rectCount >= maxRects)
        break;

    int x      = static_cast<int>(box.x);
    int y      = static_cast<int>(box.y);
    int width  = static_cast<int>(box.w);
    int height = static_cast<int>(box.h);

    if (x < 0) x = 0;
    if (y < 0) y = 0;

    if (x >= static_cast<int>(w) || y >= static_cast<int>(h))
        continue;

    if (x + width > static_cast<int>(w))
        width = static_cast<int>(w) - x;

    if (y + height > static_cast<int>(h))
        height = static_cast<int>(h) - y;

    if (width <= 0 || height <= 0)
        continue;

    BlurRect& r = payload.rects[payload.rectCount++];

    r.x      = x;
    r.y      = y;
    r.width  = width;
    r.height = height;
    r.type   = 1; // Blackout
}

                // 即使识别结果为 0，也发布空 payload，用来清掉旧遮罩。
                filter->maskChannel.publish(payload);

                blog(LOG_INFO, "[securecast][ocr] OCR boxes: %d", payload.rectCount);
            }
        }
    }

    // --- Step 4: Consume latest OCR mask result ---
    MaskPayload newMask{};
    if (filter->maskChannel.consume(newMask))
        filter->lastMask = newMask;

    // --- Step 5: Draw delayed frame ---
    gs_texture_t* delayedTex = delayedSlot->getTexture();
    gs_effect_t* draw = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_effect_set_texture(gs_effect_get_param_by_name(draw, "image"), delayedTex);
    while (gs_effect_loop(draw, "Draw"))
        gs_draw_sprite(delayedTex, 0, w, h);

    // --- Step 6: Draw masking overlay ---
    if (filter->lastMask.rectCount > 0) {
        gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
        gs_eparam_t* colorParam = gs_effect_get_param_by_name(solid, "color");
        gs_effect_set_color(colorParam, 0xFF000000);

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

// ---------------------------------------------------------
// Tick (Slow-Path)
// ---------------------------------------------------------

static void securecast_video_tick(void* data, float seconds)
{
    SecureCastFilter* filter = (SecureCastFilter*)data;
    if (!filter || !filter->isActive)
        return;

    sc_tracker_tick(seconds, &filter->trackerAccumulator);
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
    info.video_tick   = securecast_video_tick;
    info.video_render = securecast_video_render;
    return info;
}();
