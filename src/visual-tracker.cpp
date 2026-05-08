#include "visual-tracker.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// ──────────────────────────────────────────────
// Static helpers
// ──────────────────────────────────────────────

void VisualTrackerManager::bgra_to_gray(
    const uint8_t* bgra, int width, int height, int stride,
    std::vector<uint8_t>& gray)
{
    gray.resize(static_cast<size_t>(width * height));
    for (int r = 0; r < height; ++r) {
        const uint8_t* row  = bgra + static_cast<ptrdiff_t>(r) * stride;
        uint8_t*       grow = gray.data() + static_cast<ptrdiff_t>(r) * width;
        for (int c = 0; c < width; ++c) {
            const uint8_t b  = row[c * 4 + 0];
            const uint8_t g  = row[c * 4 + 1];
            const uint8_t rv = row[c * 4 + 2];
            grow[c] = static_cast<uint8_t>((29 * b + 150 * g + 77 * rv) >> 8);
        }
    }
}

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
    const float ax0 = a.x,      ay0 = a.y,      ax1 = a.x + a.w,   ay1 = a.y + a.h;
    const float bx0 = b.x,      by0 = b.y,      bx1 = b.x + b.bw,  by1 = b.y + b.bh;
    const float ix0 = std::max(ax0, bx0), iy0 = std::max(ay0, by0);
    const float ix1 = std::min(ax1, bx1), iy1 = std::min(ay1, by1);
    if (ix0 >= ix1 || iy0 >= iy1) return 0.0f;
    const float inter = (ix1 - ix0) * (iy1 - iy0);
    const float u = a.w * a.h + b.bw * b.bh - inter;
    return (u > 0.0f) ? inter / u : 0.0f;
}

// ──────────────────────────────────────────────
// NCC at a single search position
// ──────────────────────────────────────────────

float VisualTrackerManager::ncc_at(
    const uint8_t* gray, int gstride, int gw, int gh,
    const std::vector<uint8_t>& tmpl, int tw, int th,
    int sx, int sy) const
{
    if (sx < 0 || sy < 0 || sx + tw > gw || sy + th > gh) return -1.0f;
    if (tmpl.empty() || tw <= 0 || th <= 0) return -1.0f;

    const int n = tw * th;

    // Template mean
    double tmean = 0.0;
    for (int i = 0; i < n; ++i) tmean += tmpl[i];
    tmean /= n;

    // Image patch mean
    double imean = 0.0;
    for (int r = 0; r < th; ++r)
        for (int c = 0; c < tw; ++c)
            imean += gray[static_cast<ptrdiff_t>(sy + r) * gstride + (sx + c)];
    imean /= n;

    // NCC numerator / denominators
    double num = 0.0, dT = 0.0, dI = 0.0;
    for (int r = 0; r < th; ++r) {
        for (int c = 0; c < tw; ++c) {
            const double t  = tmpl[r * tw + c] - tmean;
            const double iv = gray[static_cast<ptrdiff_t>(sy + r) * gstride + (sx + c)] - imean;
            num += t * iv;
            dT  += t  * t;
            dI  += iv * iv;
        }
    }
    const double denom = std::sqrt(dT * dI);
    if (denom < 1e-6) return 0.0f; // uniform patch — no signal
    return static_cast<float>(num / denom);
}

// ──────────────────────────────────────────────
// update_one: stride-2 grid search → 위치 갱신
// ──────────────────────────────────────────────

void VisualTrackerManager::update_one(
    Tracker& tr,
    const uint8_t* gray, int gstride, int gw, int gh)
{
    if (tr.tmpl.empty() || tr.tw <= 0 || tr.th <= 0) return;

    const int radius = (tr.lastScore >= SCORE_OK) ? SEARCH_NEAR : SEARCH_FAR;
    // 박스 중심 기준으로 검색 범위 계산
    const int cx = static_cast<int>(tr.x + tr.bw * 0.5f);
    const int cy = static_cast<int>(tr.y + tr.bh * 0.5f);

    // 검색할 template top-left 범위 (stride-2)
    const int sx0 = cx - radius - tr.tw / 2;
    const int sy0 = cy - radius - tr.th / 2;
    const int sx1 = cx + radius - tr.tw / 2;
    const int sy1 = cy + radius - tr.th / 2;

    float bestScore = -1.0f;
    int   bestX     = static_cast<int>(tr.x);
    int   bestY     = static_cast<int>(tr.y);

    for (int sy = sy0; sy <= sy1; sy += 2) {
        for (int sx = sx0; sx <= sx1; sx += 2) {
            const float score = ncc_at(gray, gstride, gw, gh,
                                       tr.tmpl, tr.tw, tr.th, sx, sy);
            if (score > bestScore) {
                bestScore = score;
                bestX     = sx;
                bestY     = sy;
            }
        }
    }

    tr.lastScore = bestScore;
    if (bestScore >= SCORE_LOST) {
        tr.x              = static_cast<float>(bestX);
        tr.y              = static_cast<float>(bestY);
        tr.framesSinceMatch = 0;
    } else {
        ++tr.framesSinceMatch;
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

    for (auto& tr : trackers_)
        update_one(tr, gray.data(), width, width, height);

    // dead tracker 제거 (역순: erase 시 인덱스 shift 방지)
    for (int i = static_cast<int>(trackers_.size()) - 1; i >= 0; --i) {
        if (trackers_[i].framesSinceMatch >= FRAMES_LOST)
            trackers_.erase(trackers_.begin() + i);
    }
}

void VisualTrackerManager::register_or_update(
    const std::vector<VtOcrBox>& ocr_boxes,
    const uint8_t* bgra, int width, int height, int stride)
{
    if (ocr_boxes.empty()) return;

    std::vector<uint8_t> gray;
    bgra_to_gray(bgra, width, height, stride, gray);

    std::lock_guard<std::mutex> lock(mtx_);

    const int N = static_cast<int>(ocr_boxes.size());
    const int M = static_cast<int>(trackers_.size());

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
            matchedOcr[n]  = true;
            matchedTr[bestM] = true;

            // 좌표 재동기화 + 템플릿 재추출
            auto& tr = trackers_[bestM];
            tr.x  = ocr_boxes[n].x;
            tr.y  = ocr_boxes[n].y;
            tr.bw = ocr_boxes[n].w;
            tr.bh = ocr_boxes[n].h;
            tr.framesSinceMatch = 0;
            tr.lastScore = 1.0f;

            int tw, th;
            auto crop = extract_gray_crop(gray.data(), width, width, height,
                                          static_cast<int>(tr.x), static_cast<int>(tr.y),
                                          static_cast<int>(tr.bw), static_cast<int>(tr.bh),
                                          tw, th);
            if (!crop.empty()) {
                tr.tw   = tw;
                tr.th   = th;
                tr.tmpl = std::move(crop);
            }
        }
    }

    // 매칭 안 된 OCR 박스 → 신규 트래커
    for (int n = 0; n < N; ++n) {
        if (matchedOcr[n]) continue;
        const auto& box = ocr_boxes[n];

        int tw, th;
        auto crop = extract_gray_crop(gray.data(), width, width, height,
                                      static_cast<int>(box.x), static_cast<int>(box.y),
                                      static_cast<int>(box.w), static_cast<int>(box.h),
                                      tw, th);
        if (crop.empty()) continue;

        Tracker tr;
        tr.id              = nextId_++;
        tr.type            = box.type;
        tr.x               = box.x;
        tr.y               = box.y;
        tr.bw              = box.w;
        tr.bh              = box.h;
        tr.tw              = tw;
        tr.th              = th;
        tr.tmpl            = std::move(crop);
        tr.lastScore       = 1.0f;
        tr.framesSinceMatch = 0;
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
