#include "visual-tracker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

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
void VisualTrackerManager::update_one_pyramid(Tracker &tr, const uint8_t *gray,
                                              int gw, int gh,
                                              const uint8_t *quarterGray,
                                              int qw, int qh) {
  if (tr.tmpl.empty() || tr.tw <= 0 || tr.th <= 0)
    return;

  // P0-3: 속도 기반 예측 위치
  const float predX = tr.x + tr.vx;
  const float predY = tr.y + tr.vy;

  // 적응형 탐색 반경: 기본 반경 + 속도 크기, SEARCH_FAR 상한
  const int baseRadius = (tr.lastScore >= SCORE_OK) ? SEARCH_NEAR : SEARCH_FAR;
  const int velBonus =
      std::min((int)(std::abs(tr.vx) + std::abs(tr.vy)), SEARCH_FAR);
  const int radius = std::min(baseRadius + velBonus, SEARCH_FAR);

  // ----- Level 0: coarse (1/4 해상도) -----
  // 템플릿 4×4 avg 다운샘플 (on-the-fly; 160×80→40×20 = 800 ops)
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

  // 예측 중심 (1/4 스케일), 탐색 반경 (1/4 스케일)
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
  // quarter stride-2 → 8px step at full scale; ±6px fine이 항상 커버
  const int refineX = bestQX * 4;
  const int refineY = bestQY * 4;

  float bestScore = -1.0f;
  int bestX = (int)(predX + (tr.bw - tr.tw) * 0.5f);
  int bestY = (int)(predY + (tr.bh - tr.th) * 0.5f);

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
      it->framesSinceMatch = lt.framesSinceMatch;
      it->framesSinceOcrValidate = lt.framesSinceOcrValidate;
      it->consecutiveLostFrames = lt.consecutiveLostFrames;

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
    for (int i = (int)trackers_.size() - 1; i >= 0; --i) {
      if (trackers_[i].framesSinceMatch >= FRAMES_LOST ||
          trackers_[i].framesSinceOcrValidate >= HARD_EXPIRY)
        trackers_.erase(trackers_.begin() + i);
    }
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

  // greedy IoU 매칭 (IoU threshold = 0.30)
  for (int n = 0; n < N; ++n) {
    float bestIou = 0.30f;
    int bestM = -1;
    for (int m = 0; m < M; ++m) {
      if (matchedTr[m])
        continue;
      const float iou = box_iou(ocr_boxes[n], trackers_[m]);
      if (iou > bestIou) {
        bestIou = iou;
        bestM = m;
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
void VisualTrackerManager::register_or_update_gray(
    const std::vector<VtOcrBox> &ocr_boxes, const uint8_t *gray, int gw,
    int gh) {
  if (ocr_boxes.empty() || !gray)
    return;

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

  for (int n = 0; n < N; ++n) {
    float bestIou = 0.30f;
    int bestM = -1;
    for (int m = 0; m < M; ++m) {
      if (matchedTr[m])
        continue;
      const float iou = box_iou(ocr_boxes[n], trackers_[m]);
      if (iou > bestIou) {
        bestIou = iou;
        bestM = m;
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
    }
  }

  for (int n = 0; n < N; ++n) {
    if (matchedOcr[n])
      continue;
    if (static_cast<int>(trackers_.size()) >= MAX_TRACKERS)
      break;

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
    precompute_tmpl_stats(tr);
    trackers_.push_back(std::move(tr));
  }
}

std::vector<VtOcrBox> VisualTrackerManager::active_boxes() const {
  // 1-A: shared_lock — 렌더 스레드에서 NCC(unique_lock)와 거의 동시 호출해도
  // 차단 없음
  std::shared_lock<std::shared_mutex> lock(stateMtx_);
  std::vector<VtOcrBox> result;
  result.reserve(trackers_.size());
  for (const auto &tr : trackers_)
    result.push_back({tr.type, tr.x, tr.y, tr.bw, tr.bh});
  return result;
}

void VisualTrackerManager::clear() {
  std::unique_lock<std::shared_mutex> lock(stateMtx_);
  trackers_.clear();
  nextId_ = 0;
}
