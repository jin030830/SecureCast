#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

// ============================================================
// Visual Tracker — NCC 기반 프레임별 박스 위치 추적
//
// OCR("what": ~250ms)과 Tracker("where": ~16ms)를 분리:
//   - update_all()       : 매 렌더 프레임마다 NCC로 위치 갱신
//   - register_or_update(): OCR 완료 시 박스 등록/재동기화
//   - active_boxes()     : 렌더 스레드가 현재 블러 좌표 조회
//
// 스레드 안전: 내부 뮤텍스로 보호. 렌더/OCR 양쪽에서 안전하게 호출 가능.
// ============================================================

// OCR 결과를 Tracker에 전달하는 경량 구조체.
// ocr-engine.h의 SecureCastOcrBox와 동일 레이아웃이지만
// 순환 include를 피하기 위해 별도 선언.
struct VtOcrBox {
    const char* type;   // "RRN", "PHONE", "EMAIL", ... (string literal)
    float x, y, w, h;  // 픽셀 좌표 (top-left + 크기)
};

class VisualTrackerManager {
public:
    static constexpr float SCORE_OK      = 0.70f; // 이 점수 이상이면 정상 추적
    static constexpr float SCORE_LOST    = 0.40f; // 이 점수 미만이면 실패 카운트
    static constexpr float SCORE_REFRESH = 0.85f; // 이 점수 이상이면 템플릿 갱신 (드리프트 방지)
    static constexpr int   FRAMES_LOST   = 3;     // 연속 실패 이 횟수 초과 시 제거
    static constexpr int   SEARCH_NEAR   = 30;    // lastScore >= SCORE_OK 일 때 반경
    static constexpr int   SEARCH_FAR    = 60;    // lastScore <  SCORE_OK 일 때 반경

    // 템플릿 크기 상한 (픽셀): 대형 박스에서 NCC 연산량 폭증 방지.
    // 박스가 이 크기를 초과하면 중앙 MAX_TMPL_W × MAX_TMPL_H 영역만 추출한다.
    static constexpr int MAX_TMPL_W = 160;
    static constexpr int MAX_TMPL_H = 80;

    // 매 렌더 프레임마다 호출 — 모든 트래커의 NCC 검색 + 소멸 판정
    void update_all(const uint8_t* bgra, int width, int height, int stride);

    // OCR 완료 시 호출 — IoU 매칭으로 기존 트래커 재동기화, 신규 박스 등록
    void register_or_update(const std::vector<VtOcrBox>& ocr_boxes,
                            const uint8_t* bgra, int width, int height, int stride);

    // 렌더 스레드에서 현재 블러 좌표 조회 (복사 반환)
    std::vector<VtOcrBox> active_boxes() const;

    void clear();

private:
    struct Tracker {
        int          id;
        const char*  type;
        std::vector<uint8_t> tmpl;    // grayscale crop (tw × th, row-major)
        int          tw = 0, th = 0;  // template 실제 픽셀 크기
        float        x  = 0, y  = 0; // 현재 top-left (float)
        float        bw = 0, bh = 0; // 박스 너비/높이
        float        lastScore = 1.0f;
        int          framesSinceMatch = 0;
    };

    mutable std::mutex    mtx_;
    std::vector<Tracker>  trackers_;
    int                   nextId_ = 0;

    // NCC score: template top-left at (sx, sy) in gray frame
    float ncc_at(const uint8_t* gray, int gstride, int gw, int gh,
                 const std::vector<uint8_t>& tmpl, int tw, int th,
                 int sx, int sy) const;

    // stride-2 탐색으로 최고 점수 위치를 찾아 tr 갱신
    void update_one(Tracker& tr,
                    const uint8_t* gray, int gstride, int gw, int gh);

    // BGRA → grayscale (gray stride = width)
    static void bgra_to_gray(const uint8_t* bgra, int width, int height, int stride,
                              std::vector<uint8_t>& gray);

    // gray 프레임의 (cx, cy, cw, ch) 영역을 crop (프레임 경계 자동 클램프)
    static std::vector<uint8_t> extract_gray_crop(
        const uint8_t* gray, int gstride, int gw, int gh,
        int cx, int cy, int cw, int ch,
        int& out_tw, int& out_th);

    static float box_iou(const VtOcrBox& a, const Tracker& b);
};
