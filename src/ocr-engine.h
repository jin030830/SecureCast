#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ============================================================
// Role B — 온디바이스 AI 엔진
// 담당 기능: ② 위험 키워드 블러
// 구성: OCR + Google RE2 정규식 + 휴리스틱 필터 + Dirty Rect Skip
// ============================================================

// === OCR / PII 탐지 결과 Bounding Box ===
struct SecureCastOcrBox {
    const char *type;   // "RRN", "PHONE", "EMAIL", "CARD", "IP", "ACCOUNT"
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

    // ========================================================
    // 1. Windows.Media.Ocr — 텍스트 인식
    // ========================================================

    // === STEP 0 / STEP 1: OCR 엔진 초기화 ===
    bool init();

    // === OCR 사용 가능 여부 ===
    bool available() const;

    // ========================================================
    // 전체 파이프라인 실행
    // Dirty Skip → OCR → RE2 PII 탐지 → 휴리스틱 필터
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
    // 4. 적응형 Dirty Rect Skip — 성능 최적화
    // 현재 구현: FNV-1a 샘플링 해시 기반 Dirty Skip
    // ========================================================

    uint64_t lastFrameHash_ = 0;
    bool hasLastFrameHash_ = false;
    std::vector<SecureCastOcrBox> lastBoxes_;

    uint64_t compute_frame_hash(
        const uint8_t *pixels,
        int width,
        int height,
        int stride
    );

    // ========================================================
    // 1. Windows.Media.Ocr — 텍스트와 좌표 추출
    // ========================================================

    std::vector<SecureCastOcrLine> recognize_text(
        const uint8_t *pixels,
        int width,
        int height,
        int stride
    );

    // ========================================================
    // 2. Google RE2 — 정규표현식 패턴 매칭
    // ========================================================

    std::vector<SecureCastOcrBox> detect_pii(
        const std::vector<SecureCastOcrLine>& lines
    );

    // === OCR 오류 보정: O/o → 0, I/l → 1 ===
    std::string normalize_numeric_candidate(
        const std::string& text
    );

    // ========================================================
    // 3. 휴리스틱 필터 — 2단계 보정
    // ========================================================

    bool heuristic_filter(
        const SecureCastOcrBox& box,
        const std::string& rawText
    );
};
