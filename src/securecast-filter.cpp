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

// NOMINMAX must be defined before any Windows header to prevent min/max macro conflicts
// with std::max / std::min from <algorithm>.
#define NOMINMAX
#include "securecast-filter.h"
#include "plugin-support.h"   // obs_log
#include "window_tracker.h"   // sc_tracker_tick (Role A: 블랙리스트 앱 좌표 수집)
#include <algorithm>
#include <chrono>
#include <stdlib.h>
#include <string.h>

// window_tracker.cpp의 SCAN_INTERVAL_SEC(0.15f) 이상이면 즉시 스캔 트리거.
static constexpr float SCAN_INTERVAL_FORCE = 1.0f;

// ================================================================
// [Role A] 윈도우 좌표 → OBS 소스 픽셀 좌표 변환 + 15% BBox 팽창
//
// TrackedWindow.bounds : DWM 화면 절대좌표 (물리 픽셀)
// src_w / src_h        : OBS 소스 해상도 (= 캡처 모니터 해상도와 같다고 가정)
//
// 변환 순서:
//   1. MonitorFromWindow → 창이 속한 모니터의 원점(rcMonitor.left/top) 파악
//   2. 창 좌표 - 모니터 원점 → 모니터 상대 좌표
//   3. (모니터 상대 좌표 / 모니터 크기) * 소스 크기 → 소스 픽셀 좌표
//   4. 15% BBox 팽창 후 소스 경계로 clamp
// ================================================================
#ifdef _WIN32
static BlurRect tracked_window_to_blur_rect(const TrackedWindow &tw,
                                             uint32_t src_w, uint32_t src_h)
{
    BlurRect r{};
    // MonitorFromRect을 사용해야 lingering window(이미 닫힌 hwnd)에도 동작한다.
    HMONITOR hmon = MonitorFromRect(&tw.bounds, MONITOR_DEFAULTTONEAREST);
    if (!hmon)
        return r;

    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfo(hmon, &mi))
        return r;

    int mon_w = mi.rcMonitor.right  - mi.rcMonitor.left;
    int mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (mon_w <= 0 || mon_h <= 0)
        return r;

    float sx = (float)src_w / mon_w;
    float sy = (float)src_h / mon_h;

    int x  = (int)((tw.bounds.left - mi.rcMonitor.left) * sx);
    int y  = (int)((tw.bounds.top  - mi.rcMonitor.top ) * sy);
    int bw = (int)((tw.bounds.right  - tw.bounds.left) * sx);
    int bh = (int)((tw.bounds.bottom - tw.bounds.top ) * sy);

    // 비대칭 BBox 팽창 — 위쪽(타이틀바 위)은 최소, 좌/우/아래는 빠른 이동 여유 포함.
    // 위: 1%  / 좌우: 5% (이동 시 trailing edge 커버) / 아래: 3%
    int exp_top    = (int)(bh * 0.01f);
    int exp_sides  = (int)(bw * 0.025f);
    int exp_bottom = (int)(bh * 0.015f);
    x  = std::max(0,          x  - exp_sides);
    y  = std::max(0,          y  - exp_top);
    bw = std::min((int)src_w - x, bw + exp_sides * 2);
    bh = std::min((int)src_h - y, bh + exp_top + exp_bottom);

    r = {x, y, bw, bh, 0}; // type 0 = Blur (Blackout과 시각적으로 구분 가능)
    return r;
}
#endif

// ================================================================
// [Role A] blur.effect 셰이더로 BlurRect 1개를 렌더링
//
// type == 0 (Blur)    : image 텍스처를 box_offset/size 기준으로 5x5 평균
// type == 1 (Blackout): 단색 검정
// ================================================================
static void render_blur_rect(gs_effect_t *fx, gs_texture_t *img_tex,
                              const BlurRect &r, uint32_t src_w, uint32_t src_h)
{
    if (r.width <= 0 || r.height <= 0)
        return;

    const char *tech = (r.type == 0) ? "Blur" : "Blackout";

    if (r.type == 0) {
        struct vec2 box_off = {(float)r.x, (float)r.y};
        struct vec2 box_sz  = {(float)r.width, (float)r.height};
        struct vec2 img_sz  = {(float)src_w, (float)src_h};
        gs_effect_set_texture(gs_effect_get_param_by_name(fx, "image"),      img_tex);
        gs_effect_set_vec2(   gs_effect_get_param_by_name(fx, "box_offset"), &box_off);
        gs_effect_set_vec2(   gs_effect_get_param_by_name(fx, "box_size"),   &box_sz);
        gs_effect_set_vec2(   gs_effect_get_param_by_name(fx, "image_size"), &img_sz);
        gs_effect_set_float(  gs_effect_get_param_by_name(fx, "blur_radius"), 8.0f);
    }

    gs_matrix_push();
    gs_matrix_identity();
    gs_matrix_translate3f((float)r.x, (float)r.y, 0.0f);
    while (gs_effect_loop(fx, tech))
        gs_draw_sprite(nullptr, 0, (uint32_t)r.width, (uint32_t)r.height);
    gs_matrix_pop();
}

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

#ifdef _WIN32
void FrameRingBuffer::pushFrame(obs_source_t* source, uint64_t timestamp,
                                 const TrackedWindowList* wlist)
#else
void FrameRingBuffer::pushFrame(obs_source_t* source, uint64_t timestamp)
#endif
{
    if (!m_initialized)
        return;

#ifdef _WIN32
    if (wlist)
        m_slots[m_head].windowSnapshot = *wlist;
    else
        m_slots[m_head].windowSnapshot = TrackedWindowList{};
#endif

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
    return peekSlotAtOffset(SC_RING_BUFFER_SLOTS);
}

const FrameRingBuffer::Slot* FrameRingBuffer::peekSlotAtOffset(int framesBack) const
{
    if (framesBack <= 0 || m_frameCount < framesBack)
        return nullptr;
    int idx = ((m_head - framesBack) % SC_RING_BUFFER_SLOTS + SC_RING_BUFFER_SLOTS)
              % SC_RING_BUFFER_SLOTS;
    return &m_slots[idx];
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

        // Role B(실제 OCR/AI) 미구현 — 빈 페이로드 전달 (중앙 박스 제거)
        MaskPayload payload{};
        payload.rectCount = 0;

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

#ifdef _WIN32
    filter->winListener.start();
#endif

    // HLSL 셰이더 컴파일 (그래픽스 컨텍스트 필요)
    obs_enter_graphics();
    char *effect_path = obs_module_file("securecast_blur.effect");
    if (effect_path) {
        filter->blurEffect = gs_effect_create_from_file(effect_path, nullptr);
        bfree(effect_path);
    }
    obs_leave_graphics();
    if (!filter->blurEffect)
        blog(LOG_WARNING, "[SecureCast] blur effect load failed; falling back to solid blackout.");
    else
        blog(LOG_INFO, "[SecureCast] blur effect loaded.");

    blog(LOG_INFO, "Filter created (Role C: Mock Pipeline Active).");
    return filter;
}

// 필터 인스턴스 정리. OBS는 이 시점 이후 동일 data 포인터를 다시 안 넘긴다.
static void securecast_destroy(void* data)
{
    SecureCastFilter* filter = static_cast<SecureCastFilter*>(data);

    blog(LOG_INFO, "Destroying filter...");

#ifdef _WIN32
    filter->winListener.stop();
#endif

    // AI 워커 먼저 중지 (콜백이 ring buffer에 접근하지 않도록)
    filter->mockWorker.stop();

    // GPU 리소스 해제
    obs_enter_graphics();
    if (filter->blurEffect) {
        gs_effect_destroy(filter->blurEffect);
        filter->blurEffect = nullptr;
    }
    obs_leave_graphics();

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
    // OBS 소스 내부 버퍼링으로 obs_source_video_render()가 반환하는 픽셀은
    // 실제 DWM 쿼리보다 ~1프레임 뒤처진다.
    // captureWindowList(직전 프레임에서 저장한 DWM 좌표)를 스냅샷으로 쓰면
    // 픽셀 내용과 마스크 위치가 정확히 동기화된다.
    uint64_t ts = obs_get_video_frame_time();
#ifdef _WIN32
    filter->ringBuffer.pushFrame(parent, ts, &filter->captureWindowList);
    // push 이후에 DWM 갱신 → 다음 프레임의 captureWindowList로 저장
    sc_update_tracked_bounds(&filter->windowList);
    filter->captureWindowList = filter->windowList;
#else
    filter->ringBuffer.pushFrame(parent, ts);
#endif

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

    // --- 마스킹 오버레이 ---
    // Role A: delayedSlot->windowSnapshot (프레임 캡처 시점의 창 위치 → 프레임과 동기화됨)
    // Role B/C: lastMask (AI/MockAI 결과)
    BlurRect all_rects[SC_MAX_BLUR_RECTS + SC_MAX_TRACKED_WINDOWS];
    int all_count = 0;

#ifdef _WIN32
    // N프레임 전 스냅샷 (현재 렌더링 중인 지연 프레임과 동기화)
    for (int i = 0; i < delayedSlot->windowSnapshot.count &&
                    all_count < (int)(sizeof(all_rects) / sizeof(all_rects[0])); i++) {
        BlurRect r = tracked_window_to_blur_rect(delayedSlot->windowSnapshot.items[i], w, h);
        if (r.width > 0 && r.height > 0)
            all_rects[all_count++] = r;
    }
    // N-1프레임 전 스냅샷 합집합: 한 프레임 이동 궤적 전체를 마스킹 (빠른 드래그 노출 방지)
    {
        const FrameRingBuffer::Slot* slotN1 =
            filter->ringBuffer.peekSlotAtOffset(SC_RING_BUFFER_SLOTS - 1);
        if (slotN1) {
            for (int i = 0; i < slotN1->windowSnapshot.count &&
                            all_count < (int)(sizeof(all_rects) / sizeof(all_rects[0])); i++) {
                BlurRect r = tracked_window_to_blur_rect(slotN1->windowSnapshot.items[i], w, h);
                if (r.width > 0 && r.height > 0)
                    all_rects[all_count++] = r;
            }
        }
    }
    // Lingering rects: 사라진 창의 N프레임 잔영 (ring buffer에 남은 과거 프레임 커버)
    for (int li = 0; li < filter->lingeringCount &&
                     all_count < (int)(sizeof(all_rects) / sizeof(all_rects[0])); ++li) {
        BlurRect r = tracked_window_to_blur_rect(filter->lingeringWindows[li].window, w, h);
        if (r.width > 0 && r.height > 0)
            all_rects[all_count++] = r;
    }
#endif
    for (int i = 0; i < filter->lastMask.rectCount &&
                    all_count < (int)(sizeof(all_rects) / sizeof(all_rects[0])); i++)
        all_rects[all_count++] = filter->lastMask.rects[i];

    if (all_count == 0)
        return;

    if (filter->blurEffect) {
        // blur.effect 셰이더 사용 (Blur / Blackout technique)
        for (int i = 0; i < all_count; i++)
            render_blur_rect(filter->blurEffect, delayedTex, all_rects[i], w, h);
    } else {
        // 셰이더 로드 실패 시 fallback: 단색 검정 박스
        gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
        gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xFF000000);
        gs_matrix_push();
        while (gs_effect_loop(solid, "Solid")) {
            for (int i = 0; i < all_count; i++) {
                gs_matrix_identity();
                gs_matrix_translate3f((float)all_rects[i].x, (float)all_rects[i].y, 0.0f);
                gs_draw_sprite(nullptr, 0, (uint32_t)all_rects[i].width,
                               (uint32_t)all_rects[i].height);
            }
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

#ifdef _WIN32
    if (filter->winListener.checkAndClearRescan()) {
        filter->trackerAccumulator = SCAN_INTERVAL_FORCE;

        // Quick restore: foreground 전환 이벤트 직후 recentlySeenList 조회 → 즉시 복원.
        // EnumWindows 스캔(느림) 전에 captureWindowList를 채워
        // 이번 render의 pushFrame 시점부터 마스킹이 적용되도록 한다.
        HWND fgHwnd = GetForegroundWindow();
        if (fgHwnd) {
            for (int ri = 0; ri < filter->recentlySeenList.count; ++ri) {
                if (filter->recentlySeenList.items[ri].hwnd != fgHwnd)
                    continue;
                bool alreadyTracked = false;
                for (int wi = 0; wi < filter->windowList.count; ++wi) {
                    if (filter->windowList.items[wi].hwnd == fgHwnd) {
                        alreadyTracked = true;
                        break;
                    }
                }
                if (!alreadyTracked && filter->windowList.count < SC_MAX_TRACKED_WINDOWS) {
                    const TrackedWindow& tw = filter->recentlySeenList.items[ri];
                    filter->windowList.items[filter->windowList.count++] = tw;
                    // captureWindowList에도 즉시 반영: 이번 render pushFrame 스냅샷에 포함
                    if (filter->captureWindowList.count < SC_MAX_TRACKED_WINDOWS)
                        filter->captureWindowList.items[filter->captureWindowList.count++] = tw;
                    // Quick restore 성공: 즉시 강제 스캔을 하지 않는다.
                    // EVENT 직후 EnumWindows의 z-order 체크(is_window_top_at_center)는
                    // 창이 방금 포그라운드가 되어 z-order가 아직 안 정착한 상태라
                    // 일시적으로 실패할 수 있다. 실패하면 scan 결과가 {}가 되어
                    // windowList와 captureWindowList를 덮어쓰고 방금 복원한 항목이 사라진다.
                    // 대신 다음 render의 sc_update_tracked_bounds가 안정적으로 확인하게 맡긴다.
                    filter->trackerAccumulator = 0.0f;
                }
                break;
            }
        }
    }
    sc_tracker_tick(seconds, &filter->trackerAccumulator, &filter->windowList);

    // recentlySeenList 유지: windowList 항목을 upsert, 완전히 닫힌 HWND 제거.
    // recentlySeenList는 앱이 다시 등장했을 때 quick restore의 소스가 된다.
    for (int wi = 0; wi < filter->windowList.count; ++wi) {
        HWND wh = filter->windowList.items[wi].hwnd;
        bool found = false;
        for (int ri = 0; ri < filter->recentlySeenList.count; ++ri) {
            if (filter->recentlySeenList.items[ri].hwnd == wh) {
                filter->recentlySeenList.items[ri] = filter->windowList.items[wi];
                found = true;
                break;
            }
        }
        if (!found && filter->recentlySeenList.count < SC_MAX_TRACKED_WINDOWS)
            filter->recentlySeenList.items[filter->recentlySeenList.count++] = filter->windowList.items[wi];
    }
    // 완전히 닫힌 프로세스의 HWND 정리 (최소화/숨김은 IsWindow=true라 유지됨)
    for (int ri = filter->recentlySeenList.count - 1; ri >= 0; --ri) {
        if (!IsWindow(filter->recentlySeenList.items[ri].hwnd))
            filter->recentlySeenList.items[ri] = filter->recentlySeenList.items[--filter->recentlySeenList.count];
    }

    // Lingering: 직전 스캔에 있었지만 이번엔 사라진 창 감지.
    // windowList는 slow-scan 주기(150ms)마다만 바뀌므로 비교는 실질적으로 그때만 의미 있다.
    for (int pi = 0; pi < filter->prevWindowList.count; ++pi) {
        HWND ph = filter->prevWindowList.items[pi].hwnd;
        bool found = false;
        for (int ci = 0; ci < filter->windowList.count && !found; ++ci)
            found = (filter->windowList.items[ci].hwnd == ph);
        if (!found) {
            bool already = false;
            for (int li = 0; li < filter->lingeringCount; ++li) {
                if (filter->lingeringWindows[li].window.hwnd == ph) {
                    filter->lingeringWindows[li].ticksRemaining = SC_RING_BUFFER_SLOTS;
                    already = true;
                    break;
                }
            }
            if (!already && filter->lingeringCount < SC_MAX_LINGERING)
                filter->lingeringWindows[filter->lingeringCount++] = {
                    filter->prevWindowList.items[pi], SC_RING_BUFFER_SLOTS
                };
        }
    }

    // New window detection: 이번 스캔에서 새로 등장한 창을 즉시 lingering에 prime.
    // 탐지 전에 ring buffer에 이미 쌓인 프레임(최대 SC_RING_BUFFER_SLOTS개)이
    // 출력될 때도 마스킹되도록 SC_RING_BUFFER_SLOTS+1 틱을 부여한다.
    // (+1: 탐지 틱의 render에서 captureWindowList가 업데이트되기 전에
    //  pushFrame이 먼저 실행되어 생기는 1프레임 갭을 커버)
    for (int ci = 0; ci < filter->windowList.count; ++ci) {
        HWND ch = filter->windowList.items[ci].hwnd;
        bool wasPrev = false;
        for (int pi = 0; pi < filter->prevWindowList.count; ++pi) {
            if (filter->prevWindowList.items[pi].hwnd == ch) { wasPrev = true; break; }
        }
        if (!wasPrev) {
            bool already = false;
            for (int li = 0; li < filter->lingeringCount; ++li) {
                if (filter->lingeringWindows[li].window.hwnd == ch) {
                    filter->lingeringWindows[li].ticksRemaining = SC_RING_BUFFER_SLOTS + 1;
                    already = true; break;
                }
            }
            if (!already && filter->lingeringCount < SC_MAX_LINGERING)
                filter->lingeringWindows[filter->lingeringCount++] = {
                    filter->windowList.items[ci], SC_RING_BUFFER_SLOTS + 1
                };
        }
    }

    filter->prevWindowList = filter->windowList;

    // 매 tick 카운트다운 → 정확히 N프레임(ring buffer 지연) 후 자연 제거
    for (int li = filter->lingeringCount - 1; li >= 0; --li) {
        if (--filter->lingeringWindows[li].ticksRemaining <= 0)
            filter->lingeringWindows[li] = filter->lingeringWindows[--filter->lingeringCount];
    }
#endif
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
    return info;
}();
