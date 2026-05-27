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
#include <dwmapi.h>           // DwmGetWindowAttribute (Role D: 알림 토스트 탐지)
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
// 기존 창이 있으면 window 좌표와 TTL(SC_RING_BUFFER_SLOTS+1)을 갱신하고,
// 없으면 슬롯이 남은 경우에만 신규 추가한다.
// TTL = SC_RING_BUFFER_SLOTS + 1 로 통일:
//   N슬롯 지연 분 + 탐지 틱의 render가 captureWindowList를 업데이트하기 전에
//   pushFrame이 먼저 실행되는 1프레임 갭을 커버.
// ────────────────────────────────────────────────────────────
static void register_lingering_window(SecureCastFilter *filter,
                                      const TrackedWindow &win,
                                      bool fromPreview = false) {
  for (int li = 0; li < filter->lingeringCount; ++li) {
    if (filter->lingeringWindows[li].window.hwnd == win.hwnd) {
      filter->lingeringWindows[li].window = win;
      filter->lingeringWindows[li].ticksRemaining = SC_RING_BUFFER_SLOTS + 1;
      filter->lingeringWindows[li].fromPreview = fromPreview;
      return;
    }
  }
  if (filter->lingeringCount < SC_MAX_LINGERING) {
    filter->lingeringWindows[filter->lingeringCount++] = {
        win, SC_RING_BUFFER_SLOTS + 1, fromPreview};
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
// 모니터 절대좌표의 단일 RECT를 OBS 소스 픽셀 좌표의 BlurRect로 변환.
// `anchor`는 모니터 정보 조회용(보통 창 전체 bounds — visible sub-rect를 그대로
// 넘기면 작은 sub-rect 하나가 두 모니터에 걸친 경우 정확히 잡지 못할 수 있다).
// `expand`가 true면 비대칭 BBox 팽창을 적용(빠른 창 이동 시 trailing edge 커버용).
// false면 입력 좌표 그대로 변환 — z-order 차감 결과처럼 앞 창과 정확히 경계가
// 맞는 사각형에 사용. 팽창을 적용하면 앞 창 영역으로 새어들 수 있다.
static BlurRect rect_to_blur_rect(const RECT &monitor_rect, const RECT &anchor,
                                  uint32_t src_w, uint32_t src_h, bool expand) {
  BlurRect r{};
  HMONITOR hmon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
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

  int x = (int)((monitor_rect.left - mi.rcMonitor.left) * sx);
  int y = (int)((monitor_rect.top - mi.rcMonitor.top) * sy);
  int bw = (int)((monitor_rect.right - monitor_rect.left) * sx);
  int bh = (int)((monitor_rect.bottom - monitor_rect.top) * sy);

  if (expand) {
    // 비대칭 BBox 팽창 — 위쪽(타이틀바 위)은 최소, 좌/우/아래는 빠른 이동 여유
    // 포함. 위: 1% / 좌우: 2.5% / 아래: 1.5%
    int exp_top = (int)(bh * 0.01f);
    int exp_sides = (int)(bw * 0.025f);
    int exp_bottom = (int)(bh * 0.015f);
    x = std::max(0, x - exp_sides);
    y = std::max(0, y - exp_top);
    bw = std::min((int)src_w - x, bw + exp_sides * 2);
    bh = std::min((int)src_h - y, bh + exp_top + exp_bottom);
  } else {
    // 클램프만.
    if (x < 0) { bw += x; x = 0; }
    if (y < 0) { bh += y; y = 0; }
    bw = std::min(bw, (int)src_w - x);
    bh = std::min(bh, (int)src_h - y);
  }

  r = {x, y, bw, bh, 0}; // type 0 = Blur (Blackout과 시각적으로 구분 가능)
  return r;
}

// TrackedWindow의 visibleRects(z-order 차감 후 노출된 부분들)를 BlurRect 배열로
// 변환해 out에 채워 반환. visibleCount == 0이면 전체 bounds를 1개로 폴백(레거시
// 호환 — visible 계산이 안 된 경로용).
// visibleRects 경로는 팽창 OFF — 차감 결과는 앞 창과 정확히 경계가 맞으므로
// 팽창하면 앞 창 영역으로 새어들어 같이 블러된다.
// 폴백(전체 bounds) 경로는 팽창 ON — 추적 지연 보완.
// 반환값: out에 채운 BlurRect 개수.
static int tracked_window_to_blur_rects(const TrackedWindow &tw, uint32_t src_w,
                                        uint32_t src_h, BlurRect *out,
                                        int max_out) {
  if (max_out <= 0)
    return 0;
  int count = 0;
  if (tw.visibleCount > 0) {
    const int n = (tw.visibleCount < max_out) ? tw.visibleCount : max_out;
    for (int i = 0; i < n; ++i) {
      BlurRect br = rect_to_blur_rect(tw.visibleRects[i], tw.bounds, src_w,
                                       src_h, /*expand=*/false);
      if (br.width > 0 && br.height > 0)
        out[count++] = br;
    }
  } else {
    BlurRect br = rect_to_blur_rect(tw.bounds, tw.bounds, src_w, src_h,
                                     /*expand=*/true);
    if (br.width > 0 && br.height > 0)
      out[count++] = br;
  }
  return count;
}
#endif

// ================================================================
// [Role A] blur.effect 셰이더로 BlurRect 1개를 렌더링
//
// type == 0 (Blur)    : image 텍스처를 box_offset/size 기준으로 5x5 평균
// type == 1 (Blackout): 단색 검정
// ================================================================

// 고정 블러 강도 (셰이더 blur_radius — 5x5 탭 간격 배수).
// PII 가림 강도는 보안에 직결되므로 사용자 조절을 허용하지 않고
// 검증된 값으로 고정한다.
static constexpr float SC_BLUR_RADIUS = 8.0f;

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
    gs_effect_set_float(gs_effect_get_param_by_name(fx, "blur_radius"),
                        SC_BLUR_RADIUS);
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

// 현재 OBS 소스 프레임을 HEAD 슬롯에 캡처하고, 창 좌표 스냅샷을 함께 저장한다.
// HEAD를 한 칸 전진시켜 다음 pushFrame이 다음 슬롯에 쓰도록 한다.
#ifdef _WIN32
void FrameRingBuffer::pushFrame(uint64_t timestamp,
                                obs_source_t *filter_context,
                                const TrackedWindowList *wlist,
                                const std::vector<VtOcrBox> *trackerSnap,
                                uint64_t dependentOcrFrameId)
#else
void FrameRingBuffer::pushFrame(uint64_t timestamp,
                                obs_source_t *filter_context,
                                const std::vector<VtOcrBox> *trackerSnap,
                                uint64_t dependentOcrFrameId)
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
  // notifRect는 매 프레임 video_render의 backfillRecentNotifRect가 채운다.
  // 슬롯 재사용 시 옛 값이 남지 않도록 여기서 initialize.
  slot.notifRect = BlurRect{};
#endif

  // [Window anchor v2] 트래커 박스 스냅샷 저장 (재사용 슬롯의 옛 값 클리어).
  if (trackerSnap)
    slot.trackerSnapshot = *trackerSnap;
  else
    slot.trackerSnapshot.clear();

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

// [Bounded Exposure backfill]
// 새 OCR claim 직후 호출. 링 내 슬롯 중 frameId가 newOcrFrameId 미만이면서
// 현재 dependent도 그보다 작은 것들의 dependent를 newOcrFrameId로 끌어올린다.
//
// 시나리오: T=0.5s에 PII 등장(frame30 push, dependent=0).
//           T=1.0s에 OCR60 claim → backfill(60) → frame30.dependent=60.
//           T=2.0s에 OCR60 완료(watermark=60) → frame30 송출 시 60<=60 → safe.
//           OCR60 박스로 블러 적용된 채 송출 → 노출 방지.
//
// 이미 더 큰 dependent를 가진 슬롯은 보존 (중첩 OCR 사이클 안전).
void FrameRingBuffer::backfillDependentOcr(uint64_t newOcrFrameId) {
  if (newOcrFrameId == 0)
    return;
  for (Slot &slot : m_slots) {
    if (slot.frameId > 0 && slot.frameId < newOcrFrameId &&
        slot.dependentOcrFrameId < newOcrFrameId) {
      slot.dependentOcrFrameId = newOcrFrameId;
    }
  }
}

#ifdef _WIN32
// 최근 maxAgeNs 이내에 캡처된 슬롯들의 windowSnapshot에 win을 소급 추가한다.
// 새 블랙리스트 창은 감지 지연(scan latency) 동안 캡처된 슬롯의 스냅샷에서
// 빠져 있어, 그 슬롯이 송출될 때 마스킹 없이 노출된다. 이를 보정한다.
// 슬롯을 최신→과거 순으로 순회하며 maxAgeNs를 넘는 슬롯에서 중단한다.
void FrameRingBuffer::backfillRecentSnapshots(const TrackedWindow &win,
                                              uint64_t nowNs,
                                              uint64_t maxAgeNs) {
  for (int k = 0; k < m_frameCount; ++k) {
    int idx =
        ((m_head - 1 - k) % SC_RING_BUFFER_SLOTS + SC_RING_BUFFER_SLOTS) %
        SC_RING_BUFFER_SLOTS;
    Slot &s = m_slots[idx];
    if (s.timestamp == 0 || nowNs < s.timestamp ||
        nowNs - s.timestamp > maxAgeNs)
      break; // 더 과거 슬롯은 갭 범위 밖
    TrackedWindowList &snap = s.windowSnapshot;
    bool found = false;
    for (int i = 0; i < snap.count; ++i) {
      if (snap.items[i].hwnd == win.hwnd) {
        snap.items[i] = win; // 좌표 갱신
        found = true;
        break;
      }
    }
    if (!found && snap.count < SC_MAX_TRACKED_WINDOWS)
      snap.items[snap.count++] = win;
  }
}

// 최근 maxAgeNs 이내에 캡처된 슬롯들의 notifRect를 rect로 설정한다.
// 알림 블러도 windowSnapshot처럼 지연 송출 프레임과 동기화하기 위함이다.
void FrameRingBuffer::backfillRecentNotifRect(const BlurRect &rect,
                                              uint64_t nowNs,
                                              uint64_t maxAgeNs) {
  for (int k = 0; k < m_frameCount; ++k) {
    int idx =
        ((m_head - 1 - k) % SC_RING_BUFFER_SLOTS + SC_RING_BUFFER_SLOTS) %
        SC_RING_BUFFER_SLOTS;
    Slot &s = m_slots[idx];
    if (s.timestamp == 0 || nowNs < s.timestamp ||
        nowNs - s.timestamp > maxAgeNs)
      break;
    s.notifRect = rect;
  }
}
#endif

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
// 1-G: half-size BGRA GPU 다운스케일 리소스 해제
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
  filter->ocrDownW_ = 0;
  filter->ocrDownH_ = 0;
}

// 1-G: 1440p+ OCR용 half-size BGRA stagesurf 확보. 해상도 변경 시 재생성.
static bool ensure_ocr_down_stage(SecureCastFilter *filter, uint32_t fullW,
                                  uint32_t fullH) {
  if (!filter)
    return false;
  const uint32_t dw = fullW / 2, dh = fullH / 2;
  if (dw == 0 || dh == 0)
    return false;

  if (filter->ocrDownRender_ && filter->ocrDownW_ == dw &&
      filter->ocrDownH_ == dh)
    return true;

  destroy_ocr_down_stage(filter);

  filter->ocrDownRender_ = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
  if (!filter->ocrDownRender_)
    return false;

  filter->ocrDownStage_ = gs_stagesurface_create(dw, dh, GS_BGRA);
  if (!filter->ocrDownStage_) {
    gs_texrender_destroy(filter->ocrDownRender_);
    filter->ocrDownRender_ = nullptr;
    return false;
  }

  filter->ocrDownW_ = dw;
  filter->ocrDownH_ = dh;
  return true;
}

// 1-G: GPU에서 srcTex를 절반 크기 BGRA로 다운샘플 후 CPU 버퍼로 readback.
// 반환: true = outPixels에 dw×dh BGRA 기록됨, outStride = dw*4.
static bool read_texture_bgra_half_gpu(SecureCastFilter *filter,
                                       gs_texture_t *srcTex, uint32_t fullW,
                                       uint32_t fullH,
                                       std::vector<uint8_t> &outPixels,
                                       int &outStride) {
  if (!filter || !srcTex)
    return false;
  if (!filter->trackerGrayEffect_)
    return false;
  if (!ensure_ocr_down_stage(filter, fullW, fullH))
    return false;

  const uint32_t dw = filter->ocrDownW_, dh = filter->ocrDownH_;

  gs_texrender_reset(filter->ocrDownRender_);
  if (!gs_texrender_begin(filter->ocrDownRender_, static_cast<int>(dw),
                          static_cast<int>(dh)))
    return false;

  gs_effect_t *eff = filter->trackerGrayEffect_; // downsample.effect 재사용
  gs_eparam_t *pImg = gs_effect_get_param_by_name(eff, "image");
  gs_eparam_t *pUV = gs_effect_get_param_by_name(eff, "uv_bounds");

  gs_effect_set_texture(pImg, srcTex);
  if (pUV) {
    struct vec4 bounds = {0.0f, 0.0f, 1.0f, 1.0f};
    gs_effect_set_vec4(pUV, &bounds);
  }
  while (gs_effect_loop(eff, "BGRADownsample2x"))
    gs_draw_sprite(srcTex, 0, dw, dh);

  gs_texrender_end(filter->ocrDownRender_);

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
#ifdef _WIN32
  filter->ocrPendingWindowSnapshot.count = 0;
#endif
}

static void submit_ocr_frame(SecureCastFilter *filter,
                             std::vector<uint8_t> &&pixels, int width,
                             int height, int stride, uint64_t frameId
#ifdef _WIN32
                             ,
                             const TrackedWindowList &windowSnapshot,
                             const TrackedWindowList &allWindows
#endif
) {
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
    filter->ocrFramePending = true;
#ifdef _WIN32
    // [Window anchor] OCR 워커가 PII↔HWND 매칭에 사용할 windowList 스냅샷.
    filter->ocrPendingWindowSnapshot = windowSnapshot;
    // [Window anchor v6] 캡처 시점의 모든 가시 top-level 창.
    filter->ocrPendingAllWindows = allWindows;
#endif
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
    int width = 0;
    int height = 0;
    int stride = 0;
    uint64_t frameId = 0;
#ifdef _WIN32
    TrackedWindowList windowSnapshot{};
    TrackedWindowList allWindows{};
#endif

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
      frameId = filter->ocrPendingFrameId;
      width = filter->ocrPendingWidth;
      height = filter->ocrPendingHeight;
      stride = filter->ocrPendingStride;
#ifdef _WIN32
      // [Window anchor] 프레임 push 시점의 windowList 스냅샷 인수.
      windowSnapshot = filter->ocrPendingWindowSnapshot;
      filter->ocrPendingWindowSnapshot.count = 0;
      // [Window anchor v6] 캡처 시점의 모든 가시 top-level 창.
      allWindows = filter->ocrPendingAllWindows;
      filter->ocrPendingAllWindows.count = 0;
#endif

      filter->ocrPendingFrameId = 0;
      filter->ocrPendingWidth = 0;
      filter->ocrPendingHeight = 0;
      filter->ocrPendingStride = 0;
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
    // ★ 다운스케일 비활성화: 표 케이스에서 큰 헤더 + 작은 데이터가 섞이면
    //   avgLineH가 중간값(28px)이라 0.57× 다운스케일 적용 → 작은 데이터 행이
    //   더 작아져 OCR 미인식 → 다음 사이클도 같은 avg 유지 → 영원히 누락.
    //   downscale 차단(min 1.0×)으로 progression 차단.
    // 첫 사이클(avgH=0): 무조건 2× 업스케일 (1440p+ fallback 0.5×도 제거).
    constexpr float kOcrTargetH = 16.0f; // Windows.Media.Ocr 최적 구간 중간값
    constexpr float kScaleMin = 1.0f;    // 0.5 → 1.0 (다운스케일 차단)
    constexpr float kScaleMax = 2.5f;
    const float avgLineH =
        filter->ocrEngine ? filter->ocrEngine->averageLineHeight() : 0.0f;

    float adaptScale;
    if (avgLineH > 0.0f) {
      adaptScale =
          std::max(kScaleMin, std::min(kOcrTargetH / avgLineH, kScaleMax));
    } else {
      adaptScale = 2.0f; // 첫 사이클 fallback: 항상 2× 업
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
      int upW = static_cast<int>(width * adaptScale + 0.5f);
      int upH = static_cast<int>(height * adaptScale + 0.5f);
      // 4096 캡 초과 시 adaptScale을 가능한 한계까지 줄여 재계산.
      // 이전엔 캡 초과면 업스케일을 통째로 스킵해 ocrPx가 원본 그대로 OCR로
      // 넘어가 kScaleMax=2.5의 의미가 사라지는 버그가 있었다.
      if (upW > 4096 || upH > 4096) {
        const float cappedScale =
            std::min(4096.0f / static_cast<float>(width),
                     4096.0f / static_cast<float>(height));
        if (cappedScale > 1.0f) {
          adaptScale = cappedScale;
          upW = static_cast<int>(width * adaptScale + 0.5f);
          upH = static_cast<int>(height * adaptScale + 0.5f);
        }
      }
      if (upW <= 4096 && upH <= 4096 && adaptScale > 1.0f) {
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

    auto ocrBoxes =
        filter->ocrEngine->analyze_bgra_frame(ocrPx, ocrW2, ocrH2, ocrStride2);

    // 좌표를 원본 해상도로 복원 (coordScale = 1/adaptScale)
    if (coordScale != 1.0f) {
      for (auto &b : ocrBoxes) {
        b.x = std::round(b.x * coordScale);
        b.y = std::round(b.y * coordScale);
        b.w = std::round(b.w * coordScale);
        b.h = std::round(b.h * coordScale);
      }
    }

    // OCR worker가 자신의 픽셀로 직접 register_or_update_gray 호출.
    // (render thread 경유 시 프레임 불일치로 garbage template → ghost tracker
    // 발생) 1-E: BGRA→gray 1회만 수행, register_or_update_gray로 중복 변환
    // 제거.
    {
      std::vector<VtOcrBox> vtBoxes;
      vtBoxes.reserve(ocrBoxes.size());
      for (const auto &b : ocrBoxes)
        vtBoxes.push_back({b.type, b.x, b.y, b.w, b.h});
      std::vector<uint8_t> grayForTracker;
      VisualTrackerManager::bgra_to_gray(pixels.data(), width, height, stride,
                                         grayForTracker);

      // [Window anchor] PII 박스 → owner HWND 매칭.
      //   1차: windowSnapshot(블랙리스트 앱)에서 contains 매칭
      //   2차: WindowFromPoint로 어떤 일반 창이든 owner 탐색
      // 모니터 변환은 snapshot 첫 항목 or 주모니터를 source의 모니터로 가정.
      std::vector<VtBoxOwner> owners(vtBoxes.size());
#ifdef _WIN32
      // 1차: 블랙리스트 창 매칭.
      if (windowSnapshot.count > 0) {
        struct SrcRect {
          int x0, y0, x1, y1;
          HWND hwnd;
          int32_t monL, monT, monR, monB;
        };
        std::vector<SrcRect> srcRects;
        srcRects.reserve(static_cast<size_t>(windowSnapshot.count) *
                         SC_MAX_VISIBLE_SUBRECTS);
        for (int i = 0; i < windowSnapshot.count; ++i) {
          const TrackedWindow &tw = windowSnapshot.items[i];
          // visibleRects 각각을 별도 SrcRect로 등록 — PII 박스가 가려진 부분에
          // 있으면 그 박스의 owner는 카톡이 아니라 위 앞 창이므로 매칭 안 시킴.
          // monL/monT(트래커 anchor)는 전체 bounds 원점으로 통일해 창 이동 시
          // 트래커 좌표 보정이 깨지지 않게 한다.
          BlurRect brs[SC_MAX_VISIBLE_SUBRECTS];
          int n = tracked_window_to_blur_rects(tw, static_cast<uint32_t>(width),
                                               static_cast<uint32_t>(height),
                                               brs, SC_MAX_VISIBLE_SUBRECTS);
          for (int k = 0; k < n; ++k) {
            const BlurRect &br = brs[k];
            srcRects.push_back(
                {br.x, br.y, br.x + br.width, br.y + br.height, tw.hwnd,
                 static_cast<int32_t>(tw.bounds.left),
                 static_cast<int32_t>(tw.bounds.top),
                 static_cast<int32_t>(tw.bounds.right),
                 static_cast<int32_t>(tw.bounds.bottom)});
          }
        }
        for (size_t b = 0; b < vtBoxes.size(); ++b) {
          const float cx = vtBoxes[b].x + vtBoxes[b].w * 0.5f;
          const float cy = vtBoxes[b].y + vtBoxes[b].h * 0.5f;
          for (const auto &sr : srcRects) {
            if (cx >= static_cast<float>(sr.x0) &&
                cx < static_cast<float>(sr.x1) &&
                cy >= static_cast<float>(sr.y0) &&
                cy < static_cast<float>(sr.y1)) {
              owners[b].hwnd = reinterpret_cast<void *>(sr.hwnd);
              owners[b].windowL = sr.monL;
              owners[b].windowT = sr.monT;
              owners[b].windowR = sr.monR;
              owners[b].windowB = sr.monB;
              break;
            }
          }
        }
      }

      // 2차: 1차에서 owner 못 찾은 박스에 대해 캡처-시점 가시창 목록에서 매칭.
      // (이전엔 WindowFromPoint 실시간 호출 — OCR 실행 ~250ms 사이에 창이 움직였으면
      //  엉뚱한 정적 owner를 잡아서 trail/잔상 박스를 만들었음. allWindows는 캡처
      //  시점에 enum된 목록이라 그 시점의 z-order/위치 그대로.)
      if (allWindows.count > 0) {
        HMONITOR srcMon = MonitorFromRect(&allWindows.items[0].bounds,
                                          MONITOR_DEFAULTTONEAREST);
        if (!srcMon) {
          POINT origin{0, 0};
          srcMon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        }
        MONITORINFO mi{};
        mi.cbSize = sizeof(MONITORINFO);
        if (srcMon && GetMonitorInfo(srcMon, &mi)) {
          const int mon_w = mi.rcMonitor.right - mi.rcMonitor.left;
          const int mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
          if (mon_w > 0 && mon_h > 0 && width > 0 && height > 0) {
            const float sx = static_cast<float>(mon_w) / width;
            const float sy = static_cast<float>(mon_h) / height;
            for (size_t b = 0; b < vtBoxes.size(); ++b) {
              if (owners[b].hwnd)
                continue; // 1차에서 매칭됨
              // source-coord 박스 중심 → 모니터 절대 좌표
              const float cx = vtBoxes[b].x + vtBoxes[b].w * 0.5f;
              const float cy = vtBoxes[b].y + vtBoxes[b].h * 0.5f;
              const long monX = mi.rcMonitor.left +
                                static_cast<long>(cx * sx);
              const long monY = mi.rcMonitor.top +
                                static_cast<long>(cy * sy);
              // allWindows는 Z-order 위→아래 — 첫 contains가 가장 위에 있는 창.
              for (int i = 0; i < allWindows.count; ++i) {
                const RECT &r = allWindows.items[i].bounds;
                if (monX >= r.left && monX < r.right && monY >= r.top &&
                    monY < r.bottom) {
                  owners[b].hwnd =
                      reinterpret_cast<void *>(allWindows.items[i].hwnd);
                  owners[b].windowL = r.left;
                  owners[b].windowT = r.top;
                  owners[b].windowR = r.right;
                  owners[b].windowB = r.bottom;
                  break;
                }
              }
            }
          }
        }
      }
#endif

      filter->trackerMgr.register_or_update_gray(vtBoxes, owners,
                                                 grayForTracker.data(), width,
                                                 height);
    }

    const int boxCount = static_cast<int>(ocrBoxes.size());
    blog(LOG_DEBUG, "[securecast][ocr] OCR boxes: %d", boxCount);
    if (boxCount != filter->lastLoggedOcrCount) {
      blog(LOG_INFO, "[securecast][ocr] mask count changed: %d -> %d",
           filter->lastLoggedOcrCount, boxCount);
      filter->lastLoggedOcrCount = boxCount;
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

  // worker 정지 후 lastSubmitted/lastCompleted 모두 0으로 리셋.
  // 그렇지 않으면 worker 재시작 전까지 게이트가 영구 unsafe로 묶인다.
  filter->lastSubmittedOcrFrameId.store(0, std::memory_order_release);
  filter->lastCompletedOcrFrameId.store(0, std::memory_order_release);
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
  // 최소화 애니메이션 가드 — pre-minimize bounds 캡처용 시스템 훅 등록.
  sc_minimize_tracker_init();
  // Resize 가드 — MOVESIZESTART/END WinEvent + showCmd 폴링.
  sc_resize_tracker_init();
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
  // 최소화 가드 훅 refcount 감소 (마지막 filter면 실제 unhook).
  sc_minimize_tracker_shutdown();
  sc_resize_tracker_shutdown();
#endif

  // OCR/Tracker 워커 먼저 중지 (trackerMgr·ring buffer 해제 전 race 방지)
  stop_tracker_thread(filter); // tracker thread 먼저 중지 (trackerMgr 공유)
  stop_ocr_worker(filter);
  filter->trackerMgr.clear(); // OCR worker 종료 후 tracker 정리

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
  destroy_last_safe_render(filter);
#ifdef _WIN32
  // 1-G: half-size OCR 다운스케일 리소스 해제
  destroy_ocr_down_stage(filter);
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
//   │  3. Visual Tracker에서 현재 블러 박스 조회         │
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
    filter->lastSubmittedOcrFrameId.store(0, std::memory_order_release);
    filter->unverifiedFrameLogCounter = 0;
    // 첫 초기화 시 OCR 워커 + Tracker 스레드 시작
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

    // 1. 워커 중지 (OCR → Tracker 순서로 중지해야 trackerMgr race 없음)
    stop_ocr_worker(filter);
    stop_tracker_thread(filter);

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
    filter->lastSubmittedOcrFrameId.store(0, std::memory_order_release);
    filter->unverifiedFrameLogCounter = 0;

    // 3. 새 해상도로 워커 재시작
    filter->trackerMgr.clear(); // 해상도 변경 시 기존 박스 좌표 무효화
    filter->trackerFrameSkip_ = 0;
    start_ocr_worker(filter);
    start_tracker_thread(filter);
    blog(LOG_INFO,
         "[securecast][ocr] Async OCR worker restarted after resize.");
  }

  // --- Panic Mode: pushFrame 이전에 차단 ---
  // 패닉 중에는 링 버퍼를 파괴해 GPU 낭비를 막고,
  // 해제 직후 패닉 중 캡처된 프레임이 스트림에 유출되는 것을 방지한다.
  // ringBuffer.destroy()는 이미 파괴된 경우 no-op이라 매 프레임 호출해도 안전.
  if (filter->panicMode.load(std::memory_order_relaxed)) {
    filter->ringBuffer.destroy();
    destroy_last_safe_render(filter);
    filter->lastCompletedOcrFrameId.store(0, std::memory_order_release);
    filter->lastSubmittedOcrFrameId.store(0, std::memory_order_release);
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

  // --- Step 2: 현재 프레임을 Ring Buffer HEAD에 Push ---
  // OBS 소스 내부 버퍼링으로 obs_source_video_render()가 반환하는 픽셀은
  // 실제 DWM 쿼리보다 ~1프레임 뒤처진다.
  // captureWindowList(직전 프레임에서 저장한 DWM 좌표)를 스냅샷으로 쓰면
  // 픽셀 내용과 마스크 위치가 정확히 동기화된다.
  uint64_t ts = obs_get_video_frame_time();
  // [Window anchor v4] push 직전에 owner-anchored 박스 좌표를 즉시 계산해
  // 슬롯에 저장. owner 바인딩된 트래커는 현재 DWM bounds 기준으로 refX/refY +
  // delta로 정확한 좌표 계산 → NCC lag 없이 그 프레임의 창 위치와 일치. 슬롯이
  // 지연 dequeue되므로 송출 화면의 텍스트와 정확히 같이 움직임.
  const float tScalePush = filter->trackerCoordScale_;
  const uint32_t srcWTrkPush =
      (tScalePush > 0.0f)
          ? static_cast<uint32_t>(static_cast<float>(w) / tScalePush)
          : w;
  const uint32_t srcHTrkPush =
      (tScalePush > 0.0f)
          ? static_cast<uint32_t>(static_cast<float>(h) / tScalePush)
          : h;
  const auto trackerSnap =
      filter->trackerMgr.snapshot_for_push(srcWTrkPush, srcHTrkPush);
#ifdef _WIN32
  filter->ringBuffer.pushFrame(ts, filter->context, &filter->captureWindowList,
                               &trackerSnap,
                               filter->lastSubmittedOcrFrameId.load(std::memory_order_relaxed));

  // [지연 동기화] 방금 push한 스냅샷에 새로 등장한 블랙리스트 창을 최근
  // 슬롯들에 소급 추가한다. 감지 지연(scan latency) 동안 캡처돼 스냅샷에서
  // 빠진 갭 프레임을 보정 — 실시간 lingering으로 현재(지연) 프레임에 미리
  // 블러를 깔던 방식과 달리, 블러 박스가 송출 화면에서 창과 동시에 뜬다.
  {
    const TrackedWindowList &pushed = filter->captureWindowList;
    const uint64_t maxAgeNs =
        filter->isGameMode.load(std::memory_order_acquire)
            ? 550000000ULL  // 게임 모드 스캔 0.5s + 여유
            : 200000000ULL; // 일반 스캔 0.15s + 여유
    for (int i = 0; i < pushed.count; ++i) {
      bool wasPrev = false;
      for (int j = 0; j < filter->prevPushedWindowList.count; ++j) {
        if (filter->prevPushedWindowList.items[j].hwnd ==
            pushed.items[i].hwnd) {
          wasPrev = true;
          break;
        }
      }
      if (!wasPrev)
        filter->ringBuffer.backfillRecentSnapshots(pushed.items[i], ts,
                                                   maxAgeNs);
    }
    filter->prevPushedWindowList = pushed;
  }

  // [지연 동기화] 알림 블러도 동일하게 — 현재 알림 union rect를 최근 슬롯들의
  // notifRect에 소급 기록한다. 렌더는 지연 슬롯의 notifRect를 쓰므로, 블러
  // 박스가 송출 화면에서 알림과 같은 타이밍에 뜬다 (실시간 주입은 ~1초 먼저
  // 떴음).
  {
    BlurRect notif{};
    {
      std::lock_guard<std::mutex> lock(filter->settingsMutex);
      if (filter->notifBlurActive)
        notif = filter->notifBlurRect;
    }
    if (notif.width > 0 && notif.height > 0)
      filter->ringBuffer.backfillRecentNotifRect(notif, ts, 200000000ULL);
  }
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

  // [최소화 가드] EVENT_SYSTEM_MINIMIZESTART로 캡처된 pre-bounds를 lingering에
  // 강제 등록. 애니메이션 동안 DWM bounds가 줄어들거나 트래커 박스가 원래
  // 위치에 고정돼 텍스트 가장자리가 노출되는 현상을 막는다.
  //
  // 대상: (1) 블랙리스트 windowList의 HWND, (2) trackerMgr의 OCR owner 창.
  // 무관한 일반 창의 최소화 이벤트는 무시(불필요한 블러 방지).
  // bounds 선택: 훅이 캡처한 pre-bounds와 filter가 직전 프레임에 가지고 있던
  // `before` bounds 중 면적이 더 큰 것을 채택 — 훅 콜백 진입 시점에 이미
  // animation이 시작돼 DWM bounds가 작아져 있을 수 있으므로 filter의 최신 캐시가
  // 더 안전한 경우가 많음.
  // ageMaxMs는 애니메이션(~300ms) + ring buffer 지연(~1s) + 여유. 매 프레임
  // upsert로 TTL refresh, MINIMIZEEND/age 초과 시 evict.
  {
    constexpr int MAX_POLL = 16;
    constexpr uint64_t kMaxAgeMs = 1500;
    ScMinimizingEntry minz[MAX_POLL];
    const int mn = sc_poll_minimizing_windows(minz, MAX_POLL, kMaxAgeMs);
    if (mn > 0) {
      const std::vector<void *> ownerWins =
          filter->trackerMgr.active_owner_windows();
      for (int i = 0; i < mn; ++i) {
        HWND mh = minz[i].hwnd;
        bool relevant = false;
        RECT cached{};
        bool hasCached = false;
        // (1) 블랙리스트 — before(직전 프레임 bounds 보존).
        for (int j = 0; j < before.count; ++j) {
          if (before.items[j].hwnd == mh) {
            relevant = true;
            cached = before.items[j].bounds;
            hasCached = true;
            break;
          }
        }
        // (2) OCR 트래커 owner.
        if (!relevant) {
          for (void *w : ownerWins) {
            if (reinterpret_cast<HWND>(w) == mh) {
              relevant = true;
              break;
            }
          }
        }
        if (!relevant)
          continue;

        // 두 bounds 중 면적 큰 것 채택.
        auto area = [](const RECT &r) -> long long {
          long long w = r.right - r.left;
          long long h = r.bottom - r.top;
          return (w > 0 && h > 0) ? (w * h) : 0;
        };
        RECT pre = minz[i].preBounds;
        if (hasCached && area(cached) > area(pre))
          pre = cached;

        TrackedWindow tw{};
        tw.hwnd = mh;
        tw.bounds = pre;
        tw.visibleCount = 1;
        tw.visibleRects[0] = pre;
        tw.exe_name[0] = L'\0';
        register_lingering_window(filter, tw);
      }
    }
  }

  // [작업표시줄 hover 미리보기 가드]
  //   peek 검출 신호: TaskListThumbnailWnd 존재 OR 마우스가 작업표시줄 위.
  //   두 신호 중 하나라도 참이면 peek 상태로 간주해 recentlySeenList의 모든
  //   블랙리스트 앱 last-known bounds를 lingering으로 강제 등록한다.
  //
  //   이유: Aero Peek는 DWM이 직접 썸네일을 렌더하는 경우가 있어 카톡 HWND
  //   자체가 cloak되거나 z-order 변경 없이 화면에만 노출되는 케이스가 있다.
  //   HWND 추적만으론 잡히지 않아 mouse-over-taskbar를 보조 신호로 사용.
  //
  //   thumbnail strip이 보이면 그 strip bounds도 함께 lingering (소형 썸네일
  //   영역 가림). previewActiveNs로 slot.timestamp 컷오프 → peek 끝나면 자연
  //   소멸.
  {
    // peek 발동 두 조건 모두 충족 시에만 트리거:
    //   (A) 마우스가 직전에 작업표시줄 위에 있었다 (썸네일이 실제로 떠 있음)
    //   (B) 마우스가 현재 썸네일 영역(작업표시줄 인접 ~400px 띠)에 있다
    // (A) 없이 (B)만으로 판정하면 화면 하단을 그냥 지나가도 발동돼서 빈 영역에
    // 블러 박스가 뜬다. (A)는 ~2초 grace로 유지.
    const bool mouseOverTaskbar = sc_mouse_over_taskbar();
    const bool mouseInZone = sc_mouse_over_thumbnail_zone();
    const uint64_t nowTick = GetTickCount64();
    if (mouseOverTaskbar)
      filter->lastOverTaskbarTick = nowTick;
    constexpr uint64_t kThumbnailVisibleHystMs = 2000;
    const bool thumbnailLikelyVisible =
        filter->lastOverTaskbarTick != 0 &&
        (nowTick - filter->lastOverTaskbarTick) < kThumbnailVisibleHystMs;

    const bool peekTrigger = mouseInZone && thumbnailLikelyVisible;
    if (peekTrigger)
      filter->lastInThumbnailZoneTick = nowTick;
    constexpr uint64_t kPreviewHystMs = 1500;
    const bool zoneHystActive =
        filter->lastInThumbnailZoneTick != 0 &&
        (nowTick - filter->lastInThumbnailZoneTick) < kPreviewHystMs;
    const bool previewActive = peekTrigger || zoneHystActive;

    // [hover 식별 — 역방향 매핑 + 양방향 hysteresis + fail-safe]
    // 살아있는 블랙리스트 앱의 작업표시줄 버튼 BoundingRectangle을 UIA로 사전 매핑한
    // 캐시(5초 TTL)와 마우스 위치를 비교 — exe 이름 기반이라 환경/언어 무관.
    //   BLACKLIST → lastHoverBlacklistTick 갱신
    //   OTHER     → lastHoverNotBlacklistTick 갱신
    //   UNKNOWN   → 둘 다 손대지 않음 (직전 신호 hysteresis 살림)
    //
    // 가드 결정은 마지막으로 본 신호(BL vs OTHER) 기준 — 마우스가 작업표시줄에서
    // 미리보기 영역으로 옮겨가면 hoverResult가 UNKNOWN으로 떨어지지만, hysteresis
    // 안에 OTHER 신호가 살아있으면 OFF 유지(메모장 peek 동안 카톡 블러 박스가
    // 끌려오는 회귀 차단).
    ScHoverInfo hoverInfo{};
    hoverInfo.result = SC_TB_HOVER_UNKNOWN;
    if (mouseOverTaskbar)
      hoverInfo = sc_taskbar_hover_blacklist_btn();
    const ScTaskbarHoverResult hoverResult = hoverInfo.result;
    if (hoverResult == SC_TB_HOVER_BLACKLIST) {
      filter->lastHoverBlacklistTick = nowTick;
      // 매칭 exe 갱신 — lingering 등록 시 alive BL 중 이 exe만 통과.
      size_t i = 0;
      const size_t cap = sizeof(filter->lastHoverExe) /
                          sizeof(filter->lastHoverExe[0]);
      while (hoverInfo.exe[i] && i + 1 < cap) {
        filter->lastHoverExe[i] = hoverInfo.exe[i];
        ++i;
      }
      filter->lastHoverExe[i] = 0;
    } else if (hoverResult == SC_TB_HOVER_OTHER) {
      filter->lastHoverNotBlacklistTick = nowTick;
    }
    // UNKNOWN은 두 tick 다 손대지 않음 (fail-safe). lastHoverExe도 보존하여
    // 미리보기 영역으로 이동한 직후의 회귀를 막는다.

    // 미리보기 영역에 머무는 동안(mouseOverTaskbar=false but previewActive=true)
    // hover_blacklist_btn이 호출되지 않아 직전 신호가 점차 만료되고 결국 fail-safe
    // (gate=1)로 떨어진다 — 사용자가 메모장 미리보기를 1.5초 이상 보면 카톡 블러가
    // 발동하는 회귀의 원인. peek 미리보기는 "마지막으로 hover한 아이콘" 기준이므로
    // 그 신호를 미리보기 동안 매 frame refresh해 hysteresis가 끊기지 않게 한다.
    // previewActive가 false로 떨어지면 refresh 멈춰 자연 만료(1.5초 후).
    if (previewActive && !mouseOverTaskbar) {
      if (filter->lastHoverBlacklistTick > 0 &&
          filter->lastHoverBlacklistTick >= filter->lastHoverNotBlacklistTick) {
        filter->lastHoverBlacklistTick = nowTick;
      } else if (filter->lastHoverNotBlacklistTick > 0) {
        filter->lastHoverNotBlacklistTick = nowTick;
      }
    }

    constexpr uint64_t kHoverBlHystMs = 1500;
    const bool blRecent =
        filter->lastHoverBlacklistTick != 0 &&
        (nowTick - filter->lastHoverBlacklistTick) < kHoverBlHystMs;
    const bool otherRecent =
        filter->lastHoverNotBlacklistTick != 0 &&
        (nowTick - filter->lastHoverNotBlacklistTick) < kHoverBlHystMs;
    // 가장 최근 신호 = 두 tick 중 큰 쪽 (둘 다 0이면 신호 없음)
    const bool blIsMoreRecent =
        filter->lastHoverBlacklistTick > filter->lastHoverNotBlacklistTick;

    // 가드 발동 조건:
    //   - 현재 BLACKLIST hover 확정      → ON (즉시)
    //   - BL이 hysteresis 안 + 최근 신호  → ON (strip 이동 갭 보전)
    //   - OTHER가 hysteresis 안 + 최근 신호 → OFF (메모장 peek 등)
    //   - 둘 다 hysteresis 밖             → UNKNOWN fail-safe (안전 우선 ON)
    bool hoverGateOpen;
    if (hoverResult == SC_TB_HOVER_BLACKLIST) {
      hoverGateOpen = true;
    } else if (blRecent && blIsMoreRecent) {
      hoverGateOpen = true;
    } else if (otherRecent) {
      hoverGateOpen = false;
    } else {
      // 작업표시줄/미리보기 진입 직후 등 신호가 아직 없는 상태 — UIA가 한 번도
      // 작업표시줄을 못 잡는 환경 포함. 안전을 위해 ON.
      hoverGateOpen = true;
    }

    if (previewActive && hoverGateOpen) {
      TrackedWindowList aliveBl{};
      sc_find_all_alive_blacklist_windows(&aliveBl);
      if (aliveBl.count > 0) {
        filter->previewActiveNs = os_gettime_ns();
        // exe 필터링: blRecent 동안에는 hover된 그 BL exe만 lingering 등록해
        // "카톡 hover 시 Discord까지 가려짐" 회귀를 차단. fail-safe(blRecent 만료)
        // 시에는 stale exe 사용을 피하기 위해 전체 등록 — UIA 실패 환경 등 안전 우선.
        const bool filterByExe = blRecent && filter->lastHoverExe[0] != 0;
        for (int i = 0; i < aliveBl.count; ++i) {
          if (filterByExe &&
              _wcsicmp(aliveBl.items[i].exe_name, filter->lastHoverExe) != 0)
            continue;
          register_lingering_window(filter, aliveBl.items[i],
                                    /*fromPreview=*/true);
        }
      }
    }
  }
#else
  filter->ringBuffer.pushFrame(ts, filter->context, &trackerSnap,
                               filter->lastSubmittedOcrFrameId.load(std::memory_order_relaxed));
#endif

  // OCR 박스 좌표는 Visual Tracker(trackerMgr)가 직접 관리한다.

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
    if (filter->ocrIsDown.load(std::memory_order_acquire)) {
      filter->currentState = SecurityState::RISK;
    } else if (hasTrackerBoxes || blacklistSnapshot.rectCount > 0 ||
               manualOrNotifActive) {
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

    // ocrWorkerIdle: 단일 소비자(렌더 스레드)가 load→조건부 store(false),
    //               단일 생산자(OCR 워커 또는 렌더 실패 경로)가 store(true).
    // 이 구조상 load와 store(false) 사이의 원자성 갭은 race를 일으키지 않는다.
    // 멀티 제출 경로가 추가될 경우 compare_exchange_strong으로 교체 필요.
    const bool ocrIdle = filter->ocrWorkerIdle.load(std::memory_order_acquire);
    const bool ocrCanSubmit =
        ocrIdle && filter->ocrWorkerRunning.load(std::memory_order_acquire);
    if (ocrCanSubmit) {
      filter->ocrWorkerIdle.store(false, std::memory_order_release);
      // release: claim 실패 분기의 load(acquire)와 한 쌍.
      // relaxed면 다음 video_render에서 lastSubmitted가 아직 0으로 보일 수
      // 있어 dependent=0 누수가 재발한다.
      filter->lastSubmittedOcrFrameId.store(analysisSlot->frameId,
                                            std::memory_order_release);
      // analysisSlot은 방금 제출되었으므로, 자신을 대표 ID로 삼음 (const cast 필요)
      const_cast<FrameRingBuffer::Slot *>(analysisSlot)->dependentOcrFrameId =
          analysisSlot->frameId;
      // [Backfill — 빈도 1/4]
      // 매 OCR claim마다 backfill을 호출하면 링 내 모든 슬롯 dependent가
      // 최신 OCR frameId로 끌어올려져 freeze가 자주 발생.
      // 4번에 1번만 발동 → 새 PII 보호는 유지하되 freeze 빈도 1/4로 감소.
      // 사이 3번은 pushFrame이 박은 dependent=lastSubmitted로 자연스럽게
      // 게이트 통과 (정상 운영 시).
      if (++filter->ocrBackfillCounter_ >= 4) {
        filter->ocrBackfillCounter_ = 0;
        filter->ringBuffer.backfillDependentOcr(analysisSlot->frameId);
      }
    } else {
      // [Bounded Exposure 누수 차단]
      // claim 실패(=OCR worker busy)한 슬롯이 dependent=0으로 송출되면
      // 게이트(dependent<=watermark)를 자동 통과해 무방비 노출.
      // 가장 최근 submit한 OCR frameId를 의존성으로 박아 그 OCR이 완료될
      // 때까지 last-safe-frame으로 freeze 유지한다.
      const uint64_t lastSubmitted =
          filter->lastSubmittedOcrFrameId.load(std::memory_order_acquire);
      if (lastSubmitted > 0) {
        const_cast<FrameRingBuffer::Slot *>(analysisSlot)->dependentOcrFrameId =
            lastSubmitted;
      }
      // lastSubmitted==0(부팅 직후, 아직 어떤 OCR도 submit되지 않음)에는
      // dependent=0 유지 → 초기 freeze 회피.
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

    // --- [좌표계 동기화] use1GPath 활성 여부 사전 판별 ---
    // 트래커 gray 제출 전에 결정해야 트래커와 OCR이 동일 해상도 공간을 공유함.
    // ⚠️ 1080p(1920px)에서는 절반(960×540)이 OCR 인식 한계 이하이므로
    //    2K(2560×1440) 이상에서만 활성화. 1080p는 full-res OCR 사용.
#ifdef _WIN32
    const bool use1GPath =
        (w >= 2560 && h >= 1440) && filter->trackerGrayEffect_;
#else
    constexpr bool use1GPath = false;
#endif

    if (grayOk) {
      // [좌표계 동기화] use1GPath 시: 트래커도 half-res 공간에서 추적해야
      // OCR이 넘겨준 박스 좌표(half-res)와 공간이 일치한다.
      // trackerCoordScale_=2.0f로 저장해두어 렌더 시 원본 해상도로 복원.
      if (use1GPath) {
        std::vector<uint8_t> halfGray;
        int hw = 0, hh = 0;
        VisualTrackerManager::downsample_2x_into(grayPixels.data(), (int)w,
                                                 (int)h, halfGray, hw, hh);
        filter->trackerCoordScale_ = 2.0f;
        std::lock_guard<std::mutex> lock(filter->trackerInputMutex_);
        filter->trackerInputGray_.swap(halfGray);
        filter->trackerInputW_ = hw;
        filter->trackerInputH_ = hh;
        filter->trackerInputReady_ = true;
      } else {
        // full-res 모드: 스케일 복원 불필요
        filter->trackerCoordScale_ = 1.0f;
        std::lock_guard<std::mutex> lock(filter->trackerInputMutex_);
        filter->trackerInputGray_.swap(grayPixels);
        filter->trackerInputW_ = (int)w;
        filter->trackerInputH_ = (int)h;
        filter->trackerInputReady_ = true;
      }
      filter->trackerInputCv_.notify_one();
    }

    // OCR 제출: ocrIdle일 때만 (~4fps)
    // 1-G: 1280px+ → GPU 2× 다운샘플 readback으로 속도 최적화.
    //      트래커도 동일 half-res 공간을 사용하므로 좌표계 불일치 없음.
    if (ocrCanSubmit) {
      std::vector<uint8_t> ocrPixels;
      int ocrW = static_cast<int>(w), ocrH = static_cast<int>(h), ocrStride = 0;
      bool ocrSubmit = false;

#ifdef _WIN32
      if (use1GPath) {
        // 1-G: GPU BGRADownsample2x → half-size BGRA readback
        if (read_texture_bgra_half_gpu(filter, analysisTex, w, h, ocrPixels,
                                       ocrStride)) {
          ocrW = static_cast<int>(filter->ocrDownW_);
          ocrH = static_cast<int>(filter->ocrDownH_);
          ocrSubmit = true;
        }
      }
#endif
      if (!ocrSubmit) {
        // 폴백: 전체 BGRA readback (use1GPath 미활성 또는 GPU 다운스케일 실패)
        if (!bgraPixels.empty()) {
          ocrPixels = bgraPixels;
          ocrStride = static_cast<int>(w) * 4;
          ocrSubmit = true;
        } else if (read_texture_bgra_to_cpu(filter, analysisTex, w, h,
                                            ocrPixels, ocrStride)) {
          ocrSubmit = true;
        }
      }

      if (ocrSubmit) {
        // markAnalysisSubmitted 호출 삭제됨
#ifdef _WIN32
        // [Window anchor v6] 캡처 시점에 모든 가시 top-level 창 enum.
        // OCR 워커는 이 캡처-시점 목록으로 owner를 정확히 매칭한다.
        TrackedWindowList allWindows{};
        sc_enum_all_visible_windows(&allWindows);
#endif
        submit_ocr_frame(filter, std::move(ocrPixels), ocrW, ocrH, ocrStride,
                         analysisSlot->frameId
#ifdef _WIN32
                         ,
                         filter->captureWindowList, allWindows
#endif
        );

        if (++filter->trackerLogCounter >= 150) {
          filter->trackerLogCounter = 0;
          blog(LOG_INFO, "[SC-tracker] active=%zu",
               filter->trackerMgr.active_boxes().size());
        }
      } else {
        filter->ocrWorkerIdle.store(true, std::memory_order_release);
      }
    } else if (ocrIdle && !grayOk) {
      // gray도 실패, OCR도 건너뜀 → ocrIdle 복원
      filter->ocrWorkerIdle.store(true, std::memory_order_release);
    }
  }

  // --- Step 5b: 분석 완료 프레임만 그리기 ---
  // delayedSlot이 의존하는 대표 OCR 프레임이 완료 표시를 받았는지 확인한다.
  // 완료되지 않았다면 원본 송출 금지. 마스크까지 합성해둔 마지막 안전 출력 프레임을 재송출(Freeze)한다.
  uint64_t safeWatermarkId = filter->lastCompletedOcrFrameId.load(std::memory_order_acquire);
  uint64_t delayedId = delayedSlot ? delayedSlot->frameId : 0;
  // 게이트 +2 마진: OCR이 2프레임(≈33ms@60fps) 늦어도 통과 허용.
  // 60fps 환경에서 OCR 완료 ~ render의 watermark 갱신 사이 1~2프레임 race가
  // 발생해 4프레임 차이만으로도 freeze 진입하던 문제를 완화. 노출 시간은
  // 최대 33ms(2프레임)로 제한되어 체감 노출은 거의 없음.
  static constexpr uint64_t kGateMarginFrames = 2;
  bool isSafeToRender = (safeWatermarkId > 0) && delayedSlot &&
                        (delayedSlot->dependentOcrFrameId <=
                         safeWatermarkId + kGateMarginFrames);

  if (!delayedSlot || !delayedSlot->getTexture() || !isSafeToRender) {
    if (++filter->unverifiedFrameLogCounter >= 60) {
      filter->unverifiedFrameLogCounter = 0;
      blog(LOG_WARNING,
           "[SecureCast][gate] delayed frame %" PRIu64
           " dependent on OCR %" PRIu64 " not analyzed (watermark %" PRIu64 "); freezing last safe output",
           delayedId, delayedSlot ? delayedSlot->dependentOcrFrameId : 0, safeWatermarkId);
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
  // 동기화됨)
  // capacity: 각 TrackedWindow가 visible 차감으로 최대 SC_MAX_VISIBLE_SUBRECTS개
  //           서브 사각형으로 쪼개질 수 있으므로 windowSnapshot×2와 lingering에
  //           sub-rect 계수를 곱한다.
  // MAX_TRACKERS는 VisualTrackerManager 클래스 static 상수이므로
  // securecast-filter.cpp에서는 직접 사용 불가. 실제 값(8)을 리터럴로 대체.
  // x2: 슬롯의 N프레임 전 snap + 현재 snap 둘 다 사용 (anim 지연 보정).
  static constexpr int kMaxTrackerSlots = 8;
  BlurRect all_rects[SC_MAX_BLUR_RECTS * 2 +
                     SC_MAX_TRACKED_WINDOWS * SC_MAX_VISIBLE_SUBRECTS * 2 +
                     SC_MAX_LINGERING * SC_MAX_VISIBLE_SUBRECTS +
                     kMaxTrackerSlots * 2 + 8];
  const int kAllRectsCap =
      static_cast<int>(sizeof(all_rects) / sizeof(all_rects[0]));
  int all_count = 0;

#ifdef _WIN32
  // N프레임 전 스냅샷 (현재 렌더링 중인 지연 프레임과 동기화)
  for (int i = 0; i < outputSlot->windowSnapshot.count &&
                  all_count + SC_MAX_VISIBLE_SUBRECTS <= kAllRectsCap;
       i++) {
    int n = tracked_window_to_blur_rects(outputSlot->windowSnapshot.items[i], w,
                                         h, &all_rects[all_count],
                                         SC_MAX_VISIBLE_SUBRECTS);
    all_count += n;
  }
  // N-1프레임 전 스냅샷 합집합: 한 프레임 이동 궤적 전체를 마스킹 (빠른 드래그
  // 노출 방지)
  {
    const FrameRingBuffer::Slot *slotN1 =
        filter->ringBuffer.peekSlotAtOffset(SC_RING_BUFFER_SLOTS - 1);
    if (slotN1) {
      for (int i = 0; i < slotN1->windowSnapshot.count &&
                      all_count + SC_MAX_VISIBLE_SUBRECTS <= kAllRectsCap;
           i++) {
        int n = tracked_window_to_blur_rects(slotN1->windowSnapshot.items[i], w,
                                             h, &all_rects[all_count],
                                             SC_MAX_VISIBLE_SUBRECTS);
        all_count += n;
      }
    }
  }
  // Lingering rects: 사라진 창의 N프레임 잔영 (ring buffer에 남은 과거 프레임
  // 커버).
  // 컷오프 분기:
  //   fromPreview=true (taskbar hover 가드): previewActiveNs 이후 캡처된 슬롯은
  //     peek이 끝난 뒤 프레임이라 그리지 않음 — 빈 영역 잔상 방지.
  //   fromPreview=false (정상 lingering): MINIMIZEEND가 있으면 endNs 이후 슬롯은
  //     skip — 동일 원리.
  for (int li = 0; li < filter->lingeringCount &&
                   all_count + SC_MAX_VISIBLE_SUBRECTS <= kAllRectsCap;
       ++li) {
    const auto &linger = filter->lingeringWindows[li];
    if (linger.fromPreview) {
      if (filter->previewActiveNs != 0 &&
          outputSlot->timestamp > filter->previewActiveNs)
        continue;
    } else {
      const uint64_t endNs = sc_get_minimize_end_ns(linger.window.hwnd);
      if (endNs != 0 && outputSlot->timestamp > endNs)
        continue;
    }
    int n = tracked_window_to_blur_rects(linger.window, w, h,
                                         &all_rects[all_count],
                                         SC_MAX_VISIBLE_SUBRECTS);
    all_count += n;
  }
#endif
  // OCR 박스 — Visual Tracker가 제공하는 NCC 추적 위치
  // [좌표계 동기화] use1GPath 모드에서는 트래커가 half-res 공간에서 추적하므로
  // trackerCoordScale_(=2.0f)를 곱해 원본 해상도로 좌표를 복원한다.
  //
  // [Anim 지연 보정] N프레임 전 슬롯의 박스(T-N) + 현재 시점에서 다시 계산한
  // 박스(T) 둘 다 사용. OBS의 N프레임 지연 송출 특성 활용:
  //   - 슬롯 박스는 캡처 시점에 작을 수 있음 (sticky 미활성)
  //   - 현재 시점 박스는 sticky 확장된 큰 박스
  //   - 두 시점 모두 마스킹 → 송출되는 과거 픽셀이 미래의 큰 박스로 보호됨
  {
    const float tScale = filter->trackerCoordScale_;
    auto push_tracker_box = [&](const VtOcrBox &tb) {
      if (all_count >= (int)(sizeof(all_rects) / sizeof(all_rects[0])))
        return;
      BlurRect r{};
      r.x = static_cast<int>(tb.x * tScale);
      r.y = static_cast<int>(tb.y * tScale);
      r.width = static_cast<int>(tb.w * tScale);
      r.height = static_cast<int>(tb.h * tScale);
      r.type = 0; // Blur
      if (r.width > 0 && r.height > 0)
        all_rects[all_count++] = r;
    };

    // 1) 슬롯 저장 박스 (캡처 시점 매칭)
    for (const auto &tb : outputSlot->trackerSnapshot)
      push_tracker_box(tb);

    // 2) 현재 시점 박스 (sticky 확장 적용된 큰 박스 — lookahead)
    const uint32_t srcWNow =
        (tScale > 0.0f)
            ? static_cast<uint32_t>(static_cast<float>(w) / tScale)
            : w;
    const uint32_t srcHNow =
        (tScale > 0.0f)
            ? static_cast<uint32_t>(static_cast<float>(h) / tScale)
            : h;
    const auto currentSnap = filter->trackerMgr.snapshot_for_push(srcWNow,
                                                                  srcHNow);
    for (const auto &tb : currentSnap)
      push_tracker_box(tb);
  }
#ifdef _WIN32
  // [Role D] 알림 영역 자동 블러 — 지연 슬롯의 notifRect 주입 (송출 동기화).
  // notifRect는 video_render가 매 프레임 backfillRecentNotifRect로 채운다.
  if (outputSlot->notifRect.width > 0 && outputSlot->notifRect.height > 0 &&
      all_count < (int)(sizeof(all_rects) / sizeof(all_rects[0]))) {
    all_rects[all_count++] = outputSlot->notifRect;
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

#endif
}

#ifdef _WIN32
// ================================================================
// [Role D] 알림 토스트 영역 탐지
//
// 알림 토스트는 앱·OS 버전마다 창 클래스가 제각각이라(Windows 토스트,
// Chrome/Electron 자체 알림 등) 클래스로 식별하지 않는다. 대신 위치+크기
// 로 판정한다: 보이는·uncloaked 최상위 창 중 주 모니터 작업영역 우하단
// 모서리에 토스트 크기로 떠 있는 창을 토스트로 간주한다. 자기 자신의
// 오버레이 HUD는 클래스로 제외한다.
//
// 여러 토스트가 스택되면 모두의 합집합(bounding box)을 반환한다.
// 반환 좌표는 화면(모니터) 좌표 — 블랙리스트 마스킹과 동일 좌표계.
// 한계(v1): 주 모니터 한정.
// ================================================================

// 토스트 탐지 throttle 주기(초).
static constexpr float NOTIF_SCAN_INTERVAL_SEC = 0.1f;

// 토스트 영역에 더하는 여유 마진(px) — 둥근 모서리·슬라이드 애니메이션 대비.
static constexpr int NOTIF_MARGIN = 16;

// 토스트 판정 기하 임계값 (창 클래스 무관, 위치+크기 기반).
static constexpr int NOTIF_MIN_W = 250;    // 최소 너비
static constexpr int NOTIF_MAX_W = 620;    // 최대 너비 (DPI 배율 고려)
static constexpr int NOTIF_MIN_H = 60;     // 최소 높이
static constexpr int NOTIF_MAX_H = 620;    // 최대 높이 (스택·리치 토스트)
static constexpr int NOTIF_EDGE_X = 96;    // 우측 모서리 허용 여백(px)
static constexpr int NOTIF_REGION_Y = 360; // 우하단 영역 세로 범위(px)

struct NotifScanCtx {
  RECT workArea; // 주 모니터 작업 영역 (작업표시줄 제외)
  RECT bbox;     // 탐지된 토스트들의 합집합
  bool found;
};

static BOOL CALLBACK notif_enum_proc(HWND hwnd, LPARAM lparam) {
  auto *ctx = reinterpret_cast<NotifScanCtx *>(lparam);

  if (!IsWindowVisible(hwnd))
    return TRUE;

  // 셸 창(CoreWindow 등)은 평소 DWM cloak 상태 — uncloaked만 관심 대상.
  int cloaked = 0;
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                      sizeof(cloaked))) &&
      cloaked != 0)
    return TRUE;

  // DWM 정확 좌표 (그림자 제외). 실패 시 GetWindowRect로 폴백.
  RECT r{};
  if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r,
                                   sizeof(r)))) {
    if (!GetWindowRect(hwnd, &r))
      return TRUE;
  }

  const RECT &wa = ctx->workArea;
  const int rw = r.right - r.left;
  const int rh = r.bottom - r.top;

  wchar_t cls[96] = {};
  GetClassNameW(hwnd, cls, 96);

  // 실제 토스트 휴리스틱 — 창 클래스 무관, 위치+크기 기반.
  // 알림 토스트는 앱 종류와 무관하게(Windows·Chrome·Discord 등) 우하단
  // 모서리에 토스트 크기로 뜬다. 우리 자신의 오버레이 HUD는 제외한다.
  if (wcscmp(cls, L"SecureCastOverlayV1") == 0)
    return TRUE;
  if (rw < NOTIF_MIN_W || rw > NOTIF_MAX_W || rh < NOTIF_MIN_H ||
      rh > NOTIF_MAX_H)
    return TRUE;                             // 토스트 크기 범위 밖
  if (r.right < wa.right - NOTIF_EDGE_X)     // 우측 모서리에 안 붙음
    return TRUE;
  if (r.bottom < wa.bottom - NOTIF_REGION_Y) // 우하단 영역보다 위
    return TRUE;
  if (r.bottom > wa.bottom + 24)             // 작업영역 아래 (sanity)
    return TRUE;

  // 토스트로 간주 — 합집합 누적 (여러 토스트 스택 대비).
  if (!ctx->found) {
    ctx->bbox = r;
    ctx->found = true;
  } else {
    ctx->bbox.left = std::min(ctx->bbox.left, r.left);
    ctx->bbox.top = std::min(ctx->bbox.top, r.top);
    ctx->bbox.right = std::max(ctx->bbox.right, r.right);
    ctx->bbox.bottom = std::max(ctx->bbox.bottom, r.bottom);
  }
  return TRUE;
}

// 주 모니터 우하단의 Windows 토스트 알림 영역을 탐지한다.
// 발견 시 true + outRect(화면 좌표)를 채운다.
static bool detect_notification_toast(RECT *outRect) {
  NotifScanCtx ctx{};
  if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &ctx.workArea, 0))
    return false; // 작업 영역 조회 실패 — 탐지 불가
  EnumWindows(notif_enum_proc, reinterpret_cast<LPARAM>(&ctx));
  if (ctx.found && outRect)
    *outRect = ctx.bbox;
  return ctx.found;
}

// 두 BlurRect의 합집합(bounding box). width<=0이면 빈 영역으로 취급한다.
static BlurRect blur_rect_union(const BlurRect &a, const BlurRect &b) {
  if (a.width <= 0 || a.height <= 0)
    return b;
  if (b.width <= 0 || b.height <= 0)
    return a;
  int x1 = std::min(a.x, b.x);
  int y1 = std::min(a.y, b.y);
  int x2 = std::max(a.x + a.width, b.x + b.width);
  int y2 = std::max(a.y + a.height, b.y + b.height);
  return {x1, y1, x2 - x1, y2 - y1, 0};
}
#endif // _WIN32

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
  // [Window anchor] 실시간 트래커 좌표 보정은 여기서 하지 않는다. pushFrame
  // 직전에 trackerMgr.snapshot_for_push()가 owner 창의 현재 DWM bounds로 박스
  // 좌표를 다시 계산해 슬롯에 저장한다.

  // [Role D] windowList 스캔 결과를 blacklistMask에 반영 (video_render에서
  // 최우선 차단에 사용). 각 창의 visibleRects(앞 창에 가려진 부분 제외)만
  // 마스크로 넘긴다 — 앞 창이 대부분 가린 상태에서 노출된 띠도 정확히 블러.
  {
    std::lock_guard<std::mutex> lock(filter->blacklistMutex);
    int outCount = 0;
    for (int i = 0;
         i < filter->windowList.count && outCount < SC_MAX_BLUR_RECTS; ++i) {
      const TrackedWindow &tw = filter->windowList.items[i];
      const int n = (tw.visibleCount > 0) ? tw.visibleCount : 1;
      for (int v = 0; v < n && outCount < SC_MAX_BLUR_RECTS; ++v) {
        const RECT &r = (tw.visibleCount > 0) ? tw.visibleRects[v] : tw.bounds;
        filter->blacklistMask.rects[outCount++] = {
            (int)r.left, (int)r.top, (int)(r.right - r.left),
            (int)(r.bottom - r.top), 0};
      }
    }
    filter->blacklistMask.rectCount = outCount;
    if (filter->windowList.count > 0 && filter->logScanThrottle++ % 10 == 0)
      blog(LOG_INFO,
           "[SecureCast] %d blacklisted windows → %d visible rects in blacklistMask.",
           filter->windowList.count, outCount);
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

  // 새 창은 video_render의 backfillRecentSnapshots로 처리한다 — 실시간
  // lingering 대신 최근 슬롯 스냅샷을 소급 보정해 이른 블러를 없앤다.

  filter->prevWindowList = filter->windowList;

  // 매 tick 카운트다운 → 정확히 N프레임(ring buffer 지연) 후 자연 제거
  for (int li = filter->lingeringCount - 1; li >= 0; --li) {
    if (--filter->lingeringWindows[li].ticksRemaining <= 0)
      filter->lingeringWindows[li] =
          filter->lingeringWindows[--filter->lingeringCount];
  }

  // [Role D] 알림 영역 자동 블러 — 매 스캔 "현재 보이는" 토스트들의 union을
  // 반영한다. 토스트가 사라지면 union이 즉시 줄어든다(에피소드 유지 X) —
  // 스택에서 하나씩 닫으면 그 영역이 차례로 빠진다. 직전 스캔 union과 합쳐
  // 스캔 사이 이동(재배열 슬라이드)을 덮는다. 송출 동기화·지연 노출 방지는
  // video_render의 슬롯 notifRect 기록이 담당하므로 hold(쿨다운)는 없다.
  filter->notifScanAccumulator += seconds;
  if (filter->notifScanAccumulator >= NOTIF_SCAN_INTERVAL_SEC) {
    filter->notifScanAccumulator = 0.0f;
    RECT toastRect{};
    BlurRect cur{};
    if (detect_notification_toast(&toastRect)) {
      cur = {static_cast<int>(toastRect.left) - NOTIF_MARGIN,
             static_cast<int>(toastRect.top) - NOTIF_MARGIN,
             static_cast<int>(toastRect.right - toastRect.left) +
                 2 * NOTIF_MARGIN,
             static_cast<int>(toastRect.bottom - toastRect.top) +
                 2 * NOTIF_MARGIN,
             0}; // type 0 = Blur
    }
    // 현재 union ∪ 직전 N스캔 union. 스캔 간 이동을 덮고, 토스트가 사라진
    // 뒤에도 블러가 N스캔(약 0.3초)만큼 더 유지된다 → 송출 화면에서 팝업이
    // 먼저 사라지고 블러가 아주 조금 뒤에 사라진다.
    BlurRect merged = cur;
    for (int h = 0; h < SecureCastFilter::SC_NOTIF_LINGER_SCANS; ++h)
      merged = blur_rect_union(merged, filter->notifScanHist[h]);
    for (int h = SecureCastFilter::SC_NOTIF_LINGER_SCANS - 1; h > 0; --h)
      filter->notifScanHist[h] = filter->notifScanHist[h - 1];
    filter->notifScanHist[0] = cur;
    std::lock_guard<std::mutex> lock(filter->settingsMutex);
    const bool active = (merged.width > 0 && merged.height > 0);
    if (active && !filter->notifBlurActive)
      blog(LOG_INFO, "[SecureCast][D] Notification toast detected.");
    else if (!active && filter->notifBlurActive)
      blog(LOG_INFO, "[SecureCast][D] Notification cleared.");
    filter->notifBlurActive = active;
    filter->notifBlurRect = merged;
  }
#endif
}

// ================================================================
// [Role D] Properties UI
// ================================================================

#define SC_SETTING_BLACKLIST "sc_blacklist"
// #define SC_SETTING_GAME_MODE   "sc_game_mode"  // [v2] 게임 모드 — 현재
// 스코프 외
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

  obs_data_array_t *emptyArr = obs_data_array_create();
  obs_data_set_default_array(settings, SC_SETTING_MANUAL_RECTS, emptyArr);
  obs_data_array_release(emptyArr);
}

static obs_properties_t *securecast_get_properties(void *data) {
  obs_properties_t *props = obs_properties_create();
  obs_properties_add_text(props, SC_SETTING_BLACKLIST,
                          "Blacklist Apps (one per line)", OBS_TEXT_MULTILINE);

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
  blog(LOG_INFO, "[SecureCast][D] Settings updated.");

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
