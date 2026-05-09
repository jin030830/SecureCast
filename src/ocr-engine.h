#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ============================================================
// Role B — On-device AI engine
// 담당 기능: 위험 키워드 블러
// 구성: OCR + Google RE2 정규식 + PII 탐지 + Dirty Rect Skip
// ============================================================

// === OCR / PII 탐지 결과 Bounding Box ===
struct SecureCastOcrBox {
    // "RRN", "PHONE", "EMAIL", "CARD", "IP", "ACCOUNT", "NAME", "ADDRESS"
    const char *type;
    float x;
    float y;
    float w;
    float h;
};

// === Windows.Media.Ocr 결과 한 줄 정보 ===
struct SecureCastOcrLine {
    std::string text;
    float x;
    float y;
    float w;
    float h;
};

class SecureCastOcrEngine {
public:
    SecureCastOcrEngine();
    ~SecureCastOcrEngine();

    // === OCR 엔진 초기화 ===
    // 주의: render thread에서 호출하지 말고 OCR worker thread에서 호출하는 것이 안전하다.
    bool init();

#ifdef SC_ENABLE_TESTS
    // detect_pii를 단위 테스트에서 직접 호출하기 위한 진입점.
    // 프로덕션 빌드에는 포함되지 않는다.
    std::vector<SecureCastOcrBox> detect_pii_for_test(
        const std::vector<SecureCastOcrLine>& lines)
    {
        return detect_pii(lines);
    }
#endif

    // === OCR 사용 가능 여부 ===
    bool available() const;

    // === 진행 중인 RecognizeAsync 취소 ===
    // stop_ocr_worker가 join() 전에 호출하여 종료 지연을 방지한다.
    // 완료된 op에 Cancel()은 no-op이므로 경쟁 조건 없이 안전하다.
    void cancel_current();

    // ========================================================
    // 전체 파이프라인 실행
    // Dirty Skip → OCR → RE2 PII 탐지
    //
    // 주의: 이 함수는 RecognizeAsync(...).get()을 내부에서 호출하므로
    // video_render thread에서 직접 동기 호출하면 프레임 드랍이 발생할 수 있다.
    // render thread가 아니라 별도 OCR worker thread에서 호출해야 한다.
    // ========================================================
    std::vector<SecureCastOcrBox> analyze_bgra_frame(
        const uint8_t *pixels,
        int width,
        int height,
        int stride
    );

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool available_ = false;

    // ========================================================
    // Dirty Skip — dHash 기반 2-Level 캐시
    //
    // L1: ROI dHash — PII 박스 영역만 해시. 시계·커서 등 배경 노이즈 무시.
    //     hamming_distance ≤ 2 → 동일 프레임 간주 → OCR 생략.
    //
    // L2: 라인별 dHash — 변경된 라인만 crop OCR 재인식.
    //     변경 없는 라인은 이전 박스 재사용 (OCR 비용 1/5~1/10).
    //
    // lastFrameWidth_/Height_: 해상도 변경 시 좌표 오탐 방지.
    // ========================================================
    uint64_t lastRoiDhash_ = 0;
    bool hasLastRoiDhash_ = false;
    int lastFrameWidth_ = 0;
    int lastFrameHeight_ = 0;
    std::vector<SecureCastOcrBox> lastBoxes_;

    struct LineDHashCache {
        int x, y, w, h;
        uint64_t dhash;
        bool hasBox;
        SecureCastOcrBox box;
    };
    std::vector<LineDHashCache> lastLineDhashes_;
    int consecutiveSkips_ = 0;
    // 이 값 이상 연속 L1 히트 시 주기적 full OCR (새 텍스트 발견용).
    // 10→2: 최대 탐지 지연 2.5s → 500ms 단축.
    //   Visual Tracker가 30Hz로 실행되므로 OCR은 재확인 역할만 하면 충분.
    //   값=2 → OCR 3회 중 1회는 full scan (새 PII 즉시 포착).
    static constexpr int kMaxConsecutiveSkips = 2;

    // 8×8 구역 dHash: 9×8 샘플 격자 → 인접 밝기 비교 → 64비트
    uint64_t compute_dhash_region(const uint8_t* px, int stride,
                                   int x, int y, int w, int h) const;
    // ROI 합산 dHash: 박스 있으면 각 박스 영역 FNV 혼합, 없으면 전체 coarse dHash
    uint64_t compute_roi_dhash(const uint8_t* px, int stride, int width, int height,
                                const std::vector<SecureCastOcrBox>& boxes) const;
    // 해밍 거리 (XOR 후 set bit 수)
    static int hamming_distance(uint64_t a, uint64_t b);
    // L2용: crop 영역만 OCR 실행, 반환 좌표는 원본 프레임 기준
    std::vector<SecureCastOcrLine> recognize_text_crop(
        const uint8_t* px, int width, int height, int stride,
        int cx, int cy, int cw, int ch);

    // === Windows.Media.Ocr — 텍스트와 좌표 추출 ===
    std::vector<SecureCastOcrLine> recognize_text(
        const uint8_t *pixels,
        int width,
        int height,
        int stride
    );

    // === Google RE2 — 정규표현식 기반 PII 탐지 ===
    std::vector<SecureCastOcrBox> detect_pii(
        const std::vector<SecureCastOcrLine>& lines
    );

    // === OCR 오류 보정: O/o → 0, I/l → 1 ===
    std::string normalize_numeric_candidate(
        const std::string& text
    );
};
