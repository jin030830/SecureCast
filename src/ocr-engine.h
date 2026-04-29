#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SecureCastOcrBox {
    const char *type;
    float x;
    float y;
    float w;
    float h;
};

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

    std::vector<SecureCastOcrLine> recognize_text(
        const uint8_t *pixels,
        int width,
        int height,
        int stride
    );

    std::vector<SecureCastOcrBox> detect_pii(
        const std::vector<SecureCastOcrLine>& lines
    );

    std::string normalize_numeric_candidate(const std::string& text);
};
