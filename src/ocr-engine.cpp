#include "ocr-engine.h"

#include <regex>
#include <string>
#include <vector>

bool SecureCastOcrEngine::init()
{
    // TODO(Role B): Windows.Media.Ocr 초기화 예정
    // 현재 단계에서는 OCR 엔진 인터페이스와 PII 탐지 파이프라인을 먼저 구성한다.
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
    if (!available_ || pixels == nullptr || width <= 0 || height <= 0 || stride <= 0) {
        return {};
    }

    auto lines = recognize_text(pixels, width, height, stride);
    return detect_pii(lines);
}

std::vector<SecureCastOcrLine> SecureCastOcrEngine::recognize_text(
    const uint8_t *pixels,
    int width,
    int height,
    int stride
) {
    (void)pixels;
    (void)width;
    (void)height;
    (void)stride;

    // TODO(Role B):
    // Windows.Media.Ocr 연동 지점.
    // 이후 pixels(BGRA frame)를 SoftwareBitmap으로 변환한 뒤
    // OcrEngine::RecognizeAsync(bitmap)을 호출하여 OcrLine 목록을 반환한다.
    //
    // 현재 PR에서는 실제 OCR 호출 전 단계로,
    // PII 탐지 인터페이스와 정규식 기반 탐지 구조를 먼저 제공한다.

    return {};
}

std::string SecureCastOcrEngine::normalize_numeric_candidate(const std::string& text)
{
    std::string out = text;

    for (char& c : out) {
        // 숫자형 개인정보 후보에서만 적용할 OCR 보정
        if (c == 'O' || c == 'o') {
            c = '0';
        } else if (c == 'I' || c == 'l') {
            c = '1';
        }
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

    // 숫자/구분자/OCR 혼동 문자로 구성된 후보만 보정 대상으로 본다.
    static const std::regex numeric_like_candidate(
        R"([0-9OoIl\-\s\.–+]{6,})"
    );

    std::vector<SecureCastOcrBox> boxes;

    for (const auto& line : lines) {
        const std::string& raw_text = line.text;

        const char *type = nullptr;

        // 이메일은 일반 단어가 포함되므로 원문 그대로 검사한다.
        if (std::regex_search(raw_text, email)) {
            type = "EMAIL";
        } else {
            std::string numeric_text = raw_text;

            // 숫자형 개인정보 후보일 때만 OCR 오인식 보정 적용
            if (std::regex_search(raw_text, numeric_like_candidate)) {
                numeric_text = normalize_numeric_candidate(raw_text);
            }

            if (std::regex_search(numeric_text, rrn)) {
                type = "RRN";
            } else if (std::regex_search(numeric_text, phone)) {
                type = "PHONE";
            } else if (std::regex_search(numeric_text, card)) {
                type = "CARD";
            } else if (std::regex_search(numeric_text, ip)) {
                type = "IP";
            } else if (std::regex_search(numeric_text, account)) {
                type = "ACCOUNT";
            }
        }

        if (type != nullptr) {
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
