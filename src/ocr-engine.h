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

    // === OCR 사용 가능 여부 ===
    bool available() const;

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
    // Dirty Rect Skip 캐시
    //
    // lastFrameWidth_ / lastFrameHeight_는 해상도 변경 시
    // 이전 프레임의 Bounding Box 좌표가 재사용되는 문제를 막기 위해 필요하다.
    // ========================================================
    uint64_t lastFrameHash_ = 0;
    bool hasLastFrameHash_ = false;
    int lastFrameWidth_ = 0;
    int lastFrameHeight_ = 0;
    std::vector<SecureCastOcrBox> lastBoxes_;

    // FNV-1a 기반 프레임 해시.
    // 작은 텍스트 변경 누락을 줄이기 위해 샘플링이 아니라 전체 픽셀 기준으로 계산한다.
    uint64_t compute_frame_hash(
        const uint8_t *pixels,
        int width,
        int height,
        int stride
    );

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

    // === PII 타입 안전 필터 ===
    // detect_pii() 내부에서 이미 타입을 엄격하게 결정한다.
    // 이 함수는 호환성과 방어적 검사를 위해 유지한다.
    bool heuristic_filter(
        const SecureCastOcrBox& box,
        const std::string& rawText
    );
};
