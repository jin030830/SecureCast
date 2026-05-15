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
#ifdef _WIN32
#include "window_tracker.h"   // sc_tracker_tick (Role A: 블랙리스트 앱 좌표 수집)
#include <obs-frontend-api.h> // obs_hotkey_register_frontend
#endif
#include "ocr-engine.h"       // Role B: OCR engine

#include <algorithm>
#include <chrono>
#include <stdlib.h>
#include <string.h>

// ================================================================
// SecureCastFilter 구현부
// ================================================================

SecureCastFilter::SecureCastFilter()
    : ocrEngine(std::make_unique<SecureCastOcrEngine>())
{
}

SecureCastFilter::~SecureCastFilter() = default;

// trackerAccumulator에 이 값을 대입하면 다음 sc_tracker_tick 호출 시 임계를 즉시 초과 → 강제 스캔 트리거.

static constexpr float SCAN_INTERVAL_FORCE    = 1.0f;
static constexpr float SCAN_INTERVAL_NORMAL   = 0.15f; // 일반 모드 스캔 주기
static constexpr float SCAN_INTERVAL_GAME     = 0.5f;  // 게임 모드 스캔 주기

// Game mode thresholds
static constexpr float GM_CPU_ENTER     = 40.0f; // 진입 임계값 (%)
static constexpr float GM_CPU_EXIT      = 30.0f; // 해제 임계값 (%)
static constexpr float GM_ENTER_TIME    = 3.0f;  // 진입까지 ≥40% 유지 시간 (초)
static constexpr float GM_EXIT_TIME     = 5.0f;  // 해제까지 ≤30%  유지 시간 (초)
static constexpr float GM_SAMPLE_INTERVAL = 1.0f; // CPU 샘플링 주기 (초)

// ================================================================
// [Game Mode] GetSystemTimes 기반 시스템 전체 CPU 사용률 샘플링 (WIN32)
//
// GetSystemTimes: kernel 시간에 idle이 포함되므로
//   CPU % = (kernel + user - idle) / (kernel + user) * 100
// 멀티코어 기준 전체 평균값. 오버헤드 거의 0 (단순 syscall).
// ================================================================
#ifdef _WIN32
static float sampleCpuUsage(FILETIME* prevIdle, FILETIME* prevKernel, FILETIME* prevUser)
{
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user))
        return 0.0f;

    auto ft2u64 = [](FILETIME ft) -> uint64_t {
        return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };

    uint64_t idleDiff   = ft2u64(idle)   - ft2u64(*prevIdle);
    uint64_t kernelDiff = ft2u64(kernel) - ft2u64(*prevKernel);
    uint64_t userDiff   = ft2u64(user)   - ft2u64(*prevUser);

    *prevIdle   = idle;
    *prevKernel = kernel;
    *prevUser   = user;

    uint64_t total = kernelDiff + userDiff;
    if (total == 0 || idleDiff > total)
        return 0.0f;

    return std::clamp((float)(total - idleDiff) / (float)total * 100.0f, 0.0f, 100.0f);
}
#endif

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

// GPU에 gs_texrender 슬롯을 할당한다. video_render 첫 호출 또는 해상도 변경 시 호출.
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

// 모든 슬롯의 GPU 텍스처를 해제하고 버퍼를 초기 상태로 되돌린다. 소멸자 또는 해상도 변경 시 호출.
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

// 현재 OBS 소스 프레임을 HEAD 슬롯에 캡처하고, 창 좌표 스냅샷을 함께 저장한다.
// HEAD를 한 칸 전진시켜 다음 pushFrame이 다음 슬롯에 쓰도록 한다.
#ifdef _WIN32
void FrameRingBuffer::pushFrame(uint64_t timestamp, obs_source_t* filter_context,
                                 const TrackedWindowList* wlist)
#else
void FrameRingBuffer::pushFrame(uint64_t timestamp, obs_source_t* filter_context)
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

// N프레임(SC_RING_BUFFER_SLOTS) 전 슬롯을 반환한다. 인코더로 출력하는 "안전한" 지연 프레임.
const FrameRingBuffer::Slot* FrameRingBuffer::peekDelayedSlot() const
{
    return peekSlotAtOffset(SC_RING_BUFFER_SLOTS);
}

// HEAD에서 framesBack만큼 이전 슬롯을 반환한다. N-1 슬롯과의 합집합 마스킹(빠른 이동 커버)에 사용.
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

// 워커 스레드를 시작한다. AI 분석이 완료될 때마다 callback으로 마스킹 결과를 전달.
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

// 워커 스레드에 종료 신호를 보내고 join한다. securecast_destroy() 에서 ring buffer 해제 전에 반드시 호출.
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

// 게임 모드 진입/해제 시 호출. paused=true면 스레드는 cv.wait에서 대기해 CPU를 점유하지 않는다.
void MockAIWorker::setPaused(bool paused)
{
    if (!m_running.load())
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_paused.store(paused, std::memory_order_relaxed);
    }
    m_cv.notify_all();
    blog(LOG_INFO, "MockAIWorker %s.", paused ? "paused" : "resumed");
}

// 워커 스레드 본체. 50ms 주기로 빈 페이로드를 publish한다.
// Role B 구현 시 여기서 OCR 호출 후 실제 BlurRect를 채워 publish하도록 교체.
void MockAIWorker::workerLoop()
{
    while (m_running.load()) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_paused.load(std::memory_order_relaxed)) {
                // 게임 모드 일시정지: stop() 또는 resume 신호까지 대기
                m_cv.wait(lock, [this]() {
                    return !m_running.load() ||
                           !m_paused.load(std::memory_order_relaxed);
                });
            } else {
                // AI 처리 시간 시뮬레이션 (실제 OCR 처리 예상 지연: ~40~60ms)
                m_cv.wait_for(lock, std::chrono::milliseconds(50), [this]() {
                    return !m_running.load() ||
                            m_paused.load(std::memory_order_relaxed);
                });
            }
        }

        if (!m_running.load() || m_paused.load(std::memory_order_relaxed))
            continue;

        // [F4 Fix] 프로덕션에서 가짜 마스킹 박스가 영구 출력되는 데모 회귀 방지를 위해 디버그용 MOCK 가드를 둡니다.
#ifdef SC_DEBUG_MOCK
        MaskPayload payload{};
        payload.rectCount = 0;

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

// AI 스레드 → Render 스레드 결과 전달. 뮤텍스로 m_pending 기록 보호 후 ready 플래그를 set.
void AtomicMaskChannel::publish(const MaskPayload& payload)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending = payload;
    m_ready.store(true, std::memory_order_release);
}

// Render 스레드가 새 마스킹 결과를 꺼낸다. 새 데이터가 없으면 false 반환 (lastMask 유지).
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
// [Role B/C] GPU texture -> CPU BGRA pixels 재사용 readback
// ================================================================

static void destroy_ocr_stage_surface(SecureCastFilter* filter)
{
    if (!filter || !filter->ocrStageSurface)
        return;

    gs_stagesurface_destroy(filter->ocrStageSurface);
    filter->ocrStageSurface = nullptr;
    filter->ocrStageWidth = 0;
    filter->ocrStageHeight = 0;
}

static bool ensure_ocr_stage_surface(SecureCastFilter* filter, uint32_t width, uint32_t height)
{
    if (!filter || width == 0 || height == 0)
        return false;

    if (filter->ocrStageSurface &&
        filter->ocrStageWidth == width &&
        filter->ocrStageHeight == height) {
        return true;
    }

    destroy_ocr_stage_surface(filter);

    filter->ocrStageSurface = gs_stagesurface_create(width, height, GS_BGRA);
    if (!filter->ocrStageSurface) {
        blog(LOG_ERROR, "[securecast][ocr] Failed to create staging surface.");
        return false;
    }

    filter->ocrStageWidth = width;
    filter->ocrStageHeight = height;
    return true;
}

static bool read_texture_bgra_to_cpu(
    SecureCastFilter* filter,
    gs_texture_t* texture,
    uint32_t width,
    uint32_t height,
    std::vector<uint8_t>& outPixels,
    int& outStride)
{
    if (!filter || !texture || width == 0 || height == 0)
        return false;

    if (!ensure_ocr_stage_surface(filter, width, height))
        return false;

    gs_stagesurf_t* stage = filter->ocrStageSurface;
    gs_stage_texture(stage, texture);

    uint8_t* mappedData = nullptr;
    uint32_t mappedStride = 0;

    if (!gs_stagesurface_map(stage, &mappedData, &mappedStride)) {
        blog(LOG_WARNING, "[securecast][ocr] Failed to map staging surface.");
        return false;
    }

    const uint32_t tightStride = width * 4;
    outPixels.resize((size_t)tightStride * height);

    for (uint32_t y = 0; y < height; y++) {
        memcpy(outPixels.data() + (size_t)y * tightStride,
               mappedData + (size_t)y * mappedStride,
               tightStride);
    }

    gs_stagesurface_unmap(stage);

    outStride = (int)tightStride;
    return true;
}

// ================================================================
// [Role B] OCR worker 보조 함수
// ================================================================

static MaskPayload build_mask_payload_from_ocr_boxes(
    const std::vector<SecureCastOcrBox>& boxes,
    int frameWidth,
    int frameHeight)
{
    MaskPayload payload{};
    payload.rectCount = 0;

    const int maxRects = (int)(sizeof(payload.rects) / sizeof(payload.rects[0]));

    for (const auto& box : boxes) {
        if (payload.rectCount >= maxRects)
            break;

        int x      = static_cast<int>(box.x);
        int y      = static_cast<int>(box.y);
        int width  = static_cast<int>(box.w);
        int height = static_cast<int>(box.h);

        if (x < 0) x = 0;
        if (y < 0) y = 0;

        if (x >= frameWidth || y >= frameHeight)
            continue;

        if (x + width > frameWidth)
            width = frameWidth - x;

        if (y + height > frameHeight)
            height = frameHeight - y;

        if (width <= 0 || height <= 0)
            continue;

        BlurRect& r = payload.rects[payload.rectCount++];
        r.x      = x;
        r.y      = y;
        r.width  = width;
        r.height = height;
        r.type   = 1;
    }

    return payload;
}

static void clear_pending_ocr_frame(SecureCastFilter* filter)
{
    if (!filter)
        return;

    std::lock_guard<std::mutex> lock(filter->ocrWorkerMutex);
    filter->ocrFramePending = false;
    filter->ocrPendingPixels.clear();
    filter->ocrPendingWidth = 0;
    filter->ocrPendingHeight = 0;
    filter->ocrPendingStride = 0;
}

static void submit_ocr_frame(
    SecureCastFilter* filter,
    std::vector<uint8_t>&& pixels,
    int width,
    int height,
    int stride)
{
    if (!filter || pixels.empty() || width <= 0 || height <= 0 || stride <= 0)
        return;

    if (!filter->ocrWorkerRunning.load(std::memory_order_acquire))
        return;

    {
        std::lock_guard<std::mutex> lock(filter->ocrWorkerMutex);
        filter->ocrPendingPixels = std::move(pixels);
        filter->ocrPendingWidth = width;
        filter->ocrPendingHeight = height;
        filter->ocrPendingStride = stride;
        filter->ocrFramePending = true;
    }

    filter->ocrWorkerCv.notify_one();
}

static void ocr_worker_loop(SecureCastFilter* filter)
{
    if (!filter)
        return;

    const bool ocrReady = filter->ocrEngine ? filter->ocrEngine->init() : false;
    if (ocrReady) {
        blog(LOG_INFO, "[securecast][ocr] OCR worker initialized.");
    } else {
        blog(LOG_WARNING, "[securecast][ocr] OCR worker initialization failed.");
    }

    while (true) {
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
        int stride = 0;

        {
            std::unique_lock<std::mutex> lock(filter->ocrWorkerMutex);
            filter->ocrWorkerCv.wait(lock, [filter]() {
                return !filter->ocrWorkerRunning.load(std::memory_order_acquire) ||
                       filter->ocrFramePending;
            });

            if (!filter->ocrWorkerRunning.load(std::memory_order_acquire) &&
                !filter->ocrFramePending) {
                break;
            }

            pixels.swap(filter->ocrPendingPixels);
            width = filter->ocrPendingWidth;
            height = filter->ocrPendingHeight;
            stride = filter->ocrPendingStride;

            filter->ocrPendingWidth = 0;
            filter->ocrPendingHeight = 0;
            filter->ocrPendingStride = 0;
            filter->ocrFramePending = false;
        }

        if (!ocrReady || pixels.empty() || width <= 0 || height <= 0 || stride <= 0)
            continue;

        auto ocrBoxes = filter->ocrEngine->analyze_bgra_frame(
            pixels.data(), width, height, stride);

        MaskPayload payload = build_mask_payload_from_ocr_boxes(ocrBoxes, width, height);

        filter->maskChannel.publish(payload);

        blog(LOG_INFO, "[securecast][ocr] OCR boxes: %d", payload.rectCount);
    }

    blog(LOG_INFO, "[securecast][ocr] OCR worker stopped.");
}

static void start_ocr_worker(SecureCastFilter* filter)
{
    if (!filter)
        return;

    bool expected = false;
    if (!filter->ocrWorkerRunning.compare_exchange_strong(expected, true,
                                                          std::memory_order_acq_rel)) {
        return;
    }

    filter->ocrWorkerThread = std::thread([filter]() {
        ocr_worker_loop(filter);
    });
}

static void stop_ocr_worker(SecureCastFilter* filter)
{
    if (!filter)
        return;

    bool wasRunning = filter->ocrWorkerRunning.exchange(false, std::memory_order_acq_rel);
    if (!wasRunning)
        return;

    filter->ocrWorkerCv.notify_all();

    if (filter->ocrWorkerThread.joinable())
        filter->ocrWorkerThread.join();

    clear_pending_ocr_frame(filter);
}

// ================================================================
// Filter Lifecycle Callbacks
// ================================================================

// Panic 핫키 콜백 — 누를 때(pressed=true)만 토글. 해제 이벤트는 무시.
static void panic_hotkey_cb(void* data, obs_hotkey_id, obs_hotkey_t*, bool pressed)
{
    if (!pressed)
        return;
    auto* filter = static_cast<SecureCastFilter*>(data);
    if (filter->isDestroying.load(std::memory_order_acquire))
        return;
    bool next = !filter->panicMode.load(std::memory_order_relaxed);
    filter->panicMode.store(next, std::memory_order_relaxed);
    blog(LOG_INFO, "[SecureCast] Panic mode %s.", next ? "ON" : "OFF");
}

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

#ifdef _WIN32
    filter->winListener.start();
    // CPU 샘플링 기준점 초기화 (첫 샘플에서 diff가 0이 되지 않도록)
    GetSystemTimes(&filter->prevIdleTime, &filter->prevKernelTime, &filter->prevUserTime);
#endif

    // Panic 핫키 등록 (Ctrl+Shift+F12 기본 바인딩)
    filter->panicHotkeyId = obs_hotkey_register_frontend(
        "securecast_panic_toggle",
        obs_module_text("PanicButton"),
        panic_hotkey_cb, filter);
    if (filter->panicHotkeyId != OBS_INVALID_HOTKEY_ID) {
        obs_data_t*       combo = obs_data_create();
        obs_data_array_t* arr   = obs_data_array_create();
        obs_data_set_bool(combo,   "control", true);
        obs_data_set_bool(combo,   "shift",   true);
        obs_data_set_bool(combo,   "alt",     false);
        obs_data_set_string(combo, "key",     "OBS_KEY_F12");
        obs_data_array_push_back(arr, combo);
        obs_hotkey_load(filter->panicHotkeyId, arr);
        obs_data_array_release(arr);
        obs_data_release(combo);
        blog(LOG_INFO, "[SecureCast] Panic hotkey registered (Ctrl+Shift+F12).");
    }

#ifdef _WIN32
    // [Role D] 드래그 블러 선택 핫키 등록 (Ctrl+Shift+B)
    filter->selectHotkeyId = obs_hotkey_register_frontend(
        "securecast_select_blur",
        obs_module_text("SelectBlurRegion"),
        [](void* data, obs_hotkey_id, obs_hotkey_t*, bool pressed) {
            if (!pressed) return;
            auto* f = static_cast<SecureCastFilter*>(data);
            if (f->isDestroying.load(std::memory_order_acquire)) return;
            if (f->selectionOverlay.isActive()) {
                f->selectionOverlay.cancel(); // 두 번 누르면 취소
                return;
            }
            f->selectionOverlay.start([f](BlurRect rect) {
                // 모니터 픽셀 좌표 → 소스 픽셀 좌표 변환
                HMONITOR hmon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
                MONITORINFO mi{}; mi.cbSize = sizeof(mi);
                GetMonitorInfo(hmon, &mi);
                int monW = mi.rcMonitor.right  - mi.rcMonitor.left;
                int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
                int monL = mi.rcMonitor.left;
                int monT = mi.rcMonitor.top;
                uint32_t srcW = f->lastSourceW.load(std::memory_order_acquire);
                uint32_t srcH = f->lastSourceH.load(std::memory_order_acquire);
                if (monW > 0 && monH > 0 && srcW > 0 && srcH > 0) {
                    rect.x      = (int)((float)(rect.x - monL)    / monW * srcW);
                    rect.y      = (int)((float)(rect.y - monT)    / monH * srcH);
                    rect.width  = (int)((float)rect.width          / monW * srcW);
                    rect.height = (int)((float)rect.height         / monH * srcH);
                    std::lock_guard<std::mutex> lock(f->settingsMutex);
                    // 새 드래그 시 기존 수동 블러 교체 (누적 아님)
                    f->manualRectCount = 0;
                    f->manualRects[f->manualRectCount++] = rect;
                    blog(LOG_INFO, "[SecureCast][D] Manual rect replaced. scaled=(%d,%d %dx%d)",
                         rect.x, rect.y, rect.width, rect.height);
                } else {
                    blog(LOG_WARNING, "[SecureCast][D] Manual rect skipped: source dimensions unavailable (srcW=%u, srcH=%u)",
                         srcW, srcH);
                }
            });
        }, filter);
    if (filter->selectHotkeyId != OBS_INVALID_HOTKEY_ID) {
        obs_data_t*       combo = obs_data_create();
        obs_data_array_t* arr   = obs_data_array_create();
        obs_data_set_bool(combo,   "control", true);
        obs_data_set_bool(combo,   "shift",   true);
        obs_data_set_bool(combo,   "alt",     false);
        obs_data_set_string(combo, "key",     "OBS_KEY_B");
        obs_data_array_push_back(arr, combo);
        obs_hotkey_load(filter->selectHotkeyId, arr);
        obs_data_array_release(arr);
        obs_data_release(combo);
        blog(LOG_INFO, "[SecureCast] Select hotkey registered (Ctrl+Shift+B).");
    }
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

    return filter;
}

// 필터 인스턴스 정리. OBS는 이 시점 이후 동일 data 포인터를 다시 안 넘긴다.
static void securecast_destroy(void* data)
{
    SecureCastFilter* filter = static_cast<SecureCastFilter*>(data);

    blog(LOG_INFO, "Destroying filter...");

    // 진행 중인 핫키 콜백이 filter 멤버에 접근하지 못하도록 즉시 플래그 설정
    filter->isDestroying.store(true, std::memory_order_release);

    // 핫키 먼저 해제 — 콜백이 해제된 filter에 접근하지 못하도록
    if (filter->panicHotkeyId != OBS_INVALID_HOTKEY_ID) {
        obs_hotkey_unregister(filter->panicHotkeyId);
        filter->panicHotkeyId = OBS_INVALID_HOTKEY_ID;
    }
#ifdef _WIN32
    if (filter->selectHotkeyId != OBS_INVALID_HOTKEY_ID) {
        obs_hotkey_unregister(filter->selectHotkeyId);
        filter->selectHotkeyId = OBS_INVALID_HOTKEY_ID;
    }
    filter->selectionOverlay.cancel();
    filter->selectionOverlay.wait_and_join();
#endif

#ifdef _WIN32
    // [Role D] 오버레이 HUD 먼저 종료 (메시지 루프 스레드 join)
    filter->overlay.destroy();
    filter->winListener.stop();
#endif

    // AI 워커 먼저 중지 (콜백이 ring buffer에 접근하지 않도록)
    stop_ocr_worker(filter);
    filter->mockWorker.stop();

    obs_enter_graphics();
    destroy_ocr_stage_surface(filter);
#ifdef _WIN32
    // [C2-5 수정] shutdown 경로에서는 spin-wait가 없는 destroyImmediate() 사용.
    filter->readback.destroyImmediate();
#endif
    if (filter->blurEffect) {
        gs_effect_destroy(filter->blurEffect);
        filter->blurEffect = nullptr;
    }
    filter->ringBuffer.destroy();   // [NEW-2] graphics context 안에서 안전
    obs_leave_graphics();

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
    filter->lastSourceW.store(w, std::memory_order_release);
    filter->lastSourceH.store(h, std::memory_order_release);

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
        start_ocr_worker(filter);
        blog(LOG_INFO, "[securecast][ocr] Async OCR worker started.");
    } else if (filter->ringBuffer.getWidth() != w || filter->ringBuffer.getHeight() != h) {
        // [P1 수정] 소스 해상도가 바뀌면 텍스처를 재생성해야 화면 깨짐 및 크래시 방지
        blog(LOG_INFO, "Resolution changed (%dx%d -> %dx%d). Reinitializing ring buffer.",
                filter->ringBuffer.getWidth(), filter->ringBuffer.getHeight(), w, h);

        // 1. AI 워커 중지
        filter->mockWorker.stop();

        // 2. 링 버퍼 재구성
        filter->ringBuffer.destroy();
        destroy_ocr_stage_surface(filter);
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
        start_ocr_worker(filter);
        blog(LOG_INFO, "[securecast][ocr] Async OCR worker restarted after resize.");

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

    // --- Panic Mode: pushFrame 이전에 차단 ---
    // 패닉 중에는 링 버퍼를 파괴해 GPU 낭비를 막고,
    // 해제 직후 패닉 중 캡처된 프레임이 스트림에 유출되는 것을 방지한다.
    // ringBuffer.destroy()는 이미 파괴된 경우 no-op이라 매 프레임 호출해도 안전.
    if (filter->panicMode.load(std::memory_order_relaxed)) {
        filter->ringBuffer.destroy();

        gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);

        gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xFF000000);
        while (gs_effect_loop(solid, "Solid"))
            gs_draw_sprite(nullptr, 0, w, h);

        constexpr uint32_t BORDER = 6;
        gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xFFFF0000);
        while (gs_effect_loop(solid, "Solid")) {
            gs_matrix_push(); gs_matrix_identity();
            gs_draw_sprite(nullptr, 0, w, BORDER);
            gs_matrix_pop();
            gs_matrix_push(); gs_matrix_identity();
            gs_matrix_translate3f(0.0f, (float)(h - BORDER), 0.0f);
            gs_draw_sprite(nullptr, 0, w, BORDER);
            gs_matrix_pop();
            gs_matrix_push(); gs_matrix_identity();
            gs_draw_sprite(nullptr, 0, BORDER, h);
            gs_matrix_pop();
            gs_matrix_push(); gs_matrix_identity();
            gs_matrix_translate3f((float)(w - BORDER), 0.0f, 0.0f);
            gs_draw_sprite(nullptr, 0, BORDER, h);
            gs_matrix_pop();
        }
        return;
    }

    // --- Step 2: 현재 프레임을 Ring Buffer HEAD에 Push ---
    // OBS 소스 내부 버퍼링으로 obs_source_video_render()가 반환하는 픽셀은
    // 실제 DWM 쿼리보다 ~1프레임 뒤처진다.
    // captureWindowList(직전 프레임에서 저장한 DWM 좌표)를 스냅샷으로 쓰면
    // 픽셀 내용과 마스크 위치가 정확히 동기화된다.
    uint64_t ts = obs_get_video_frame_time();
#ifdef _WIN32
    filter->ringBuffer.pushFrame(ts, filter->context, &filter->captureWindowList);
    // push 이후에 DWM 갱신 → 다음 프레임의 captureWindowList로 저장
    sc_update_tracked_bounds(&filter->windowList);
    filter->captureWindowList = filter->windowList;
#else
    filter->ringBuffer.pushFrame(ts, filter->context);
#endif

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
        bool manualOrNotifActive = filter->manualRectCount > 0 || filter->notifBlurActive;
        if (filter->health.isCritical()) {
            filter->currentState = SecurityState::RISK;
        } else if (ocrSnapshot.rectCount > 0 || blacklistSnapshot.rectCount > 0 || manualOrNotifActive) {
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

    // --- Step 5a: OCR 프레임 제출. OCR 인식으로 render thread를 막지 않는다. ---
    filter->ocrFrameCounter++;
    if (filter->ocrFrameCounter >= 15) {
        filter->ocrFrameCounter = 0;

        std::vector<uint8_t> bgraPixels;
        int stride = 0;

        gs_texture_t* ocrTex = delayedSlot->getTexture();
        if (read_texture_bgra_to_cpu(filter, ocrTex, w, h, bgraPixels, stride)) {
            submit_ocr_frame(filter, std::move(bgraPixels), (int)w, (int)h, stride);
        }
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

#ifdef _WIN32
    // [Role D] 알림 영역 자동 블러 — 쿨다운 중이면 rect를 합산
    {
        std::lock_guard<std::mutex> lock(filter->settingsMutex);
        if (filter->notifBlurActive &&
            filter->notifBlurRect.width > 0 && filter->notifBlurRect.height > 0 &&
            all_count < (int)(sizeof(all_rects) / sizeof(all_rects[0]))) {
            all_rects[all_count++] = filter->notifBlurRect;
        }
    }

    // [Role D] 수동 드래그 블러 — 확정 rects + 드래그 중 미리보기
    {
        std::lock_guard<std::mutex> lock(filter->settingsMutex);
        // 확정된 수동 블러 영역
        for (int i = 0; i < filter->manualRectCount &&
                        all_count < (int)(sizeof(all_rects) / sizeof(all_rects[0])); i++) {
            all_rects[all_count++] = filter->manualRects[i];
        }
        // 드래그 중 미리보기 rect (마우스 업 전까지 실시간 표시)
        if (filter->dragActive) {
            int px  = std::min(filter->dragStartX, filter->dragCurX);
            int py  = std::min(filter->dragStartY, filter->dragCurY);
            int pbw = std::abs(filter->dragCurX - filter->dragStartX);
            int pbh = std::abs(filter->dragCurY - filter->dragStartY);
            if (pbw > 8 && pbh > 8 &&
                all_count < (int)(sizeof(all_rects) / sizeof(all_rects[0]))) {
                all_rects[all_count++] = {px, py, pbw, pbh, 0};
            }
        }
    }
#endif

    if (all_count > 0) {
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

    // [Role D] 블랙리스트 마스크를 불투명 검정으로 최우선 덮어씌우기 (blur shader 위에 덮어씀)
    if (blacklistSnapshot.rectCount > 0) {
        gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
        gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xFF000000);
        gs_matrix_push();
        while (gs_effect_loop(solid, "Solid")) {
            for (int i = 0; i < blacklistSnapshot.rectCount; i++) {
                const BlurRect& r = blacklistSnapshot.rects[i];
                gs_matrix_identity();
                gs_matrix_translate3f((float)r.x, (float)r.y, 0.0f);
                gs_draw_sprite(nullptr, 0, (uint32_t)r.width, (uint32_t)r.height);
            }
        }
        gs_matrix_pop();
    }

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
    // 게임 모드 활성 시: 우상단에 빨간 네모 박스 표시 (임시 인디케이터)
    if (filter->isGameMode.load(std::memory_order_acquire)) {
        gs_effect_t* solid = obs_get_base_effect(OBS_EFFECT_SOLID);
        constexpr uint32_t GM_SZ = 20, GM_PAD = 8;
        gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xFFFF0000);
        while (gs_effect_loop(solid, "Solid")) {
            gs_matrix_push();
            gs_matrix_identity();
            gs_matrix_translate3f((float)(w - GM_SZ - GM_PAD), (float)GM_PAD, 0.0f);
            gs_draw_sprite(nullptr, 0, GM_SZ, GM_SZ);
            gs_matrix_pop();
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
    if (!filter)
        return;

    // [Role D] isActive는 GUI 스레드(update)에서도 쓸 수 있으므로 settingsMutex로 보호
    {
        std::lock_guard<std::mutex> lock(filter->settingsMutex);
        if (!filter->isActive)
            return;
    }

#ifdef _WIN32
    // ── CPU 샘플링 & 게임 모드 상태머신 (1초 주기) ──────────────────────
    filter->cpuSampleAccumulator += seconds;
    if (filter->cpuSampleAccumulator >= GM_SAMPLE_INTERVAL) {
        filter->cpuSampleAccumulator = 0.0f;
        filter->cpuUsage = sampleCpuUsage(&filter->prevIdleTime,
                                           &filter->prevKernelTime,
                                           &filter->prevUserTime);

        if (!filter->isGameMode.load(std::memory_order_acquire)) {
            if (filter->cpuUsage >= GM_CPU_ENTER) {
                filter->gameModeEntryTimer += GM_SAMPLE_INTERVAL;
                if (filter->gameModeEntryTimer >= GM_ENTER_TIME) {
                    filter->isGameMode.store(true, std::memory_order_release);
                    filter->gameModeEntryTimer = 0.0f;
                    filter->gameModeExitTimer  = 0.0f;
                    filter->mockWorker.setPaused(true);
                    blog(LOG_INFO, "[SecureCast] Game mode ON  (CPU: %.0f%%)", filter->cpuUsage);
                }
            } else {
                filter->gameModeEntryTimer = 0.0f;
            }
        } else {
            if (filter->cpuUsage <= GM_CPU_EXIT) {
                filter->gameModeExitTimer += GM_SAMPLE_INTERVAL;
                if (filter->gameModeExitTimer >= GM_EXIT_TIME) {
                    filter->isGameMode.store(false, std::memory_order_release);
                    filter->gameModeExitTimer = 0.0f;
                    filter->mockWorker.setPaused(false);
                    blog(LOG_INFO, "[SecureCast] Game mode OFF (CPU: %.0f%%, 5s cooldown)", filter->cpuUsage);
                }
            } else {
                filter->gameModeExitTimer = 0.0f;
            }
        }
    }

    // ── WinEvent: 포그라운드 전환 감지 → Quick Restore ─
    // 게임 모드는 CPU 임계값(≤30%, 5s)으로만 해제한다.
    // WinEvent로 해제하면 게임 실행 중 발생하는 내부 창 이벤트(알림, 팝업 등)에
    // 의해 게임 모드가 즉시 끊겼다가 3초 후 재진입하는 깜빡임이 발생한다.
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

    const float scanInterval = filter->isGameMode.load(std::memory_order_acquire) ? SCAN_INTERVAL_GAME : SCAN_INTERVAL_NORMAL;
    sc_tracker_tick(seconds, &filter->trackerAccumulator, &filter->windowList, scanInterval);

    // [Role D] windowList 스캔 결과를 blacklistMask에 반영 (video_render에서 최우선 차단에 사용)
    {
        std::lock_guard<std::mutex> lock(filter->blacklistMutex);
        filter->blacklistMask.rectCount = (filter->windowList.count > SC_MAX_BLUR_RECTS)
                                              ? SC_MAX_BLUR_RECTS : filter->windowList.count;
        for (int i = 0; i < filter->blacklistMask.rectCount; ++i) {
            filter->blacklistMask.rects[i] = {
                (int)filter->windowList.items[i].bounds.left,
                (int)filter->windowList.items[i].bounds.top,
                (int)(filter->windowList.items[i].bounds.right  - filter->windowList.items[i].bounds.left),
                (int)(filter->windowList.items[i].bounds.bottom - filter->windowList.items[i].bounds.top),
                0
            };
        }
        if (filter->windowList.count > 0 && filter->logScanThrottle++ % 10 == 0)
            blog(LOG_INFO, "[SecureCast] %d blacklisted windows in blacklistMask.", filter->windowList.count);
    }

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

    // [Role D] 알림 영역 자동 블러 쿨다운 카운트다운 (3초 경과 시 해제)
    {
        std::lock_guard<std::mutex> lock(filter->settingsMutex);
        if (filter->notifBlurActive) {
            filter->notifBlurCooldown -= seconds;
            if (filter->notifBlurCooldown <= 0.0f) {
                filter->notifBlurActive   = false;
                filter->notifBlurCooldown = 0.0f;
                blog(LOG_INFO, "[SecureCast][D] Notification blur expired.");
            }
        }
    }
#endif
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

#ifdef _WIN32
    // [Role D] 수동 드로그 블로 초기화 버튼
    obs_properties_add_button(props, "sc_clear_manual", "Clear Manual Blurs",
        [](obs_properties_t*, obs_property_t*, void* btn_data) -> bool {
            auto* filter = static_cast<SecureCastFilter*>(btn_data);
            std::lock_guard<std::mutex> lock(filter->settingsMutex);
            filter->manualRectCount = 0;
            filter->dragActive      = false;
            blog(LOG_INFO, "[SecureCast][D] Manual blur rects cleared (Properties button).");
            return true;
        });
#endif

    return props;
}

// GUI 스레드에서 호출되면서 settingsMutex로 보호 (Render Thread와 data race 방지)
static void securecast_update(void* data, obs_data_t* settings)
{
    SecureCastFilter* filter = static_cast<SecureCastFilter*>(data);

    std::lock_guard<std::mutex> lock(filter->settingsMutex);

    filter->blacklistApps = obs_data_get_string(settings, SC_SETTING_BLACKLIST);
    filter->blurIntensity = (float)obs_data_get_double(settings, SC_SETTING_BLUR_INTENSITY);
    // filter->isGameMode = obs_data_get_bool(settings, SC_SETTING_GAME_MODE);  // [v2]
    filter->sensitivity   = (float)obs_data_get_double(settings, SC_SETTING_SENSITIVITY);

    blog(LOG_INFO, "[SecureCast][D] Settings updated -- blur=%.1f sensitivity=%.2f",
         filter->blurIntensity, filter->sensitivity);
}

// ================================================================
// [Role D] 수동 드래그 블러 -- OBS Interaction API 콜백
// mouse_click : 좌클릭 DOWN -> 드래그 시작 / UP -> BlurRect 확정
//               우클릭 DOWN -> 수동 블러 전체 초기화
// mouse_move  : 드래그 중 현재 커서 좌표 갱신 (미리보기용)
// 좌표계: obs_mouse_event.x/y 는 소스 픽셀 좌표 (0~srcW, 0~srcH)
// 스레드: UI 스레드에서 호출 -> settingsMutex로 Render 스레드와 동기화
// ================================================================
#ifdef _WIN32
static void securecast_mouse_click(void* data,
    const struct obs_mouse_event* event,
    int32_t type, bool mouse_up, uint32_t /*click_count*/)
{
    auto* filter = static_cast<SecureCastFilter*>(data);
    std::lock_guard<std::mutex> lock(filter->settingsMutex);

    // 우클릭 DOWN: 수동 블러 전체 초기화
    if (type == MOUSE_RIGHT && !mouse_up) {
        filter->manualRectCount = 0;
        filter->dragActive      = false;
        blog(LOG_INFO, "[SecureCast][D] Manual blur rects cleared (right-click).");
        return;
    }

    if (type != MOUSE_LEFT)
        return;

    if (!mouse_up) {
        // 좌클릭 DOWN: 드래그 시작
        filter->dragActive = true;
        filter->dragStartX = event->x;
        filter->dragStartY = event->y;
        filter->dragCurX   = event->x;
        filter->dragCurY   = event->y;
    } else if (filter->dragActive) {
        // 좌클릭 UP: 드래그 완료 -> BlurRect 확정
        filter->dragActive = false;
        int x  = std::min(filter->dragStartX, event->x);
        int y  = std::min(filter->dragStartY, event->y);
        int bw = std::abs(event->x - filter->dragStartX);
        int bh = std::abs(event->y - filter->dragStartY);
        if (bw > 8 && bh > 8 &&
            filter->manualRectCount < filter->SC_MAX_MANUAL_RECTS) {
            filter->manualRects[filter->manualRectCount++] = {x, y, bw, bh, 0};
            blog(LOG_INFO, "[SecureCast][D] Manual blur added: (%d,%d %dx%d) total=%d",
                 x, y, bw, bh, filter->manualRectCount);
        }
    }
}

static void securecast_mouse_move(void* data,
    const struct obs_mouse_event* event, bool mouse_leave)
{
    auto* filter = static_cast<SecureCastFilter*>(data);
    std::lock_guard<std::mutex> lock(filter->settingsMutex);
    if (mouse_leave) {
        filter->dragActive = false;
        return;
    }
    if (filter->dragActive) {
        filter->dragCurX = event->x;
        filter->dragCurY = event->y;
    }
}
#endif

// ================================================================
// Source Info Dispatch Table
// ================================================================
struct obs_source_info securecast_filter_info = []() {
    struct obs_source_info info = {};
    info.id           = "securecast_filter";
    info.type         = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO;
#ifdef _WIN32
    info.output_flags |= OBS_SOURCE_INTERACTION;
#endif
    info.get_name       = securecast_get_name;
    info.create         = securecast_create;
    info.destroy        = securecast_destroy;
    info.video_tick     = securecast_video_tick;
    info.video_render   = securecast_video_render;
    info.get_properties = securecast_get_properties;
    info.get_defaults   = securecast_get_defaults;
    info.update         = securecast_update;
#ifdef _WIN32
    info.mouse_click    = securecast_mouse_click;
    info.mouse_move     = securecast_mouse_move;
#endif
    return info;
}();
