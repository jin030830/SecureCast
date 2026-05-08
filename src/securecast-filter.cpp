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
            // [F13 Fix] 실패 시 이미 생성된 렌더러들을 로컬에서 직접 파괴하여 메모리 누수를 막고 destroy() 내 obs_enter_graphics() 이중 잠금 차단
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

void FrameRingBuffer::pushFrame(uint64_t timestamp, obs_source_t* filter_context)
{
    if (!m_initialized)
        return;

    gs_texrender_t* tr = m_slots[m_head].texrender;
    gs_texrender_reset(tr);

    // gs_texrender_begin/end가 내부적으로 렌더 타겟, 뷰포트, 투영 행렬을
    // 모두 저장/복원하므로 별도 백업은 불필요합니다.
    if (gs_texrender_begin(tr, m_width, m_height)) {
        struct vec4 clearColor;
        vec4_zero(&clearColor);
        gs_clear(GS_CLEAR_COLOR, &clearColor, 1.0f, 0);

        gs_ortho(0.0f, (float)m_width, 0.0f, (float)m_height, -100.0f, 100.0f);

        // [핵심] obs_filter_get_target은 필터 체인에서 "이 필터 바로 아래"를 반환.
        // parent를 렌더링하면 이 필터가 재호출되어 무한 루프가 발생하지만,
        // target을 렌더링하면 이 필터를 건너뛰므로 안전합니다.
        obs_source_t* target = obs_filter_get_target(filter_context);
        if (target)
            obs_source_video_render(target);

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
        return nullptr; // 아직 충분한 지연이 쌓이지 않음

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

        // [F4 Fix] 프로덕션에서 가짜 마스킹 박스가 영구 출력되는 데모 회귀 방지를 위해 디버그용 MOCK 가드를 둡니다.
#ifdef SC_DEBUG_MOCK
        MaskPayload payload{};
        payload.rectCount = 1;
        payload.rects[0].x     = static_cast<int>(m_frameWidth  / 2) - 100;
        payload.rects[0].y     = static_cast<int>(m_frameHeight / 2) - 50;
        payload.rects[0].width  = 200;
        payload.rects[0].height = 100;
        payload.rects[0].type   = 1; // Blackout

        if (m_callback)
            m_callback(payload);
#else
        // 실운영(Production) 시에는 빈 마스크를 주기적으로 전달하거나 스핀만 유지시킵니다.
        MaskPayload payload{};
        if (m_callback)
            m_callback(payload);
#endif
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
    // filter->isGameMode = false;  // [v2] 게임 모드 — 현재 스코프 외
    filter->currentState = SecurityState::SAFE;
    filter->trackerAccumulator = 0.0f;  // window_tracker tick throttle 누산기

    obs_log(LOG_INFO, "[SecureCast] Filter created.");
    // [핵심 해결] 그래픽 리소스 생성 시 반드시 그래픽 컨텍스트 진입 필요
    obs_enter_graphics();
#ifdef _WIN32
    filter->readback.initialize();
#endif
    obs_leave_graphics();

    // [C-3 수정] fullScreenBuffer 미사용 멤버 제거. 실제 해시 입력은 readbackBuffer.data()(슬롯 0)에서 가져옴.

#ifdef _WIN32
    // [Role D] 스트리머 전용 오버레이 HUD 시작 (OBS 캡처에서 자동 제외)
    if (!filter->overlay.create())
        blog(LOG_WARNING, "[SecureCast][D] OverlayWindow 생성 실패 — HUD 없이 계속.");
#endif

    blog(LOG_INFO, "Filter created (Role C: 2-Stage Gate Pipeline Active).");

    return filter;
}

// 필터 인스턴스 정리. OBS는 이 시점 이후 동일 data 포인터를 다시 안 넘긴다.
static void securecast_destroy(void* data)
{
    SecureCastFilter* filter = static_cast<SecureCastFilter*>(data);

    blog(LOG_INFO, "Destroying filter...");

    // AI 워커 먼저 중지 (콜백이 ring buffer에 접근하지 않도록)
    filter->mockWorker.stop();

#ifdef _WIN32
    // [Role D] 오버레이 HUD 먼저 종료 (메시지 루프 스레드 join)
    filter->overlay.destroy();

    obs_enter_graphics();
    // [C2-5 수정] shutdown 경로에서는 spin-wait가 없는 destroyImmediate() 사용.
    // readback.destroy()는 내부에서 최대 100k 사이클 스핀을 수행하므로
    // GPU 정체 시 OBS 종료가 수초 지연될 수 있음.
    filter->readback.destroyImmediate();
    filter->ringBuffer.destroy(); // [NEW-2 수정] gs_texrender_destroy는 graphics context 안에서만 안전
    obs_leave_graphics();
#else
    filter->ringBuffer.destroy();
#endif

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

    // [Role D] isActive는 GUI 스레드(update)에서도 쓸 수 있으므로 settingsMutex로 보호
    {
        std::lock_guard<std::mutex> lock(filter->settingsMutex);
        if (!filter->isActive) {
            obs_source_skip_video_filter(filter->context);
            return;
        }
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

        // 1. AI 워커 중지
        filter->mockWorker.stop();

        // 2. 링 버퍼 재구성
        filter->ringBuffer.destroy();
        if (!filter->ringBuffer.initialize(w, h)) {
            obs_source_skip_video_filter(filter->context);
            return;
        }

        // 3. 마스킹 큐 비우기
        MaskPayload dummy;
        while (filter->maskChannel.consume(dummy)) {}
        filter->lastMask = MaskPayload{};

        // 4. 새 해상도로 AI 워커 재시작
        filter->mockWorker.start(w, h, [filter](const MaskPayload& payload) {
            filter->maskChannel.publish(payload);
        });

#ifdef _WIN32
        // [C-6 수정] 해상도 변경 시 readback 풀도 반드시 재구성
        // ocrW/ocrH 캡으로 인해 expectedBufferSize가 같아 resizePool이 누락되던 버그 수정
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

    // --- Step 2: 현재 프레임을 Ring Buffer HEAD에 Push ---
    uint64_t ts = obs_get_video_frame_time();
    filter->ringBuffer.pushFrame(ts, filter->context);

    // --- Step 3: AI 결과 채널에서 최신 마스킹 페이로드 Consume ---
    // [Fail Secure] DEGRADED 상태에서는 AI가 불완전한 데이터로 마스크를 해제하는 것을 방지합니다.
    // GPU 정체 중에 들어온 분석 결과는 정체 이전의 오래된 픽셀 데이터를 기반으로 하므로,
    // 신뢰할 수 없습니다. 정상(OK) 상태일 때만 lastMask를 갱신합니다.
    if (!filter->health.isDegraded()) {
        MaskPayload newMask{};
        if (filter->maskChannel.consume(newMask)) {
            filter->lastMask = newMask;
        }
    } else {
        // DEGRADED 중에는 채널을 비워서 큐 포화를 방지하되, lastMask는 건드리지 않음
        MaskPayload drainMask{};
        filter->maskChannel.consume(drainMask); // 결과 버림 (큐 드레인 목적)
        blog(LOG_INFO, "[SecureCast] Mask hold active (DEGRADED, stall_count=%d). Discarding new AI result.",
             filter->health.getConsecutiveFailures());
    }

    // [THREAD-SAFE] [F6 Fix / F11 Fix] currentState 갱신 및 로컬 스택 임시 객체(ocrSnapshot, blacklistSnapshot) 추출을 delayedSlot 이전으로 상향 이동하여 Torn-Read 차단 및 실시간성 확보
    MaskPayload ocrSnapshot = filter->lastMask;
    MaskPayload blacklistSnapshot;
    {
        std::lock_guard<std::mutex> lock(filter->blacklistMutex);
        blacklistSnapshot = filter->blacklistMask;
    }

    // [Role D] currentState 갱신 — settingsMutex로 보호 (GUI 스레드와 data race 방지)
    SecurityState newState;
    {
        std::lock_guard<std::mutex> lock(filter->settingsMutex);
        if (filter->health.isCritical()) {
            filter->currentState = SecurityState::RISK;
        } else if (ocrSnapshot.rectCount > 0 || blacklistSnapshot.rectCount > 0) {
            filter->currentState = SecurityState::PARTIAL;
        } else {
            filter->currentState = SecurityState::SAFE;
        }
        newState = filter->currentState;
    }
#ifdef _WIN32
    // [Role D] 오버레이 HUD에 상태 동기화 (PostMessage → thread-safe, non-blocking)
    filter->overlay.setState(newState);
#endif

#ifdef _WIN32
    // ---------------------------------------------------------
    // [C-5 수정] Phase 1(Collect+Hash) 도중에 돌려 delayedSlot early-return 앞에 실행.
    // 이로써 첫 N프레임 동안도 GPU 수확 시도가 이루어짐.
    // Phase 2(Enqueue)+3(Submit)은 delayedTex 확보 후에 수행 (아래 참조).
    // ---------------------------------------------------------
    // OCR 해상도 캡(Cap): 최대 1920x1080 [C-4 수정: totalSlots=2 제거]
    int ocrW = ((int)w > 1920) ? 1920 : (int)w;
    int ocrH = ((int)h > 1080) ? 1080 : (int)h;

    size_t expectedBufferSize = (size_t)(64 * 64 * 4) + (size_t)(ocrW * ocrH * 4);
    if (filter->readbackBuffer.size() != expectedBufferSize) {
        filter->fullScreenHash.reset();
        std::vector<std::pair<int, int>> slotSizes = {{64, 64}, {ocrW, ocrH}};
        filter->readback.resizePool(slotSizes);
        filter->readbackBuffer.resize(expectedBufferSize, 0);
    }

    // Phase 1: Collect (이전 프레임 결과 수확 — delayedSlot 없어도 무조건 시도)
    CollectResult sc_collected = filter->readback.tryCollectPreviousFrame();
    if (sc_collected == CollectResult::OK || sc_collected == CollectResult::SOFT_RECOVERED) {
        if (sc_collected == CollectResult::SOFT_RECOVERED) {
            filter->health.onForceRelease(); // [F3 Fix] 자체 소프트 복구 시 onForceRelease()를 호출하여 가짜 CRITICAL 예방
        } else {
            filter->health.onCollectSuccess();
        }
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
                    // [F7 Fix] feedFrame 비활성화 중 무의미한 8.3MB alloc + memcpy + free 방지
                    /*
                    ReadbackResult result;
                    result.ownedData.assign(ocrBuf, ocrBuf + (size_t)(ocrW * ocrH * 4));
                    result.hashData   = result.ownedData.data();
                    result.hashSize   = (size_t)(ocrW * ocrH * 4);
                    result.bbox       = finalBox;
                    result.frameIndex = filter->frameCounter;
                    */
                    // [Role B 연계 포인트]
                    // filter->aiWorker.feedFrame(result);
                    (void)finalBox;
                }
            } else {
                // [C2-3 수정] static → 멤버 변수 사용 (다중 인스턴스 공유 방지)
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
            // [C2-3 수정] static → 멤버 변수 사용
            if (filter->logStallCount++ % 30 == 0)
                blog(LOG_WARNING, "[SecureCast] Pipeline FULL, GPU not responding.");
        }
    }
#endif // _WIN32 Phase1 end

    // --- Step 4~5: N프레임 지연된 슬롯 꺼내기 ---
    const FrameRingBuffer::Slot* delayedSlot = filter->ringBuffer.peekDelayedSlot();

    if (!delayedSlot || !delayedSlot->getTexture()) {
        // [P1 Fix] Fail-Secure 최우선 정책: 버퍼가 아직 충분히 준비되지 않은 첫 N프레임 초기 구간에서 민감한 원본 화면이 노출되지 않도록 전면 블랙 렌더링 적용
        gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
        gs_eparam_t* colorParam = gs_effect_get_param_by_name(solid, "color");
        gs_effect_set_color(colorParam, 0xFF000000); // 100% 불투명 검정색
        while (gs_effect_loop(solid, "Solid")) {
            gs_draw_sprite(nullptr, 0, w, h);
        }
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

    // 1. AI가 찾은 OCR 마스크 그리기 (약간 반투명한 빨간색으로 시각적 구분)
    // [C-10 수정] 0xAAFF0000 (Alpha: AA, Red: FF, Green: 00, Blue: 00)
    renderMask(ocrSnapshot, 0xAAFF0000);

    // 2. 무조건 차단할 블랙리스트 마스크 덮어그리기 (불투명 검정색으로 최우선 차단)
    renderMask(blacklistSnapshot, 0xFF000000);

    // --- [Role D] 보안 상태 테두리 오버레이 ---
    // gs_effect_set_color()는 OBS ARGB 포맷: 상위 바이트부터 A·R·G·B 순서
    //   SAFE    0xFF00FF00 : A=FF R=00 G=FF B=00 → 초록
    //   PARTIAL 0xFFFFFF00 : A=FF R=FF G=FF B=00 → 노랑
    //   RISK    0xFFFF0000 : A=FF R=FF G=00 B=00 → 빨강
    {
        uint32_t borderColor = 0xFF00FF00; // SAFE: 초록
        if (newState == SecurityState::PARTIAL)
            borderColor = 0xFFFFFF00;      // PARTIAL: 노랑
        else if (newState == SecurityState::RISK)
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
    // Phase 2: Enqueue + Phase 3: Submit
    // delayedTex가 확보된 이후에 실행 (값이 유효함을 보장)
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
            // [C-7 수정] submitFrame은 shouldCopy 블록 안으로
            filter->readback.submitFrame();
            // [C2-2 수정] frameCounter도 shouldCopy 블록 안으로 이동 (실제 제출된 프레임만 카운트)
            filter->frameCounter++;
            // [C2-3 수정] static → 멤버 변수 사용
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
        // [C-9 수정 / F2 Fix] 복구 직후 새 마스크 결과가 들어올 때까지 풀스크린 블랙아웃(bo)을 안전하게 유지 (Fail-Secure)
        filter->readback.setForceReleasedFlag(true);
    }
#endif
}

// ---------------------------------------------------------
// Tick (Slow-Path) — 매 프레임 호출되지만 윈도우 추적은 0.15초마다
// ---------------------------------------------------------
//
// OBS 렌더링 파이프라인은 60fps라 video_tick도 60Hz로 들어온다.
// 그러나 EnumWindows는 무거운 호출이라 매 tick 돌리면 CPU 낭비가 크다.
// → trackerAccumulator를 누산하여 임계(0.15초)를 넘을 때만 sc_scan_blacklisted_windows를 실행한다.
//   [C-1 수정] 이 로직은 인라인으로 직접 수행 (sc_tracker_tick 위임 제거).
//
// seconds: 직전 tick과의 경과 시간(초). 60fps면 약 0.0167.
static void securecast_video_tick(void* data, float seconds)
{
    SecureCastFilter* filter = (SecureCastFilter*)data;

    // [Role D] isActive는 GUI 스레드(update)에서도 쓸 수 있으므로 settingsMutex로 보호
    {
        std::lock_guard<std::mutex> lock(filter->settingsMutex);
        if (!filter || !filter->isActive)
            return;
    }

    filter->trackerAccumulator += seconds;
    if (filter->trackerAccumulator >= 0.15f) {
        filter->trackerAccumulator = 0.0f;

#ifdef _WIN32
        TrackedWindowList list{};
        sc_scan_blacklisted_windows(&list);

        // [THREAD-SAFE] blacklistMask는 video_tick(비디오 트레드)와
        // video_render(렌더 트레드) 양쪽에서 접근하므로 뮤텍스로 보호합니다.
        {
            std::lock_guard<std::mutex> lock(filter->blacklistMutex);
            filter->blacklistMask.rectCount = (list.count > SC_MAX_BLUR_RECTS) ? SC_MAX_BLUR_RECTS : list.count;
            for (int i = 0; i < filter->blacklistMask.rectCount; ++i) {
                filter->blacklistMask.rects[i] = {
                    (int)list.items[i].bounds.left,
                    (int)list.items[i].bounds.top,
                    (int)(list.items[i].bounds.right - list.items[i].bounds.left),
                    (int)(list.items[i].bounds.bottom - list.items[i].bounds.top),
                    0 // 0: Blur
                };
            }

            if (list.count > 0) {
                if (filter->logScanThrottle++ % 10 == 0) { // 0.15초 * 10 = 1.5초 주기 로그
                    blog(LOG_INFO, "[SecureCast] %d blacklisted windows tracked in video_tick. Priority blackout applied.", list.count);
                }
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
