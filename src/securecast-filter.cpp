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
#ifdef _WIN32
#include "window_tracker.h"   // sc_scan_blacklisted_windows (Role A: 블랙리스트 앱 좌표 수집)
#endif
#include <chrono>
#include <stdlib.h>
#include <string.h>

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
            for (auto& clean_slot : m_slots) {
                if (clean_slot.texrender) {
                    gs_texrender_destroy(clean_slot.texrender);
                    clean_slot.texrender = nullptr;
                }
            }
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

// [C-2 수정] 미사용 source 인자 제거
void FrameRingBuffer::pushFrame(uint64_t timestamp, obs_source_t* filter_context)
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

        obs_source_t* target = obs_filter_get_target(filter_context);
        if (target)
            obs_source_video_render(target);

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
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(50),
                          [this]() { return !m_running.load(); });
        }

        if (!m_running.load())
            break;

#ifdef SC_DEBUG_MOCK
        MaskPayload payload{};
        payload.rectCount = 1;
        payload.rects[0].x     = static_cast<int>(m_frameWidth  / 2) - 100;
        payload.rects[0].y     = static_cast<int>(m_frameHeight / 2) - 50;
        payload.rects[0].width  = 200;
        payload.rects[0].height = 100;
        payload.rects[0].type   = 1;
        if (m_callback)
            m_callback(payload);
#else
        MaskPayload payload{};
        if (m_callback)
            m_callback(payload);
#endif
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
    // filter->isGameMode = false;  // [v2] 게임 모드 — 현재 스코프 외
    filter->currentState = SecurityState::SAFE;
    filter->trackerAccumulator = 0.0f;

    obs_log(LOG_INFO, "[SecureCast] Filter created.");
    obs_enter_graphics();
#ifdef _WIN32
    filter->readback.initialize();
#endif
    obs_leave_graphics();

    blog(LOG_INFO, "Filter created (Role C: 2-Stage Gate Pipeline Active).");
    return filter;
}

static void securecast_destroy(void* data)
{
    SecureCastFilter* filter = static_cast<SecureCastFilter*>(data);

    blog(LOG_INFO, "Destroying filter...");
    filter->mockWorker.stop();

#ifdef _WIN32
    obs_enter_graphics();
    filter->readback.destroyImmediate();
    filter->ringBuffer.destroy();
    obs_leave_graphics();
#else
    filter->ringBuffer.destroy();
#endif

    delete filter;
    blog(LOG_INFO, "Filter destroyed.");
}

// ================================================================
// [Role C] 핵심 렌더 루프 (60 FPS)
// ================================================================
static void securecast_video_render(void* data, gs_effect_t* effect)
{
    (void)effect;

    SecureCastFilter* filter = static_cast<SecureCastFilter*>(data);

    // [Role D] isActive는 GUI 스레드(update)에서도 쓸 수 있으므로 settingsMutex로 보호
    {
        std::lock_guard<std::mutex> lock(filter->settingsMutex);
        if (!filter->isActive) {
            obs_source_skip_video_filter(filter->context);
            return;
        }
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

    // --- Step 1: 링 버퍼 초기화 또는 해상도 변경 대응 ---
    if (!filter->ringBuffer.isInitialized()) {
        if (!filter->ringBuffer.initialize(w, h)) {
            obs_source_skip_video_filter(filter->context);
            return;
        }
        filter->mockWorker.start(w, h, [filter](const MaskPayload& payload) {
            filter->maskChannel.publish(payload);
        });
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

        filter->mockWorker.start(w, h, [filter](const MaskPayload& payload) {
            filter->maskChannel.publish(payload);
        });

#ifdef _WIN32
        // [C-6 수정] 해상도 변경 시 readback 풀도 반드시 재구성
        {
            int rW = ((int)w > 1920) ? 1920 : (int)w;
            int rH = ((int)h > 1080) ? 1080 : (int)h;
            filter->readback.destroyImmediate();
            filter->readback.initialize();
            std::vector<std::pair<int,int>> ss = {{64,64},{rW,rH}};
            filter->readback.resizePool(ss);
            filter->readbackBuffer.resize((size_t)(64*64*4) + (size_t)(rW*rH*4), 0);
            filter->fullScreenHash.reset();
            filter->health.reset();
            filter->readback.setForceReleasedFlag(true);
            blog(LOG_INFO, "[SecureCast] Readback pool rebuilt for new resolution %dx%d.", rW, rH);
        }
#endif
    }

    // --- Step 2: 현재 프레임 Push ---
    uint64_t ts = obs_get_video_frame_time();
    filter->ringBuffer.pushFrame(ts, filter->context);

    // --- Step 3: AI 결과 Consume (Fail Secure) ---
    if (!filter->health.isDegraded()) {
        MaskPayload newMask{};
        if (filter->maskChannel.consume(newMask))
            filter->lastMask = newMask;
    } else {
        MaskPayload drainMask{};
        filter->maskChannel.consume(drainMask);
        blog(LOG_INFO, "[SecureCast] Mask hold active (DEGRADED, stall_count=%d).",
             filter->health.getConsecutiveFailures());
    }

    MaskPayload ocrSnapshot = filter->lastMask;
    MaskPayload blacklistSnapshot;
    {
        std::lock_guard<std::mutex> lock(filter->blacklistMutex);
        blacklistSnapshot = filter->blacklistMask;
    }

    // currentState 업데이트 — settingsMutex로 보호
    {
        std::lock_guard<std::mutex> lock(filter->settingsMutex);
        if (filter->health.isCritical()) {
            filter->currentState = SecurityState::RISK;
        } else if (ocrSnapshot.rectCount > 0 || blacklistSnapshot.rectCount > 0) {
            filter->currentState = SecurityState::PARTIAL;
        } else {
            filter->currentState = SecurityState::SAFE;
        }
    }

#ifdef _WIN32
    // [C-5 수정] Phase 1(Collect+Hash)을 delayedSlot early-return 앞에 실행
    int ocrW = ((int)w > 1920) ? 1920 : (int)w;
    int ocrH = ((int)h > 1080) ? 1080 : (int)h;

    size_t expectedBufferSize = (size_t)(64 * 64 * 4) + (size_t)(ocrW * ocrH * 4);
    if (filter->readbackBuffer.size() != expectedBufferSize) {
        filter->fullScreenHash.reset();
        std::vector<std::pair<int, int>> slotSizes = {{64, 64}, {ocrW, ocrH}};
        filter->readback.resizePool(slotSizes);
        filter->readbackBuffer.resize(expectedBufferSize, 0);
    }

    CollectResult sc_collected = filter->readback.tryCollectPreviousFrame();
    if (sc_collected == CollectResult::OK || sc_collected == CollectResult::SOFT_RECOVERED) {
        if (sc_collected == CollectResult::SOFT_RECOVERED)
            filter->health.onForceRelease();
        else
            filter->health.onCollectSuccess();

        bool forceCheck = filter->readback.wasForceReleased();
        uint8_t* fullBuf = filter->readbackBuffer.data();
        if (filter->readback.readStagingBuffer(0, fullBuf, 64 * 4, 64)) {
            BlurRect gridBox = {0, 0, 64, 64, 0};
            bool screenChanged = forceCheck ||
                filter->fullScreenHash.hasChanged(fullBuf, 64 * 64 * 4, 12, &gridBox);
            if (screenChanged) {
                if (forceCheck) gridBox = {0, 0, 64, 64, 0};
                blog(LOG_INFO, "[SecureCast] Change detected! BBox:(%d,%d %dx%d)",
                     gridBox.x, gridBox.y, gridBox.width, gridBox.height);
                float scaleX = (float)ocrW / 64.0f, scaleY = (float)ocrH / 64.0f;
                BlurRect finalBox = {
                    (int)(gridBox.x * scaleX), (int)(gridBox.y * scaleY),
                    (int)(gridBox.width * scaleX), (int)(gridBox.height * scaleY), 0
                };
                uint8_t* ocrBuf = filter->readbackBuffer.data() + (64 * 64 * 4);
                if (filter->readback.readStagingBuffer(1, ocrBuf, ocrW * 4, ocrH)) {
                    (void)finalBox; // [Role B 연계 포인트] filter->aiWorker.feedFrame(result);
                }
            } else {
                if (++filter->logUnchangedFrames >= 120) {
                    blog(LOG_INFO, "[SecureCast] No change for 120 frames.");
                    filter->logUnchangedFrames = 0;
                }
            }
        } else {
            blog(LOG_WARNING, "[SecureCast] Failed to read Slot 0 staging buffer.");
        }
        filter->readback.releaseFrame();
    } else {
        filter->health.onCollectFailure();
        if (filter->readback.isPipelineFull()) {
            if (filter->logStallCount++ % 30 == 0)
                blog(LOG_WARNING, "[SecureCast] Pipeline FULL, GPU not responding.");
        }
    }
#endif

    // --- Step 4~5: N프레임 지연 슬롯 ---
    const FrameRingBuffer::Slot* delayedSlot = filter->ringBuffer.peekDelayedSlot();

    if (!delayedSlot || !delayedSlot->getTexture()) {
        gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
        gs_eparam_t* colorParam = gs_effect_get_param_by_name(solid, "color");
        gs_effect_set_color(colorParam, 0xFF000000);
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

    // --- 마스킹 오버레이 ---
    auto renderMask = [](const MaskPayload& mask, uint32_t color) {
        if (mask.rectCount > 0) {
            gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
            gs_eparam_t* colorParam = gs_effect_get_param_by_name(solid, "color");
            gs_effect_set_color(colorParam, color);
            gs_matrix_push();
            while (gs_effect_loop(solid, "Solid")) {
                for (int i = 0; i < mask.rectCount; i++) {
                    const BlurRect& r = mask.rects[i];
                    gs_matrix_identity();
                    gs_matrix_translate3f((float)r.x, (float)r.y, 0.0f);
                    gs_draw_sprite(nullptr, 0, (uint32_t)r.width, (uint32_t)r.height);
                }
            }
            gs_matrix_pop();
        }
    };

    // [C-10 수정] ARGB 포맷: A=AA R=FF G=00 B=00 → 반투명 빨강
    renderMask(ocrSnapshot,       0xAAFF0000);
    renderMask(blacklistSnapshot, 0xFF000000);

    // --- [Role D] 보안 상태 테두리 오버레이 ---
    // gs_effect_set_color()는 OBS ARGB 포맷: 상위 바이트부터 A·R·G·B 순서
    //   SAFE    0xFF00FF00 : A=FF R=00 G=FF B=00 → 초록
    //   PARTIAL 0xFFFFFF00 : A=FF R=FF G=FF B=00 → 노랑
    //   RISK    0xFFFF0000 : A=FF R=FF G=00 B=00 → 빨강
    {
        SecurityState state;
        {
            std::lock_guard<std::mutex> lock(filter->settingsMutex);
            state = filter->currentState;
        }

        uint32_t borderColor = 0xFF00FF00; // SAFE: 초록
        if (state == SecurityState::PARTIAL)
            borderColor = 0xFFFFFF00;      // PARTIAL: 노랑
        else if (state == SecurityState::RISK)
            borderColor = 0xFFFF0000;      // RISK: 빨강

        const int BORDER = 6;
        gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
        gs_eparam_t* colorParam = gs_effect_get_param_by_name(solid, "color");
        gs_effect_set_color(colorParam, borderColor);

        gs_matrix_push();
        while (gs_effect_loop(solid, "Solid")) {
            gs_matrix_identity();
            gs_matrix_translate3f(0.0f, 0.0f, 0.0f);
            gs_draw_sprite(nullptr, 0, w, (uint32_t)BORDER);
            gs_matrix_identity();
            gs_matrix_translate3f(0.0f, (float)(h - BORDER), 0.0f);
            gs_draw_sprite(nullptr, 0, w, (uint32_t)BORDER);
            gs_matrix_identity();
            gs_matrix_translate3f(0.0f, 0.0f, 0.0f);
            gs_draw_sprite(nullptr, 0, (uint32_t)BORDER, h);
            gs_matrix_identity();
            gs_matrix_translate3f((float)(w - BORDER), 0.0f, 0.0f);
            gs_draw_sprite(nullptr, 0, (uint32_t)BORDER, h);
        }
        gs_matrix_pop();
    }

#ifdef _WIN32
    // Phase 2+3: Enqueue + Submit
    {
        bool pipelineFull = filter->readback.isPipelineFull();
        bool shouldCopy = !pipelineFull;
        if (pipelineFull && filter->readback.isOldestSlotStalled()) {
            filter->readback.forceReleaseOldestSlot();
            filter->health.onForceRelease();
            shouldCopy = true;
        }
        if (shouldCopy) {
            BlurRect screenRect = {0, 0, (int)w, (int)h, 0};
            filter->readback.enqueueCopy(delayedTex, screenRect, 0, 64, 64);
            filter->readback.enqueueCopy(delayedTex, screenRect, 1, ocrW, ocrH);
            // [C-7 수정] submitFrame을 shouldCopy 블록 안으로
            filter->readback.submitFrame();
            filter->frameCounter++;
            if (filter->logEnqueueCount++ % 300 == 0)
                blog(LOG_INFO, "[SecureCast] Enqueued frame batch OK.");
        }
    }

    // Phase 4: Health Check & Reset
    if (filter->health.shouldReset()) {
        blog(LOG_ERROR, "[SecureCast] Pipeline CRITICAL. Resetting.");
        MaskPayload bo{}; bo.rectCount = 1;
        bo.rects[0] = {0, 0, (int)w, (int)h, 0};
        filter->lastMask = bo;
        filter->readback.destroyImmediate();
        filter->readback.initialize();
        std::vector<std::pair<int,int>> ss = {{64,64},{ocrW,ocrH}};
        filter->readback.resizePool(ss);
        filter->health.reset();
        filter->readback.setForceReleasedFlag(true);
    }
#endif
}

// ---------------------------------------------------------
// Tick (Slow-Path) — 0.15초마다 윈도우 추적
// ---------------------------------------------------------
static void securecast_video_tick(void* data, float seconds)
{
    SecureCastFilter* filter = (SecureCastFilter*)data;
    if (!filter || !filter->isActive)
        return;

    filter->trackerAccumulator += seconds;
    if (filter->trackerAccumulator >= 0.15f) {
        filter->trackerAccumulator = 0.0f;

#ifdef _WIN32
        TrackedWindowList list{};
        sc_scan_blacklisted_windows(&list);

        {
            std::lock_guard<std::mutex> lock(filter->blacklistMutex);
            filter->blacklistMask.rectCount = (list.count > SC_MAX_BLUR_RECTS) ? SC_MAX_BLUR_RECTS : list.count;
            for (int i = 0; i < filter->blacklistMask.rectCount; ++i) {
                filter->blacklistMask.rects[i] = {
                    (int)list.items[i].bounds.left,
                    (int)list.items[i].bounds.top,
                    (int)(list.items[i].bounds.right  - list.items[i].bounds.left),
                    (int)(list.items[i].bounds.bottom - list.items[i].bounds.top),
                    0
                };
            }
            if (list.count > 0) {
                if (filter->logScanThrottle++ % 10 == 0)
                    blog(LOG_INFO, "[SecureCast] %d blacklisted windows tracked.", list.count);
            }
        }
#endif
    }
}

// ================================================================
// [Role D] Properties UI
// ================================================================

#define SC_SETTING_BLACKLIST      "sc_blacklist"
#define SC_SETTING_BLUR_INTENSITY "sc_blur_intensity"
// #define SC_SETTING_GAME_MODE   "sc_game_mode"  // [v2] 게임 모드 — 현재 스코프 외
#define SC_SETTING_SENSITIVITY    "sc_sensitivity"

static void securecast_get_defaults(obs_data_t* settings)
{
    obs_data_set_default_string(settings, SC_SETTING_BLACKLIST,      "");
    obs_data_set_default_double(settings, SC_SETTING_BLUR_INTENSITY, 5.0);
    // obs_data_set_default_bool(settings, SC_SETTING_GAME_MODE, false);  // [v2]
    obs_data_set_default_double(settings, SC_SETTING_SENSITIVITY,    0.5);
}

static obs_properties_t* securecast_get_properties(void* data)
{
    (void)data;
    obs_properties_t* props = obs_properties_create();

    obs_properties_add_text(props, SC_SETTING_BLACKLIST,
        "Blacklist Apps (one per line)", OBS_TEXT_MULTILINE);

    obs_properties_add_float_slider(props, SC_SETTING_BLUR_INTENSITY,
        "Blur Intensity", 1.0, 10.0, 0.5);

    // obs_properties_add_bool(props, SC_SETTING_GAME_MODE, "Game Mode");  // [v2]

    obs_properties_add_float_slider(props, SC_SETTING_SENSITIVITY,
        "Detection Sensitivity", 0.0, 1.0, 0.05);

    return props;
}

// GUI 스레드에서 호출되므로 settingsMutex로 보호 (Render Thread와 data race 방지)
static void securecast_update(void* data, obs_data_t* settings)
{
    SecureCastFilter* filter = static_cast<SecureCastFilter*>(data);

    std::lock_guard<std::mutex> lock(filter->settingsMutex);

    filter->blacklistApps = obs_data_get_string(settings, SC_SETTING_BLACKLIST);
    filter->blurIntensity = (float)obs_data_get_double(settings, SC_SETTING_BLUR_INTENSITY);
    // filter->isGameMode = obs_data_get_bool(settings, SC_SETTING_GAME_MODE);  // [v2]
    filter->sensitivity   = (float)obs_data_get_double(settings, SC_SETTING_SENSITIVITY);

    blog(LOG_INFO, "[SecureCast][D] Settings updated — blur=%.1f sensitivity=%.2f",
         filter->blurIntensity, filter->sensitivity);
}

// ================================================================
// Source Info Dispatch Table
// ================================================================
struct obs_source_info securecast_filter_info = []() {
    struct obs_source_info info = {};
    info.id           = "securecast_filter";
    info.type         = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO;
    info.get_name       = securecast_get_name;
    info.create         = securecast_create;
    info.destroy        = securecast_destroy;
    info.video_tick     = securecast_video_tick;
    info.video_render   = securecast_video_render;
    info.get_properties = securecast_get_properties;
    info.get_defaults   = securecast_get_defaults;
    info.update         = securecast_update;
    return info;
}();
