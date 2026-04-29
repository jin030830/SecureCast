#include "ocr-engine.h"

#include <regex>
#include <vector>
#include <string>



bool SecureCastOcrEngine::init()
{
    // TODO: Windows.Media.Ocr 초기화
    available_ = true;
    return available_;
}

bool SecureCastOcrEngine::available() const
{
    return available_;
}

std::vector<SecureCastOcrBox> SecureCastOcrEngine::analyze_bgra_frame(
    const uint8_t *pixels,
    int width,
    int height,
    int stride
) {
    if (!available_ || !pixels || width <= 0 || height <= 0 || stride <= 0) {
        return {};
    }

    auto lines = recognize_text_stub(pixels, width, height, stride);
    return detect_pii(lines);
}

// 현재는 stub (OCR 미연동 상태)
std::vector<SecureCastOcrLine> SecureCastOcrEngine::recognize_text_stub(
    const uint8_t *pixels,
    int width,
    int height,
    int stride
) {
    (void)pixels;
    (void)width;
    (void)height;
    (void)stride;

    // TODO: Windows.Media.Ocr 결과로 교체
    return {};
}

// OCR 오인식 보정 (O → 0, l → 1 등)
std::string SecureCastOcrEngine::normalize_ocr_text(const std::string& text)
{
    std::string out = text;

    for (char& c : out) {
        if (c == 'O' || c == 'o') c = '0';
        if (c == 'I' || c == 'l') c = '1';
    }

    return out;
}

std::vector<SecureCastOcrBox> SecureCastOcrEngine::detect_pii(
    const std::vector<SecureCastOcrLine>& lines
) {
    static const std::regex rrn(
        R"(\d{6}[-–]\s?[1-4]\d{6})"
    );

    static const std::regex phone(
        R"((?:\+82[-\s]?)?(?:010|011|016|017|018|019|02|0\d{2})[-.\s]?\d{3,4}[-.\s]?\d{4})"
    );

    static const std::regex email(
        R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})"
    );

    static const std::regex card(
        R"(\d{4}[-\s]?\d{4}[-\s]?\d{4}[-\s]?\d{4})"
    );

    static const std::regex ip(
        R"(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})"
    );

    static const std::regex account(
        R"(\d{3,4}[-\s]?\d{2,4}[-\s]?\d{4,6}[-\s]?\d{0,3})"
    );

    std::vector<SecureCastOcrBox> boxes;

    for (const auto& line : lines) {
        std::string text = normalize_ocr_text(line.text);

        const char *type = nullptr;

        if (std::regex_search(text, rrn)) {
            type = "RRN";
        } else if (std::regex_search(text, phone)) {
            type = "PHONE";
        } else if (std::regex_search(text, email)) {
            type = "EMAIL";
        } else if (std::regex_search(text, card)) {
            type = "CARD";
        } else if (std::regex_search(text, ip)) {
            type = "IP";
        } else if (std::regex_search(text, account)) {
            type = "ACCOUNT";
        }

        if (type) {
            boxes.push_back({
                type,
                line.x,
                line.y,
                line.w,
                line.h
            });
        }
    }

    return boxes;
}
