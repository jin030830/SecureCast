#pragma once

#include <cstdint>
#include <string>
#include <vector>

// OCR 결과에서 최종적으로 전달할 PII 영역
struct SecureCastOcrBox {
    const char *type; // PHONE, EMAIL, RRN, CARD, IP, ACCOUNT
    float x;
    float y;
    float w;
    float h;
};

// OCR 한 줄 단위 결과 (stub용)
struct SecureCastOcrLine {
    std::string text;
    float x;
    float y;
    float w;
    float h;
};

class SecureCastOcrEngine {
public:
    bool init();
    bool available() const;

    std::vector<SecureCastOcrBox> analyze_bgra_frame(
        const uint8_t *pixels,
        int width,
        int height,
        int stride
    );

private:
    bool available_ = false;

    std::vector<SecureCastOcrLine> recognize_text_stub(
        const uint8_t *pixels,
        int width,
        int height,
        int stride
    );

    std::vector<SecureCastOcrBox> detect_pii(
        const std::vector<SecureCastOcrLine>& lines
    );

    std::string normalize_ocr_text(const std::string& text);
};
