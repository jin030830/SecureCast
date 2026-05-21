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
//   securecast_video_tick    : 매 프레임 (60fps), 화면 그리기 직전 — 느린
//   작업용 securecast_video_render  : 매 프레임 (60fps), 실제 픽셀 렌더링 단계
// =============================================================================

// NOMINMAX must be defined before any Windows header to prevent min/max macro
// conflicts with std::max / std::min from <algorithm>.
#define NOMINMAX
#include "securecast-filter.h"
#include "plugin-support.h" // obs_log
#ifdef _WIN32
#include "window_tracker.h" // sc_tracker_tick (Role A: 블랙리스트 앱 좌표 수집)
#include <obs-frontend-api.h> // obs_hotkey_register_frontend
#endif
#include "ocr-engine.h" // Role B: OCR engine

#include <algorithm>
#include <chrono>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <util/platform.h>

// 1-E: BGRA 스케일 SIMD 헬퍼 인클루드
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#define SC_SCALE_HAS_AVX2 1
#define SC_SCALE_HAS_SSE2 1
#elif defined(__AVX2__)
#include <immintrin.h>
#define SC_SCALE_HAS_AVX2 1
#define SC_SCALE_HAS_SSE2 1
#elif defined(__SSE2__)
#include <emmintrin.h>
#define SC_SCALE_HAS_SSE2 1
#endif

// ================================================================
// SecureCastFilter 구현부
// ================================================================

SecureCastFilter::SecureCastFilter()
    : ocrEngine(std::make_unique<SecureCastOcrEngine>()) {}

SecureCastFilter::~SecureCastFilter() = default;

// trackerAccumulator에 이 값을 대입하면 다음 sc_tracker_tick 호출 시 임계를
// 즉시 초과 → 강제 스캔 트리거.

// forward declaration — 정의는 Properties/Settings 섹션에 있음
static void save_manual_rects(SecureCastFilter *filter,
                              const MaskPayload &mask);

#ifdef _WIN32
// ────────────────────────────────────────────────────────────
// [Fix #3-A] register_lingering_window — hwnd 기반 lingering upsert 헬퍼
//
// 기존 창이 있으면 window 좌표와 TTL(SC_RING_BUFFER_SLOTS + 1 = 61)을 갱신,
// 없으면 슬롯이 남은 경우에만 신규 추가한다.
// TTL = SC_RING_BUFFER_SLOTS + 1:
//   N슬롯 지연(60) + 1프레임 갭(pushFrame이 captureWindowList 갱신보다 선행)
//   을 정확히 커버. 윈도우 자체가 사라지는 신호는 win_event_listener의
//   HIDE/DESTROY/MINIMIZESTART push로 즉시 잡히므로 추가 마진 불필요.
//   (1a051bd: TTL이 너무 크면 창 최소화 후 빈 텍스처에 블러 잔상 발생)
// ────────────────────────────────────────────────────────────
static void register_lingering_window(SecureCastFilter *filter,
                                      const TrackedWindow &win) {
  for (int li = 0; li < filter->lingeringCount; ++li) {
    if (filter->lingeringWindows[li].window.hwnd == win.hwnd) {
      filter->lingeringWindows[li].window = win;
      filter->lingeringWindows[li].ticksRemaining = SC_RING_BUFFER_SLOTS + 1;
      return;
    }
  }
  if (filter->lingeringCount < SC_MAX_LINGERING) {
    filter->lingeringWindows[filter->lingeringCount++] = {
        win, SC_RING_BUFFER_SLOTS + 1};
  } else {
    blog(LOG_WARNING, "[SecureCast][linger-full] dropping hwnd=%p",
         (void *)win.hwnd);
  }
}
#endif

static constexpr float SCAN_INTERVAL_FORCE = 1.0f;
static constexpr float SCAN_INTERVAL_NORMAL = 0.15f; // 일반 모드 스캔 주기
static constexpr float SCAN_INTERVAL_GAME = 0.5f;    // 게임 모드 스캔 주기

// Game mode thresholds
static constexpr float GM_CPU_ENTER = 40.0f; // 진입 임계값 (%)
static constexpr float GM_CPU_EXIT = 30.0f;  // 해제 임계값 (%)
static constexpr float GM_ENTER_TIME = 3.0f; // 진입까지 ≥40% 유지 시간 (초)
static constexpr float GM_EXIT_TIME = 5.0f;  // 해제까지 ≤30%  유지 시간 (초)
static constexpr float GM_SAMPLE_INTERVAL = 1.0f; // CPU 샘플링 주기 (초)

// ================================================================
// [Game Mode] GetSystemTimes 기반 시스템 전체 CPU 사용률 샘플링 (WIN32)
//
// GetSystemTimes: kernel 시간에 idle이 포함되므로
//   CPU % = (kernel + user - idle) / (kernel + user) * 100
// 멀티코어 기준 전체 평균값. 오버헤드 거의 0 (단순 syscall).
// ================================================================
#ifdef _WIN32
static float sampleCpuUsage(FILETIME *prevIdle, FILETIME *prevKernel,
                            FILETIME *prevUser) {
  FILETIME idle, kernel, user;
  if (!GetSystemTimes(&idle, &kernel, &user))
    return 0.0f;

  auto ft2u64 = [](FILETIME ft) -> uint64_t {
    return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  };

  uint64_t idleDiff = ft2u64(idle) - ft2u64(*prevIdle);
  uint64_t kernelDiff = ft2u64(kernel) - ft2u64(*prevKernel);
  uint64_t userDiff = ft2u64(user) - ft2u64(*prevUser);

  *prevIdle = idle;
  *prevKernel = kernel;
  *prevUser = user;

  uint64_t total = kernelDiff + userDiff;
  if (total == 0 || idleDiff > total)
    return 0.0f;

  return std::clamp((float)(total - idleDiff) / (float)total * 100.0f, 0.0f,
                    100.0f);
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
                                            uint32_t src_w, uint32_t src_h) {
  BlurRect r{};
  // MonitorFromRect을 사용해야 lingering window(이미 닫힌 hwnd)에도 동작한다.
  HMONITOR hmon = MonitorFromRect(&tw.bounds, MONITOR_DEFAULTTONEAREST);
  if (!hmon)
    return r;

  MONITORINFO mi{};
  mi.cbSize = sizeof(MONITORINFO);
  if (!GetMonitorInfo(hmon, &mi))
    return r;

  int mon_w = mi.rcMonitor.right - mi.rcMonitor.left;
  int mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
  if (mon_w <= 0 || mon_h <= 0)
    return r;

  float sx = (float)src_w / mon_w;
  float sy = (float)src_h / mon_h;

  int x = (int)((tw.bounds.left - mi.rcMonitor.left) * sx);
  int y = (int)((tw.bounds.top - mi.rcMonitor.top) * sy);
  int bw = (int)((tw.bounds.right - tw.bounds.left) * sx);
  int bh = (int)((tw.bounds.bottom - tw.bounds.top) * sy);

  // 비대칭 BBox 팽창 — 위쪽(타이틀바 위)은 최소, 좌/우/아래는 빠른 이동 여유
  // 포함. 위: 1%  / 좌우: 5% (이동 시 trailing edge 커버) / 아래: 3%
  int exp_top = (int)(bh * 0.01f);
  int exp_sides = (int)(bw * 0.025f);
  int exp_bottom = (int)(bh * 0.015f);
  x = std::max(0, x - exp_sides);
  y = std::max(0, y - exp_top);
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
                             const BlurRect &r, uint32_t src_w,
                             uint32_t src_h) {
  if (r.width <= 0 || r.height <= 0)
    return;

  const char *tech = (r.type == 0) ? "Blur" : "Blackout";

  if (r.type == 0) {
    struct vec2 box_off = {(float)r.x, (float)r.y};
    struct vec2 box_sz = {(float)r.width, (float)r.height};
    struct vec2 img_sz = {(float)src_w, (float)src_h};
    gs_effect_set_texture(gs_effect_get_param_by_name(fx, "image"), img_tex);
    gs_effect_set_vec2(gs_effect_get_param_by_name(fx, "box_offset"), &box_off);
    gs_effect_set_vec2(gs_effect_get_param_by_name(fx, "box_size"), &box_sz);
    gs_effect_set_vec2(gs_effect_get_param_by_name(fx, "image_size"), &img_sz);
    gs_effect_set_float(gs_effect_get_param_by_name(fx, "blur_radius"), 8.0f);
  }

  gs_matrix_push();
  gs_matrix_identity();
  gs_matrix_translate3f((float)r.x, (float)r.y, 0.0f);
  while (gs_effect_loop(fx, tech))
    gs_draw_sprite(nullptr, 0, (uint32_t)r.width, (uint32_t)r.height);
  gs_matrix_pop();
}

static void render_solid_black_frame(uint32_t w, uint32_t h) {
  gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
  gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");
  gs_effect_set_color(colorParam, 0xFF000000);
  while (gs_effect_loop(solid, "Solid"))
    gs_draw_sprite(nullptr, 0, w, h);
}

static void draw_texture_full_frame(gs_texture_t *tex, uint32_t w, uint32_t h) {
  if (!tex)
    return;
  gs_effect_t *draw = obs_get_base_effect(OBS_EFFECT_DEFAULT);
  gs_effect_set_texture(gs_effect_get_param_by_name(draw, "image"), tex);
  while (gs_effect_loop(draw, "Draw"))
    gs_draw_sprite(tex, 0, w, h);
}

static void render_masked_output(SecureCastFilter *filter, gs_texture_t *srcTex,
                                 const BlurRect *rects, int rectCount,
                                 uint32_t w, uint32_t h) {
  if (!filter || !srcTex)
    return;

  draw_texture_full_frame(srcTex, w, h);

  if (rectCount <= 0)
    return;

  if (filter->blurEffect) {
    for (int i = 0; i < rectCount; i++)
      render_blur_rect(filter->blurEffect, srcTex, rects[i], w, h);
    return;
  }

  // 셰이더 로드 실패 시 fallback: 단색 검정 박스
  gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
  gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xFF000000);
  gs_matrix_push();
  while (gs_effect_loop(solid, "Solid")) {
    for (int i = 0; i < rectCount; i++) {
      gs_matrix_identity();
      gs_matrix_translate3f((float)rects[i].x, (float)rects[i].y, 0.0f);
      gs_draw_sprite(nullptr, 0, (uint32_t)rects[i].width,
                     (uint32_t)rects[i].height);
    }
  }
  gs_matrix_pop();
}

static void destroy_last_safe_render(SecureCastFilter *filter) {
  if (!filter)
    return;
  if (filter->lastSafeRender_) {
    gs_texrender_destroy(filter->lastSafeRender_);
    filter->lastSafeRender_ = nullptr;
  }
  filter->lastSafeReady_ = false;
  filter->lastSafeW_ = 0;
  filter->lastSafeH_ = 0;
}

static bool ensure_last_safe_render(SecureCastFilter *filter, uint32_t w,
                                    uint32_t h) {
  if (!filter || w == 0 || h == 0)
    return false;
  if (filter->lastSafeRender_ && filter->lastSafeW_ == w &&
      filter->lastSafeH_ == h)
    return true;

  destroy_last_safe_render(filter);
  filter->lastSafeRender_ = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
  if (!filter->lastSafeRender_)
    return false;
  filter->lastSafeW_ = w;
  filter->lastSafeH_ = h;
  return true;
}

static bool update_last_safe_render(SecureCastFilter *filter,
                                    gs_texture_t *srcTex, const BlurRect *rects,
                                    int rectCount, uint32_t w, uint32_t h) {
  if (!ensure_last_safe_render(filter, w, h) || !srcTex)
    return false;

  gs_texrender_reset(filter->lastSafeRender_);
  if (!gs_texrender_begin(filter->lastSafeRender_, (int)w, (int)h))
    return false;

  struct vec4 clearColor;
  vec4_zero(&clearColor);
  gs_clear(GS_CLEAR_COLOR, &clearColor, 1.0f, 0);
  gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
  render_masked_output(filter, srcTex, rects, rectCount, w, h);
  gs_texrender_end(filter->lastSafeRender_);
  filter->lastSafeReady_ = true;
  return true;
}

static bool render_last_safe_frame(SecureCastFilter *filter, uint32_t w,
                                   uint32_t h) {
  if (!filter || !filter->lastSafeReady_ || !filter->lastSafeRender_)
    return false;
  gs_texture_t *tex = gs_texrender_get_texture(filter->lastSafeRender_);
  if (!tex)
    return false;
  draw_texture_full_frame(tex, w, h);
  return true;
}

// ================================================================
// [Role C] FrameRingBuffer 구현부
// ================================================================

// GPU에 gs_texrender 슬롯을 할당한다. video_render 첫 호출 또는 해상도 변경 시
// 호출.
bool FrameRingBuffer::initialize(uint32_t width, uint32_t height) {
  if (m_initialized)
    return true;

  m_width = width;
  m_height = height;

  for (auto &slot : m_slots) {
    slot.texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    if (!slot.texrender) {
      blog(LOG_ERROR, "Failed to allocate gs_texrender slot.");
      // [F13 Fix] 실패 시 이미 생성된 렌더러들을 로컬에서 직접 파괴하여 메모리
      // 누수를 막고 destroy() 내 obs_enter_graphics() 이중 잠금 차단
      for (auto &clean_slot : m_slots) {
        if (clean_slot.texrender) {
          gs_texrender_destroy(clean_slot.texrender);
          clean_slot.texrender = nullptr;
        }
      }
      return false;
    }
    slot.timestamp = 0;
    slot.frameId = 0;
    slot.dependentOcrFrameId = 0;
  }

  m_initialized = true;
  blog(LOG_INFO, "FrameRingBuffer initialized: %dx%d, %d slots (gs_texrender).",
       width, height, SC_RING_BUFFER_SLOTS);
  return true;
}

// 모든 슬롯의 GPU 텍스처를 해제하고 버퍼를 초기 상태로 되돌린다. 소멸자 또는
// 해상도 변경 시 호출.
void FrameRingBuffer::destroy() {
  if (!m_initialized)
    return;

  obs_enter_graphics();
  for (auto &slot : m_slots) {
    if (slot.texrender) {
      gs_texrender_destroy(slot.texrender);
      slot.texrender = nullptr;
    }
    slot.timestamp = 0;
    slot.frameId = 0;
    slot.dependentOcrFrameId = 0;
  }
  obs_leave_graphics();

  m_initialized = false;
  m_head = 0;
  m_frameCount = 0;
  m_nextFrameId = 1;
  blog(LOG_INFO, "FrameRingBuffer destroyed.");
}

// 현재 OBS 소스 프레임을 HEAD 슬롯에 캡처하고, 창 좌표·OCR 박스 스냅샷을 함께
// 저장한다. HEAD를 한 칸 전진시켜 다음 pushFrame이 다음 슬롯에 쓰도록 한다.
#ifdef _WIN32
void FrameRingBuffer::pushFrame(uint64_t timestamp,
                                obs_source_t *filter_context,
                                const TrackedWindowList *wlist,
                                uint64_t dependentOcrFrameId,
                                const OcrBoxSnapshot *ocrSnapshot)
#else
void FrameRingBuffer::pushFrame(uint64_t timestamp,
                                obs_source_t *filter_context,
                                uint64_t dependentOcrFrameId,
                                const OcrBoxSnapshot *ocrSnapshot)
#endif
{
  if (!m_initialized)
    return;

  Slot &slot = m_slots[m_head];
  slot.frameId = m_nextFrameId++;
  slot.dependentOcrFrameId = dependentOcrFrameId;

#ifdef _WIN32
  if (wlist)
    slot.windowSnapshot = *wlist;
  else
    slot.windowSnapshot = TrackedWindowList{};
#endif

  // OCR/Tracker 박스 스냅샷 — null 허용 (null이면 빈 snapshot으로 초기화)
  if (ocrSnapshot)
    slot.ocrBoxSnapshot = *ocrSnapshot;
  else
    slot.ocrBoxSnapshot = OcrBoxSnapshot{};

  gs_texrender_t *tr = slot.texrender;
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
    obs_source_t *target = obs_filter_get_target(filter_context);
    if (target)
      obs_source_video_render(target);

    gs_texrender_end(tr);
  }

  slot.timestamp = timestamp;

  // 다음 HEAD로 순환
  m_head = (m_head + 1) % SC_RING_BUFFER_SLOTS;
  if (m_frameCount < SC_RING_BUFFER_SLOTS)
    m_frameCount++;
}

// N프레임(SC_RING_BUFFER_SLOTS) 전 슬롯을 반환한다. 인코더로 출력하는 "안전한"
// 지연 프레임.
const FrameRingBuffer::Slot *FrameRingBuffer::peekDelayedSlot() const {
  return peekSlotAtOffset(SC_RING_BUFFER_SLOTS);
}

// HEAD에서 framesBack만큼 이전 슬롯을 반환한다. N-1 슬롯과의 합집합 마스킹(빠른
// 이동 커버)에 사용.
const FrameRingBuffer::Slot *
FrameRingBuffer::peekSlotAtOffset(int framesBack) const {
  if (framesBack <= 0 || m_frameCount < framesBack)
    return nullptr;
  int idx =
      ((m_head - framesBack) % SC_RING_BUFFER_SLOTS + SC_RING_BUFFER_SLOTS) %
      SC_RING_BUFFER_SLOTS;
  return &m_slots[idx];
}

// ================================================================
// [Role C] MockAIWorker 구현부
// ================================================================

// 워커 스레드를 시작한다. AI 분석이 완료될 때마다 callback으로 마스킹 결과를
// 전달.
void MockAIWorker::start(uint32_t frameWidth, uint32_t frameHeight,
                         ResultCallback callback) {
  if (m_running.load())
    return;

  m_frameWidth = frameWidth;
  m_frameHeight = frameHeight;
  m_callback = std::move(callback);
  m_running.store(true);

  m_thread = std::thread([this]() { workerLoop(); });
  blog(LOG_INFO, "MockAIWorker started (frame: %dx%d).", m_frameWidth,
       m_frameHeight);
}

// 워커 스레드에 종료 신호를 보내고 join한다. securecast_destroy() 에서 ring
// buffer 해제 전에 반드시 호출.
void MockAIWorker::stop() {
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

// 게임 모드 진입/해제 시 호출. paused=true면 스레드는 cv.wait에서 대기해 CPU를
// 점유하지 않는다.
void MockAIWorker::setPaused(bool paused) {
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
void MockAIWorker::workerLoop() {
  while (m_running.load()) {
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      if (m_paused.load(std::memory_order_relaxed)) {
        // 게임 모드 일시정지: stop() 또는 resume 신호까지 대기
        m_cv.wait(lock, [this]() {
          return !m_running.load() || !m_paused.load(std::memory_order_relaxed);
        });
      } else {
        // AI 처리 시간 시뮬레이션 (실제 OCR 처리 예상 지연: ~40~60ms)
        m_cv.wait_for(lock, std::chrono::milliseconds(50), [this]() {
          return !m_running.load() || m_paused.load(std::memory_order_relaxed);
        });
      }
    }

    if (!m_running.load() || m_paused.load(std::memory_order_relaxed))
      continue;

    // [F4 Fix] 프로덕션에서 가짜 마스킹 박스가 영구 출력되는 데모 회귀 방지를
    // 위해 디버그용 MOCK 가드를 둡니다.
#ifdef SC_DEBUG_MOCK
    MaskPayload payload{};
    payload.rectCount = 0;

    if (m_callback)
      m_callback(payload);
#else
    // 실운영(Production) 시에는 빈 마스크를 주기적으로 전달하거나 스핀만
    // 유지시킵니다.
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

// AI 스레드 → Render 스레드 결과 전달. 뮤텍스로 m_pending 기록 보호 후 ready
// 플래그를 set.
void AtomicMaskChannel::publish(const MaskPayload &payload) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_pending = payload;
  m_ready.store(true, std::memory_order_release);
}

// Render 스레드가 새 마스킹 결과를 꺼낸다. 새 데이터가 없으면 false 반환
// (lastMask 유지).
bool AtomicMaskChannel::consume(MaskPayload &out) {
  if (!m_ready.load(std::memory_order_acquire))
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_ready.exchange(false, std::memory_order_acq_rel))
    return false; // 다른 consumer가 먼저 가져간 경우 (현재는 단일이지만 방어적
                  // 처리)
  out = m_pending;
  return true;
}

// ================================================================
// [Role B/C] GPU texture -> CPU BGRA pixels 재사용 readback
// ================================================================

static void destroy_ocr_stage_surface(SecureCastFilter *filter) {
  if (!filter || !filter->ocrStageSurface)
    return;

  gs_stagesurface_destroy(filter->ocrStageSurface);
  filter->ocrStageSurface = nullptr;
  filter->ocrStageWidth = 0;
  filter->ocrStageHeight = 0;
}

#ifdef _WIN32
// 1-G: half-size BGRA GPU 다운스케일 리소스 해제 (+ ocrMidRender_ 정리)
static void destroy_ocr_down_stage(SecureCastFilter *filter) {
  if (!filter)
    return;
  if (filter->ocrDownStage_) {
    gs_stagesurface_destroy(filter->ocrDownStage_);
    filter->ocrDownStage_ = nullptr;
  }
  if (filter->ocrDownRender_) {
    gs_texrender_destroy(filter->ocrDownRender_);
    filter->ocrDownRender_ = nullptr;
  }
  if (filter->ocrMidRender_) {
    gs_texrender_destroy(filter->ocrMidRender_);
    filter->ocrMidRender_ = nullptr;
  }
  filter->ocrDownW_ = 0;
  filter->ocrDownH_ = 0;
}

// OCR 다운스케일 stagesurf + 중간 렌더타겟 확보.
// dstW × dstH 텍스처 + stagesurf + ocrMidRender_ 일괄 재생성.
// Mitchell+USM 2-pass 사용 시 ocrMidRender_ 필요. bilinear 단일 pass도 호환.
static bool ensure_ocr_down_stage(SecureCastFilter *filter, uint32_t dstW,
                                  uint32_t dstH) {
  if (!filter)
    return false;
  if (dstW == 0 || dstH == 0)
    return false;

  if (filter->ocrDownRender_ && filter->ocrDownW_ == dstW &&
      filter->ocrDownH_ == dstH)
    return true;

  destroy_ocr_down_stage(filter);

  filter->ocrDownRender_ = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
  if (!filter->ocrDownRender_)
    return false;

  filter->ocrDownStage_ = gs_stagesurface_create(dstW, dstH, GS_BGRA);
  if (!filter->ocrDownStage_) {
    gs_texrender_destroy(filter->ocrDownRender_);
    filter->ocrDownRender_ = nullptr;
    return false;
  }

  // Mitchell + USM 2-pass용 중간 텍스처. 1-pass(bilinear) 경로에서는 사용 안
  // 됨.
  filter->ocrMidRender_ = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
  if (!filter->ocrMidRender_) {
    gs_stagesurface_destroy(filter->ocrDownStage_);
    filter->ocrDownStage_ = nullptr;
    gs_texrender_destroy(filter->ocrDownRender_);
    filter->ocrDownRender_ = nullptr;
    return false;
  }

  filter->ocrDownW_ = dstW;
  filter->ocrDownH_ = dstH;
  return true;
}

// GPU에서 srcTex를 임의 비율로 다운샘플 후 CPU 버퍼로 readback.
// tier == Half      : 0.5× bilinear (기존 BGRADownsample2x 셰이더, 1-pass)
// tier == TwoThirds : 0.667× Mitchell + USM (downsample_ocr.effect, 2-pass)
// tier == Full      : 호출 금지 (Full은 readback 자체를 하지 않음)
// 반환: outPixels에 dstW×dstH BGRA 기록, outStride = dstW*4.
//       outDstW/H에 실제 다운스케일 크기를 기록한다.
static bool read_texture_bgra_scaled_gpu(SecureCastFilter *filter,
                                         gs_texture_t *srcTex, uint32_t fullW,
                                         uint32_t fullH, OcrScaleTier tier,
                                         std::vector<uint8_t> &outPixels,
                                         int &outStride, uint32_t *outDstW,
                                         uint32_t *outDstH) {
  if (!filter || !srcTex || tier == OcrScaleTier::Full)
    return false;

  // 목표 크기 결정 — 1080p TwoThirds는 1280×720으로 하드 매칭.
  uint32_t dw = 0, dh = 0;
  if (tier == OcrScaleTier::Half) {
    dw = fullW / 2;
    dh = fullH / 2;
  } else { // TwoThirds
    if (fullW == 1920 && fullH == 1080) {
      dw = 1280;
      dh = 720;
    } else {
      const float r = 2.0f / 3.0f;
      dw = static_cast<uint32_t>(std::lround(fullW * r));
      dh = static_cast<uint32_t>(std::lround(fullH * r));
    }
  }
  if (dw == 0 || dh == 0)
    return false;

  if (!ensure_ocr_down_stage(filter, dw, dh))
    return false;

  // Half: 기존 bilinear 1-pass 경로 (trackerGrayEffect_ 필요).
  // TwoThirds: Mitchell + USM 2-pass (ocrDownsampleEffect_ 필요).
  const bool useMitchell = (tier == OcrScaleTier::TwoThirds) &&
                           (filter->ocrDownsampleEffect_ != nullptr);

  if (useMitchell) {
    // pass1: src → ocrMidRender_ (Mitchell)
    gs_effect_t *eff = filter->ocrDownsampleEffect_;
    gs_eparam_t *pImg = gs_effect_get_param_by_name(eff, "image");
    gs_eparam_t *pUV = gs_effect_get_param_by_name(eff, "uv_bounds");
    gs_eparam_t *pSrcTexel = gs_effect_get_param_by_name(eff, "src_texel_size");
    gs_eparam_t *pMidTexel = gs_effect_get_param_by_name(eff, "mid_texel_size");
    gs_eparam_t *pAmount = gs_effect_get_param_by_name(eff, "usm_amount");

    gs_texrender_reset(filter->ocrMidRender_);
    if (!gs_texrender_begin(filter->ocrMidRender_, static_cast<int>(dw),
                            static_cast<int>(dh)))
      return false;

    gs_effect_set_texture(pImg, srcTex);
    if (pUV) {
      struct vec4 bounds = {0.0f, 0.0f, 1.0f, 1.0f};
      gs_effect_set_vec4(pUV, &bounds);
    }
    if (pSrcTexel) {
      struct vec2 ts = {1.0f / static_cast<float>(fullW),
                        1.0f / static_cast<float>(fullH)};
      gs_effect_set_vec2(pSrcTexel, &ts);
    }
    while (gs_effect_loop(eff, "BGRADownsampleMitchell"))
      gs_draw_sprite(srcTex, 0, dw, dh);
    gs_texrender_end(filter->ocrMidRender_);

    // pass2: ocrMidRender_ → ocrDownRender_ (3×3 USM)
    gs_texture_t *midTex = gs_texrender_get_texture(filter->ocrMidRender_);
    if (!midTex)
      return false;

    gs_texrender_reset(filter->ocrDownRender_);
    if (!gs_texrender_begin(filter->ocrDownRender_, static_cast<int>(dw),
                            static_cast<int>(dh)))
      return false;

    gs_effect_set_texture(pImg, midTex);
    if (pUV) {
      struct vec4 bounds = {0.0f, 0.0f, 1.0f, 1.0f};
      gs_effect_set_vec4(pUV, &bounds);
    }
    if (pMidTexel) {
      struct vec2 ts = {1.0f / static_cast<float>(dw),
                        1.0f / static_cast<float>(dh)};
      gs_effect_set_vec2(pMidTexel, &ts);
    }
    if (pAmount) {
      gs_effect_set_float(pAmount, 0.5f);
    }
    while (gs_effect_loop(eff, "BGRAUnsharp3x3"))
      gs_draw_sprite(midTex, 0, dw, dh);
    gs_texrender_end(filter->ocrDownRender_);
  } else {
    // Half(또는 Mitchell 미로드 폴백): 기존 bilinear 1-pass.
    if (!filter->trackerGrayEffect_)
      return false;
    gs_effect_t *eff = filter->trackerGrayEffect_;
    gs_eparam_t *pImg = gs_effect_get_param_by_name(eff, "image");
    gs_eparam_t *pUV = gs_effect_get_param_by_name(eff, "uv_bounds");

    gs_texrender_reset(filter->ocrDownRender_);
    if (!gs_texrender_begin(filter->ocrDownRender_, static_cast<int>(dw),
                            static_cast<int>(dh)))
      return false;

    gs_effect_set_texture(pImg, srcTex);
    if (pUV) {
      struct vec4 bounds = {0.0f, 0.0f, 1.0f, 1.0f};
      gs_effect_set_vec4(pUV, &bounds);
    }
    while (gs_effect_loop(eff, "BGRADownsample2x"))
      gs_draw_sprite(srcTex, 0, dw, dh);
    gs_texrender_end(filter->ocrDownRender_);
  }

  // staging copy + map → CPU
  gs_texture_t *downTex = gs_texrender_get_texture(filter->ocrDownRender_);
  if (!downTex)
    return false;
  gs_stage_texture(filter->ocrDownStage_, downTex);

  uint8_t *mapped = nullptr;
  uint32_t rowPitch = 0;
  if (!gs_stagesurface_map(filter->ocrDownStage_, &mapped, &rowPitch))
    return false;

  const uint32_t tightStride = dw * 4;
  outPixels.resize(static_cast<size_t>(tightStride) * dh);
  for (uint32_t y = 0; y < dh; ++y)
    std::memcpy(outPixels.data() + static_cast<size_t>(y) * tightStride,
                mapped + static_cast<size_t>(y) * rowPitch, tightStride);
  gs_stagesurface_unmap(filter->ocrDownStage_);

  outStride = static_cast<int>(tightStride);
  if (outDstW)
    *outDstW = dw;
  if (outDstH)
    *outDstH = dh;
  return true;
}
#endif // _WIN32

// ================================================================
// [Tier 1] GPU Grayscale Readback 헬퍼
//
// GPU에서 full-res BGRA → R8 grayscale 렌더 후 스테이징 readback.
// 기존 8MB BGRA readback → 2MB gray (1 byte/pixel) 로 대체.
// R8 렌더 타겟을 지원하지 않는 GPU에서는 false를 반환하며,
// 호출자는 CPU bgra_to_gray 경로로 폴백한다.
// ================================================================

static bool ensure_tracker_gray_surfaces(SecureCastFilter *filter, uint32_t w,
                                         uint32_t h) {
  if (!filter || w == 0 || h == 0)
    return false;
  if (filter->trackerGrayRender_ && filter->trackerGrayW_ == w &&
      filter->trackerGrayH_ == h)
    return true;

  if (filter->trackerGrayStage_) {
    gs_stagesurface_destroy(filter->trackerGrayStage_);
    filter->trackerGrayStage_ = nullptr;
  }
  if (filter->trackerGrayRender_) {
    gs_texrender_destroy(filter->trackerGrayRender_);
    filter->trackerGrayRender_ = nullptr;
  }

  filter->trackerGrayRender_ = gs_texrender_create(GS_R8, GS_ZS_NONE);
  if (!filter->trackerGrayRender_)
    return false;

  filter->trackerGrayStage_ = gs_stagesurface_create(w, h, GS_R8);
  if (!filter->trackerGrayStage_) {
    gs_texrender_destroy(filter->trackerGrayRender_);
    filter->trackerGrayRender_ = nullptr;
    return false;
  }

  filter->trackerGrayW_ = w;
  filter->trackerGrayH_ = h;
  return true;
}

// GPU에서 gray 렌더 → 스테이징 → CPU 버퍼로 복사.
// 반환: true = grayPixels에 grayW×grayH 크기의 1-byte/pixel gray 데이터 기록됨.
static bool read_tracker_gray_gpu(SecureCastFilter *filter,
                                  gs_texture_t *srcTex, uint32_t w, uint32_t h,
                                  std::vector<uint8_t> &grayPixels) {
  if (!filter || !filter->trackerGrayEffect_ || !srcTex)
    return false;
  if (!ensure_tracker_gray_surfaces(filter, w, h))
    return false;

  gs_texrender_t *render = filter->trackerGrayRender_;
  gs_stagesurf_t *stage = filter->trackerGrayStage_;

  // GPU render: srcTex → full-res R8 gray via GrayDownsample 기법
  gs_texrender_reset(render);
  if (!gs_texrender_begin(render, (int)w, (int)h))
    return false;

  gs_effect_t *eff = filter->trackerGrayEffect_;
  gs_eparam_t *pImg = gs_effect_get_param_by_name(eff, "image");
  gs_eparam_t *pUV = gs_effect_get_param_by_name(eff, "uv_bounds");

  gs_effect_set_texture(pImg, srcTex);
  if (pUV) {
    struct vec4 bounds = {0.0f, 0.0f, 1.0f, 1.0f};
    gs_effect_set_vec4(pUV, &bounds);
  }
  while (gs_effect_loop(eff, "GrayDownsample"))
    gs_draw_sprite(srcTex, 0, (uint32_t)w, (uint32_t)h);

  gs_texrender_end(render);

  // stage copy + map
  gs_texture_t *grayTex = gs_texrender_get_texture(render);
  if (!grayTex)
    return false;
  gs_stage_texture(stage, grayTex);

  uint8_t *mapped = nullptr;
  uint32_t rowStride = 0;
  if (!gs_stagesurface_map(stage, &mapped, &rowStride))
    return false;

  // R8: 1 byte/pixel; rowStride >= w (GPU alignment 고려)
  grayPixels.resize(static_cast<size_t>(w) * h);
  for (uint32_t y = 0; y < h; ++y)
    std::memcpy(grayPixels.data() + (size_t)y * w,
                mapped + (size_t)y * rowStride, w);

  gs_stagesurface_unmap(stage);
  return true;
}

static bool ensure_ocr_stage_surface(SecureCastFilter *filter, uint32_t width,
                                     uint32_t height) {
  if (!filter || width == 0 || height == 0)
    return false;

  if (filter->ocrStageSurface && filter->ocrStageWidth == width &&
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

static bool read_texture_bgra_to_cpu(SecureCastFilter *filter,
                                     gs_texture_t *texture, uint32_t width,
                                     uint32_t height,
                                     std::vector<uint8_t> &outPixels,
                                     int &outStride) {
  if (!filter || !texture || width == 0 || height == 0)
    return false;

  if (!ensure_ocr_stage_surface(filter, width, height))
    return false;

  gs_stagesurf_t *stage = filter->ocrStageSurface;
  gs_stage_texture(stage, texture);

  uint8_t *mappedData = nullptr;
  uint32_t mappedStride = 0;

  if (!gs_stagesurface_map(stage, &mappedData, &mappedStride)) {
    blog(LOG_WARNING, "[securecast][ocr] Failed to map staging surface.");
    return false;
  }

  const uint32_t tightStride = width * 4;
  outPixels.resize((size_t)tightStride * height);

  for (uint32_t y = 0; y < height; y++) {
    memcpy(outPixels.data() + (size_t)y * tightStride,
           mappedData + (size_t)y * mappedStride, tightStride);
  }

  gs_stagesurface_unmap(stage);

  outStride = (int)tightStride;
  return true;
}

// ================================================================
// [1-E] BGRA 스케일 SIMD 헬퍼
// ================================================================

// 2× box-filter 다운스케일 (BGRA → BGRA/2).
// AVX2: 4 output pixels / iteration. 스칼라 폴백 포함.
static void downsample2x_bgra_simd(const uint8_t *src, int sw, int sh,
                                   int sStride, uint8_t *dst, int dStride) {
  const int dw = sw / 2, dh = sh / 2;
#ifdef SC_SCALE_HAS_AVX2
  const __m256i shuf = _mm256_set_epi32(7, 5, 3, 1, 6, 4, 2, 0);
  for (int dy = 0; dy < dh; ++dy) {
    const uint8_t *s0 = src + (ptrdiff_t)(2 * dy) * sStride;
    const uint8_t *s1 = src + (ptrdiff_t)(2 * dy + 1) * sStride;
    uint8_t *d = dst + (ptrdiff_t)dy * dStride;
    int dx = 0;
    for (; dx <= dw - 4; dx += 4) {
      __m256i r0 = _mm256_loadu_si256((const __m256i *)(s0 + dx * 8));
      __m256i r1 = _mm256_loadu_si256((const __m256i *)(s1 + dx * 8));
      __m256i vavg = _mm256_avg_epu8(r0, r1);
      __m256i perm = _mm256_permutevar8x32_epi32(vavg, shuf);
      __m128i evn = _mm256_castsi256_si128(perm);
      __m128i odd = _mm256_extracti128_si256(perm, 1);
      _mm_storeu_si128((__m128i *)(d + dx * 4), _mm_avg_epu8(evn, odd));
    }
    for (; dx < dw; ++dx)
      for (int c = 0; c < 4; ++c)
        d[dx * 4 + c] = (uint8_t)(((int)s0[dx * 8 + c] + s0[dx * 8 + 4 + c] +
                                   s1[dx * 8 + c] + s1[dx * 8 + 4 + c]) >>
                                  2);
  }
  return;
#endif
  for (int dy = 0; dy < dh; ++dy) {
    const uint8_t *s0 = src + (ptrdiff_t)(2 * dy) * sStride;
    const uint8_t *s1 = src + (ptrdiff_t)(2 * dy + 1) * sStride;
    uint8_t *d = dst + (ptrdiff_t)dy * dStride;
    for (int dx = 0; dx < dw; ++dx)
      for (int c = 0; c < 4; ++c)
        d[dx * 4 + c] = (uint8_t)(((int)s0[dx * 8 + c] + s0[dx * 8 + 4 + c] +
                                   s1[dx * 8 + c] + s1[dx * 8 + 4 + c]) >>
                                  2);
  }
}

// 2× nearest-neighbor 업스케일 (BGRA → 2× BGRA).
// SSE2: 4 input pixels → 8 output pixels. 스칼라 NN 폴백 포함.
static void upsample2x_bgra_simd(const uint8_t *src, int sw, int sh,
                                 int sStride, uint8_t *dst, int dStride) {
#ifdef SC_SCALE_HAS_SSE2
  for (int sy = 0; sy < sh; ++sy) {
    const uint8_t *sRow = src + (ptrdiff_t)sy * sStride;
    uint8_t *dRow0 = dst + (ptrdiff_t)(sy * 2) * dStride;
    uint8_t *dRow1 = dst + (ptrdiff_t)(sy * 2 + 1) * dStride;
    int sx = 0;
    for (; sx <= sw - 4; sx += 4) {
      __m128i in = _mm_loadu_si128((const __m128i *)(sRow + sx * 4));
      __m128i lo = _mm_unpacklo_epi32(in, in);
      __m128i hi = _mm_unpackhi_epi32(in, in);
      _mm_storeu_si128((__m128i *)(dRow0 + sx * 8), lo);
      _mm_storeu_si128((__m128i *)(dRow0 + sx * 8 + 16), hi);
      _mm_storeu_si128((__m128i *)(dRow1 + sx * 8), lo);
      _mm_storeu_si128((__m128i *)(dRow1 + sx * 8 + 16), hi);
    }
    for (; sx < sw; ++sx) {
      const uint8_t *s = sRow + sx * 4;
      for (int c = 0; c < 4; ++c)
        dRow0[sx * 8 + c] = dRow0[sx * 8 + 4 + c] = dRow1[sx * 8 + c] =
            dRow1[sx * 8 + 4 + c] = s[c];
    }
  }
  return;
#endif
  for (int sy = 0; sy < sh; ++sy) {
    const uint8_t *sRow = src + (ptrdiff_t)sy * sStride;
    for (int sx = 0; sx < sw; ++sx) {
      const uint8_t *s = sRow + sx * 4;
      for (int r = 0; r < 2; ++r) {
        uint8_t *dRow = dst + (ptrdiff_t)(sy * 2 + r) * dStride;
        for (int c = 0; c < 2; ++c) {
          uint8_t *d = dRow + (sx * 2 + c) * 4;
          d[0] = s[0];
          d[1] = s[1];
          d[2] = s[2];
          d[3] = s[3];
        }
      }
    }
  }
}

// ================================================================
// [Role B] OCR worker 보조 함수
// ================================================================

static void clear_pending_ocr_frame(SecureCastFilter *filter) {
  if (!filter)
    return;

  std::lock_guard<std::mutex> lock(filter->ocrWorkerMutex);
  filter->ocrFramePending = false;
  filter->ocrPendingFrameId = 0;
  filter->ocrPendingPixels.clear();
  filter->ocrPendingWidth = 0;
  filter->ocrPendingHeight = 0;
  filter->ocrPendingStride = 0;
  filter->ocrPendingTrackerGray.clear();
  filter->ocrPendingTrackerW = 0;
  filter->ocrPendingTrackerH = 0;
  filter->ocrPendingTier = OcrScaleTier::Full;
}

// OCR 워커에 한 프레임 제출. pixels(OCR 입력 BGRA) + trackerGray(트래커
// 등록용 gray) + tier를 모두 같은 lock 안에서 push해 워커가 pop 시 한 번에
// 가져가도록 한다.
static void submit_ocr_frame(SecureCastFilter *filter,
                             std::vector<uint8_t> &&pixels, int width,
                             int height, int stride, uint64_t frameId,
                             std::vector<uint8_t> &&trackerGray, int trackerW,
                             int trackerH, OcrScaleTier tier) {
  if (!filter || pixels.empty() || width <= 0 || height <= 0 || stride <= 0 ||
      frameId == 0)
    return;

  if (!filter->ocrWorkerRunning.load(std::memory_order_acquire))
    return;

  {
    std::lock_guard<std::mutex> lock(filter->ocrWorkerMutex);
    filter->ocrPendingFrameId = frameId;
    filter->ocrPendingPixels = std::move(pixels);
    filter->ocrPendingWidth = width;
    filter->ocrPendingHeight = height;
    filter->ocrPendingStride = stride;
    filter->ocrPendingTrackerGray = std::move(trackerGray);
    filter->ocrPendingTrackerW = trackerW;
    filter->ocrPendingTrackerH = trackerH;
    filter->ocrPendingTier = tier;
    filter->ocrPendingEnqueueTs = std::chrono::steady_clock::now();
    filter->ocrFramePending = true;
  }

  filter->ocrWorkerCv.notify_one();
}

static void ocr_worker_loop(SecureCastFilter *filter) {
  if (!filter)
    return;

#ifdef _WIN32
  // OCR은 RecognizeAsync가 CPU를 점유할 수 있으므로 렌더 스레드보다 낮은
  // 우선순위로 실행.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

  const bool ocrReady = filter->ocrEngine ? filter->ocrEngine->init() : false;
  if (ocrReady) {
    blog(LOG_INFO, "[securecast][ocr] OCR worker initialized.");
  } else {
    blog(LOG_WARNING, "[securecast][ocr] OCR worker initialization failed.");
  }

  while (true) {
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> trackerGray;
    int width = 0;
    int height = 0;
    int stride = 0;
    int trackerW = 0;
    int trackerH = 0;
    uint64_t frameId = 0;
    OcrScaleTier tier = OcrScaleTier::Full;
    std::chrono::steady_clock::time_point enqueueTs{};

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
      trackerGray.swap(filter->ocrPendingTrackerGray);
      frameId = filter->ocrPendingFrameId;
      width = filter->ocrPendingWidth;
      height = filter->ocrPendingHeight;
      stride = filter->ocrPendingStride;
      trackerW = filter->ocrPendingTrackerW;
      trackerH = filter->ocrPendingTrackerH;
      tier = filter->ocrPendingTier;
      enqueueTs = filter->ocrPendingEnqueueTs;

      filter->ocrPendingFrameId = 0;
      filter->ocrPendingWidth = 0;
      filter->ocrPendingHeight = 0;
      filter->ocrPendingStride = 0;
      filter->ocrPendingTrackerW = 0;
      filter->ocrPendingTrackerH = 0;
      filter->ocrPendingTier = OcrScaleTier::Full;
      filter->ocrFramePending = false;
    }

    // dHash 캐시 무효화 요청 처리 — GUI 스레드(securecast_update)가 플래그를
    // 세우고, 워커 스레드가 여기서 안전하게 소비한다. ocrReady 여부와 무관하게
    // 실행한다.
    if (filter->ocrClearCachePending.exchange(false,
                                              std::memory_order_acq_rel)) {
      if (filter->ocrEngine)
        filter->ocrEngine->clearDHashCache();
    }

    if (!ocrReady) {
      // init 영구 실패: 회복 불가능. 상태 전파 및 idle 복원 후 스레드 종료.
      // continue를 쓰면 렌더-워커 간 4fps 공회전이 영원히 반복된다.
      filter->ocrWorkerRunning.store(false, std::memory_order_release);
      filter->ocrIsDown.store(true, std::memory_order_release);
      blog(LOG_WARNING, "[SecureCast] OCR engine init failed permanently — "
                        "worker terminating");
      filter->ocrWorkerIdle.store(true, std::memory_order_release);
      break;
    }
    if (pixels.empty() || width <= 0 || height <= 0 || stride <= 0) {
      // 일시적 빈 프레임: idle 복원 후 다음 프레임 대기.
      filter->ocrWorkerIdle.store(true, std::memory_order_release);
      continue;
    }

    // 2-C: 적응형 스케일 — 직전 사이클 평균 라인 높이를 14~20px 대역으로 맞춤.
    // 첫 사이클(avgH=0): 1440p+ 0.5× 다운, 그 외엔 1.0×(풀해상도)로 안전.
    //   ※ 이전엔 1080p- 2× 업스케일 → 3840×2160 폭주 → adaptScale 진동.
    constexpr float kOcrTargetH = 16.0f; // Windows.Media.Ocr 최적 구간 중간값
    constexpr float kScaleMin = 0.5f;
    constexpr float kScaleMax = 2.5f;
    const float avgLineH =
        filter->ocrEngine ? filter->ocrEngine->averageLineHeight() : 0.0f;

    const bool is1440p = (width >= 2560 && height >= 1440);
    // preScaled: GPU가 이미 OCR-목표 해상도로 다운한 입력. CPU adaptScale 금지.
    const bool preScaled = (tier != OcrScaleTier::Full);
    float adaptScale = 1.0f;
    if (!preScaled) {
      if (avgLineH > 0.0f) {
        adaptScale =
            std::max(kScaleMin, std::min(kOcrTargetH / avgLineH, kScaleMax));
      } else {
        // 첫 사이클 fallback. 1440p+만 GPU 다운 없을 때 CPU 0.5× 시도, 그 외는
        // 풀해상도 그대로 (2× 업스케일 폭주 차단).
        adaptScale = is1440p ? 0.5f : 1.0f;
      }
    }

    // 스케일 적용 (0.5× 다운 or 2× 업만 SIMD; 그 외 스칼라 bilinear)
    std::vector<uint8_t> scaledBuf;
    float coordScale = 1.0f;
    const uint8_t *ocrPx = pixels.data();
    int ocrW2 = width;
    int ocrH2 = height;
    int ocrStride2 = stride;

    if (adaptScale < 1.0f) {
      // 다운스케일: 0.5× SIMD (정수 나눗셈, 정확히 2×)
      const int dsW = width / 2;
      const int dsH = height / 2;
      if (dsW >= 640 && dsH >= 360) {
        scaledBuf.resize((size_t)dsW * dsH * 4);
        downsample2x_bgra_simd(pixels.data(), width, height, stride,
                               scaledBuf.data(), dsW * 4);
        ocrPx = scaledBuf.data();
        ocrW2 = dsW;
        ocrH2 = dsH;
        ocrStride2 = dsW * 4;
        coordScale = 1.0f / adaptScale; // ≈ 2.0
      }
    } else if (adaptScale > 1.0f) {
      // 업스케일
      const int upW = static_cast<int>(width * adaptScale + 0.5f);
      const int upH = static_cast<int>(height * adaptScale + 0.5f);
      if (upW <= 4096 && upH <= 4096) {
        scaledBuf.resize((size_t)upW * upH * 4);
        if (adaptScale == 2.0f) {
          upsample2x_bgra_simd(pixels.data(), width, height, stride,
                               scaledBuf.data(), upW * 4);
        } else {
          // 일반 bilinear (adaptScale ≠ 2.0)
          for (int dy = 0; dy < upH; ++dy) {
            const float sy = (dy + 0.5f) * height / upH - 0.5f;
            const int sy0 =
                std::max(0, std::min(static_cast<int>(sy), height - 1));
            const int sy1 = std::min(sy0 + 1, height - 1);
            const float fy = sy - static_cast<float>(sy0);
            for (int dx = 0; dx < upW; ++dx) {
              const float sx = (dx + 0.5f) * width / upW - 0.5f;
              const int sx0 =
                  std::max(0, std::min(static_cast<int>(sx), width - 1));
              const int sx1 = std::min(sx0 + 1, width - 1);
              const float fx = sx - static_cast<float>(sx0);
              const uint8_t *p00 =
                  pixels.data() + (ptrdiff_t)sy0 * stride + sx0 * 4;
              const uint8_t *p01 =
                  pixels.data() + (ptrdiff_t)sy0 * stride + sx1 * 4;
              const uint8_t *p10 =
                  pixels.data() + (ptrdiff_t)sy1 * stride + sx0 * 4;
              const uint8_t *p11 =
                  pixels.data() + (ptrdiff_t)sy1 * stride + sx1 * 4;
              uint8_t *d = scaledBuf.data() + (ptrdiff_t)dy * upW * 4 + dx * 4;
              for (int c = 0; c < 4; ++c) {
                float v = (1.0f - fy) * ((1.0f - fx) * p00[c] + fx * p01[c]) +
                          fy * ((1.0f - fx) * p10[c] + fx * p11[c]);
                d[c] = v < 0.0f     ? 0u
                       : v > 255.0f ? 255u
                                    : static_cast<uint8_t>(v);
              }
            }
          }
        }
        ocrPx = scaledBuf.data();
        ocrW2 = upW;
        ocrH2 = upH;
        ocrStride2 = upW * 4;
        coordScale = 1.0f / adaptScale;
      }
    }

    OcrAnalysisResult result =
        filter->ocrEngine->analyze_bgra_frame(ocrPx, ocrW2, ocrH2, ocrStride2);
    auto &ocrBoxes = result.boxes;

    // 좌표 복원 — tier별 정책:
    //   TwoThirds: OCR 박스(1280×720 좌표)를 원본 1920×1080 좌표로 복원하여
    //              트래커에 넘김. 트래커는 원본 gray 공간에서 동작.
    //   Half:      OCR 박스(half 좌표)를 그대로 유지. 트래커도 half gray 공간.
    //              렌더 시 trackerCoordScale_=2.0f로 복원.
    //   Full + adaptScale(coordScale!=1): 기존 CPU adaptScale 보정 그대로.
    if (tier == OcrScaleTier::TwoThirds) {
      // trackerGray가 원본 해상도이므로 OCR 박스를 원본 좌표로 올림.
      const float scaleX =
          static_cast<float>(trackerW) / static_cast<float>(width);
      const float scaleY =
          static_cast<float>(trackerH) / static_cast<float>(height);
      for (auto &b : ocrBoxes) {
        b.x = std::round(b.x * scaleX);
        b.y = std::round(b.y * scaleY);
        b.w = std::round(b.w * scaleX);
        b.h = std::round(b.h * scaleY);
      }
    } else if (!preScaled && coordScale != 1.0f) {
      // Full tier에서 CPU adaptScale을 사용한 경우만 역변환.
      for (auto &b : ocrBoxes) {
        b.x = std::round(b.x * coordScale);
        b.y = std::round(b.y * coordScale);
        b.w = std::round(b.w * coordScale);
        b.h = std::round(b.h * coordScale);
      }
    }

    // 트래커 등록: 워커 pop에서 이미 받은 trackerGray를 사용.
    //   - Full / TwoThirds: 원본 해상도 gray
    //   - Half: half 해상도 gray
    // trackerGray가 비어있으면(렌더 스레드 실패) BGRA→gray 폴백 변환.
    {
      std::vector<VtOcrBox> vtBoxes;
      vtBoxes.reserve(ocrBoxes.size());
      for (const auto &b : ocrBoxes)
        vtBoxes.push_back({b.type, b.x, b.y, b.w, b.h});

      if (!trackerGray.empty() && trackerW > 0 && trackerH > 0) {
        filter->trackerMgr.register_or_update_gray(vtBoxes, trackerGray.data(),
                                                   trackerW, trackerH);
      } else {
        // 폴백: trackerGray 미전달 시 OCR 입력에서 직접 gray 변환.
        std::vector<uint8_t> grayForTracker;
        VisualTrackerManager::bgra_to_gray(pixels.data(), width, height, stride,
                                           grayForTracker);
        filter->trackerMgr.register_or_update_gray(
            vtBoxes, grayForTracker.data(), width, height);
      }
    }

    const int boxCount = static_cast<int>(ocrBoxes.size());
    blog(LOG_DEBUG, "[securecast][ocr] OCR boxes: %d", boxCount);
    if (boxCount != filter->lastLoggedOcrCount) {
      blog(LOG_INFO, "[securecast][ocr] mask count changed: %d -> %d",
           filter->lastLoggedOcrCount, boxCount);
      filter->lastLoggedOcrCount = boxCount;
    }

    // 셀프힐링 — fullRecognitionRan==true일 때만 escalate 판정.
    // L1/L2 캐시 히트는 OCR 마비가 아니므로 streak 변경 없음.
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    const bool hasActiveBoxes = !filter->trackerMgr.active_boxes().empty();

    if (result.fullRecognitionRan) {
      if (result.effectiveLineCount == 0) {
        const int s =
            filter->ocrZeroLineStreak_.fetch_add(1, std::memory_order_acq_rel) +
            1;
        const OcrTierOverride curOv =
            filter->ocrOverrideTier_.load(std::memory_order_acquire);
        const OcrScaleTier curPolicy =
            filter->ocrPolicyTier_.load(std::memory_order_acquire);
        const OcrScaleTier curEff =
            SecureCastFilter::ocr_effective_tier(curPolicy, curOv);
        if (s >= 3 && curEff != OcrScaleTier::Full) {
          if (!hasActiveBoxes) {
            // Full 에스컬레이션 제거: Full 1080p는 ~1036ms로 링 버퍼(1000ms)를
            // 초과해 오히려 PII 노출 위험 증가. TwoThirds가 최대 안전 모드.
            const OcrTierOverride next = OcrTierOverride::TwoThirds;
            filter->ocrOverrideTier_.store(next, std::memory_order_release);
            filter->ocrZeroLineStreak_.store(0, std::memory_order_release);
            filter->ocrLastEscalateMs_.store(nowMs, std::memory_order_release);
            blog(LOG_WARNING,
                 "[SC-ocr] self-heal escalate eff=%d ov->%d (3 cycles zero "
                 "lines)",
                 static_cast<int>(curEff), static_cast<int>(next));
          } else {
            // 텍스트는 못 찾았지만 PII 트래커가 작동 중이므로 격상 보류
            filter->ocrZeroLineStreak_.store(0, std::memory_order_release);
          }
        }
      } else {
        // 정상 인식: streak 즉시 리셋.
        filter->ocrZeroLineStreak_.store(0, std::memory_order_release);
      }
    }

    // 60초 경과 시 override를 policy 방향으로 한 단계 완화.
    {
      const OcrTierOverride curOv =
          filter->ocrOverrideTier_.load(std::memory_order_acquire);
      const OcrScaleTier curPolicy =
          filter->ocrPolicyTier_.load(std::memory_order_acquire);
      const int64_t last =
          filter->ocrLastEscalateMs_.load(std::memory_order_acquire);
      if (curOv != OcrTierOverride::Auto && last > 0 && nowMs - last > 60000) {
        if (!hasActiveBoxes) {
          const OcrTierOverride relaxed =
              SecureCastFilter::relax_override(curOv, curPolicy);
          filter->ocrOverrideTier_.store(relaxed, std::memory_order_release);
          filter->ocrLastEscalateMs_.store(nowMs, std::memory_order_release);
          blog(LOG_INFO, "[SC-ocr] self-heal relax override %d->%d after 60s",
               static_cast<int>(curOv), static_cast<int>(relaxed));
        }
      }
    }

    // 진단 로깅: enqueue→완료까지 소요 ms를 누적, 100사이클마다 평균 출력.
    if (enqueueTs.time_since_epoch().count() != 0) {
      const auto procMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - enqueueTs)
                              .count();
      filter->ocrEnqueueLatencyAccumMs_ += procMs;
      if (++filter->ocrEnqueueLatencyCount_ >= 100) {
        const double avgMs =
            filter->ocrEnqueueLatencyAccumMs_ / filter->ocrEnqueueLatencyCount_;
        const OcrTierOverride curOv2 =
            filter->ocrOverrideTier_.load(std::memory_order_acquire);
        blog(LOG_INFO,
             "[SC-ocr] avg_ms=%.1f tier=%d(ov=%d) input=%dx%d effLines=%d "
             "boxes=%zu fullRan=%d",
             avgMs, static_cast<int>(tier), static_cast<int>(curOv2), ocrW2,
             ocrH2, result.effectiveLineCount, ocrBoxes.size(),
             static_cast<int>(result.fullRecognitionRan));
        filter->ocrEnqueueLatencyAccumMs_ = 0.0;
        filter->ocrEnqueueLatencyCount_ = 0;
      }
    }

    // 이 프레임은 OCR + tracker registration까지 완료됨. 렌더 스레드가
    // 같은 frameId를 가진 ring-buffer slot에 완료 표시를 붙이고, 완료되지
    // 않은 지연 슬롯은 송출하지 않는다.
    filter->lastCompletedOcrFrameId.store(frameId, std::memory_order_release);

    // back-pressure 해제: 다음 프레임 readback 허용
    filter->ocrWorkerIdle.store(true, std::memory_order_release);
  }

  blog(LOG_INFO, "[securecast][ocr] OCR worker stopped.");
}

static void start_ocr_worker(SecureCastFilter *filter) {
  if (!filter)
    return;

  bool expected = false;
  if (!filter->ocrWorkerRunning.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) {
    return;
  }

  filter->ocrWorkerThread =
      std::thread([filter]() { ocr_worker_loop(filter); });
}

// ================================================================
// [P1] Visual Tracker Thread — 30Hz NCC 추적
// ================================================================

static void tracker_thread_loop(SecureCastFilter *filter) {
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

  while (filter->trackerThreadRunning_.load(std::memory_order_acquire)) {
    std::vector<uint8_t> gray;
    int w = 0, h = 0;

    {
      std::unique_lock<std::mutex> lock(filter->trackerInputMutex_);
      filter->trackerInputCv_.wait(lock, [filter]() {
        return !filter->trackerThreadRunning_.load(std::memory_order_acquire) ||
               filter->trackerInputReady_;
      });
      if (!filter->trackerThreadRunning_.load(std::memory_order_acquire))
        break;

      // P0-4: gray 버퍼 수신 (BGRA→gray 변환은 렌더 스레드에서 완료)
      gray.swap(filter->trackerInputGray_);
      w = filter->trackerInputW_;
      h = filter->trackerInputH_;
      filter->trackerInputReady_ = false;
    }

    if (!gray.empty() && w > 0 && h > 0)
      filter->trackerMgr.update_all_gray(gray.data(), w, h);
  }
}

static void start_tracker_thread(SecureCastFilter *filter) {
  if (!filter || filter->trackerThreadRunning_.load(std::memory_order_acquire))
    return;
  filter->trackerThreadRunning_.store(true, std::memory_order_release);
  filter->trackerThread_ =
      std::thread([filter]() { tracker_thread_loop(filter); });
  blog(LOG_INFO, "[SC-tracker] Tracker thread started (30Hz NCC).");
}

static void stop_tracker_thread(SecureCastFilter *filter) {
  if (!filter || !filter->trackerThreadRunning_.load(std::memory_order_acquire))
    return;
  filter->trackerThreadRunning_.store(false, std::memory_order_release);
  filter->trackerInputCv_.notify_all();
  if (filter->trackerThread_.joinable())
    filter->trackerThread_.join();
  blog(LOG_INFO, "[SC-tracker] Tracker thread stopped.");
}

static void stop_ocr_worker(SecureCastFilter *filter) {
  if (!filter)
    return;

  filter->ocrWorkerRunning.store(false, std::memory_order_release);

  // 진행 중인 RecognizeAsync를 취소하여 .get() 블로킹을 즉시 해제한다.
  if (filter->ocrEngine)
    filter->ocrEngine->cancel_current();

  filter->ocrWorkerCv.notify_all();

  if (filter->ocrWorkerThread.joinable())
    filter->ocrWorkerThread.join();

  clear_pending_ocr_frame(filter);
}

// ================================================================
// Filter Lifecycle Callbacks
// ================================================================

// Panic 핫키 콜백 — 누를 때(pressed=true)만 토글. 해제 이벤트는 무시.
static void panic_hotkey_cb(void *data, obs_hotkey_id, obs_hotkey_t *,
                            bool pressed) {
  if (!pressed)
    return;
  auto *filter = static_cast<SecureCastFilter *>(data);
  if (filter->isDestroying.load(std::memory_order_acquire))
    return;
  bool next = !filter->panicMode.load(std::memory_order_relaxed);
  filter->panicMode.store(next, std::memory_order_relaxed);
  blog(LOG_INFO, "[SecureCast] Panic mode %s.", next ? "ON" : "OFF");
}

// OBS가 필터 메뉴/관리 UI에 표시할 이름.
// 인자 type_data는 obs_source_info::type_data 필드 — 우리는 안 씀.
static const char *securecast_get_name(void *type_data) {
  (void)type_data;
  return "SecureCast Privacy Masking";
}

// 사용자가 어떤 비디오 소스에 SecureCast 필터를 추가할 때 호출.
// settings는 OBS Properties UI에서 사용자가 입력한 값 (현재 미사용),
// context는 OBS가 만든 이 필터의 source 핸들.
//
// 반환값은 OBS가 보관하다가 이후 모든 콜백의 data 인자로 다시 넘겨준다.
static void *securecast_create(obs_data_t *settings, obs_source_t *context) {
  (void)settings;

  SecureCastFilter *filter = new SecureCastFilter();
  filter->context = context;
  filter->isActive = true;
  filter->isGameMode = false;
  filter->currentState = SecurityState::SAFE;
  filter->trackerAccumulator = 0.0f; // window_tracker tick throttle 누산기

  obs_log(LOG_INFO, "[SecureCast] Filter created.");
  // [핵심 해결] 그래픽 리소스 생성 시 반드시 그래픽 컨텍스트 진입 필요
  obs_enter_graphics();
#ifdef _WIN32
  filter->readback.initialize();
#endif
  obs_leave_graphics();

  // [C-3 수정] fullScreenBuffer 미사용 멤버 제거. 실제 해시 입력은
  // readbackBuffer.data()(슬롯 0)에서 가져옴.

#ifdef _WIN32
  // [Role D] 스트리머 전용 오버레이 HUD 시작 (OBS 캡처에서 자동 제외)
  if (!filter->overlay.create())
    blog(LOG_WARNING,
         "[SecureCast][D] OverlayWindow 생성 실패 — HUD 없이 계속.");
#endif

  blog(LOG_INFO, "Filter created (Role C: 2-Stage Gate Pipeline Active).");

#ifdef _WIN32
  filter->winListener.start();
  // CPU 샘플링 기준점 초기화 (첫 샘플에서 diff가 0이 되지 않도록)
  GetSystemTimes(&filter->prevIdleTime, &filter->prevKernelTime,
                 &filter->prevUserTime);
#endif

  // Panic 핫키 등록 (Ctrl+Shift+F12 기본 바인딩)
  filter->panicHotkeyId = obs_hotkey_register_frontend(
      "securecast_panic_toggle", obs_module_text("PanicButton"),
      panic_hotkey_cb, filter);
  if (filter->panicHotkeyId != OBS_INVALID_HOTKEY_ID) {
    obs_data_t *combo = obs_data_create();
    obs_data_array_t *arr = obs_data_array_create();
    obs_data_set_bool(combo, "control", true);
    obs_data_set_bool(combo, "shift", true);
    obs_data_set_bool(combo, "alt", false);
    obs_data_set_string(combo, "key", "OBS_KEY_F12");
    obs_data_array_push_back(arr, combo);
    obs_hotkey_load(filter->panicHotkeyId, arr);
    obs_data_array_release(arr);
    obs_data_release(combo);
    blog(LOG_INFO, "[SecureCast] Panic hotkey registered (Ctrl+Shift+F12).");
  }

#ifdef _WIN32
  // [Role D] 드래그 블러 선택 핫키 등록 (Ctrl+Shift+B)
  filter->selectHotkeyId = obs_hotkey_register_frontend(
      "securecast_select_blur", obs_module_text("SelectBlurRegion"),
      [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
        if (!pressed)
          return;
        auto *f = static_cast<SecureCastFilter *>(data);
        if (f->isDestroying.load(std::memory_order_acquire))
          return;
        if (f->selectionOverlay.isActive()) {
          f->selectionOverlay.cancel(); // 두 번 누르면 취소
          return;
        }
        f->selectionOverlay.start([f](BlurRect rect) {
          // 모니터 픽셀 좌표 → 소스 픽셀 좌표 변환
          HMONITOR hmon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
          MONITORINFO mi{};
          mi.cbSize = sizeof(mi);
          GetMonitorInfo(hmon, &mi);
          int monW = mi.rcMonitor.right - mi.rcMonitor.left;
          int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
          int monL = mi.rcMonitor.left;
          int monT = mi.rcMonitor.top;
          uint32_t srcW = f->lastSourceW.load(std::memory_order_acquire);
          uint32_t srcH = f->lastSourceH.load(std::memory_order_acquire);
          if (monW > 0 && monH > 0 && srcW > 0 && srcH > 0) {
            rect.x = (int)((float)(rect.x - monL) / monW * srcW);
            rect.y = (int)((float)(rect.y - monT) / monH * srcH);
            rect.width = (int)((float)rect.width / monW * srcW);
            rect.height = (int)((float)rect.height / monH * srcH);
            MaskPayload snapshot{};
            {
              std::lock_guard<std::mutex> lock(f->settingsMutex);
              f->manualBlurMask.rectCount = 0;
              f->manualBlurMask.rects[f->manualBlurMask.rectCount++] = rect;
              snapshot = f->manualBlurMask;
            }
            save_manual_rects(f, snapshot);
            blog(LOG_INFO,
                 "[SecureCast][D] Manual rect replaced. scaled=(%d,%d %dx%d)",
                 rect.x, rect.y, rect.width, rect.height);
          } else {
            blog(LOG_WARNING,
                 "[SecureCast][D] Manual rect skipped: source dimensions "
                 "unavailable (srcW=%u, srcH=%u)",
                 srcW, srcH);
          }
        });
      },
      filter);
  if (filter->selectHotkeyId != OBS_INVALID_HOTKEY_ID) {
    obs_data_t *combo = obs_data_create();
    obs_data_array_t *arr = obs_data_array_create();
    obs_data_set_bool(combo, "control", true);
    obs_data_set_bool(combo, "shift", true);
    obs_data_set_bool(combo, "alt", false);
    obs_data_set_string(combo, "key", "OBS_KEY_B");
    obs_data_array_push_back(arr, combo);
    obs_hotkey_load(filter->selectHotkeyId, arr);
    obs_data_array_release(arr);
    obs_data_release(combo);
    blog(LOG_INFO, "[SecureCast] Select hotkey registered (Ctrl+Shift+B).");
  }
#endif

  // HLSL 셰이더 컴파일 (그래픽스 컨텍스트 필요)
  obs_enter_graphics();
  {
    char *effect_path = obs_module_file("securecast_blur.effect");
    if (effect_path) {
      filter->blurEffect = gs_effect_create_from_file(effect_path, nullptr);
      bfree(effect_path);
    }
  }
  {
    // Tier 1: GPU gray readback용 downsample.effect GrayDownsample 기법
    char *ds_path = obs_module_file("downsample.effect");
    if (ds_path) {
      filter->trackerGrayEffect_ = gs_effect_create_from_file(ds_path, nullptr);
      bfree(ds_path);
    }
  }
  {
    // OCR 전용 Mitchell+USM 다운스케일 셰이더. 미로드 시 compute_policy()가
    // Full을 반환해 풀해상도 OCR로 안전 폴백.
    char *ocr_ds_path = obs_module_file("downsample_ocr.effect");
    if (ocr_ds_path) {
      filter->ocrDownsampleEffect_ =
          gs_effect_create_from_file(ocr_ds_path, nullptr);
      bfree(ocr_ds_path);
    }
  }
  obs_leave_graphics();
  if (!filter->blurEffect)
    blog(LOG_WARNING, "[SecureCast] blur effect load failed; falling back to "
                      "solid blackout.");
  else
    blog(LOG_INFO, "[SecureCast] blur effect loaded.");
  if (!filter->trackerGrayEffect_)
    blog(LOG_WARNING, "[SecureCast] downsample effect load failed; tracker "
                      "uses CPU gray path.");
  else
    blog(LOG_INFO,
         "[SecureCast] downsample effect loaded (Tier 1 GPU gray active).");
  if (!filter->ocrDownsampleEffect_)
    blog(LOG_WARNING, "[SecureCast] downsample_ocr.effect load failed; OCR "
                      "uses full-res input.");
  else
    blog(LOG_INFO, "[SecureCast] downsample_ocr.effect loaded (Mitchell+USM "
                   "OCR path active).");

  return filter;
}

// 필터 인스턴스 정리. OBS는 이 시점 이후 동일 data 포인터를 다시 안 넘긴다.
static void securecast_destroy(void *data) {
  SecureCastFilter *filter = static_cast<SecureCastFilter *>(data);

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
  stop_tracker_thread(filter); // tracker thread 먼저 중지 (trackerMgr 공유)
  stop_ocr_worker(filter);
  filter->trackerMgr.clear(); // OCR worker 종료 후 tracker 정리
  filter->mockWorker.stop();

  obs_enter_graphics();
  destroy_ocr_stage_surface(filter);
  // Tier 1: GPU gray readback 리소스 해제
  if (filter->trackerGrayStage_) {
    gs_stagesurface_destroy(filter->trackerGrayStage_);
    filter->trackerGrayStage_ = nullptr;
  }
  if (filter->trackerGrayRender_) {
    gs_texrender_destroy(filter->trackerGrayRender_);
    filter->trackerGrayRender_ = nullptr;
  }
  if (filter->trackerGrayEffect_) {
    gs_effect_destroy(filter->trackerGrayEffect_);
    filter->trackerGrayEffect_ = nullptr;
  }
  if (filter->ocrDownsampleEffect_) {
    gs_effect_destroy(filter->ocrDownsampleEffect_);
    filter->ocrDownsampleEffect_ = nullptr;
  }
  destroy_last_safe_render(filter);
#ifdef _WIN32
  // 1-G: half-size OCR 다운스케일 리소스 해제
  destroy_ocr_down_stage(filter);
  // [C2-5 수정] shutdown 경로에서는 spin-wait가 없는 destroyImmediate() 사용.
  filter->readback.destroyImmediate();
#endif
  if (filter->blurEffect) {
    gs_effect_destroy(filter->blurEffect);
    filter->blurEffect = nullptr;
  }
  filter->ringBuffer.destroy(); // [NEW-2] graphics context 안에서 안전
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
static void securecast_video_render(void *data, gs_effect_t *effect) {
  (void)effect;

  SecureCastFilter *filter = static_cast<SecureCastFilter *>(data);

  // [Role D] isActive는 GUI 스레드(update)에서도 쓸 수 있으므로 settingsMutex로
  // 보호
  {
    std::lock_guard<std::mutex> lock(filter->settingsMutex);
    if (!filter->isActive) {
      obs_source_skip_video_filter(filter->context);
      return;
    }
  }

  // 상위 소스의 실제 해상도 가져오기
  obs_source_t *parent = obs_filter_get_parent(filter->context);
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
  // [Role D] 모니터→소스 좌표 변환용 캐시 갱신 (수동 드래그 블러 핫키에서 참조)
  filter->lastSourceW.store(w, std::memory_order_release);
  filter->lastSourceH.store(h, std::memory_order_release);

  // --- Step 1: 링 버퍼 지연 초기화 또는 해상도 변경 대응 ---
  if (!filter->ringBuffer.isInitialized()) {
    if (!filter->ringBuffer.initialize(w, h)) {
      render_solid_black_frame(w, h);
      return;
    }
    filter->lastCompletedOcrFrameId.store(0, std::memory_order_release);
    filter->unverifiedFrameLogCounter = 0;
    // 첫 초기화 시 AI 워커 + Tracker 스레드 시작
    filter->mockWorker.start(w, h, [filter](const MaskPayload &payload) {
      filter->maskChannel.publish(payload);
    });
    start_ocr_worker(filter);
    start_tracker_thread(filter);
    blog(LOG_INFO,
         "[securecast][ocr] Async OCR worker + 30Hz tracker thread started.");
  } else if (filter->ringBuffer.getWidth() != w ||
             filter->ringBuffer.getHeight() != h) {
    // [P1 수정] 소스 해상도가 바뀌면 텍스처를 재생성해야 화면 깨짐 및 크래시
    // 방지
    blog(LOG_INFO,
         "Resolution changed (%dx%d -> %dx%d). Reinitializing ring buffer.",
         filter->ringBuffer.getWidth(), filter->ringBuffer.getHeight(), w, h);

    // 1. 워커 중지 (OCR → Tracker → Mock 순서로 중지해야 trackerMgr race 없음)
    stop_ocr_worker(filter);
    stop_tracker_thread(filter);
    filter->mockWorker.stop();

    // 2. 링 버퍼 재구성
    filter->ringBuffer.destroy();
    destroy_last_safe_render(filter);
    destroy_ocr_stage_surface(filter);
#ifdef _WIN32
    destroy_ocr_down_stage(
        filter); // 1-G: 해상도 변경 시 half-size stagesurf 재생성
#endif
    if (!filter->ringBuffer.initialize(w, h)) {
      render_solid_black_frame(w, h);
      return;
    }
    filter->lastCompletedOcrFrameId.store(0, std::memory_order_release);
    filter->unverifiedFrameLogCounter = 0;

    // 3. 마스킹 큐 비우기
    MaskPayload dummy;
    while (filter->maskChannel.consume(dummy)) {
    }
    filter->lastMask = MaskPayload{};
    filter->trackerMgr.clear(); // 해상도 변경 시 기존 박스 좌표 무효화
    filter->trackerFrameSkip_ = 0;

    // 4. 새 해상도로 워커 재시작
    filter->mockWorker.start(w, h, [filter](const MaskPayload &payload) {
      filter->maskChannel.publish(payload);
    });
    start_ocr_worker(filter);
    start_tracker_thread(filter);
    blog(LOG_INFO,
         "[securecast][ocr] Async OCR worker restarted after resize.");

#ifdef _WIN32
    // [C-6 수정] 해상도 변경 시 readback 풀도 반드시 재구성
    // ocrW/ocrH 캡으로 인해 expectedBufferSize가 같아 resizePool이 누락되던
    // 버그 수정
    {
      int rW = ((int)w > 1920) ? 1920 : (int)w;
      int rH = ((int)h > 1080) ? 1080 : (int)h;
      filter->readback.destroyImmediate();
      filter->readback.initialize();
      std::vector<std::pair<int, int>> ss = {{64, 64}, {rW, rH}};
      filter->readback.resizePool(ss);
      filter->readbackBuffer.resize(
          (size_t)(64 * 64 * 4) + (size_t)(rW * rH * 4), 0);
      filter->fullScreenHash.reset();
      filter->health.reset();
      filter->readback.setForceReleasedFlag(true);
      blog(LOG_INFO,
           "[SecureCast] Readback pool rebuilt for new resolution %dx%d.", rW,
           rH);
    }
#endif
  }

  // --- Panic Mode: pushFrame 이전에 차단 ---
  // 패닉 중에는 링 버퍼를 파괴해 GPU 낭비를 막고,
  // 해제 직후 패닉 중 캡처된 프레임이 스트림에 유출되는 것을 방지한다.
  // ringBuffer.destroy()는 이미 파괴된 경우 no-op이라 매 프레임 호출해도 안전.
  if (filter->panicMode.load(std::memory_order_relaxed)) {
    filter->ringBuffer.destroy();
    destroy_last_safe_render(filter);
    filter->lastCompletedOcrFrameId.store(0, std::memory_order_release);
    filter->unverifiedFrameLogCounter = 0;

    gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);

    gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"),
                        0xFF000000);
    while (gs_effect_loop(solid, "Solid"))
      gs_draw_sprite(nullptr, 0, w, h);

    constexpr uint32_t BORDER = 6;
    gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"),
                        0xFFFF0000);
    while (gs_effect_loop(solid, "Solid")) {
      gs_matrix_push();
      gs_matrix_identity();
      gs_draw_sprite(nullptr, 0, w, BORDER);
      gs_matrix_pop();
      gs_matrix_push();
      gs_matrix_identity();
      gs_matrix_translate3f(0.0f, (float)(h - BORDER), 0.0f);
      gs_draw_sprite(nullptr, 0, w, BORDER);
      gs_matrix_pop();
      gs_matrix_push();
      gs_matrix_identity();
      gs_draw_sprite(nullptr, 0, BORDER, h);
      gs_matrix_pop();
      gs_matrix_push();
      gs_matrix_identity();
      gs_matrix_translate3f((float)(w - BORDER), 0.0f, 0.0f);
      gs_draw_sprite(nullptr, 0, BORDER, h);
      gs_matrix_pop();
    }
    return;
  }

  // --- Step 2: OCR 박스 스냅샷 갱신 + 현재 프레임을 Ring Buffer HEAD에 Push
  // --- OBS 소스 내부 버퍼링으로 obs_source_video_render()가 반환하는 픽셀은
  // 실제 DWM 쿼리보다 ~1프레임 뒤처진다.
  // captureWindowList(직전 프레임에서 저장한 DWM 좌표)를 스냅샷으로 쓰면
  // 픽셀 내용과 마스크 위치가 정확히 동기화된다.

  // [OCR 박스 동기화] trackerMgr.active_boxes()를 slotHead에 봉인.
  // windowSnapshot 패턴과 동일: 이 프레임 텍스처와 박스 좌표를 함께 저장.
  // 60프레임 후 송출 시 outputSlot->ocrBoxSnapshot 사용 → 1프레임도 어긋나지
  // 않음.
  {
    OcrBoxSnapshot nextSnap{};
    const float tScale = filter->trackerCoordScale_;
    const auto activeBxs = filter->trackerMgr.active_boxes();

    // 1. 현재 살아있는 박스 추가 (TTL 최대로 리셋)
    for (const auto &tb : activeBxs) {
      if (nextSnap.count >= SC_MAX_SLOT_OCR_BOXES)
        break;
      BlurRect r{};
      r.x = static_cast<int>(tb.x * tScale);
      r.y = static_cast<int>(tb.y * tScale);
      r.width = static_cast<int>(tb.w * tScale);
      r.height = static_cast<int>(tb.h * tScale);
      r.type = 0;
      if (r.width > 0 && r.height > 0) {
        nextSnap.rects[nextSnap.count] = r;
        nextSnap.ticksRemaining[nextSnap.count] = SC_OCR_LINGER_FRAMES;
        nextSnap.count++;
      }
    }

    // 2. Lingering (T5): NCC lost 후에도 직전 스냅샷 박스를 N프레임 유지.
    //    windowSnapshot의 lingeringWindows 패턴과 동일한 목적.
    for (int li = 0; li < filter->liveOcrSnapshot_.count; ++li) {
      const int ttl = filter->liveOcrSnapshot_.ticksRemaining[li];
      if (ttl <= 0)
        continue;
      const BlurRect &lr = filter->liveOcrSnapshot_.rects[li];
      // active 박스가 이미 같은 영역을 커버하면 lingering 불필요
      bool covered = false;
      for (int ai = 0; ai < nextSnap.count; ++ai) {
        const BlurRect &ar = nextSnap.rects[ai];
        // 두 박스의 중심 간 거리 < 각 반폭 합산 → 겹침으로 판정
        int dx = abs((lr.x + lr.width / 2) - (ar.x + ar.width / 2));
        int dy = abs((lr.y + lr.height / 2) - (ar.y + ar.height / 2));
        if (dx < (lr.width + ar.width) / 2 &&
            dy < (lr.height + ar.height) / 2) {
          covered = true;
          break;
        }
      }
      if (!covered && nextSnap.count < SC_MAX_SLOT_OCR_BOXES) {
        nextSnap.rects[nextSnap.count] = lr;
        nextSnap.ticksRemaining[nextSnap.count] = ttl - 1;
        nextSnap.count++;
      }
    }

    filter->liveOcrSnapshot_ = nextSnap;
  }

  uint64_t ts = obs_get_video_frame_time();
#ifdef _WIN32
  filter->ringBuffer.pushFrame(
      ts, filter->context, &filter->captureWindowList,
      filter->lastSubmittedOcrFrameId.load(std::memory_order_relaxed),
      &filter->liveOcrSnapshot_);
  // push 이후에 DWM 갱신 → 다음 프레임의 captureWindowList로 저장
  // [Fix #3-B] 사라진 hwnd를 render 경로에서 즉시 lingering에 등록
  //   → tick(slow-scan)보다 훨씬 빠르게 잔영을 보장
  TrackedWindowList before = filter->windowList;
  sc_update_tracked_bounds(&filter->windowList);
  for (int i = 0; i < before.count; ++i) {
    HWND ph = before.items[i].hwnd;
    bool stillThere = false;
    for (int j = 0; j < filter->windowList.count; ++j) {
      if (filter->windowList.items[j].hwnd == ph) {
        stillThere = true;
        break;
      }
    }
    if (!stillThere)
      register_lingering_window(filter, before.items[i]);
  }
  filter->captureWindowList = filter->windowList;
#else
  filter->ringBuffer.pushFrame(
      ts, filter->context,
      filter->lastSubmittedOcrFrameId.load(std::memory_order_relaxed),
      &filter->liveOcrSnapshot_);
#endif

  // --- Step 3: maskChannel drain (OCR 좌표는 Visual Tracker가 직접 관리) ---
  // maskChannel은 더 이상 OCR 박스 전달에 사용하지 않는다.
  // lastMask는 health.shouldReset() 경로의 풀스크린 비상 블랙아웃 전용으로만
  // 남긴다.
  {
    MaskPayload drainMask{};
    filter->maskChannel.consume(drainMask); // 채널이 포화되지 않도록 드레인
  }

  // [THREAD-SAFE] currentState 갱신 — tracker + blacklist 기반
  MaskPayload blacklistSnapshot;
  {
    std::lock_guard<std::mutex> lock(filter->blacklistMutex);
    blacklistSnapshot = filter->blacklistMask;
  }
  const bool hasTrackerBoxes = !filter->trackerMgr.active_boxes().empty();

  SecurityState newState;
  {
    std::lock_guard<std::mutex> lock(filter->settingsMutex);
    // [Role D] 수동/알림 블러 활성도 PARTIAL 판정에 포함
    bool manualOrNotifActive =
        filter->manualBlurMask.rectCount > 0 || filter->notifBlurActive;
    if (filter->health.isCritical() ||
        filter->ocrIsDown.load(std::memory_order_acquire)) {
      filter->currentState = SecurityState::RISK;
    } else if (hasTrackerBoxes || filter->lastMask.rectCount > 0 ||
               blacklistSnapshot.rectCount > 0 || manualOrNotifActive) {
      filter->currentState = SecurityState::PARTIAL;
    } else {
      filter->currentState = SecurityState::SAFE;
    }
    newState = filter->currentState;
  }
#ifdef _WIN32
  // [Role D] 오버레이 HUD에 상태 동기화 (PostMessage → thread-safe,
  // non-blocking)
  filter->overlay.setState(newState);
#endif

#ifdef _WIN32
  // ---------------------------------------------------------
  // [C-5 수정] Phase 1(Collect+Hash) 도중에 돌려 delayedSlot early-return 앞에
  // 실행. 이로써 첫 N프레임 동안도 GPU 수확 시도가 이루어짐. Phase
  // 2(Enqueue)+3(Submit)은 최신 분석 슬롯 확보 후에 수행 (아래 참조).
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
  // [DORMANT] enqueueCopy / submitFrame을 호출하는 경로가 없어 m_pendingCount=0
  // 고착 → tryCollectPreviousFrame()은 항상 PENDING을 반환하므로
  // OK / SOFT_RECOVERED 분기는 실행되지 않는다.
  // 활성화하려면 video_render에서 enqueueCopy → submitFrame 호출 경로를
  // 연결해야 한다.
  CollectResult sc_collected = filter->readback.tryCollectPreviousFrame();
  if (sc_collected == CollectResult::OK ||
      sc_collected == CollectResult::SOFT_RECOVERED) {
    if (sc_collected == CollectResult::SOFT_RECOVERED) {
      filter->health
          .onForceRelease(); // [F3 Fix] 자체 소프트 복구 시 onForceRelease()를
                             // 호출하여 가짜 CRITICAL 예방
    } else {
      filter->health.onCollectSuccess();
    }
    bool forceCheck = filter->readback.wasForceReleased();
    uint8_t *fullBuf = filter->readbackBuffer.data();
    if (filter->readback.readStagingBuffer(0, fullBuf, 64 * 4, 64)) {
      BlurRect gridBox = {0, 0, 64, 64, 0};
      bool screenChanged =
          forceCheck ||
          filter->fullScreenHash.hasChanged(fullBuf, 64 * 64 * 4, 12, &gridBox);
      if (screenChanged) {
        if (forceCheck)
          gridBox = {0, 0, 64, 64, 0};
        blog(LOG_INFO, "[SecureCast] Change detected! BBox:(%d,%d %dx%d)",
             gridBox.x, gridBox.y, gridBox.width, gridBox.height);
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
    // PENDING(m_pendingCount==0)은 파이프라인 미사용 상태이므로 헬스 실패로
    // 집계하지 않는다. FAILED만 실제 D3D11 오류이므로 헬스에 반영한다.
    if (sc_collected == CollectResult::FAILED) {
      filter->health.onCollectFailure();
    }
    if (filter->readback.isPipelineFull()) {
      // [C2-3 수정] static → 멤버 변수 사용
      if (filter->logStallCount++ % 30 == 0)
        blog(LOG_WARNING, "[SecureCast] Pipeline FULL, GPU not responding.");
    }
  }
#endif // _WIN32 Phase1 end

  // --- Step 4~5: N프레임 지연된 슬롯 꺼내기 ---
  const FrameRingBuffer::Slot *delayedSlot =
      filter->ringBuffer.peekDelayedSlot();
  const FrameRingBuffer::Slot *analysisSlot =
      filter->ringBuffer.peekSlotAtOffset(1);

  // --- Step 5a: 30Hz Tracker readback + OCR 제출 ---
  // Tracker: 2프레임마다 GPU readback → tracker thread에 swap 전달 (30Hz NCC)
  // OCR:     tracker readback과 동일 프레임 중 ocrWorkerIdle일 때만 제출
  // (~4fps) update_all()은 tracker thread에서 실행 — 렌더 스레드 블로킹 없음.
  ++filter->trackerFrameSkip_;
  gs_texture_t *analysisTex =
      analysisSlot ? analysisSlot->getTexture() : nullptr;
  if (analysisTex && filter->trackerFrameSkip_ >= 2) {
    filter->trackerFrameSkip_ = 0;

    // --- [OCR 다운스케일 tier 결정] ---
    // policyTier: 해상도 + 로드된 effect에 따라 결정.
    //   Half      : ≥1440p (downsample.effect bilinear 필요)
    //   TwoThirds : ≥1080p AND ocrDownsampleEffect_ 로드됨
    //   Full      : 그 외 또는 effect 미로드 시 안전 폴백.
    // overrideTier: 셀프힐링이 강제하는 안전 모드.
    // effective = min(policy, override)로 더 안전한 쪽 선택.
#ifdef _WIN32
    auto compute_policy = [&]() -> OcrScaleTier {
      if (w >= 2560 && h >= 1440 && filter->trackerGrayEffect_)
        return OcrScaleTier::Half;
      if (w >= 1920 && h >= 1080 && filter->ocrDownsampleEffect_)
        return OcrScaleTier::TwoThirds;
      return OcrScaleTier::Full;
    };
    const OcrScaleTier policyTier = compute_policy();
    filter->ocrPolicyTier_.store(policyTier, std::memory_order_release);
    const OcrTierOverride ov =
        filter->ocrOverrideTier_.load(std::memory_order_acquire);
    const OcrScaleTier ocrTier =
        SecureCastFilter::ocr_effective_tier(policyTier, ov);

    // tier 전환 감지 → trackerMgr.clear() (이전 tier 박스의 좌표계 불일치 차단)
    const OcrScaleTier prevTier =
        filter->lastEffectiveTier_.exchange(ocrTier, std::memory_order_acq_rel);
    if (prevTier != ocrTier) {
      filter->trackerMgr.clear();
      blog(LOG_INFO, "[SC-ocr] tier transition %d->%d -> trackerMgr cleared",
           static_cast<int>(prevTier), static_cast<int>(ocrTier));
    }
#else
    constexpr OcrScaleTier ocrTier = OcrScaleTier::Full;
#endif

    // OCR 슬롯 claim. exchange(false): 이전 값이 true(idle)면 claim 성공.
    const bool claimedOcr =
        filter->ocrWorkerRunning.load(std::memory_order_acquire) &&
        filter->ocrWorkerIdle.exchange(false, std::memory_order_acq_rel);
    if (claimedOcr) {
      filter->lastSubmittedOcrFrameId.store(analysisSlot->frameId,
                                            std::memory_order_relaxed);
      // analysisSlot은 방금 제출 예정이므로 자신을 대표 ID로 설정.
      // OCR readback이 실패해도 dependentOcrFrameId가 살아있으면 render는
      // last-safe로 freeze. 새 PII가 무방비로 송출되는 일은 없음.
      const_cast<FrameRingBuffer::Slot *>(analysisSlot)->dependentOcrFrameId =
          analysisSlot->frameId;
    }

    std::vector<uint8_t> bgraPixels;
    int stride = 0;

    // Tier 1: GPU gray readback (30Hz, 2MB) — 기존 8MB BGRA readback 대체
    // GS_R8 미지원 시 false 반환 → CPU bgra_to_gray 폴백 경로 사용.
    std::vector<uint8_t> grayPixels;
    bool grayOk = read_tracker_gray_gpu(filter, analysisTex, w, h, grayPixels);
    if (!grayOk) {
      // 폴백: 전체 BGRA readback → CPU gray 변환
      if (read_texture_bgra_to_cpu(filter, analysisTex, w, h, bgraPixels,
                                   stride)) {
        VisualTrackerManager::bgra_to_gray(bgraPixels.data(), (int)w, (int)h,
                                           stride, grayPixels);
        grayOk = true;
      }
    }

    // tier별 트래커 입력 + 워커용 trackerGray 준비.
    //   Half(≥1440p): 트래커도 half 공간. trackerCoordScale_=2.0f.
    //   TwoThirds(1080p) / Full: 트래커는 원본 공간. trackerCoordScale_=1.0f.
    // copy 먼저, swap 나중 — swap 후 빈 벡터 복사를 방지.
    std::vector<uint8_t> trackerGrayForWorker;
    int trkW = 0, trkH = 0;
    if (grayOk) {
      if (ocrTier == OcrScaleTier::Half) {
        std::vector<uint8_t> halfGray;
        int hw = 0, hh = 0;
        VisualTrackerManager::downsample_2x_into(grayPixels.data(), (int)w,
                                                 (int)h, halfGray, hw, hh);
        if (claimedOcr)
          trackerGrayForWorker = halfGray; // [1] copy 먼저
        filter->trackerCoordScale_ = 2.0f;
        {
          std::lock_guard<std::mutex> lock(filter->trackerInputMutex_);
          filter->trackerInputGray_.swap(halfGray); // [2] swap 나중
          filter->trackerInputW_ = hw;
          filter->trackerInputH_ = hh;
          filter->trackerInputReady_ = true;
        }
        trkW = hw;
        trkH = hh;
      } else {
        if (claimedOcr)
          trackerGrayForWorker = grayPixels; // [1] copy 먼저
        filter->trackerCoordScale_ = 1.0f;
        {
          std::lock_guard<std::mutex> lock(filter->trackerInputMutex_);
          filter->trackerInputGray_.swap(grayPixels); // [2] swap 나중
          filter->trackerInputW_ = (int)w;
          filter->trackerInputH_ = (int)h;
          filter->trackerInputReady_ = true;
        }
        trkW = (int)w;
        trkH = (int)h;
      }
      filter->trackerInputCv_.notify_one();
    }

    // OCR 픽셀 확보 → submit_ocr_frame.
    if (claimedOcr) {
      std::vector<uint8_t> ocrPixels;
      int ocrW = static_cast<int>(w), ocrH = static_cast<int>(h), ocrStride = 0;
      bool ocrSubmitted = false;

#ifdef _WIN32
      if (ocrTier != OcrScaleTier::Full) {
        uint32_t outDw = 0, outDh = 0;
        if (read_texture_bgra_scaled_gpu(filter, analysisTex, w, h, ocrTier,
                                         ocrPixels, ocrStride, &outDw,
                                         &outDh)) {
          ocrW = static_cast<int>(outDw);
          ocrH = static_cast<int>(outDh);
          ocrSubmitted = true;
        }
      }
#endif
      if (!ocrSubmitted) {
        // Full tier 또는 scaled readback 실패: 풀해상도 BGRA로 제출.
        if (!bgraPixels.empty()) {
          ocrPixels = bgraPixels;
          ocrStride = static_cast<int>(w) * 4;
          ocrSubmitted = true;
        } else if (read_texture_bgra_to_cpu(filter, analysisTex, w, h,
                                            ocrPixels, ocrStride)) {
          ocrSubmitted = true;
        }
      }

      if (ocrSubmitted) {
        submit_ocr_frame(filter, std::move(ocrPixels), ocrW, ocrH, ocrStride,
                         analysisSlot->frameId, std::move(trackerGrayForWorker),
                         trkW, trkH, ocrTier);
        if (++filter->trackerLogCounter >= 150) {
          filter->trackerLogCounter = 0;
          blog(LOG_INFO, "[SC-tracker] active=%zu",
               filter->trackerMgr.active_boxes().size());
        }
      } else {
        // 본 pass가 claim 했지만 픽셀 확보 실패 → idle 복원 (다음 pass가 시도).
        filter->ocrWorkerIdle.store(true, std::memory_order_release);
      }
    }
    // claimedOcr==false인 경우: 워커가 점유 중이므로 idle 슬롯을 건드리지 않음.
  }

  // --- Step 5b: 분석 완료 프레임만 그리기 ---
  // delayedSlot이 의존하는 대표 OCR 프레임이 완료 표시를 받았는지 확인한다.
  // 완료되지 않았다면 원본 송출 금지. 마스크까지 합성해둔 마지막 안전 출력
  // 프레임을 재송출(Freeze)한다.
  uint64_t safeWatermarkId =
      filter->lastCompletedOcrFrameId.load(std::memory_order_acquire);
  uint64_t delayedId = delayedSlot ? delayedSlot->frameId : 0;
  bool isSafeToRender = (safeWatermarkId > 0) && delayedSlot &&
                        (delayedSlot->dependentOcrFrameId <= safeWatermarkId);

  if (!delayedSlot || !delayedSlot->getTexture() || !isSafeToRender) {
    if (++filter->unverifiedFrameLogCounter >= 60) {
      filter->unverifiedFrameLogCounter = 0;
      blog(LOG_WARNING,
           "[SecureCast][gate] delayed frame %" PRIu64
           " dependent on OCR %" PRIu64 " not analyzed (watermark %" PRIu64
           "); freezing last safe output",
           delayedId, delayedSlot ? delayedSlot->dependentOcrFrameId : 0,
           safeWatermarkId);
    }
    if (!render_last_safe_frame(filter, w, h))
      render_solid_black_frame(w, h);
    return;
  }
  filter->unverifiedFrameLogCounter = 0;

  const FrameRingBuffer::Slot *outputSlot = delayedSlot;
  gs_texture_t *outputTex = outputSlot->getTexture();

  // --- 마스킹 오버레이 ---
  // Role A: outputSlot->windowSnapshot (프레임 캡처 시점의 창 위치 → 프레임과
  // 동기화됨) Role B/C: lastMask (AI/MockAI 결과)
  // [Fix #3-D] capacity: manualBlur(32) + lastMask(32) + windowSnapshot×2(32)
  //            + lingering(16) + trackers(8) + notif/여유(8) = 128
  // MAX_TRACKERS는 VisualTrackerManager 클래스 static 상수이므로
  // securecast-filter.cpp에서는 직접 사용 불가. 실제 값(8)을 리터럴로 대체.
  static constexpr int kMaxTrackerSlots = 8;
  BlurRect all_rects[SC_MAX_BLUR_RECTS * 2 + SC_MAX_TRACKED_WINDOWS * 2 +
                     SC_MAX_LINGERING + kMaxTrackerSlots + 8];
  int all_count = 0;

#ifdef _WIN32
  // N프레임 전 스냅샷 (현재 렌더링 중인 지연 프레임과 동기화)
  for (int i = 0; i < outputSlot->windowSnapshot.count &&
                  all_count < (int)(sizeof(all_rects) / sizeof(all_rects[0]));
       i++) {
    BlurRect r =
        tracked_window_to_blur_rect(outputSlot->windowSnapshot.items[i], w, h);
    if (r.width > 0 && r.height > 0)
      all_rects[all_count++] = r;
  }
  // N-1프레임 전 스냅샷 합집합: 한 프레임 이동 궤적 전체를 마스킹 (빠른 드래그
  // 노출 방지)
  {
    const FrameRingBuffer::Slot *slotN1 =
        filter->ringBuffer.peekSlotAtOffset(SC_RING_BUFFER_SLOTS - 1);
    if (slotN1) {
      for (int i = 0;
           i < slotN1->windowSnapshot.count &&
           all_count < (int)(sizeof(all_rects) / sizeof(all_rects[0]));
           i++) {
        BlurRect r =
            tracked_window_to_blur_rect(slotN1->windowSnapshot.items[i], w, h);
        if (r.width > 0 && r.height > 0)
          all_rects[all_count++] = r;
      }
    }
  }
  // Lingering rects: 사라진 창의 N프레임 잔영 (ring buffer에 남은 과거 프레임
  // 커버)
  for (int li = 0; li < filter->lingeringCount &&
                   all_count < (int)(sizeof(all_rects) / sizeof(all_rects[0]));
       ++li) {
    BlurRect r =
        tracked_window_to_blur_rect(filter->lingeringWindows[li].window, w, h);
    if (r.width > 0 && r.height > 0)
      all_rects[all_count++] = r;
  }
#endif
  // OCR 박스 — 슬롯 캡처 시점의 스냅샷 사용 (프레임 ↔ 박스 위치 완벽 동기화)
  // ocrBoxSnapshot은 pushFrame 직전 trackerMgr.active_boxes() + lingering으로
  // 구성된 원본 해상도 좌표이므로 별도 tScale 보정 불필요.
  //
  // Fallback: 스냅샷이 비어 있을 때(창 복원 직후 — 스냅샷 캡처 시점에 아직
  // OCR이 미완료) live active_boxes()로 보완.
  // 스냅샷이 있으면 항상 스냅샷 우선 (위치 동기화 보장).
  {
    const OcrBoxSnapshot &snap = outputSlot->ocrBoxSnapshot;
    if (snap.count > 0) {
      for (int bi = 0; bi < snap.count; ++bi) {
        if (all_count >= (int)(sizeof(all_rects) / sizeof(all_rects[0])))
          break;
        const BlurRect &r = snap.rects[bi];
        if (r.width > 0 && r.height > 0)
          all_rects[all_count++] = r;
      }
    } else {
      // 스냅샷 empty → live 트래커 fallback (창 복원 직후 OCR 미완료 구간 보호)
      const float tScale = filter->trackerCoordScale_;
      const auto liveBoxes = filter->trackerMgr.active_boxes();
      for (const auto &tb : liveBoxes) {
        if (all_count >= (int)(sizeof(all_rects) / sizeof(all_rects[0])))
          break;
        BlurRect r{};
        r.x = static_cast<int>(tb.x * tScale);
        r.y = static_cast<int>(tb.y * tScale);
        r.width = static_cast<int>(tb.w * tScale);
        r.height = static_cast<int>(tb.h * tScale);
        r.type = 0;
        if (r.width > 0 && r.height > 0)
          all_rects[all_count++] = r;
      }
    }
  }
#ifdef _WIN32
  // [Role D] 알림 영역 자동 블러 — 쿨다운 중이면 rect 합산
  {
    std::lock_guard<std::mutex> lock(filter->settingsMutex);
    if (filter->notifBlurActive && filter->notifBlurRect.width > 0 &&
        filter->notifBlurRect.height > 0 &&
        all_count < (int)(sizeof(all_rects) / sizeof(all_rects[0]))) {
      all_rects[all_count++] = filter->notifBlurRect;
    }
  }

  // [Role D] 수동 드래그 블러 — 확정 rects + 드래그 중 미리보기
  {
    std::lock_guard<std::mutex> lock(filter->settingsMutex);
    for (int i = 0; i < filter->manualBlurMask.rectCount &&
                    all_count < (int)(sizeof(all_rects) / sizeof(all_rects[0]));
         i++) {
      all_rects[all_count++] = filter->manualBlurMask.rects[i];
    }
    if (filter->dragActive) {
      int px = std::min(filter->dragStartX, filter->dragCurX);
      int py = std::min(filter->dragStartY, filter->dragCurY);
      int pbw = std::abs(filter->dragCurX - filter->dragStartX);
      int pbh = std::abs(filter->dragCurY - filter->dragStartY);
      if (pbw > 8 && pbh > 8 &&
          all_count < (int)(sizeof(all_rects) / sizeof(all_rects[0]))) {
        all_rects[all_count++] = {px, py, pbw, pbh, 0};
      }
    }
  }
#endif

  // 비상 블랙아웃 (health.shouldReset() 경로에서만 설정됨)
  for (int i = 0; i < filter->lastMask.rectCount &&
                  all_count < (int)(sizeof(all_rects) / sizeof(all_rects[0]));
       i++)
    all_rects[all_count++] = filter->lastMask.rects[i];

  // [Fix #3-E] 렌더 비용 측정 로그 (60프레임마다 1회)
  {
    static int s_render_log_throttle = 0;
    if (++s_render_log_throttle >= 60) {
      s_render_log_throttle = 0;
      blog(LOG_INFO, "[SecureCast][render] rects=%d", all_count);
    }
  }

  if (update_last_safe_render(filter, outputTex, all_rects, all_count, w, h))
    render_last_safe_frame(filter, w, h);
  else
    render_masked_output(filter, outputTex, all_rects, all_count, w, h);

#ifdef _WIN32
  // 게임 모드 활성 시: 우상단에 빨간 네모 박스 표시 (임시 인디케이터)
  if (filter->isGameMode.load(std::memory_order_acquire)) {
    gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    constexpr uint32_t GM_SZ = 20, GM_PAD = 8;
    gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"),
                        0xFFFF0000);
    while (gs_effect_loop(solid, "Solid")) {
      gs_matrix_push();
      gs_matrix_identity();
      gs_matrix_translate3f((float)(w - GM_SZ - GM_PAD), (float)GM_PAD, 0.0f);
      gs_draw_sprite(nullptr, 0, GM_SZ, GM_SZ);
      gs_matrix_pop();
    }
  }

  // [Role D] 보안 상태 테두리 오버레이 (색상: SAFE=초록, PARTIAL=노랑,
  // RISK=빨강)
  {
    uint32_t borderColor = 0xFF00FF00;
    if (newState == SecurityState::PARTIAL)
      borderColor = 0xFFFFFF00;
    else if (newState == SecurityState::RISK)
      borderColor = 0xFFFF0000;

    constexpr int BORDER = 6;
    gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"),
                        borderColor);
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

  // Phase 4: Health Check & Reset
  if (filter->health.shouldReset()) {
    blog(LOG_ERROR, "[SecureCast] Pipeline CRITICAL. Resetting.");
    MaskPayload bo{};
    bo.rectCount = 1;
    bo.rects[0] = {0, 0, (int)w, (int)h, 0};
    filter->lastMask = bo;
    filter->readback.destroyImmediate();
    filter->readback.initialize();
    std::vector<std::pair<int, int>> ss = {{64, 64}, {ocrW, ocrH}};
    filter->readback.resizePool(ss);
    filter->health.reset();
    // [C-9 수정 / F2 Fix] 복구 직후 새 마스크 결과가 들어올 때까지 풀스크린
    // 블랙아웃(bo)을 안전하게 유지 (Fail-Secure)
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
// → trackerAccumulator를 누산하여 임계(0.15초)를 넘을 때만
// sc_scan_blacklisted_windows를 실행한다.
//   [C-1 수정] 이 로직은 인라인으로 직접 수행 (sc_tracker_tick 위임 제거).
//
// seconds: 직전 tick과의 경과 시간(초). 60fps면 약 0.0167.
static void securecast_video_tick(void *data, float seconds) {
  SecureCastFilter *filter = (SecureCastFilter *)data;
  if (!filter)
    return;
  // [Role D] isActive는 GUI 스레드(update)에서도 쓸 수 있으므로 settingsMutex로
  // 보호
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
    filter->cpuUsage = sampleCpuUsage(
        &filter->prevIdleTime, &filter->prevKernelTime, &filter->prevUserTime);

    if (!filter->isGameMode.load(std::memory_order_acquire)) {
      if (filter->cpuUsage >= GM_CPU_ENTER) {
        filter->gameModeEntryTimer += GM_SAMPLE_INTERVAL;
        if (filter->gameModeEntryTimer >= GM_ENTER_TIME) {
          filter->isGameMode.store(true, std::memory_order_release);
          filter->gameModeEntryTimer = 0.0f;
          filter->gameModeExitTimer = 0.0f;
          filter->mockWorker.setPaused(true);
          blog(LOG_INFO, "[SecureCast] Game mode ON  (CPU: %.0f%%)",
               filter->cpuUsage);
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
          blog(LOG_INFO,
               "[SecureCast] Game mode OFF (CPU: %.0f%%, 5s cooldown)",
               filter->cpuUsage);
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
    // M5: 창/소스 전환 시 dHash 캐시 무효화 (방어적)
    filter->ocrClearCachePending.store(true, std::memory_order_release);

    // Quick restore: foreground 전환 이벤트 직후 recentlySeenList 조회 → 즉시
    // 복원. EnumWindows 스캔(느림) 전에 captureWindowList를 채워 이번 render의
    // pushFrame 시점부터 마스킹이 적용되도록 한다.
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
        if (!alreadyTracked &&
            filter->windowList.count < SC_MAX_TRACKED_WINDOWS) {
          const TrackedWindow &tw = filter->recentlySeenList.items[ri];
          filter->windowList.items[filter->windowList.count++] = tw;
          if (filter->captureWindowList.count < SC_MAX_TRACKED_WINDOWS)
            filter->captureWindowList.items[filter->captureWindowList.count++] =
                tw;
          // Quick restore 성공: 즉시 강제 스캔을 하지 않는다.
          // EVENT 직후 EnumWindows의 z-order 체크(is_window_top_at_center)는
          // 창이 방금 포그라운드가 되어 z-order가 아직 안 정착한 상태라
          // 일시적으로 실패할 수 있다. 실패하면 scan 결과가 {}가 되어
          // windowList와 captureWindowList를 덮어쓰고 방금 복원한 항목이
          // 사라진다. 대신 다음 render의 sc_update_tracked_bounds가 안정적으로
          // 확인하게 맡긴다.
          filter->trackerAccumulator = 0.0f;
        }
        break;
      }
    }
  }

  const float scanInterval = filter->isGameMode.load(std::memory_order_acquire)
                                 ? SCAN_INTERVAL_GAME
                                 : SCAN_INTERVAL_NORMAL;
  sc_tracker_tick(seconds, &filter->trackerAccumulator, &filter->windowList,
                  scanInterval);

  // [Role D] windowList 스캔 결과를 blacklistMask에 반영 (video_render에서
  // 최우선 차단에 사용)
  {
    std::lock_guard<std::mutex> lock(filter->blacklistMutex);
    filter->blacklistMask.rectCount =
        (filter->windowList.count > SC_MAX_BLUR_RECTS)
            ? SC_MAX_BLUR_RECTS
            : filter->windowList.count;
    for (int i = 0; i < filter->blacklistMask.rectCount; ++i) {
      filter->blacklistMask.rects[i] = {
          (int)filter->windowList.items[i].bounds.left,
          (int)filter->windowList.items[i].bounds.top,
          (int)(filter->windowList.items[i].bounds.right -
                filter->windowList.items[i].bounds.left),
          (int)(filter->windowList.items[i].bounds.bottom -
                filter->windowList.items[i].bounds.top),
          0};
    }
    if (filter->windowList.count > 0 && filter->logScanThrottle++ % 10 == 0)
      blog(LOG_INFO, "[SecureCast] %d blacklisted windows in blacklistMask.",
           filter->windowList.count);
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
      filter->recentlySeenList.items[filter->recentlySeenList.count++] =
          filter->windowList.items[wi];
  }
  // 완전히 닫힌 프로세스의 HWND 정리 (최소화/숨김은 IsWindow=true라 유지됨)
  for (int ri = filter->recentlySeenList.count - 1; ri >= 0; --ri) {
    if (!IsWindow(filter->recentlySeenList.items[ri].hwnd))
      filter->recentlySeenList.items[ri] =
          filter->recentlySeenList.items[--filter->recentlySeenList.count];
  }

  // [Fix #3-C] Lingering: 직전 스캔에 있었지만 이번엔 사라진 창 감지.
  // register_lingering_window helper 호출로 TTL(SC_RING_BUFFER_SLOTS+1)로 통일.
  for (int pi = 0; pi < filter->prevWindowList.count; ++pi) {
    HWND ph = filter->prevWindowList.items[pi].hwnd;
    bool found = false;
    for (int ci = 0; ci < filter->windowList.count && !found; ++ci)
      found = (filter->windowList.items[ci].hwnd == ph);
    if (!found)
      register_lingering_window(filter, filter->prevWindowList.items[pi]);
  }

  // New window detection: 이번 스캔에서 새로 등장한 창을 즉시 lingering에
  // prime. register_lingering_window로 TTL SC_RING_BUFFER_SLOTS+1 보장.
  for (int ci = 0; ci < filter->windowList.count; ++ci) {
    HWND ch = filter->windowList.items[ci].hwnd;
    bool wasPrev = false;
    for (int pi = 0; pi < filter->prevWindowList.count; ++pi) {
      if (filter->prevWindowList.items[pi].hwnd == ch) {
        wasPrev = true;
        break;
      }
    }
    if (!wasPrev)
      register_lingering_window(filter, filter->windowList.items[ci]);
  }

  filter->prevWindowList = filter->windowList;

  // 매 tick 카운트다운 → 정확히 N프레임(ring buffer 지연) 후 자연 제거
  for (int li = filter->lingeringCount - 1; li >= 0; --li) {
    if (--filter->lingeringWindows[li].ticksRemaining <= 0)
      filter->lingeringWindows[li] =
          filter->lingeringWindows[--filter->lingeringCount];
  }

  // [Role D] 알림 영역 자동 블러 쿨다운 카운트다운 (3초 경과 시 해제)
  {
    std::lock_guard<std::mutex> lock(filter->settingsMutex);
    if (filter->notifBlurActive) {
      filter->notifBlurCooldown -= seconds;
      if (filter->notifBlurCooldown <= 0.0f) {
        filter->notifBlurActive = false;
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

#define SC_SETTING_BLACKLIST "sc_blacklist"
#define SC_SETTING_BLUR_INTENSITY "sc_blur_intensity"
// #define SC_SETTING_GAME_MODE   "sc_game_mode"  // [v2] 게임 모드 — 현재
// 스코프 외
#define SC_SETTING_SENSITIVITY "sc_sensitivity"
#define SC_SETTING_MANUAL_RECTS "sc_manual_rects"

// manualBlurMask → obs_data_array 직렬화 후 source settings에 write-back.
// settingsMutex 밖에서 호출해야 함 — obs_source_get_settings가 OBS 내부 락을
// 잡을 수 있음. mask는 락 안에서 복사한 스냅샷을 전달한다.
static void save_manual_rects(SecureCastFilter *filter,
                              const MaskPayload &mask) {
  obs_data_array_t *arr = obs_data_array_create();
  for (int i = 0; i < mask.rectCount; ++i) {
    const BlurRect &r = mask.rects[i];
    obs_data_t *item = obs_data_create();
    obs_data_set_int(item, "x", r.x);
    obs_data_set_int(item, "y", r.y);
    obs_data_set_int(item, "width", r.width);
    obs_data_set_int(item, "height", r.height);
    obs_data_set_int(item, "type", r.type);
    obs_data_array_push_back(arr, item);
    obs_data_release(item);
  }
  obs_data_t *settings = obs_source_get_settings(filter->context);
  obs_data_set_array(settings, SC_SETTING_MANUAL_RECTS, arr);
  obs_data_release(settings);
  obs_data_array_release(arr);
}

static void securecast_get_defaults(obs_data_t *settings) {
  obs_data_set_default_string(settings, SC_SETTING_BLACKLIST, "");
  obs_data_set_default_double(settings, SC_SETTING_BLUR_INTENSITY, 5.0);
  obs_data_set_default_double(settings, SC_SETTING_SENSITIVITY, 0.5);

  obs_data_array_t *emptyArr = obs_data_array_create();
  obs_data_set_default_array(settings, SC_SETTING_MANUAL_RECTS, emptyArr);
  obs_data_array_release(emptyArr);
}

static obs_properties_t *securecast_get_properties(void *data) {
  obs_properties_t *props = obs_properties_create();
  obs_properties_add_text(props, SC_SETTING_BLACKLIST,
                          "Blacklist Apps (one per line)", OBS_TEXT_MULTILINE);
  obs_properties_add_float_slider(props, SC_SETTING_BLUR_INTENSITY,
                                  "Blur Intensity", 1.0, 10.0, 0.5);
  obs_properties_add_float_slider(props, SC_SETTING_SENSITIVITY,
                                  "Detection Sensitivity", 0.0, 1.0, 0.05);

#ifdef _WIN32
  // [Role D] 수동 드래그 블러 초기화 버튼
  obs_properties_add_button(
      props, "sc_clear_manual", "Clear Manual Blurs",
      [](obs_properties_t *, obs_property_t *, void *btn_data) -> bool {
        auto *filter = static_cast<SecureCastFilter *>(btn_data);
        MaskPayload snapshot{};
        {
          std::lock_guard<std::mutex> lock(filter->settingsMutex);
          filter->manualBlurMask.rectCount = 0;
          filter->dragActive = false;
          snapshot = filter->manualBlurMask;
        }
        save_manual_rects(filter, snapshot);
        blog(LOG_INFO,
             "[SecureCast][D] Manual blur rects cleared (Properties button).");
        return true;
      });
  (void)data;
#else
  (void)data;
#endif

  return props;
}

// GUI 스레드에서 호출되므로 settingsMutex로 보호 (Render Thread와 data race
// 방지)
static void securecast_update(void *data, obs_data_t *settings) {
  SecureCastFilter *filter = static_cast<SecureCastFilter *>(data);
  std::lock_guard<std::mutex> lock(filter->settingsMutex);
  filter->blacklistApps = obs_data_get_string(settings, SC_SETTING_BLACKLIST);
  filter->blurIntensity =
      (float)obs_data_get_double(settings, SC_SETTING_BLUR_INTENSITY);
  filter->sensitivity =
      (float)obs_data_get_double(settings, SC_SETTING_SENSITIVITY);
  blog(LOG_INFO,
       "[SecureCast][D] Settings updated — blur=%.1f sensitivity=%.2f",
       filter->blurIntensity, filter->sensitivity);

  // 수동 블러 rect 역직렬화
  obs_data_array_t *arr = obs_data_get_array(settings, SC_SETTING_MANUAL_RECTS);
  if (arr) {
    size_t count = std::min(obs_data_array_count(arr),
                            (size_t)SecureCastFilter::SC_MAX_MANUAL_RECTS);
    filter->manualBlurMask.rectCount = 0;
    for (size_t i = 0; i < count; ++i) {
      obs_data_t *item = obs_data_array_item(arr, i);
      BlurRect &r =
          filter->manualBlurMask.rects[filter->manualBlurMask.rectCount++];
      r.x = (int)obs_data_get_int(item, "x");
      r.y = (int)obs_data_get_int(item, "y");
      r.width = (int)obs_data_get_int(item, "width");
      r.height = (int)obs_data_get_int(item, "height");
      r.type = (int)obs_data_get_int(item, "type");
      obs_data_release(item);
    }
    obs_data_array_release(arr);
    blog(LOG_INFO, "[SecureCast][D] Manual rects loaded: %d rect(s).",
         filter->manualBlurMask.rectCount);
  }

  // 소스/설정 전환 시 dHash 캐시 무효화 요청.
  // clearDHashCache()를 여기서 직접 호출하면 GUI 스레드↔OCR 워커 data race
  // 발생. 플래그만 세우고 워커 스레드가 다음 사이클에 안전하게 처리한다.
  filter->ocrClearCachePending.store(true, std::memory_order_release);
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
static void securecast_mouse_click(void *data,
                                   const struct obs_mouse_event *event,
                                   int32_t type, bool mouse_up,
                                   uint32_t /*click_count*/) {
  auto *filter = static_cast<SecureCastFilter *>(data);

  MaskPayload snapshot{};
  bool save = false;

  {
    std::lock_guard<std::mutex> lock(filter->settingsMutex);

    if (type == MOUSE_RIGHT && !mouse_up) {
      // 우클릭 DOWN: 수동 블러 전체 초기화
      filter->manualBlurMask.rectCount = 0;
      filter->dragActive = false;
      snapshot = filter->manualBlurMask;
      save = true;
      blog(LOG_INFO,
           "[SecureCast][D] Manual blur rects cleared (right-click).");

    } else if (type == MOUSE_LEFT) {
      if (!mouse_up) {
        // 좌클릭 DOWN: 드래그 시작
        filter->dragActive = true;
        filter->dragStartX = event->x;
        filter->dragStartY = event->y;
        filter->dragCurX = event->x;
        filter->dragCurY = event->y;
      } else if (filter->dragActive) {
        // 좌클릭 UP: 드래그 완료 -> BlurRect 확정
        filter->dragActive = false;
        int x = std::min(filter->dragStartX, event->x);
        int y = std::min(filter->dragStartY, event->y);
        int bw = std::abs(event->x - filter->dragStartX);
        int bh = std::abs(event->y - filter->dragStartY);
        if (bw > 8 && bh > 8 &&
            filter->manualBlurMask.rectCount <
                SecureCastFilter::SC_MAX_MANUAL_RECTS) {
          filter->manualBlurMask.rects[filter->manualBlurMask.rectCount++] = {
              x, y, bw, bh, 0};
          snapshot = filter->manualBlurMask;
          save = true;
          blog(LOG_INFO,
               "[SecureCast][D] Manual blur added: (%d,%d %dx%d) total=%d", x,
               y, bw, bh, filter->manualBlurMask.rectCount);
        }
      }
    }
  } // settingsMutex 해제 후 OBS API 호출

  if (save)
    save_manual_rects(filter, snapshot);
}

static void securecast_mouse_move(void *data,
                                  const struct obs_mouse_event *event,
                                  bool mouse_leave) {
  auto *filter = static_cast<SecureCastFilter *>(data);
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
  info.id = "securecast_filter";
  info.type = OBS_SOURCE_TYPE_FILTER;
  info.output_flags = OBS_SOURCE_VIDEO;
#ifdef _WIN32
  info.output_flags |= OBS_SOURCE_INTERACTION;
#endif
  info.get_name = securecast_get_name;
  info.create = securecast_create;
  info.destroy = securecast_destroy;
  info.video_tick = securecast_video_tick;
  info.video_render = securecast_video_render;
  info.get_properties = securecast_get_properties; // [Role D] Properties UI
  info.get_defaults = securecast_get_defaults;     // [Role D]
  info.update = securecast_update; // [Role D] settingsMutex 보호
#ifdef _WIN32
  info.mouse_click = securecast_mouse_click;
  info.mouse_move = securecast_mouse_move;
#endif
  return info;
}();
