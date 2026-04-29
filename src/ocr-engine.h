#pragma once

#include <cstdint>
#include <memory>
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
    SecureCastOcrEngine();
    ~SecureCastOcrEngine();

    bool init();
    bool available() const;

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

    uint64_t lastFrameHash_ = 0;
    bool hasLastFrameHash_ = false;
    std::vector<SecureCastOcrBox> lastBoxes_;

    std::vector<SecureCastOcrLine> recognize_text(
        const uint8_t *pixels,
        int width,
        int height,
        int stride
    );

    std::vector<SecureCastOcrBox> detect_pii(
        const std::vector<SecureCastOcrLine>& lines
    );

    bool heuristic_filter(
        const SecureCastOcrBox& box,
        const std::string& rawText
    );

    std::string normalize_numeric_candidate(const std::string& text);
    uint64_t compute_frame_hash(
        const uint8_t *pixels,
        int width,
        int height,
        int stride
    );
};
