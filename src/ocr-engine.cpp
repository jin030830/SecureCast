#include "ocr-engine.h"
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Security.Cryptography.h>
#endif

#ifdef _WIN32
struct SecureCastOcrEngine::Impl {
    winrt::Windows::Media::Ocr::OcrEngine engine{nullptr};
};
#else
struct SecureCastOcrEngine::Impl {};
#endif

SecureCastOcrEngine::SecureCastOcrEngine()
    : impl_(std::make_unique<Impl>())
{
}

SecureCastOcrEngine::~SecureCastOcrEngine() = default;

bool SecureCastOcrEngine::init()
{
#ifdef _WIN32
    try {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (const winrt::hresult_error&) {
            // 이미 초기화된 경우 계속 진행
        }

        using winrt::Windows::Globalization::Language;
        using winrt::Windows::Media::Ocr::OcrEngine;

        Language ko(L"ko");

        if (OcrEngine::IsLanguageSupported(ko)) {
            impl_->engine = OcrEngine::TryCreateFromLanguage(ko);
        } else {
            impl_->engine = OcrEngine::TryCreateFromUserProfileLanguages();
        }

        available_ = impl_->engine != nullptr;
        return available_;
    } catch (...) {
        available_ = false;
        return false;
    }
#else
    available_ = false;
    return false;
#endif
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

    uint64_t currentHash = compute_frame_hash(pixels, width, height, stride);

    if (hasLastFrameHash_ && currentHash == lastFrameHash_) {
        return lastBoxes_;
    }

    auto lines = recognize_text(pixels, width, height, stride);
    auto boxes = detect_pii(lines);

    lastFrameHash_ = currentHash;
    hasLastFrameHash_ = true;
    lastBoxes_ = boxes;

    return boxes;
}

uint64_t SecureCastOcrEngine::compute_frame_hash(
    const uint8_t *pixels,
    int width,
    int height,
    int stride
) {
    // FNV-1a 기반 간단 Dirty Skip.
    // 전체 프레임을 다 읽지 않고 16px 간격으로 샘플링해 비용을 낮춘다.
    uint64_t hash = 14695981039346656037ULL;

    constexpr uint64_t prime = 1099511628211ULL;
    constexpr int sampleStep = 16;

    for (int y = 0; y < height; y += sampleStep) {
        const uint8_t *row = pixels + static_cast<size_t>(y) * static_cast<size_t>(stride);

        for (int x = 0; x < width; x += sampleStep) {
            const uint8_t *px = row + static_cast<size_t>(x) * 4;

            hash ^= px[0];
            hash *= prime;
            hash ^= px[1];
            hash *= prime;
            hash ^= px[2];
            hash *= prime;
        }
    }

    return hash;
}

std::vector<SecureCastOcrLine> SecureCastOcrEngine::recognize_text(
    const uint8_t *pixels,
    int width,
    int height,
    int stride
) {
#ifdef _WIN32
    std::vector<SecureCastOcrLine> lines;

    if (!impl_->engine || pixels == nullptr || width <= 0 || height <= 0 || stride <= 0) {
        return lines;
    }

    try {
        using winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
        using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
        using winrt::Windows::Graphics::Imaging::SoftwareBitmap;
        using winrt::Windows::Security::Cryptography::CryptographicBuffer;

        const int bytesPerPixel = 4;
        const int rowBytes = width * bytesPerPixel;

        std::vector<uint8_t> packed;
        packed.resize(static_cast<size_t>(rowBytes) * static_cast<size_t>(height));

        for (int y = 0; y < height; ++y) {
            const uint8_t *srcRow =
                pixels + static_cast<size_t>(y) * static_cast<size_t>(stride);
            uint8_t *dstRow =
                packed.data() + static_cast<size_t>(y) * static_cast<size_t>(rowBytes);

            std::memcpy(dstRow, srcRow, static_cast<size_t>(rowBytes));
        }

        auto buffer = CryptographicBuffer::CreateFromByteArray(packed);

        SoftwareBitmap bitmap = SoftwareBitmap::CreateCopyFromBuffer(
            buffer,
            BitmapPixelFormat::Bgra8,
            width,
            height,
            BitmapAlphaMode::Premultiplied
        );

        auto result = impl_->engine.RecognizeAsync(bitmap).get();

auto ocrLines = result.Lines();

for (uint32_t i = 0; i < ocrLines.Size(); ++i) {
    auto line = ocrLines.GetAt(i);

    SecureCastOcrLine outLine{};
    outLine.text = winrt::to_string(line.Text());

    bool hasBounds = false;
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;

    auto words = line.Words();

    for (uint32_t j = 0; j < words.Size(); ++j) {
        auto word = words.GetAt(j);
        auto bounds = word.BoundingRect();

        const float x = static_cast<float>(bounds.X);
        const float y = static_cast<float>(bounds.Y);
        const float right = static_cast<float>(bounds.X + bounds.Width);
        const float bottom = static_cast<float>(bounds.Y + bounds.Height);

        if (!hasBounds) {
            minX = x;
            minY = y;
            maxX = right;
            maxY = bottom;
            hasBounds = true;
        } else {
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, right);
            maxY = std::max(maxY, bottom);
        }
    }

    if (hasBounds && !outLine.text.empty()) {
        outLine.x = minX;
        outLine.y = minY;
        outLine.w = maxX - minX;
        outLine.h = maxY - minY;
        lines.push_back(outLine);
    }
}
    } catch (...) {
        return {};
    }

    return lines;
#else
    (void)pixels;
    (void)width;
    (void)height;
    (void)stride;
    return {};
#endif
}

std::string SecureCastOcrEngine::normalize_numeric_candidate(const std::string& text)
{
    std::string out = text;

    for (char& c : out) {
        if (c == 'O' || c == 'o') {
            c = '0';
        } else if (c == 'I' || c == 'l') {
            c = '1';
        }
    }

    return out;
}

bool SecureCastOcrEngine::heuristic_filter(
    const SecureCastOcrBox& box,
    const std::string& rawText
) {
    (void)box;

    // 고위험 문맥 키워드가 있으면 유지
    static const std::regex highRiskContext(
        R"((주소|전화|휴대폰|계좌|카드|결제|주민|이메일|email|mail|tel|phone|account|card))",
        std::regex_constants::icase
    );

    if (std::regex_search(rawText, highRiskContext)) {
        return true;
    }

    // 기본 정책: 개인정보로 보이면 보수적으로 마스킹
    return true;
}

std::vector<SecureCastOcrBox> SecureCastOcrEngine::detect_pii(
    const std::vector<SecureCastOcrLine>& lines
) {
    static const std::regex rrn(R"(\d{6}[-–]\s?[1-4]\d{6})");

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

    static const std::regex numericLikeCandidate(
        R"([0-9OoIl\-\s\.–+]{6,})"
    );

    std::vector<SecureCastOcrBox> boxes;

    for (const auto& line : lines) {
        const std::string& rawText = line.text;
        const char *type = nullptr;

        if (std::regex_search(rawText, email)) {
            type = "EMAIL";
        } else {
            std::string numericText = rawText;

            if (std::regex_search(rawText, numericLikeCandidate)) {
                numericText = normalize_numeric_candidate(rawText);
            }

            if (std::regex_search(numericText, rrn)) {
                type = "RRN";
            } else if (std::regex_search(numericText, phone)) {
                type = "PHONE";
            } else if (std::regex_search(numericText, card)) {
                type = "CARD";
            } else if (std::regex_search(numericText, ip)) {
                type = "IP";
            } else if (std::regex_search(numericText, account)) {
                type = "ACCOUNT";
            }
        }

        if (type != nullptr) {
            SecureCastOcrBox box{
                type,
                line.x,
                line.y,
                line.w,
                line.h
            };

            if (heuristic_filter(box, rawText)) {
                boxes.push_back(box);
            }
        }
    }

    return boxes;
}
