#include "visual-tracker.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// SSSE3 intrinsics (x64 MSVC: always available via <intrin.h>)
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#define SC_HAS_SSSE3 1
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
//   모든 합은 uint16 범위(≤65280=256×255)에 들어오므로 16비트 모듈러 산술로 정확.
// ──────────────────────────────────────────────────────────────
void VisualTrackerManager::bgra_to_gray(
    const uint8_t* bgra, int width, int height, int stride,
    std::vector<uint8_t>& gray)
{
    gray.resize(static_cast<size_t>(width * height));
    uint8_t* dst_base = gray.data();

#ifdef SC_HAS_SSSE3
    // 계수 벡터: [B_coeff, G_coeff, R_coeff, A_coeff, ×2] — 픽셀당 4개 채널
    // _mm_set_epi16(w7,w6,w5,w4,w3,w2,w1,w0) — w0이 word 0 (lowest)
    const __m128i coeff = _mm_set_epi16(0, 77, 150, 29, 0, 77, 150, 29);

    // packus 후 gray 값이 [g0,g1,g2,g3,0,0,0,0, g4,g5,g6,g7,0,0,0,0] 위치에 있으므로
    // pshufb로 [g0..g7] 연속 배열로 정렬
    const __m128i shuf_pack = _mm_set_epi8(
        (char)-1,(char)-1,(char)-1,(char)-1,
        (char)-1,(char)-1,(char)-1,(char)-1,
        11, 10, 9, 8, 3, 2, 1, 0);

    const __m128i zero = _mm_setzero_si128();

    for (int r = 0; r < height; ++r) {
        const uint8_t* src = bgra     + static_cast<ptrdiff_t>(r) * stride;
        uint8_t*       dst = dst_base + static_cast<ptrdiff_t>(r) * width;
        int c = 0;

        for (; c + 8 <= width; c += 8, src += 32, dst += 8) {
            // 첫 번째 16바이트 (픽셀 0-3)
            __m128i p0  = _mm_loadu_si128((const __m128i*)src);
            // 픽셀 0-1: 8비트 → 16비트 확장 (하위 8바이트)
            __m128i lo0 = _mm_unpacklo_epi8(p0, zero); // [B0,G0,R0,A0,B1,G1,R1,A1] as uint16
            // 픽셀 2-3: 8비트 → 16비트 확장 (상위 8바이트)
            __m128i hi0 = _mm_unpackhi_epi8(p0, zero); // [B2,G2,R2,A2,B3,G3,R3,A3] as uint16
            // 채널별 곱셈 (low 16비트 반환, 최대 150×255=38250 < 65535 → 오버플로 없음)
            __m128i pl0 = _mm_mullo_epi16(lo0, coeff); // [B0*29,G0*150,R0*77,0,B1*29,G1*150,R1*77,0]
            __m128i ph0 = _mm_mullo_epi16(hi0, coeff); // [B2*29,...]
            // 두 번의 수평 합산으로 픽셀당 1개 합 계산 (16비트 래핑 — 최종 uint16 합은 ≤65280)
            // hadd1: [B0*29+G0*150, R0*77, B1*29+G1*150, R1*77, B2*29+G2*150, R2*77, B3*29+G3*150, R3*77]
            // hadd2 with zero: [gray0_×256, gray1_×256, gray2_×256, gray3_×256, 0,0,0,0]
            __m128i s0 = _mm_srli_epi16(
                _mm_hadd_epi16(_mm_hadd_epi16(pl0, ph0), zero), 8);
            // s0: [gray0, gray1, gray2, gray3, 0,0,0,0] as uint16

            // 두 번째 16바이트 (픽셀 4-7)
            __m128i p1  = _mm_loadu_si128((const __m128i*)(src + 16));
            __m128i lo1 = _mm_unpacklo_epi8(p1, zero);
            __m128i hi1 = _mm_unpackhi_epi8(p1, zero);
            __m128i pl1 = _mm_mullo_epi16(lo1, coeff);
            __m128i ph1 = _mm_mullo_epi16(hi1, coeff);
            __m128i s1  = _mm_srli_epi16(
                _mm_hadd_epi16(_mm_hadd_epi16(pl1, ph1), zero), 8);

            // uint16 → uint8 패킹, 이후 pshufb로 [g0..g7] 연속 정렬
            // packus(s0,s1): [g0,g1,g2,g3,0,0,0,0, g4,g5,g6,g7,0,0,0,0] as bytes
            __m128i packed = _mm_shuffle_epi8(_mm_packus_epi16(s0, s1), shuf_pack);
            _mm_storel_epi64((__m128i*)dst, packed); // 하위 8바이트 저장
        }
        // 스칼라 나머지
        for (; c < width; ++c, src += 4)
            *dst++ = (uint8_t)((29 * src[0] + 150 * src[1] + 77 * src[2]) >> 8);
    }
#else
    for (int r = 0; r < height; ++r) {
        const uint8_t* row  = bgra     + static_cast<ptrdiff_t>(r) * stride;
        uint8_t*       grow = dst_base + static_cast<ptrdiff_t>(r) * width;
        for (int c = 0; c < width; ++c)
            grow[c] = (uint8_t)((29 * row[c*4+0] + 150 * row[c*4+1] + 77 * row[c*4+2]) >> 8);
    }
#endif
}

// ──────────────────────────────────────────────────────────────
// P0-1: 2× 박스-필터 다운샘플
// ──────────────────────────────────────────────────────────────
std::vector<uint8_t> VisualTrackerManager::downsample_2x(
    const uint8_t* gray, int gw, int gh,
    int& out_w, int& out_h)
{
    out_w = gw / 2;
    out_h = gh / 2;
    if (out_w <= 0 || out_h <= 0) { out_w = out_h = 0; return {}; }

    std::vector<uint8_t> result(static_cast<size_t>(out_w * out_h));
    for (int r = 0; r < out_h; ++r) {
        const uint8_t* row0 = gray + static_cast<ptrdiff_t>(r * 2)     * gw;
        const uint8_t* row1 = gray + static_cast<ptrdiff_t>(r * 2 + 1) * gw;
        uint8_t*       dst  = result.data() + static_cast<ptrdiff_t>(r) * out_w;
        for (int c = 0; c < out_w; ++c)
            dst[c] = (uint8_t)((row0[c*2] + row0[c*2+1] + row1[c*2] + row1[c*2+1] + 2) >> 2);
    }
    return result;
}

// ──────────────────────────────────────────────
// Static helpers
// ──────────────────────────────────────────────

std::vector<uint8_t> VisualTrackerManager::extract_gray_crop(
    const uint8_t* gray, int gstride, int gw, int gh,
    int cx, int cy, int cw, int ch,
    int& out_tw, int& out_th)
{
    const int x0 = std::max(cx, 0);
    const int y0 = std::max(cy, 0);
    const int x1 = std::min(cx + cw, gw);
    const int y1 = std::min(cy + ch, gh);
    out_tw = x1 - x0;
    out_th = y1 - y0;

    if (out_tw <= 0 || out_th <= 0) return {};

    std::vector<uint8_t> crop(static_cast<size_t>(out_tw * out_th));
    for (int r = 0; r < out_th; ++r)
        std::memcpy(crop.data() + static_cast<ptrdiff_t>(r) * out_tw,
                    gray + static_cast<ptrdiff_t>(y0 + r) * gstride + x0,
                    static_cast<size_t>(out_tw));
    return crop;
}

float VisualTrackerManager::box_iou(const VtOcrBox& a, const Tracker& b)
{
    const float ax0 = a.x,  ay0 = a.y,  ax1 = a.x + a.w,  ay1 = a.y + a.h;
    const float bx0 = b.x,  by0 = b.y,  bx1 = b.x + b.bw, by1 = b.y + b.bh;
    const float ix0 = std::max(ax0, bx0), iy0 = std::max(ay0, by0);
    const float ix1 = std::min(ax1, bx1), iy1 = std::min(ay1, by1);
    if (ix0 >= ix1 || iy0 >= iy1) return 0.0f;
    const float inter = (ix1 - ix0) * (iy1 - iy0);
    const float u = a.w * a.h + b.bw * b.bh - inter;
    return (u > 0.0f) ? inter / u : 0.0f;
}

// ──────────────────────────────────────────────
// NCC at a single search position (float arithmetic)
// ──────────────────────────────────────────────

float VisualTrackerManager::ncc_at(
    const uint8_t* gray, int gstride, int gw, int gh,
    const std::vector<uint8_t>& tmpl, int tw, int th,
    int sx, int sy) const
{
    if (sx < 0 || sy < 0 || sx + tw > gw || sy + th > gh) return -1.0f;
    if (tmpl.empty() || tw <= 0 || th <= 0) return -1.0f;

    const int n = tw * th;

    float tmean = 0.0f;
    for (int i = 0; i < n; ++i) tmean += tmpl[i];
    tmean /= n;

    float imean = 0.0f;
    for (int r = 0; r < th; ++r)
        for (int c = 0; c < tw; ++c)
            imean += gray[static_cast<ptrdiff_t>(sy + r) * gstride + (sx + c)];
    imean /= n;

    float num = 0.0f, dT = 0.0f, dI = 0.0f;
    for (int r = 0; r < th; ++r) {
        for (int c = 0; c < tw; ++c) {
            const float t  = tmpl[r * tw + c] - tmean;
            const float iv = gray[static_cast<ptrdiff_t>(sy + r) * gstride + (sx + c)] - imean;
            num += t * iv;
            dT  += t  * t;
            dI  += iv * iv;
        }
    }
    const float denom = std::sqrt(dT * dI);
    if (denom < 1e-5f) return 0.0f;
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
//   - coarse best 위치 주변 ±4px (stride-1) 정밀 탐색 (coarse stride-2=4px gap 완전 커버)
//
// 속도 (P0-3):
//   - 성공 시: vx/vy EMA 업데이트 (0.7×old + 0.3×new)
//   - 실패 시: 속도 0.5× 감쇠
// ──────────────────────────────────────────────────────────────

void VisualTrackerManager::update_one_pyramid(
    Tracker& tr,
    const uint8_t* gray, int gw, int gh,
    const uint8_t* halfGray, int hw, int hh)
{
    if (tr.tmpl.empty() || tr.tw <= 0 || tr.th <= 0) return;

    // P0-3: 속도 기반 예측 위치
    const float predX = tr.x + tr.vx;
    const float predY = tr.y + tr.vy;

    // 적응형 탐색 반경: 기본 반경 + 속도 크기, SEARCH_FAR 상한
    const int baseRadius = (tr.lastScore >= SCORE_OK) ? SEARCH_NEAR : SEARCH_FAR;
    const int velBonus   = std::min((int)(std::abs(tr.vx) + std::abs(tr.vy)), SEARCH_FAR);
    const int radius     = std::min(baseRadius + velBonus, SEARCH_FAR);

    // ----- Level 0: coarse (1/2 해상도) -----
    // 템플릿 1/2 다운샘플 (매 프레임 on-the-fly; 160×80→80×40 ≈ 3200 ops, 무시 가능)
    const int htw = std::max(tr.tw / 2, 1);
    const int hth = std::max(tr.th / 2, 1);
    std::vector<uint8_t> htmpl(static_cast<size_t>(htw * hth));
    for (int r = 0; r < hth; ++r) {
        for (int c = 0; c < htw; ++c) {
            const int r2 = r * 2, c2 = c * 2;
            int sum = tr.tmpl[r2 * tr.tw + c2]; int cnt = 1;
            if (c2 + 1 < tr.tw) { sum += tr.tmpl[r2 * tr.tw + c2 + 1]; ++cnt; }
            if (r2 + 1 < tr.th) { sum += tr.tmpl[(r2+1) * tr.tw + c2]; ++cnt; }
            if (c2 + 1 < tr.tw && r2 + 1 < tr.th) { sum += tr.tmpl[(r2+1)*tr.tw+c2+1]; ++cnt; }
            htmpl[r * htw + c] = (uint8_t)(sum / cnt);
        }
    }

    // 예측 중심 (1/2 스케일), 탐색 반경 (1/2 스케일)
    const int hcx    = (int)((predX + tr.bw * 0.5f) * 0.5f);
    const int hcy    = (int)((predY + tr.bh * 0.5f) * 0.5f);
    const int hr     = (radius + 1) / 2;

    const int hsx0 = hcx - hr - htw / 2;
    const int hsy0 = hcy - hr - hth / 2;
    const int hsx1 = hcx + hr - htw / 2;
    const int hsy1 = hcy + hr - hth / 2;

    float bestCoarseScore = -1.0f;
    int   bestHX = hcx - htw / 2;
    int   bestHY = hcy - hth / 2;

    for (int sy = hsy0; sy <= hsy1; sy += 2) {
        for (int sx = hsx0; sx <= hsx1; sx += 2) {
            const float sc = ncc_at(halfGray, hw, hw, hh, htmpl, htw, hth, sx, sy);
            if (sc > bestCoarseScore) { bestCoarseScore = sc; bestHX = sx; bestHY = sy; }
        }
    }

    // ----- Level 1: fine (원해상도, coarse 주변 ±2px) -----
    const int refineX = bestHX * 2;
    const int refineY = bestHY * 2;

    float bestScore = -1.0f;
    int   bestX     = (int)(predX + (tr.bw - tr.tw) * 0.5f);
    int   bestY     = (int)(predY + (tr.bh - tr.th) * 0.5f);

    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            const float sc = ncc_at(gray, gw, gw, gh, tr.tmpl, tr.tw, tr.th,
                                    refineX + dx, refineY + dy);
            if (sc > bestScore) { bestScore = sc; bestX = refineX + dx; bestY = refineY + dy; }
        }
    }

    tr.lastScore = bestScore;
    if (bestScore >= SCORE_LOST) {
        // bestX/Y: template top-left → box top-left
        const float newX = (float)bestX - (tr.bw - (float)tr.tw) * 0.5f;
        const float newY = (float)bestY - (tr.bh - (float)tr.th) * 0.5f;

        // P0-3: EMA 속도 업데이트
        const float rawVx = newX - tr.x;
        const float rawVy = newY - tr.y;
        tr.vx = 0.7f * tr.vx + 0.3f * rawVx;
        tr.vy = 0.7f * tr.vy + 0.3f * rawVy;

        tr.x = newX;
        tr.y = newY;
        tr.framesSinceMatch = 0;

        if (bestScore >= SCORE_REFRESH) {
            int rdummy, cdummy;
            auto refreshed = extract_gray_crop(gray, gw, gw, gh,
                                               bestX, bestY, tr.tw, tr.th,
                                               rdummy, cdummy);
            if (rdummy == tr.tw && cdummy == tr.th && !refreshed.empty())
                tr.tmpl = std::move(refreshed);
        }
    } else {
        ++tr.framesSinceMatch;
        // 속도 감쇠 (실패 시)
        tr.vx *= 0.5f;
        tr.vy *= 0.5f;
    }
}

// ──────────────────────────────────────────────────────────────
// update_all_impl — lock 없는 공통 구현 (mtx_ 보유 상태에서 호출)
//
// P0-1: 1/2 다운샘플 halfGray를 한 번 계산해 모든 트래커가 공유
// P0-2: framesSinceOcrValidate 증가; HARD_EXPIRY 초과 트래커 제거
// ──────────────────────────────────────────────────────────────
void VisualTrackerManager::update_all_impl(const uint8_t* gray, int gw, int gh)
{
    // coarse 레벨용 1/2 다운샘플 (모든 트래커 공유)
    int hw, hh;
    std::vector<uint8_t> halfGray = downsample_2x(gray, gw, gh, hw, hh);

    for (auto& tr : trackers_) {
        if (hw > 0 && hh > 0)
            update_one_pyramid(tr, gray, gw, gh, halfGray.data(), hw, hh);
        ++tr.framesSinceOcrValidate;
    }

    // dead tracker 제거 (역순: erase 시 인덱스 shift 방지)
    for (int i = (int)trackers_.size() - 1; i >= 0; --i) {
        if (trackers_[i].framesSinceMatch  >= FRAMES_LOST ||
            trackers_[i].framesSinceOcrValidate >= HARD_EXPIRY)
            trackers_.erase(trackers_.begin() + i);
    }
}

// ──────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────

void VisualTrackerManager::update_all(
    const uint8_t* bgra, int width, int height, int stride)
{
    std::vector<uint8_t> gray;
    bgra_to_gray(bgra, width, height, stride, gray);

    std::lock_guard<std::mutex> lock(mtx_);
    update_all_impl(gray.data(), width, height);
}

void VisualTrackerManager::update_all_gray(const uint8_t* gray, int gw, int gh)
{
    std::lock_guard<std::mutex> lock(mtx_);
    update_all_impl(gray, gw, gh);
}

void VisualTrackerManager::register_or_update(
    const std::vector<VtOcrBox>& ocr_boxes,
    const uint8_t* bgra, int width, int height, int stride)
{
    if (ocr_boxes.empty()) return;

    std::vector<uint8_t> gray;
    bgra_to_gray(bgra, width, height, stride, gray);

    std::lock_guard<std::mutex> lock(mtx_);

    const int N = (int)ocr_boxes.size();
    const int M = (int)trackers_.size();

    std::vector<bool> matchedOcr(N, false);
    std::vector<bool> matchedTr(M, false);

    // greedy IoU 매칭 (IoU threshold = 0.30)
    for (int n = 0; n < N; ++n) {
        float bestIou = 0.30f;
        int   bestM   = -1;
        for (int m = 0; m < M; ++m) {
            if (matchedTr[m]) continue;
            const float iou = box_iou(ocr_boxes[n], trackers_[m]);
            if (iou > bestIou) { bestIou = iou; bestM = m; }
        }
        if (bestM >= 0) {
            matchedOcr[n]    = true;
            matchedTr[bestM] = true;

            auto& tr = trackers_[bestM];
            // x/y는 NCC가 관리 — OCR 좌표로 덮어쓰지 않음 (OCR은 250ms 전 프레임 기준)
            // 크기(bw/bh)는 OCR 결과로 갱신 (텍스트 길이 변경 대응)
            tr.bw = ocr_boxes[n].w;
            tr.bh = ocr_boxes[n].h;
            tr.framesSinceMatch       = 0;
            tr.framesSinceOcrValidate = 0;
            tr.lastScore = 1.0f;

            // 템플릿은 현재 NCC 위치(tr.x, tr.y)에서 재추출 (OCR 위치 아님)
            int tcx = (int)tr.x, tcy = (int)tr.y;
            int tcw = (int)tr.bw, tch = (int)tr.bh;
            if (tcw > MAX_TMPL_W) { tcx += (tcw - MAX_TMPL_W) / 2; tcw = MAX_TMPL_W; }
            if (tch > MAX_TMPL_H) { tcy += (tch - MAX_TMPL_H) / 2; tch = MAX_TMPL_H; }

            int tw, th;
            auto crop = extract_gray_crop(gray.data(), width, width, height,
                                          tcx, tcy, tcw, tch, tw, th);
            if (!crop.empty()) { tr.tw = tw; tr.th = th; tr.tmpl = std::move(crop); }
        }
    }

    // 매칭 안 된 OCR 박스 → 신규 트래커 (P0-2: MAX_TRACKERS 초과 시 거부)
    for (int n = 0; n < N; ++n) {
        if (matchedOcr[n]) continue;
        if ((int)trackers_.size() >= MAX_TRACKERS) break; // ghost 누적 방지

        const auto& box = ocr_boxes[n];
        int tcx = (int)box.x, tcy = (int)box.y;
        int tcw = (int)box.w, tch = (int)box.h;
        if (tcw > MAX_TMPL_W) { tcx += (tcw - MAX_TMPL_W) / 2; tcw = MAX_TMPL_W; }
        if (tch > MAX_TMPL_H) { tcy += (tch - MAX_TMPL_H) / 2; tch = MAX_TMPL_H; }

        int tw, th;
        auto crop = extract_gray_crop(gray.data(), width, width, height,
                                      tcx, tcy, tcw, tch, tw, th);
        if (crop.empty()) continue;

        Tracker tr;
        tr.id                     = nextId_++;
        tr.type                   = box.type;
        tr.x                      = box.x;
        tr.y                      = box.y;
        tr.bw                     = box.w;
        tr.bh                     = box.h;
        tr.tw                     = tw;
        tr.th                     = th;
        tr.tmpl                   = std::move(crop);
        tr.lastScore              = 1.0f;
        tr.framesSinceMatch       = 0;
        tr.framesSinceOcrValidate = 0;
        tr.vx = 0.0f; tr.vy = 0.0f;
        trackers_.push_back(std::move(tr));
    }
}

std::vector<VtOcrBox> VisualTrackerManager::active_boxes() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<VtOcrBox> result;
    result.reserve(trackers_.size());
    for (const auto& tr : trackers_)
        result.push_back({tr.type, tr.x, tr.y, tr.bw, tr.bh});
    return result;
}

void VisualTrackerManager::clear()
{
    std::lock_guard<std::mutex> lock(mtx_);
    trackers_.clear();
    nextId_ = 0;
}
