#include "visual-tracker.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits> // [Fix #7] std::numeric_limits

// OBS 로깅 (blog, LOG_DEBUG, LOG_WARNING 등)
#include <util/base.h>

// [Window anchor] DWM 모니터 변환용 Win32 의존. dwmapi.lib는 CMakeLists에서
// 이미 link됨. 헤더에는 두지 않고 .cpp에서만 사용.
// NOMINMAX: windows.h가 정의하는 min/max 매크로가 std::min/std::max와 충돌하지
// 않도록 차단 (이 파일은 std::min/max를 광범위하게 사용).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include "window_tracker.h" // sc_compute_visible_subrects (z-order 차감)
#include "scroll_motion_hook.h" // 글로벌 휠/키 hook 시각 query
#include "visible_subrects_cache.h" // [Freeze T04] 가시영역 캐시 (DWM 호출 격감)
#endif

// SIMD intrinsics
// x64 MSVC: <intrin.h>가 SSSE3·AVX2·FMA를 모두 포함.
// FMA + AVX2는 Haswell(2013) 이상에서 지원. 런타임에 명시적으로 호출하므로
// /arch 컴파일러 플래그 없이도 인트린직 자체는 사용 가능.
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#define SC_HAS_SSSE3 1
#define SC_HAS_AVX2 1
#elif defined(__AVX2__)
#include <immintrin.h>
#define SC_HAS_SSSE3 1
#define SC_HAS_AVX2 1
#elif defined(__SSSE3__)
#include <tmmintrin.h>
#define SC_HAS_SSSE3 1
#endif

#ifdef _WIN32
// [Freeze T04] 가시영역 캐시 (프로세스 전역, 단일 인스턴스 가정 — WinEventListener
// 와 동일). snapshot_for_push가 const 멤버라 객체 멤버 대신 파일-scope에 둔다.
// OS 창 상태가 전역이고 키가 HWND라 인스턴스 간 공유해도 안전. 내부 shared_mutex
// 로 thread-safe.
//
// ★ 의도적 leak: 정적 전역 객체로 두면 OBS 플러그인 DLL 언로드 시점에 소멸자
//   (~unordered_map / ~shared_mutex)가 실행되는데, 이때 CRT 힙 teardown 순서나
//   다른 스레드와의 경합으로 _CrtIsValidHeapPointer 실패(힙 손상)가 날 수 있다.
//   프로세스 수명 캐시이므로 heap 할당 후 해제하지 않아 소멸자를 아예 돌리지
//   않는다 (plugin-global 표준 패턴). 첫 호출 시 thread-safe하게 1회 초기화.
//
// [Freeze T04 — 현재 비활성] stale 정확성 회귀로 호출처에서 직접 호출로 되돌림.
// 향후 안전한 재구현 시 재활성화 대비해 정의는 남겨둔다 (maybe_unused).
[[maybe_unused]] static VisibleSubrectsCache &visible_cache() {
  static VisibleSubrectsCache *inst = new VisibleSubrectsCache();
  return *inst;
}
#endif

// ──────────────────────────────────────────────────────────────
// P0-4: BGRA → grayscale  (SSSE3 + 스칼라 폴백)
//
// 공식: gray = (29·B + 150·G + 77·R) >> 8  (ITU-R BT.601 근사)
//
// SSSE3 경로: 8 pixels/cycle.
//   mullo_epi16로 16비트 곱셈 → hadd로 채널 합산.
//   주의: maddubs는 G×150 계수가 int8 범위(≤127)를 초과하므로 사용 불가.
//   모든 합은 uint16 범위(≤65280=256×255)에 들어오므로 16비트 모듈러 산술로
//   정확.
// ──────────────────────────────────────────────────────────────
void VisualTrackerManager::bgra_to_gray(const uint8_t *bgra, int width,
                                        int height, int stride,
                                        std::vector<uint8_t> &gray) {
  gray.resize(static_cast<size_t>(width * height));
  uint8_t *dst_base = gray.data();

#ifdef SC_HAS_SSSE3
  // 계수 벡터: [B_coeff, G_coeff, R_coeff, A_coeff, ×2] — 픽셀당 4개 채널
  // _mm_set_epi16(w7,w6,w5,w4,w3,w2,w1,w0) — w0이 word 0 (lowest)
  const __m128i coeff = _mm_set_epi16(0, 77, 150, 29, 0, 77, 150, 29);

  // packus 후 gray 값이 [g0,g1,g2,g3,0,0,0,0, g4,g5,g6,g7,0,0,0,0] 위치에
  // 있으므로 pshufb로 [g0..g7] 연속 배열로 정렬
  const __m128i shuf_pack =
      _mm_set_epi8((char)-1, (char)-1, (char)-1, (char)-1, (char)-1, (char)-1,
                   (char)-1, (char)-1, 11, 10, 9, 8, 3, 2, 1, 0);

  const __m128i zero = _mm_setzero_si128();

  for (int r = 0; r < height; ++r) {
    const uint8_t *src = bgra + static_cast<ptrdiff_t>(r) * stride;
    uint8_t *dst = dst_base + static_cast<ptrdiff_t>(r) * width;
    int c = 0;

    for (; c + 8 <= width; c += 8, src += 32, dst += 8) {
      // 첫 번째 16바이트 (픽셀 0-3)
      __m128i p0 = _mm_loadu_si128((const __m128i *)src);
      // 픽셀 0-1: 8비트 → 16비트 확장 (하위 8바이트)
      __m128i lo0 =
          _mm_unpacklo_epi8(p0, zero); // [B0,G0,R0,A0,B1,G1,R1,A1] as uint16
      // 픽셀 2-3: 8비트 → 16비트 확장 (상위 8바이트)
      __m128i hi0 =
          _mm_unpackhi_epi8(p0, zero); // [B2,G2,R2,A2,B3,G3,R3,A3] as uint16
      // 채널별 곱셈 (low 16비트 반환, 최대 150×255=38250 < 65535 → 오버플로
      // 없음)
      __m128i pl0 = _mm_mullo_epi16(
          lo0, coeff); // [B0*29,G0*150,R0*77,0,B1*29,G1*150,R1*77,0]
      __m128i ph0 = _mm_mullo_epi16(hi0, coeff); // [B2*29,...]
      // 두 번의 수평 합산으로 픽셀당 1개 합 계산 (16비트 래핑 — 최종 uint16
      // 합은 ≤65280) hadd1: [B0*29+G0*150, R0*77, B1*29+G1*150, R1*77,
      // B2*29+G2*150, R2*77, B3*29+G3*150, R3*77] hadd2 with zero: [gray0_×256,
      // gray1_×256, gray2_×256, gray3_×256, 0,0,0,0]
      __m128i s0 =
          _mm_srli_epi16(_mm_hadd_epi16(_mm_hadd_epi16(pl0, ph0), zero), 8);
      // s0: [gray0, gray1, gray2, gray3, 0,0,0,0] as uint16

      // 두 번째 16바이트 (픽셀 4-7)
      __m128i p1 = _mm_loadu_si128((const __m128i *)(src + 16));
      __m128i lo1 = _mm_unpacklo_epi8(p1, zero);
      __m128i hi1 = _mm_unpackhi_epi8(p1, zero);
      __m128i pl1 = _mm_mullo_epi16(lo1, coeff);
      __m128i ph1 = _mm_mullo_epi16(hi1, coeff);
      __m128i s1 =
          _mm_srli_epi16(_mm_hadd_epi16(_mm_hadd_epi16(pl1, ph1), zero), 8);

      // uint16 → uint8 패킹, 이후 pshufb로 [g0..g7] 연속 정렬
      // packus(s0,s1): [g0,g1,g2,g3,0,0,0,0, g4,g5,g6,g7,0,0,0,0] as bytes
      __m128i packed = _mm_shuffle_epi8(_mm_packus_epi16(s0, s1), shuf_pack);
      _mm_storel_epi64((__m128i *)dst, packed); // 하위 8바이트 저장
    }
    // 스칼라 나머지
    for (; c < width; ++c, src += 4)
      *dst++ = (uint8_t)((29 * src[0] + 150 * src[1] + 77 * src[2]) >> 8);
  }
#else
  for (int r = 0; r < height; ++r) {
    const uint8_t *row = bgra + static_cast<ptrdiff_t>(r) * stride;
    uint8_t *grow = dst_base + static_cast<ptrdiff_t>(r) * width;
    for (int c = 0; c < width; ++c)
      grow[c] = (uint8_t)((29 * row[c * 4 + 0] + 150 * row[c * 4 + 1] +
                           77 * row[c * 4 + 2]) >>
                          8);
  }
#endif
}

// ──────────────────────────────────────────────────────────────
// P0-1: 2× 박스-필터 다운샘플
// ──────────────────────────────────────────────────────────────
std::vector<uint8_t> VisualTrackerManager::downsample_2x(const uint8_t *gray,
                                                         int gw, int gh,
                                                         int &out_w,
                                                         int &out_h) {
  out_w = gw / 2;
  out_h = gh / 2;
  if (out_w <= 0 || out_h <= 0) {
    out_w = out_h = 0;
    return {};
  }

  std::vector<uint8_t> result(static_cast<size_t>(out_w * out_h));
  for (int r = 0; r < out_h; ++r) {
    const uint8_t *row0 = gray + static_cast<ptrdiff_t>(r * 2) * gw;
    const uint8_t *row1 = gray + static_cast<ptrdiff_t>(r * 2 + 1) * gw;
    uint8_t *dst = result.data() + static_cast<ptrdiff_t>(r) * out_w;
    for (int c = 0; c < out_w; ++c)
      dst[c] = (uint8_t)((row0[c * 2] + row0[c * 2 + 1] + row1[c * 2] +
                          row1[c * 2 + 1] + 2) >>
                         2);
  }
  return result;
}

void VisualTrackerManager::downsample_2x_into(const uint8_t *gray, int gw,
                                              int gh, std::vector<uint8_t> &out,
                                              int &out_w, int &out_h) {
  out_w = gw / 2;
  out_h = gh / 2;
  if (out_w <= 0 || out_h <= 0) {
    out_w = out_h = 0;
    out.clear();
    return;
  }
  out.resize(static_cast<size_t>(out_w * out_h));
  for (int r = 0; r < out_h; ++r) {
    const uint8_t *row0 = gray + static_cast<ptrdiff_t>(r * 2) * gw;
    const uint8_t *row1 = gray + static_cast<ptrdiff_t>(r * 2 + 1) * gw;
    uint8_t *dst = out.data() + static_cast<ptrdiff_t>(r) * out_w;
    for (int c = 0; c < out_w; ++c)
      dst[c] = (uint8_t)((row0[c * 2] + row0[c * 2 + 1] + row1[c * 2] +
                          row1[c * 2 + 1] + 2) >>
                         2);
  }
}

// ──────────────────────────────────────────────
// Static helpers
// ──────────────────────────────────────────────

std::vector<uint8_t>
VisualTrackerManager::extract_gray_crop(const uint8_t *gray, int gstride,
                                        int gw, int gh, int cx, int cy, int cw,
                                        int ch, int &out_tw, int &out_th) {
  const int x0 = std::max(cx, 0);
  const int y0 = std::max(cy, 0);
  const int x1 = std::min(cx + cw, gw);
  const int y1 = std::min(cy + ch, gh);
  out_tw = x1 - x0;
  out_th = y1 - y0;

  if (out_tw <= 0 || out_th <= 0)
    return {};

  std::vector<uint8_t> crop(static_cast<size_t>(out_tw * out_th));
  for (int r = 0; r < out_th; ++r)
    std::memcpy(crop.data() + static_cast<ptrdiff_t>(r) * out_tw,
                gray + static_cast<ptrdiff_t>(y0 + r) * gstride + x0,
                static_cast<size_t>(out_tw));
  return crop;
}

float VisualTrackerManager::box_iou(const VtOcrBox &a, const Tracker &b) {
  const float ax0 = a.x, ay0 = a.y, ax1 = a.x + a.w, ay1 = a.y + a.h;
  const float bx0 = b.x, by0 = b.y, bx1 = b.x + b.bw, by1 = b.y + b.bh;
  const float ix0 = std::max(ax0, bx0), iy0 = std::max(ay0, by0);
  const float ix1 = std::min(ax1, bx1), iy1 = std::min(ay1, by1);
  if (ix0 >= ix1 || iy0 >= iy1)
    return 0.0f;
  const float inter = (ix1 - ix0) * (iy1 - iy0);
  const float u = a.w * a.h + b.bw * b.bh - inter;
  return (u > 0.0f) ? inter / u : 0.0f;
}

// ──────────────────────────────────────────────────────────────
// Tier 3: precompute_tmpl_stats
//
// tmpl(uint8) → tmpl_float(centered float) + tmpl_dT + tmpl_sumCt
// tmpl이 변경될 때마다 호출해야 AVX2 NCC가 정확하게 동작한다.
// ──────────────────────────────────────────────────────────────
void VisualTrackerManager::precompute_tmpl_stats(Tracker &tr) {
  const int n = tr.tw * tr.th;
  if (n <= 0 || (int)tr.tmpl.size() < n) {
    tr.tmpl_float = nullptr;
    tr.tmpl_dT = 0.0f;
    tr.tmpl_sumCt = 0.0f;
    return;
  }

  float tmean = 0.0f;
  for (int i = 0; i < n; ++i)
    tmean += tr.tmpl[i];
  tmean /= static_cast<float>(n);

  // 1-A: new shared_ptr 로 교체 — Phase B가 이전 ptr를 참조 중이어도 안전
  auto new_float = std::make_shared<std::vector<float>>(static_cast<size_t>(n));
  float dT = 0.0f, sumCt = 0.0f;
  for (int i = 0; i < n; ++i) {
    const float ct = static_cast<float>(tr.tmpl[i]) - tmean;
    (*new_float)[i] = ct;
    dT += ct * ct;
    sumCt += ct;
  }
  tr.tmpl_float = std::move(new_float);
  tr.tmpl_dT = dT;
  tr.tmpl_sumCt = sumCt;
}

// ──────────────────────────────────────────────────────────────
// Tier 3: AVX2 NCC
//
// 계산식:
//   S1  = Σ iv_i          (이미지 값 합)
//   S12 = Σ ct_i × iv_i   (ct_i = tmpl_float[i] = tmpl[i] - tmean)
//   S2  = Σ iv_i²         (이미지 제곱합)
//   imean   = S1 / N
//   num     = S12 - imean × sumCt    (= Σ ct_i × (iv_i - imean))
//   dI      = S2  - S1 × imean       (= Σ(iv_i - imean)²)
//   NCC     = num / sqrt(tmpl_dT × dI)
//
// AVX2 경로: 8 float/cycle (FMA 사용). SSSE3-only 또는 스칼라 폴백 포함.
// ──────────────────────────────────────────────────────────────

#ifdef SC_HAS_AVX2
static inline float hsum256_ps(__m256 v) {
  __m128 lo = _mm256_castps256_ps128(v);
  __m128 hi = _mm256_extractf128_ps(v, 1);
  lo = _mm_add_ps(lo, hi);
  lo = _mm_hadd_ps(lo, lo);
  lo = _mm_hadd_ps(lo, lo);
  return _mm_cvtss_f32(lo);
}
#endif

float VisualTrackerManager::ncc_at_simd(const uint8_t *gray, int gstride,
                                        int gw, int gh, const Tracker &tr,
                                        int sx, int sy) const {
  const int tw = tr.tw, th = tr.th;
  if (sx < 0 || sy < 0 || sx + tw > gw || sy + th > gh)
    return -1.0f;
  // 1-A: tmpl_float은 shared_ptr<const vector<float>> — nullptr 또는 빈 경우
  // 스칼라 폴백
  if (!tr.tmpl_float || tr.tmpl_float->empty() || tw <= 0 || th <= 0)
    return ncc_at(gray, gstride, gw, gh, tr.tmpl, tw, th, sx, sy);

  const int n = tw * th;
  const float *ct = tr.tmpl_float->data();

  float S1 = 0.0f, S12 = 0.0f, S2 = 0.0f;

#ifdef SC_HAS_AVX2
  __m256 vS1 = _mm256_setzero_ps();
  __m256 vS12 = _mm256_setzero_ps();
  __m256 vS2 = _mm256_setzero_ps();

  for (int r = 0; r < th; ++r) {
    const float *ct_row = ct + r * tw;
    const uint8_t *iv_row =
        gray + static_cast<ptrdiff_t>(sy + r) * gstride + sx;
    int c = 0;
    for (; c + 8 <= tw; c += 8) {
      __m128i iv8 = _mm_loadl_epi64((const __m128i *)(iv_row + c));
      __m256i iv32 = _mm256_cvtepu8_epi32(iv8);
      __m256 iv_f = _mm256_cvtepi32_ps(iv32);
      __m256 ct_v = _mm256_loadu_ps(ct_row + c);
      vS1 = _mm256_add_ps(vS1, iv_f);
      vS12 = _mm256_fmadd_ps(ct_v, iv_f, vS12);
      vS2 = _mm256_fmadd_ps(iv_f, iv_f, vS2);
    }
    for (; c < tw; ++c) {
      const float iv_f = static_cast<float>(iv_row[c]);
      S1 += iv_f;
      S12 += ct_row[c] * iv_f;
      S2 += iv_f * iv_f;
    }
  }
  S1 += hsum256_ps(vS1);
  S12 += hsum256_ps(vS12);
  S2 += hsum256_ps(vS2);
#else
  // 스칼라 폴백 (AVX2 미지원 환경)
  for (int r = 0; r < th; ++r) {
    const float *ct_row = ct + r * tw;
    const uint8_t *iv_row =
        gray + static_cast<ptrdiff_t>(sy + r) * gstride + sx;
    for (int c = 0; c < tw; ++c) {
      const float iv_f = static_cast<float>(iv_row[c]);
      S1 += iv_f;
      S12 += ct_row[c] * iv_f;
      S2 += iv_f * iv_f;
    }
  }
#endif

  const float imean = S1 / static_cast<float>(n);
  const float num = S12 - imean * tr.tmpl_sumCt;
  const float dI = S2 - S1 * imean;
  if (dI < 0.0f)
    return 0.0f; // 수치 소거로 음수 → sqrt NaN 방지
  const float denom = std::sqrt(tr.tmpl_dT * dI);
  if (denom < 1e-5f)
    return 0.0f;
  return num / denom;
}

// ──────────────────────────────────────────────
// NCC at a single search position (float arithmetic)
// ──────────────────────────────────────────────

float VisualTrackerManager::ncc_at(const uint8_t *gray, int gstride, int gw,
                                   int gh, const std::vector<uint8_t> &tmpl,
                                   int tw, int th, int sx, int sy) const {
  if (sx < 0 || sy < 0 || sx + tw > gw || sy + th > gh)
    return -1.0f;
  if (tmpl.empty() || tw <= 0 || th <= 0)
    return -1.0f;

  const int n = tw * th;

  float tmean = 0.0f;
  for (int i = 0; i < n; ++i)
    tmean += tmpl[i];
  tmean /= n;

  float imean = 0.0f;
  for (int r = 0; r < th; ++r)
    for (int c = 0; c < tw; ++c)
      imean += gray[static_cast<ptrdiff_t>(sy + r) * gstride + (sx + c)];
  imean /= n;

  float num = 0.0f, dT = 0.0f, dI = 0.0f;
  for (int r = 0; r < th; ++r) {
    for (int c = 0; c < tw; ++c) {
      const float t = tmpl[r * tw + c] - tmean;
      const float iv =
          gray[static_cast<ptrdiff_t>(sy + r) * gstride + (sx + c)] - imean;
      num += t * iv;
      dT += t * t;
      dI += iv * iv;
    }
  }
  const float denom = std::sqrt(dT * dI);
  if (denom < 1e-5f)
    return 0.0f;
  return num / denom;
}

// ──────────────────────────────────────────────────────────────
// P0-1 + P0-3: 피라미드 매칭 + 속도 예측
//
// Level 0 (coarse, 1/2 해상도):
//   - 템플릿과 프레임 모두 1/2 다운샘플
//   - 예측 위치 (x + vx, y + vy) 중심으로 stride-2 탐색
//   - 탐색 반경: SEARCH_NEAR or SEARCH_FAR (속도 크기 보정)
//
// Level 1 (fine, 원해상도):
//   - coarse best 위치 주변 ±4px (stride-1) 정밀 탐색 (coarse stride-2=4px gap
//   완전 커버)
//
// 속도 (P0-3):
//   - 성공 시: vx/vy EMA 업데이트 (0.7×old + 0.3×new)
//   - 실패 시: 속도 0.5× 감쇠
// ──────────────────────────────────────────────────────────────

// ──────────────────────────────────────────────────────────────
// Tier 3: 2-Level 피라미드 매칭
//
// Level 0 (1/4 해상도 coarse):
//   - 템플릿 4×4 avg 다운샘플 (on-the-fly, 160×80→40×20=800 ops)
//   - 예측 위치 중심, SEARCH_FAR/4 반경, stride-2 탐색
//   - 탐색 공간: 최대 62² = 3844 위치 (기존 half 15876 대비 4×↓)
//
// Level 1 (원해상도 fine):
//   - quarter best ×4 위치 주변 ±6px AVX2 NCC (13×13=169 위치)
//   - quarter stride-2 = 8px 간격; coarse best ±1 stride 오차 시 fine ±6으로
//   보정
// ──────────────────────────────────────────────────────────────
// [Fix #5] ncc_pyramid_search — 무상태 순수 NCC 피라미드 탐색
//
// tr는 const& (상태 변경 없음). 결과만 NccSearchResult로 반환.
// radius: 탐색 반경 (SEARCH_NEAR 또는 SEARCH_FAR).
// ──────────────────────────────────────────────────────────────
VisualTrackerManager::NccSearchResult VisualTrackerManager::ncc_pyramid_search(
    const Tracker &tr, const uint8_t *gray, int gw, int gh,
    const uint8_t *quarterGray, int qw, int qh, float predX, float predY,
    int radius) const {

  NccSearchResult out{-1.0f, (int)(predX + (tr.bw - tr.tw) * 0.5f),
                      (int)(predY + (tr.bh - tr.th) * 0.5f)};

  if (tr.tmpl.empty() || tr.tw <= 0 || tr.th <= 0)
    return out;

  // ----- Level 0: coarse (1/4 해상도) -----
  const int qtw = std::max(tr.tw / 4, 1);
  const int qth = std::max(tr.th / 4, 1);
  std::vector<uint8_t> qtmpl(static_cast<size_t>(qtw * qth));
  for (int r = 0; r < qth; ++r) {
    for (int c = 0; c < qtw; ++c) {
      int sum = 0, cnt = 0;
      for (int dr = 0; dr < 4; ++dr) {
        const int sr = r * 4 + dr;
        if (sr >= tr.th)
          break;
        for (int dc = 0; dc < 4; ++dc) {
          const int sc2 = c * 4 + dc;
          if (sc2 >= tr.tw)
            break;
          sum += tr.tmpl[sr * tr.tw + sc2];
          ++cnt;
        }
      }
      qtmpl[r * qtw + c] = (uint8_t)(cnt > 0 ? sum / cnt : 0);
    }
  }

  assert(quarterGray != nullptr && qw > 0 && qh > 0);
  const int qcx = (int)((predX + tr.bw * 0.5f) * 0.25f);
  const int qcy = (int)((predY + tr.bh * 0.5f) * 0.25f);
  const int qr = (radius + 3) / 4;
  const int qsx0 = qcx - qr - qtw / 2;
  const int qsy0 = qcy - qr - qth / 2;
  const int qsx1 = qcx + qr - qtw / 2;
  const int qsy1 = qcy + qr - qth / 2;

  float bestQScore = -1.0f;
  int bestQX = qcx - qtw / 2;
  int bestQY = qcy - qth / 2;
  for (int sy = qsy0; sy <= qsy1; sy += 2) {
    for (int sx = qsx0; sx <= qsx1; sx += 2) {
      const float sc = ncc_at(quarterGray, qw, qw, qh, qtmpl, qtw, qth, sx, sy);
      if (sc > bestQScore) {
        bestQScore = sc;
        bestQX = sx;
        bestQY = sy;
      }
    }
  }

  // ----- Level 1: fine (원해상도, quarter best ×4 주변 ±6px) -----
  const int refineX = bestQX * 4;
  const int refineY = bestQY * 4;

  float bestScore = -1.0f;
  int bestX = out.x;
  int bestY = out.y;
  for (int dy = -6; dy <= 6; ++dy) {
    for (int dx = -6; dx <= 6; ++dx) {
      const float sc =
          ncc_at_simd(gray, gw, gw, gh, tr, refineX + dx, refineY + dy);
      if (sc > bestScore) {
        bestScore = sc;
        bestX = refineX + dx;
        bestY = refineY + dy;
      }
    }
  }

  out.score = bestScore;
  out.x = bestX;
  out.y = bestY;
  return out;
}

// ──────────────────────────────────────────────────────────────
void VisualTrackerManager::update_one_pyramid(Tracker &tr, const uint8_t *gray,
                                              int gw, int gh,
                                              const uint8_t *quarterGray,
                                              int qw, int qh) {
  if (tr.tmpl.empty() || tr.tw <= 0 || tr.th <= 0)
    return;

  // P0-3: 속도 기반 예측 위치
  const float predX = tr.x + tr.vx;
  const float predY = tr.y + tr.vy;

  // [Fix #5] NEAR 반경으로 1차 탐색
  const int velBonus =
      std::min((int)(std::abs(tr.vx) + std::abs(tr.vy)), SEARCH_FAR);
  const int radius1 = std::min(
      ((tr.lastScore >= SCORE_OK) ? SEARCH_NEAR : SEARCH_FAR) + velBonus,
      SEARCH_FAR);

  auto best = ncc_pyramid_search(tr, gray, gw, gh, quarterGray, qw, qh, predX,
                                 predY, radius1);

  // [Fix #5] NEAR 탐색 실패 시 FAR 재시도 (near→far fallback)
  if (radius1 < SEARCH_FAR && best.score < SCORE_LOST) {
    auto farBest = ncc_pyramid_search(tr, gray, gw, gh, quarterGray, qw, qh,
                                      predX, predY, SEARCH_FAR);
    if (farBest.score > best.score) {
      best = farBest;
      static int s_fallback_throttle = 0;
      if (++s_fallback_throttle >= 60) {
        s_fallback_throttle = 0;
        blog(LOG_DEBUG,
             "[SecureCast][fallback] FAR search triggered for tracker id=%d",
             tr.id);
      }
    }
  }

  const float bestScore = best.score;
  const int bestX = best.x;
  const int bestY = best.y;

  tr.lastScore = bestScore;
  if (bestScore >= SCORE_LOST) {
    // bestX/Y: template top-left → box top-left
    const float newX =
        static_cast<float>(bestX) - (tr.bw - static_cast<float>(tr.tw)) * 0.5f;
    const float newY =
        static_cast<float>(bestY) - (tr.bh - static_cast<float>(tr.th)) * 0.5f;

    // P0-3: EMA 속도 업데이트
    tr.vx = 0.7f * tr.vx + 0.3f * (newX - tr.x);
    tr.vy = 0.7f * tr.vy + 0.3f * (newY - tr.y);

    tr.x = newX;
    tr.y = newY;
    tr.framesSinceMatch = 0;

    if (bestScore >= SCORE_REFRESH) {
      int rdummy, cdummy;
      auto refreshed = extract_gray_crop(gray, gw, gw, gh, bestX, bestY, tr.tw,
                                         tr.th, rdummy, cdummy);
      if (rdummy == tr.tw && cdummy == tr.th && !refreshed.empty()) {
        tr.tmpl = std::move(refreshed);
        precompute_tmpl_stats(tr); // float 템플릿 갱신
      }
    }
  } else {
    ++tr.framesSinceMatch;
    tr.vx *= 0.5f;
    tr.vy *= 0.5f;
  }
}

// ──────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────

void VisualTrackerManager::update_all(const uint8_t *bgra, int width,
                                      int height, int stride) {
  std::vector<uint8_t> gray;
  bgra_to_gray(bgra, width, height, stride, gray);
  update_all_gray(gray.data(), width, height);
}

// ──────────────────────────────────────────────────────────────
// update_all_gray — 1-A 3-phase 재구조
//
// Phase A (unique_lock 짧게): trackers_ 스냅샷 로컬 복사.
//   - tmpl: ~12 KB × 8 트래커 = ~96 KB 복사. 락 보유 시간 < 0.1 ms.
//   - tmpl_float: shared_ptr 복사 (포인터 + refcount). 데이터 복사 없음.
//
// Phase B (락 없음): NCC, 피라미드 매칭, stale/fail 가드.
//   - ncc_at_simd가 shared_ptr->data() 직접 참조. register_or_update가
//     새 shared_ptr로 교체해도 Phase B는 이전 ptr를 계속 안전하게 사용.
//
// Phase C (unique_lock 짧게): 결과 커밋 + dead tracker 제거.
//   - templateTs 비교: register_or_update가 사이에 템플릿을 갱신했으면
//     Phase B의 템플릿 필드를 덮어쓰지 않음 (충돌 방지).
// ──────────────────────────────────────────────────────────────
void VisualTrackerManager::update_all_gray(const uint8_t *gray, int gw,
                                           int gh) {
  // ── Phase A: 스냅샷 (unique_lock 짧게) ──
  std::vector<Tracker> local;
  {
    std::unique_lock<std::shared_mutex> lock(stateMtx_);
    local = trackers_;
  }
  if (local.empty())
    return;

  // [Fix #7-D] NCC 사이클 wall-clock 측정
  const auto ncc_t0 = std::chrono::steady_clock::now();

  // ── Phase B: NCC 연산 (락 없음) ──
  int hw, hh, qw, qh;
  downsample_2x_into(gray, gw, gh, halfGrayBuf_, hw, hh);
  downsample_2x_into(halfGrayBuf_.data(), hw, hh, quarterGrayBuf_, qw, qh);

  for (auto &tr : local) {
    ++tr.framesSinceOcrValidate;

    // 3-A: stale OCR 가드 — 2초간 OCR 미갱신 시 NCC 생략, HARD_EXPIRY에 맡김
    if (tr.framesSinceOcrValidate >= STALE_OCR_FRAMES) {
      ++tr.framesSinceMatch;
      continue;
    }

    // 3-A: consecutive fail fast-fail — 연속 SCORE_LOST 미만 임계 초과 시 NCC
    // 생략
    if (tr.consecutiveLostFrames >= CONSEC_LOST_LIMIT) {
      tr.lastScore *= 0.9f;
      ++tr.framesSinceMatch;
      continue;
    }

    if (qw > 0 && qh > 0)
      update_one_pyramid(tr, gray, gw, gh, quarterGrayBuf_.data(), qw, qh);

    if (tr.lastScore < SCORE_LOST)
      ++tr.consecutiveLostFrames;
    else
      tr.consecutiveLostFrames = 0;
  }

  // [Fix #7-D] NCC 사이클 종료 — 33ms 초과 시 경고
  {
    const auto ncc_t1 = std::chrono::steady_clock::now();
    const auto ncc_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(ncc_t1 - ncc_t0)
            .count();
    if (ncc_ms > 33) {
      blog(LOG_WARNING, "[SecureCast][ncc-slow] NCC cycle took %lld ms",
           static_cast<long long>(ncc_ms));
    }
  }

  // ── Phase C: 커밋 + dead tracker 제거 (unique_lock 짧게) ──
  {
    std::unique_lock<std::shared_mutex> lock(stateMtx_);
    for (const auto &lt : local) {
      auto it = std::find_if(trackers_.begin(), trackers_.end(),
                             [&](const Tracker &t) { return t.id == lt.id; });
      if (it == trackers_.end())
        continue; // 사이에 제거/추가된 트래커

      it->x = lt.x;
      it->y = lt.y;
      it->vx = lt.vx;
      it->vy = lt.vy;
      it->lastScore = lt.lastScore;

      // [Fix #2-C] ocrRevision 비교:
      // revision이 같으면 Phase B 실행 중 OCR 리셋 없음 → 카운터 커밋
      // revision이 다르면 Phase B 실행 중 OCR이 리셋함 → 카운터 부보존
      if (it->ocrRevision == lt.ocrRevision) {
        it->framesSinceMatch = lt.framesSinceMatch;
        it->framesSinceOcrValidate = lt.framesSinceOcrValidate;
        it->consecutiveLostFrames = lt.consecutiveLostFrames;
      } else {
        // OCR이 사이에 리셋 — Phase B가 쪬은 오래된 카운터를 커밋하지 않음
        static int s_race_throttle = 0;
        if (++s_race_throttle >= 30) {
          s_race_throttle = 0;
          blog(LOG_DEBUG,
               "[SecureCast][race] tracker id=%d ocrRevision mismatch "
               "(it=%u lt=%u), preserving counters",
               it->id, it->ocrRevision, lt.ocrRevision);
        }
      }

      // templateTs 동일 → Phase B에서 refresh된 템플릿 커밋.
      // templateTs 변경 → register_or_update가 더 최신 템플릿을 설정했으므로
      // 생략.
      if (it->templateTs == lt.templateTs) {
        it->tmpl = lt.tmpl; // refresh 발생 시 갱신, 아니면 원본 그대로
        it->tmpl_float = lt.tmpl_float;
        it->tmpl_dT = lt.tmpl_dT;
        it->tmpl_sumCt = lt.tmpl_sumCt;
        it->tw = lt.tw;
        it->th = lt.th;
      }
    }

    // dead tracker 제거 (역순: erase 시 인덱스 shift 방지)
    // [Window anchor v4] owner 창이 살아있는 트래커는 NCC 실패해도 보존.
    // 빠른 드래그 시 NCC가 못 따라가도 owner 창의 DWM bounds로 좌표 계산이
    // 가능. 이 시간 동안 OCR 갱신이 없으면 텍스트 소멸로 보고 제거 (드래그 중에는
    // OCR이 motion blur로 자주 실패 → 어느 정도 유예 필요).
    // [잔상 단축] 5초→3초: 다른 창으로 전환 시 이전 위치에 블러가 남는 시간을
    // 줄인다. 3초도 일반 드래그 관용엔 충분(드래그가 3초 내내 OCR 실패할 일은 드묾).
    constexpr int kOwnerBoundExpiry = HARD_EXPIRY * 3; // ~3초 @ 30Hz
    for (int i = (int)trackers_.size() - 1; i >= 0; --i) {
      const auto &tr = trackers_[i];
      bool ownerAlive = false;
#ifdef _WIN32
      if (tr.ownerWin) {
        HWND h = reinterpret_cast<HWND>(tr.ownerWin);
        ownerAlive = IsWindow(h);
      }
#endif
      const bool nccLost = tr.framesSinceMatch >= FRAMES_LOST;
      const bool ocrExpired = tr.framesSinceOcrValidate >= HARD_EXPIRY;
      const bool ocrExpiredLong =
          tr.framesSinceOcrValidate >= kOwnerBoundExpiry;
      // owner alive: 오직 ocrExpiredLong(5초) 일 때만 제거. NCC 실패는 무시.
      // owner 없음: 기존 조건 (NCC 실패 OR OCR 만료 1초).
      const bool shouldErase =
          ownerAlive ? ocrExpiredLong : (nccLost || ocrExpired);
      if (shouldErase)
        trackers_.erase(trackers_.begin() + i);
    }

    // 트래커 stability 평가 — 모션 hysteresis 종료 조건. 모든 활성 트래커가
    // 새 위치를 안정적으로 매칭 중이면 stable. 박스 확장은 stable=false 동안
    // 유지되어 NCC가 새 위치 따라잡기 전까지 노출 방지.
    bool stable = true;
    for (const auto &tr : trackers_) {
      if (tr.lastScore < SCORE_OK || tr.framesSinceMatch > 0) {
        stable = false;
        break;
      }
    }
    allTrackersStable_.store(stable, std::memory_order_release);
  }
}

void VisualTrackerManager::expand_boxes_if_motion(std::vector<VtOcrBox> &boxes,
                                                  uint32_t src_w,
                                                  uint32_t src_h) const {
  (void)src_w; // 수직 확장만 — 인터페이스 일관성을 위해 받음
  // 신호 소스: 글로벌 mouse/keyboard low-level hook이 휠/PageUp/Down/Home/End
  // 발생 시 paint 이전에 시각을 기록한다. EMA 사후 신호와 달리 첫 모션부터
  // 즉시 활성 (사용자 입력 → OS hook → 콜백 < paint).
  const int64_t last = securecast::last_scroll_motion_time_ms();
  if (last <= 0)
    return;
  const auto now = std::chrono::steady_clock::now();
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now.time_since_epoch())
                             .count();
  const int64_t since = now_ms - last;
  // safety cap: 비정상 (트래커 영원히 unstable) 시나리오에서 무한 확장 방지.
  if (since >= MOTION_MAX_HOLD_MS)
    return;
  // 최소 hold 후엔 트래커가 모두 stable이면 즉시 종료. unstable이면 유지 →
  // "완벽 추적 전까지 원래 크기 복귀 안 함" 보장.
  if (since >= MOTION_MIN_HOLD_MS &&
      allTrackersStable_.load(std::memory_order_acquire))
    return;
  for (auto &b : boxes) {
    const float origTop = b.y;
    const float newY = origTop - MOTION_BLUR_EXPAND_PX;
    b.y = newY < 0.0f ? 0.0f : newY;
    const float gainedTop = origTop - b.y; // 위로 실제 확장된 양 (0~60)
    b.h += gainedTop + MOTION_BLUR_EXPAND_PX;
    if (src_h > 0 && b.y + b.h > static_cast<float>(src_h))
      b.h = static_cast<float>(src_h) - b.y;
    if (b.h < 0.0f)
      b.h = 0.0f;
  }
}

void VisualTrackerManager::register_or_update(
    const std::vector<VtOcrBox> &ocr_boxes, const uint8_t *bgra, int width,
    int height, int stride) {
  if (ocr_boxes.empty())
    return;

  // 3-A: OCR 갱신 타임스탬프 기록 (락 전에 기록해도 무방 — 단조 증가)
  lastOcrUpdateTsMs_.store(
      static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count()),
      std::memory_order_relaxed);

  // 무거운 BGRA→gray 변환은 락 외부에서 수행
  std::vector<uint8_t> gray;
  bgra_to_gray(bgra, width, height, stride, gray);

  std::unique_lock<std::shared_mutex> lock(stateMtx_);

  const int N = (int)ocr_boxes.size();
  const int M = (int)trackers_.size();

  std::vector<bool> matchedOcr(N, false);
  std::vector<bool> matchedTr(M, false);

  // greedy IoU 매칭 [Fix #7-B] IoU threshold 0.30 → 0.20 + 거리 fallback
  for (int n = 0; n < N; ++n) {
    float bestIou = 0.20f; // [Fix #7-B] 0.30 → 0.20
    float bestDist = std::numeric_limits<float>::max();
    int bestM = -1;
    for (int m = 0; m < M; ++m) {
      if (matchedTr[m])
        continue;
      const float iou = box_iou(ocr_boxes[n], trackers_[m]);
      if (iou > bestIou) {
        bestIou = iou;
        bestM = m;
        bestDist = 0.0f;
      } else if (iou == 0.0f) {
        // IoU 0: 타입이 같을 때 중심 거리 fallback
        if (ocr_boxes[n].type && trackers_[m].type &&
            std::strcmp(ocr_boxes[n].type, trackers_[m].type) == 0) {
          const float cx1 = ocr_boxes[n].x + ocr_boxes[n].w * 0.5f;
          const float cy1 = ocr_boxes[n].y + ocr_boxes[n].h * 0.5f;
          const float cx2 = trackers_[m].x + trackers_[m].bw * 0.5f;
          const float cy2 = trackers_[m].y + trackers_[m].bh * 0.5f;
          const float d = std::hypot(cx1 - cx2, cy1 - cy2);
          const float thresh =
              0.5f * std::min(trackers_[m].bw, trackers_[m].bh);
          if (d < thresh && d < bestDist && bestIou == 0.20f) {
            bestDist = d;
            bestM = m;
            // [Fix #7] fallback 로그
            static int s_iou_miss_throttle = 0;
            if (++s_iou_miss_throttle >= 60) {
              s_iou_miss_throttle = 0;
              blog(
                  LOG_DEBUG,
                  "[SecureCast][iou-miss] dist-fallback matched type=%s d=%.1f",
                  ocr_boxes[n].type, d);
            }
          }
        }
      }
    }
    if (bestM >= 0) {
      matchedOcr[n] = true;
      matchedTr[bestM] = true;

      auto &tr = trackers_[bestM];
      // x/y는 NCC가 관리 — OCR 좌표로 덮어쓰지 않음 (OCR은 250ms 전 프레임
      // 기준) 크기(bw/bh)는 OCR 결과로 갱신 (텍스트 길이 변경 대응)
      tr.bw = ocr_boxes[n].w;
      tr.bh = ocr_boxes[n].h;
      tr.framesSinceMatch = 0;
      tr.framesSinceOcrValidate = 0;
      tr.consecutiveLostFrames = 0; // 3-A: OCR 재확인으로 fast-fail 해제
      tr.lastScore = 1.0f;
      ++tr.ocrRevision; // [Fix #2-B] OCR worker 리셋 시그널

      // 템플릿은 현재 NCC 위치(tr.x, tr.y)에서 재추출 (OCR 위치 아님)
      int tcx = (int)tr.x, tcy = (int)tr.y;
      int tcw = (int)tr.bw, tch = (int)tr.bh;
      if (tcw > MAX_TMPL_W) {
        tcx += (tcw - MAX_TMPL_W) / 2;
        tcw = MAX_TMPL_W;
      }
      if (tch > MAX_TMPL_H) {
        tcy += (tch - MAX_TMPL_H) / 2;
        tch = MAX_TMPL_H;
      }

      int tw, th;
      auto crop = extract_gray_crop(gray.data(), width, width, height, tcx, tcy,
                                    tcw, tch, tw, th);
      if (!crop.empty()) {
        tr.tw = tw;
        tr.th = th;
        tr.tmpl = std::move(crop);
        precompute_tmpl_stats(tr); // AVX2 float 템플릿 갱신
        ++tr.templateTs; // 1-A: Phase C가 이 템플릿을 덮어쓰지 않도록 마킹
      }
    }
  }

  // 매칭 안 된 OCR 박스 → 신규 트래커 (P0-2: MAX_TRACKERS 초과 시 거부)
  for (int n = 0; n < N; ++n) {
    if (matchedOcr[n])
      continue;
    if ((int)trackers_.size() >= MAX_TRACKERS)
      break; // ghost 누적 방지

    const auto &box = ocr_boxes[n];
    int tcx = (int)box.x, tcy = (int)box.y;
    int tcw = (int)box.w, tch = (int)box.h;
    if (tcw > MAX_TMPL_W) {
      tcx += (tcw - MAX_TMPL_W) / 2;
      tcw = MAX_TMPL_W;
    }
    if (tch > MAX_TMPL_H) {
      tcy += (tch - MAX_TMPL_H) / 2;
      tch = MAX_TMPL_H;
    }

    int tw, th;
    auto crop = extract_gray_crop(gray.data(), width, width, height, tcx, tcy,
                                  tcw, tch, tw, th);
    if (crop.empty())
      continue;

    Tracker tr;
    tr.id = nextId_++;
    tr.type = box.type;
    tr.x = box.x;
    tr.y = box.y;
    tr.bw = box.w;
    tr.bh = box.h;
    tr.tw = tw;
    tr.th = th;
    tr.tmpl = std::move(crop);
    tr.lastScore = 1.0f;
    tr.framesSinceMatch = 0;
    tr.framesSinceOcrValidate = 0;
    tr.vx = 0.0f;
    tr.vy = 0.0f;
    precompute_tmpl_stats(tr); // AVX2 float 템플릿 초기화
    trackers_.push_back(std::move(tr));
  }
}

// 1-E: pre-converted gray 버퍼를 사용하는 버전 — BGRA→gray 중복 변환 없음.
// [Window anchor] owner 정보 없는 호출은 빈 owners 벡터로 새 오버로드에 위임.
void VisualTrackerManager::register_or_update_gray(
    const std::vector<VtOcrBox> &ocr_boxes, const uint8_t *gray, int gw,
    int gh) {
  static const std::vector<VtBoxOwner> kNoOwners;
  register_or_update_gray(ocr_boxes, kNoOwners, gray, gw, gh);
}

void VisualTrackerManager::register_or_update_gray(
    const std::vector<VtOcrBox> &ocr_boxes,
    const std::vector<VtBoxOwner> &owners, const uint8_t *gray, int gw,
    int gh) {
  if (ocr_boxes.empty() || !gray)
    return;

  // 박스 수와 일치할 때만 owner 정보 사용. 불일치는 owner 정보 없음으로 처리.
  const bool useOwners = owners.size() == ocr_boxes.size();

  lastOcrUpdateTsMs_.store(
      static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count()),
      std::memory_order_relaxed);

  std::unique_lock<std::shared_mutex> lock(stateMtx_);

  const int N = static_cast<int>(ocr_boxes.size());
  const int M = static_cast<int>(trackers_.size());

  std::vector<bool> matchedOcr(N, false);
  std::vector<bool> matchedTr(M, false);

  // [Window anchor v5] 각 트래커의 "유효 매칭 위치" 계산:
  // - owner 창 살아있고 DWM bounds 조회 성공: refX + (현재 owner bounds delta)
  //   → 빠른 드래그여도 anchored 위치가 OCR 검출 위치와 매칭됨.
  // - 그 외: NCC tr.x/tr.y (기존 동작).
  // 이 위치를 IoU/거리 매칭에 사용해 신규 트래커 생성 폭주를 막는다 (잔상 원인).
  struct TrEffPos {
    float x, y;
    bool isAnchored;
    int32_t curWinL, curWinT;
    int32_t curWinR, curWinB;
  };
  std::vector<TrEffPos> effPos(M);
  for (int m = 0; m < M; ++m) {
    const auto &tr = trackers_[m];
    effPos[m] = {tr.x, tr.y, false, 0, 0, 0, 0};
#ifdef _WIN32
    if (tr.ownerWin) {
      HWND h = reinterpret_cast<HWND>(tr.ownerWin);
      if (IsWindow(h)) {
        RECT cur{};
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS,
                                            &cur, sizeof(cur)))) {
          HMONITOR mon = MonitorFromRect(&cur, MONITOR_DEFAULTTONEAREST);
          MONITORINFO mi{};
          mi.cbSize = sizeof(MONITORINFO);
          if (mon && GetMonitorInfo(mon, &mi)) {
            const int mw = mi.rcMonitor.right - mi.rcMonitor.left;
            const int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
            if (mw > 0 && mh > 0) {
              const float sx = static_cast<float>(gw) / static_cast<float>(mw);
              const float sy = static_cast<float>(gh) / static_cast<float>(mh);
              effPos[m].x = tr.refX + (cur.left - tr.refWindowL) * sx;
              effPos[m].y = tr.refY + (cur.top - tr.refWindowT) * sy;
              effPos[m].curWinL = cur.left;
              effPos[m].curWinT = cur.top;
              effPos[m].curWinR = cur.right;
              effPos[m].curWinB = cur.bottom;
              effPos[m].isAnchored = true;
            }
          }
        }
      }
    }
#endif
  }

  // IoU 매칭에 사용할 anchored 위치 기반 IoU 계산 람다.
  auto iou_with_pos = [](const VtOcrBox &ob, float trX, float trY, float trW,
                         float trH) -> float {
    const float ax1 = ob.x, ay1 = ob.y;
    const float ax2 = ob.x + ob.w, ay2 = ob.y + ob.h;
    const float bx1 = trX, by1 = trY;
    const float bx2 = trX + trW, by2 = trY + trH;
    const float ix1 = std::max(ax1, bx1);
    const float iy1 = std::max(ay1, by1);
    const float ix2 = std::min(ax2, bx2);
    const float iy2 = std::min(ay2, by2);
    if (ix2 <= ix1 || iy2 <= iy1)
      return 0.0f;
    const float inter = (ix2 - ix1) * (iy2 - iy1);
    const float un = ob.w * ob.h + trW * trH - inter;
    return un > 0.0f ? inter / un : 0.0f;
  };

  // [Fix #7-B2] IoU 0.30 → 0.20 + 거리 fallback (gray 버전)
  for (int n = 0; n < N; ++n) {
    float bestIou = 0.20f;
    float bestDist = std::numeric_limits<float>::max();
    int bestM = -1;
    for (int m = 0; m < M; ++m) {
      if (matchedTr[m])
        continue;
      const float trEffX = effPos[m].x;
      const float trEffY = effPos[m].y;
      const float iou = iou_with_pos(ocr_boxes[n], trEffX, trEffY,
                                     trackers_[m].bw, trackers_[m].bh);
      if (iou > bestIou) {
        bestIou = iou;
        bestM = m;
        bestDist = 0.0f;
      } else if (iou == 0.0f) {
        if (ocr_boxes[n].type && trackers_[m].type &&
            std::strcmp(ocr_boxes[n].type, trackers_[m].type) == 0) {
          const float cx1 = ocr_boxes[n].x + ocr_boxes[n].w * 0.5f;
          const float cy1 = ocr_boxes[n].y + ocr_boxes[n].h * 0.5f;
          const float cx2 = trEffX + trackers_[m].bw * 0.5f;
          const float cy2 = trEffY + trackers_[m].bh * 0.5f;
          const float d = std::hypot(cx1 - cx2, cy1 - cy2);
          const float thresh =
              0.5f * std::min(trackers_[m].bw, trackers_[m].bh);
          if (d < thresh && d < bestDist && bestIou == 0.20f) {
            bestDist = d;
            bestM = m;
            static int s_iou_miss_g = 0;
            if (++s_iou_miss_g >= 60) {
              s_iou_miss_g = 0;
              blog(LOG_DEBUG,
                   "[SecureCast][iou-miss] dist-fallback(gray) type=%s d=%.1f",
                   ocr_boxes[n].type, d);
            }
          }
        }
      }
    }
    if (bestM >= 0) {
      matchedOcr[n] = true;
      matchedTr[bestM] = true;

      auto &tr = trackers_[bestM];
      tr.bw = ocr_boxes[n].w;
      tr.bh = ocr_boxes[n].h;
      tr.framesSinceMatch = 0;
      tr.framesSinceOcrValidate = 0;
      tr.consecutiveLostFrames = 0;
      tr.lastScore = 1.0f;
      ++tr.ocrRevision; // [Fix #2-B] OCR worker 리셋 시그널

      int tcx = static_cast<int>(tr.x), tcy = static_cast<int>(tr.y);
      int tcw = static_cast<int>(tr.bw), tch = static_cast<int>(tr.bh);
      if (tcw > MAX_TMPL_W) {
        tcx += (tcw - MAX_TMPL_W) / 2;
        tcw = MAX_TMPL_W;
      }
      if (tch > MAX_TMPL_H) {
        tcy += (tch - MAX_TMPL_H) / 2;
        tch = MAX_TMPL_H;
      }

      int tw, th;
      auto crop =
          extract_gray_crop(gray, gw, gw, gh, tcx, tcy, tcw, tch, tw, th);
      if (!crop.empty()) {
        tr.tw = tw;
        tr.th = th;
        tr.tmpl = std::move(crop);
        precompute_tmpl_stats(tr);
        ++tr.templateTs;
      }

      // [Window anchor v5] refX/refY는 새 OCR 측정으로 갱신.
      // ownerWin은 보존 (초기 정적 OCR의 정확한 binding 유지 — 모션 중
      // WindowFromPoint의 잘못된 결과로 덮어쓰지 않기 위함).
      // refWindow는 effPos에서 이미 queried한 현재 owner bounds로 갱신해
      // (refX, refWindow) 쌍이 동일 시점 snapshot이 되도록 유지.
      tr.refX = ocr_boxes[n].x;
      tr.refY = ocr_boxes[n].y;
      if (effPos[bestM].isAnchored) {
        // 기존 owner 살아있음 — bounds 갱신만, ownerWin은 그대로.
        tr.refWindowL = effPos[bestM].curWinL;
        tr.refWindowT = effPos[bestM].curWinT;
        tr.refWindowR = effPos[bestM].curWinR;
        tr.refWindowB = effPos[bestM].curWinB;
      } else if (useOwners && tr.ownerWin == nullptr && owners[n].hwnd) {
        // 기존 owner 없었음 → OCR 측에서 잡은 owner로 신규 binding.
        tr.ownerWin = owners[n].hwnd;
        tr.refWindowL = owners[n].windowL;
        tr.refWindowT = owners[n].windowT;
        tr.refWindowR = owners[n].windowR;
        tr.refWindowB = owners[n].windowB;
      }
      // 그 외(owner 있었는데 죽음, 또는 새 owner도 없음): refX/refY만 갱신,
      // 다음 사이클에 owner 재바인딩 시도.
    }
  }

  for (int n = 0; n < N; ++n) {
    if (matchedOcr[n])
      continue;

    const auto &box = ocr_boxes[n];
    int tcx = static_cast<int>(box.x), tcy = static_cast<int>(box.y);
    int tcw = static_cast<int>(box.w), tch = static_cast<int>(box.h);
    if (tcw > MAX_TMPL_W) {
      tcx += (tcw - MAX_TMPL_W) / 2;
      tcw = MAX_TMPL_W;
    }
    if (tch > MAX_TMPL_H) {
      tcy += (tch - MAX_TMPL_H) / 2;
      tch = MAX_TMPL_H;
    }

    int tw, th;
    auto crop = extract_gray_crop(gray, gw, gw, gh, tcx, tcy, tcw, tch, tw, th);
    if (crop.empty())
      continue;

    // [#6 스크롤] 슬롯이 꽉 찼으면, 이번 OCR에 매칭 안 된(=현재 화면에 없는 옛
    // 위치) 트래커 중 가장 stale(framesSinceOcrValidate 최대)한 것을 evict해 이
    // 새 검출에 자리를 준다. 스크롤 후 옛 위치 트래커가 MAX_TRACKERS 슬롯을
    // 점유해 새 위치 PII(EMAIL 포함)가 검출돼도 트래커를 못 만들고 노출되던
    // 문제 해결 — 옛 트래커 5초 만료를 기다리지 않는다. 이번 사이클에 매칭됐거나
    // 방금 새로 만든 트래커(matchedTr=true)는 evict 대상에서 제외.
    if (static_cast<int>(trackers_.size()) >= MAX_TRACKERS) {
      int evictIdx = -1, evictStale = -1;
      for (int i = 0; i < static_cast<int>(trackers_.size()); ++i) {
        if (i < static_cast<int>(matchedTr.size()) && matchedTr[i])
          continue;
        if (trackers_[i].framesSinceOcrValidate > evictStale) {
          evictStale = trackers_[i].framesSinceOcrValidate;
          evictIdx = i;
        }
      }
      if (evictIdx < 0)
        break; // 모두 이번 사이클 유효 → 진짜 초과(보류)
      trackers_.erase(trackers_.begin() + evictIdx);
      if (evictIdx < static_cast<int>(matchedTr.size()))
        matchedTr.erase(matchedTr.begin() + evictIdx);
    }

    Tracker tr{};
    tr.id = nextId_++;
    tr.type = box.type;
    tr.x = box.x;
    tr.y = box.y;
    tr.bw = box.w;
    tr.bh = box.h;
    tr.tw = tw;
    tr.th = th;
    tr.tmpl = std::move(crop);
    tr.lastScore = 1.0f;
    tr.framesSinceMatch = 0;
    tr.framesSinceOcrValidate = 0;
    tr.vx = 0.0f;
    tr.vy = 0.0f;
    // [Window anchor] 신규 트래커: ref 좌표 = 현재 OCR 박스 위치, refWindow =
    // 현재 owner 창 DWM bounds top-left. owner 없으면 anchor 비활성(NCC만).
    tr.refX = box.x;
    tr.refY = box.y;
    if (useOwners) {
      tr.ownerWin = owners[n].hwnd;
      tr.refWindowL = owners[n].windowL;
      tr.refWindowT = owners[n].windowT;
      tr.refWindowR = owners[n].windowR;
      tr.refWindowB = owners[n].windowB;
    }
    precompute_tmpl_stats(tr);
    trackers_.push_back(std::move(tr));
    matchedTr.push_back(true); // 방금 만든 트래커 보호 (이후 evict 후보 제외)
  }
}

std::vector<VtOcrBox> VisualTrackerManager::active_boxes() const {
  // 1-A: shared_lock — 렌더 스레드에서 NCC(unique_lock)와 거의 동시 호출해도
  // 차단 없음
  std::shared_lock<std::shared_mutex> lock(stateMtx_);
  std::vector<VtOcrBox> result;
  result.reserve(trackers_.size());

  // [Fix #7-C] 보조 ghost-kill 게이트:
  //   NCC 연속 실패(FRAMES_LOST) AND OCR 장기 미갱신(STALE_OCR_FRAMES) 조건을
  //   동시에 만족하는 트래커는 렌더에 노출하지 않는다.
  //   HARD_EXPIRY / STALE_OCR_FRAMES 상수는 보류(#4) 정책상 변경하지 않음.
  int ghostHidden = 0;
  for (const auto &tr : trackers_) {
    const bool nccDeadlyLost = tr.framesSinceMatch >= FRAMES_LOST &&
                               tr.framesSinceOcrValidate >= STALE_OCR_FRAMES;
    if (nccDeadlyLost) {
      ++ghostHidden;
      continue;
    }
    result.push_back({tr.type, tr.x, tr.y, tr.bw, tr.bh});
  }

  if (ghostHidden > 0) {
    static int s_ghost_throttle = 0;
    if (++s_ghost_throttle >= 60) {
      s_ghost_throttle = 0;
      blog(LOG_DEBUG, "[SecureCast][ghost] gate hidden %d tracker(s)",
           ghostHidden);
    }
  }
  expand_boxes_if_motion(result, 0, 0);
  return result;
}

bool VisualTrackerManager::hasActiveBoxes() const {
  std::shared_lock<std::shared_mutex> lock(stateMtx_);
  return !trackers_.empty();
}

void VisualTrackerManager::clear() {
  std::unique_lock<std::shared_mutex> lock(stateMtx_);
  trackers_.clear();
  nextId_ = 0;
}

std::vector<void *> VisualTrackerManager::active_owner_windows() const {
  std::shared_lock<std::shared_mutex> lock(stateMtx_);
  std::vector<void *> result;
  result.reserve(trackers_.size());
  for (const auto &tr : trackers_) {
    if (!tr.ownerWin)
      continue;
    // ghost 게이트와 동일 조건으로 죽은 트래커 제외.
    const bool nccDeadlyLost = tr.framesSinceMatch >= FRAMES_LOST &&
                               tr.framesSinceOcrValidate >= STALE_OCR_FRAMES;
    if (nccDeadlyLost)
      continue;
    // unique 보장 (트래커가 많지 않아 O(n^2)로 충분).
    bool dup = false;
    for (void *w : result) {
      if (w == tr.ownerWin) {
        dup = true;
        break;
      }
    }
    if (!dup)
      result.push_back(tr.ownerWin);
  }
  return result;
}

// [Window anchor v4] pushFrame 직전 호출. 슬롯에 저장될 박스 좌표를 미리 계산.
// owner 바인딩된 트래커: 현재 DWM bounds 조회 → refX + (curWindow - refWindow) * scale.
//   ★ 이 경로로 좌표가 잡히면 NCC ghost-kill 게이트를 우회한다. 빠른 드래그 시
//     NCC가 따라가지 못해 framesSinceMatch가 증가하더라도 owner 창의 위치가 곧
//     진실이므로 블러를 절대 풀지 않는다 (사용자 보고: 이동 시 블러 해제 문제).
//   ★ 추가로 sc_compute_visible_subrects로 z-order 차감 — owner 위를 덮은 창
//     영역에는 블러를 표시하지 않는다. 메모장이 Chrome 위를 덮을 때 메모장 위에
//     블러가 떠 PII가 그 창에 있다고 오해하던 문제 차단. 0개 가시 영역이면 트래커
//     자체를 송출에서 제외 (완전히 가려진 상태).
// owner 없거나 anchor 실패: NCC tr.x/tr.y + ghost-kill 게이트 (스크롤·텍스트 소멸 케이스).
std::vector<VtOcrBox>
VisualTrackerManager::snapshot_for_push(uint32_t src_w, uint32_t src_h) const {
  std::shared_lock<std::shared_mutex> lock(stateMtx_);
  std::vector<VtOcrBox> result;
  result.reserve(trackers_.size());

  int ghostHidden = 0;
  int occludedHidden = 0;
  for (const auto &tr : trackers_) {
    float outX = tr.x;
    float outY = tr.y;
    bool ownerAnchored = false;

#ifdef _WIN32
    if (tr.ownerWin && src_w > 0 && src_h > 0) {
      HWND target = reinterpret_cast<HWND>(tr.ownerWin);
      // [Minimize skip] owner가 minimized면 화면에 안 보이므로 블러 emit 불필요.
      // sc_minimize_tracker가 별도로 lingering 블러를 관리한다.
      if (IsWindow(target) && !IsIconic(target)) {
        RECT cur{};
        if (SUCCEEDED(DwmGetWindowAttribute(target,
                                            DWMWA_EXTENDED_FRAME_BOUNDS, &cur,
                                            sizeof(cur)))) {
          HMONITOR hmon = MonitorFromRect(&cur, MONITOR_DEFAULTTONEAREST);
          MONITORINFO mi{};
          mi.cbSize = sizeof(MONITORINFO);
          if (hmon && GetMonitorInfo(hmon, &mi)) {
            const int mon_w = mi.rcMonitor.right - mi.rcMonitor.left;
            const int mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
            if (mon_w > 0 && mon_h > 0) {
              const float sx =
                  static_cast<float>(src_w) / static_cast<float>(mon_w);
              const float sy =
                  static_cast<float>(src_h) / static_cast<float>(mon_h);
              outX = tr.refX + (cur.left - tr.refWindowL) * sx;
              outY = tr.refY + (cur.top - tr.refWindowT) * sy;
              // 소스 경계 clamp.
              if (outX < 0)
                outX = 0;
              if (outY < 0)
                outY = 0;
              if (outX + tr.bw > static_cast<float>(src_w))
                outX = static_cast<float>(src_w) - tr.bw;
              if (outY + tr.bh > static_cast<float>(src_h))
                outY = static_cast<float>(src_h) - tr.bh;
              ownerAnchored = true;

              // [Anim guard sticky] maximize/restore 시 cur가 mid-state로
              // 점진적으로 변하는 동안 픽셀은 최종 위치로 점프하면 박스 위치
              // 미스매치 → 노출. 변화 감지 시 sticky 활성, monitor 기준으로
              // 미리 큰 expansion 적용.
              const int refWinW = tr.refWindowR - tr.refWindowL;
              const int refWinH = tr.refWindowB - tr.refWindowT;
              const int curWinW = cur.right - cur.left;
              const int curWinH = cur.bottom - cur.top;
              const int deltaW =
                  (refWinW > 0) ? std::abs(curWinW - refWinW) : 0;
              const int deltaH =
                  (refWinH > 0) ? std::abs(curWinH - refWinH) : 0;
              const int deltaL = std::abs(cur.left - tr.refWindowL);
              const int deltaT = std::abs(cur.top - tr.refWindowT);
              const int64_t nowMs =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
              if (deltaW > 1 || deltaH > 1 || deltaL > 1 || deltaT > 1) {
                lastResizeDetectedMs_.store(nowMs, std::memory_order_release);
              }

              // [Option 1+2 신호 통합]
              // (1) showCmd 폴링 → maximize 버튼/단축키 트랜지션 감지
              // (2) MOVESIZESTART/END WinEvent → 사용자 드래그 리사이즈 감지
              // 둘 중 하나라도 active면 sticky 즉시 강제 활성. delta 기반보다
              // 정확한 신호 (cur가 아직 안 변해도 트랜지션 시작 감지).
              // [Freeze T05] sticky 지속 시간 = 사용자 설정값(Properties 슬라이더,
              // default 1500ms). 리사이즈 lookback과 sticky 유지 기간 모두에 동일
              // 적용 — 둘 다 "이동/리사이즈 후 보호를 얼마나 끌고 갈지"를 의미.
              const int64_t stickyMs =
                  stickyDurationMs_.load(std::memory_order_relaxed);
              sc_notify_showcmd_change(target);
              if (sc_is_window_resizing(target, static_cast<int>(stickyMs))) {
                lastResizeDetectedMs_.store(nowMs, std::memory_order_release);
              }

              const int64_t lastResize =
                  lastResizeDetectedMs_.load(std::memory_order_acquire);
              const bool stickyActive =
                  (lastResize > 0) && (nowMs - lastResize < stickyMs);

              // [Move vs Resize 구분]
              // 드래그 이동(크기 변화 없음)은 top-left translation 공식으로 정확
              // 처리됨. predictive expansion(monitor 기준)을 적용하면 화면 전체
              // 블러 부작용. 크기 변화(deltaW/H > 30)가 있을 때만 expansion 활성.
              const bool sizeChanged = (deltaW > 30 || deltaH > 30);
              if (stickyActive && sizeChanged) {
                // monitor 전체 영역 (taskbar 포함) 기준 predictive expansion.
                // cur도 union해서 양 방향 커버.
                const RECT &pred = mi.rcMonitor;
                const bool hasRB = (tr.refWindowR > tr.refWindowL) &&
                                   (tr.refWindowB > tr.refWindowT);
                const int32_t effRefR =
                    hasRB ? tr.refWindowR : tr.refWindowL;
                const int32_t effRefB =
                    hasRB ? tr.refWindowB : tr.refWindowT;

                const float dL_pred = (pred.left - tr.refWindowL) * sx;
                const float dT_pred = (pred.top - tr.refWindowT) * sy;
                const float dR_pred = (pred.right - effRefR) * sx;
                const float dB_pred = (pred.bottom - effRefB) * sy;
                const float dL_cur = (cur.left - tr.refWindowL) * sx;
                const float dT_cur = (cur.top - tr.refWindowT) * sy;
                const float dR_cur = (cur.right - effRefR) * sx;
                const float dB_cur = (cur.bottom - effRefB) * sy;

                const float dL = std::min(dL_pred, dL_cur);
                const float dR = std::max(dR_pred, dR_cur);
                const float dT = std::min(dT_pred, dT_cur);
                const float dB = std::max(dB_pred, dB_cur);

                const float moveLoX = std::min(dL, dR);
                const float moveHiX = std::max(dL, dR);
                const float moveLoY = std::min(dT, dB);
                const float moveHiY = std::max(dT, dB);

                // 비대칭 pad: 아래쪽 특히 크게 (사용자 요청 — 아래 노출 차단).
                const float padPxH = 20.0f * sx;
                const float padPxUp = 50.0f * sy;
                const float padPxDown = 200.0f * sy;
                float ux = tr.refX + moveLoX - padPxH;
                float uy = tr.refY + moveLoY - padPxUp;
                float urx = tr.refX + tr.bw + moveHiX + padPxH;
                float ury = tr.refY + tr.bh + moveHiY + padPxDown;
                if (ux < 0.0f)
                  ux = 0.0f;
                if (uy < 0.0f)
                  uy = 0.0f;
                if (urx > static_cast<float>(src_w))
                  urx = static_cast<float>(src_w);
                if (ury > static_cast<float>(src_h))
                  ury = static_cast<float>(src_h);
                float uw = urx - ux;
                float uh = ury - uy;
                if (uw > 0.0f && uh > 0.0f)
                  result.push_back({tr.type, ux, uy, uw, uh});
                continue; // sticky expansion 처리 완료
              }

              // [z-order 차감] 비-sticky 경로. owner 위 다른 창들 빼고 노출된
              // disjoint 사각형들만 마스킹.
              RECT boxScreen;
              boxScreen.left = mi.rcMonitor.left +
                               static_cast<LONG>(outX / sx);
              boxScreen.top = mi.rcMonitor.top +
                              static_cast<LONG>(outY / sy);
              boxScreen.right = mi.rcMonitor.left +
                                static_cast<LONG>((outX + tr.bw) / sx);
              boxScreen.bottom = mi.rcMonitor.top +
                                 static_cast<LONG>((outY + tr.bh) / sy);

              // [드래그 노출 보강 — 방향성 확장] 출력은 ~1초 지연되는데 박스는 owner
              // DWM bounds를 따라가므로, 이동 중에는 박스가 콘텐츠보다 살짝 앞서
              // 콘텐츠가 "온 쪽"(trailing = 이동 반대 방향) 가장자리가 노출된다. 이동
              // 방향을 cur vs refWindow delta의 부호로 판정해 trailing 쪽을 이동량
              // 비례(상한 kTrailMax)로 더 확장한다. 기본은 사방 살짝(kBasePad), 대각선
              // 이동도 X/Y 각각 처리. 이동 활동 중(stickyActive)에만 적용, 정지 시
              // stickyMs 후 자동 해제 → 정적 화면 영향 없음. (리사이즈 큰 확장 경로는
              // 위에서 이미 continue.)
              if (stickyActive) {
                const int moveX = cur.left - tr.refWindowL; // +오른쪽 / -왼쪽 이동
                const int moveY = cur.top - tr.refWindowT;  // +아래 / -위 이동
                constexpr LONG kBasePad = 12;  // 사방 기본(아주 살짝)
                constexpr LONG kTrailMax = 50; // trailing 추가 상한
                const LONG trailX =
                    std::min<LONG>(std::abs(moveX), kTrailMax);
                const LONG trailY =
                    std::min<LONG>(std::abs(moveY), kTrailMax);
                boxScreen.left -= kBasePad;
                boxScreen.top -= kBasePad;
                boxScreen.right += kBasePad;
                boxScreen.bottom += kBasePad;
                if (moveX < 0)
                  boxScreen.right += trailX; // 왼쪽 이동 → 오른쪽 더
                else if (moveX > 0)
                  boxScreen.left -= trailX; // 오른쪽 이동 → 왼쪽 더
                if (moveY < 0)
                  boxScreen.bottom += trailY; // 위로 이동 → 아래 더
                else if (moveY > 0)
                  boxScreen.top -= trailY; // 아래로 이동 → 위 더
              }

              // [스크롤 밴드 #6 C2] 창은 안 움직이지만 콘텐츠가 스크롤되는 케이스.
              // 스크롤 중에는 PII가 창 안 어디로 이동/새로 진입할지 불확실하므로
              // boxScreen을 owner 창 전체(cur)로 확장한다("창 전체 블러"). 세로 띠만
              // 덮으면 다른 x열로 새로 스크롤돼 들어오는 PII가 트래커 생기기 전 잠깐
              // 노출됐다 — 창 전체로 그 구멍을 닫는다. cur로 clamp되어 창 밖으로 안
              // 번지고, 아래 z-order 차감이 위에 덮인 창은 자동 제외(위 창에 블러 X).
              // NCC/anchor 위치 자체는 안 건드림 → 정적 화면 100% 동일.
              //
              // [T-B] 스크롤 중 + 멈춘 뒤 settle 구간(kScrollSettleMs)까지 유지한다.
              // 노출은 "멈춘 직후" 정밀 마스크(refX)가 새 위치로 수렴(OCR 재검출)하기
              // 전 틈에서 발생 → 밴드를 그 수렴 시간까지 끌고 가 메운다. 시간 기반
              // 단조라 깜빡임 없음. (kScrollBandMaxPx는 현재 미사용 — 창 전체 확장.)
              {
                const int64_t lastScroll =
                    securecast::last_scroll_motion_time_ms();
                // [잔상 수정] 스크롤 신호(last_scroll_motion_time_ms)는 프로세스
                // 전역이라 어느 창에서 휠을 굴려도 갱신된다. 이 때문에 다른 창으로
                // 이동한 직후에도 이전 창(target)의 박스가 "창 전체"로 확장돼 블러
                // 잔상이 남았다. owner 창이 실제 포그라운드(=지금 스크롤하는 창)일
                // 때만 밴드를 적용해 전역 신호를 창별로 좁힌다. 배경 창은 사용자가
                // 스크롤하지 않으므로 노출 위험이 없어 밴드 불필요.
                const bool ownerIsForeground =
                    (target == GetForegroundWindow());
                const bool scrollBandActive =
                    ownerIsForeground && (lastScroll > 0) &&
                    ((nowMs - lastScroll) < kScrollSettleMs);
                if (scrollBandActive) {
                  boxScreen.left = cur.left;
                  boxScreen.top = cur.top;
                  boxScreen.right = cur.right;
                  boxScreen.bottom = cur.bottom;
                }
              }

              // [Freeze T04 — 비활성화] 가시영역 캐시는 stale 데이터로 PII를
              // 놓치는 정확성 회귀를 일으켜 제거했다(보안 도구에선 정확성 > 성능).
              // 매 프레임 z-order를 실시간 차감하는 직접 호출로 복귀.
              // 캐시 코드(visible_subrects_cache.*)와 WinEventListener 세대 인프라는
              // 향후 안전한 재구현을 위해 파일에 남겨둔다(현재 미사용).
              RECT visRects[SC_MAX_VISIBLE_SUBRECTS];
              const int visCount = sc_compute_visible_subrects(
                  target, boxScreen, visRects, SC_MAX_VISIBLE_SUBRECTS);

              if (visCount == 0) {
                ++occludedHidden;
                continue; // 완전 occlusion — 송출 제외
              }

              for (int v = 0; v < visCount; ++v) {
                const RECT &vr = visRects[v];
                const float vx = (vr.left - mi.rcMonitor.left) * sx;
                const float vy = (vr.top - mi.rcMonitor.top) * sy;
                const float vw = (vr.right - vr.left) * sx;
                const float vh = (vr.bottom - vr.top) * sy;
                result.push_back({tr.type, vx, vy, vw, vh});
              }
              continue; // anchor + z-order 처리 완료, 아래 일반 push 우회
            }
          }
        }
      }
    }
#else
    (void)src_w;
    (void)src_h;
#endif

    // owner anchor 실패 시에만 ghost-kill 적용. anchor가 잡힌 트래커는 NCC 상태
    // 무관하게 항상 출력 — 창 위치가 곧 텍스트 위치를 결정하므로.
    if (!ownerAnchored) {
      const bool nccDeadlyLost = tr.framesSinceMatch >= FRAMES_LOST &&
                                 tr.framesSinceOcrValidate >= STALE_OCR_FRAMES;
      if (nccDeadlyLost) {
        ++ghostHidden;
        continue;
      }
    }

    result.push_back({tr.type, outX, outY, tr.bw, tr.bh});
  }

  if (occludedHidden > 0) {
    static int s_occluded_throttle = 0;
    if (++s_occluded_throttle >= 60) {
      s_occluded_throttle = 0;
      blog(LOG_DEBUG,
           "[SecureCast][zorder-push] %d tracker(s) fully occluded — hidden",
           occludedHidden);
    }
  }

  if (ghostHidden > 0) {
    static int s_ghost_push_throttle = 0;
    if (++s_ghost_push_throttle >= 60) {
      s_ghost_push_throttle = 0;
      blog(LOG_DEBUG, "[SecureCast][ghost-push] gate hidden %d tracker(s)",
           ghostHidden);
    }
  }
  expand_boxes_if_motion(result, src_w, src_h);
  return result;
}

