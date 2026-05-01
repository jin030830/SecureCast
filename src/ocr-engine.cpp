#include "ocr-engine.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <cstring>
#include <regex>
#include <string>
#include <vector>
#include <cstdint>

#ifdef _WIN32
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Security.Cryptography.h>
#endif

// ============================================================
// Role B — 온디바이스 AI 엔진
// 담당 기능: ② 위험 키워드 블러
// 구성: OCR + 정규식 + 휴리스틱 필터 + Dirty Rect Skip
// ============================================================

#ifdef _WIN32
struct SecureCastOcrEngine::Impl {
    winrt::Windows::Media::Ocr::OcrEngine engine{nullptr};
};
#else
struct SecureCastOcrEngine::Impl {};
#endif

// ============================================================
// 1. Windows.Media.Ocr — 텍스트 인식
// ============================================================

#ifdef _WIN32
namespace {

// === STEP 0: 언어팩 사전 검증 (플러그인 초기화 시 1회) ===
// PDF 샘플에는 CheckOcrLanguage()가 따로 있었지만,
// 현재 클래스 헤더를 바꾸지 않기 위해 cpp 내부 helper 함수로 구현한다.
bool check_ocr_language()
{
    using winrt::Windows::Globalization::Language;
    using winrt::Windows::Media::Ocr::OcrEngine;

    Language ko(L"ko");
    return OcrEngine::IsLanguageSupported(ko);
}

// === STEP 0-1: 언어팩 미설치 안내 ===
// 실제 UI가 있으면 여기서 사용자에게
// "Windows 설정 > 언어 > 한국어 언어 팩 설치" 안내를 띄우면 된다.
void show_language_pack_guide()
{
    // TODO: 프로젝트 UI/로그 시스템에 맞게 연결
    // 예: log_warning("한국어 OCR 언어팩이 설치되어 있지 않습니다.");
}

} // namespace
#endif

SecureCastOcrEngine::SecureCastOcrEngine()
    : impl_(std::make_unique<Impl>())
{
}

SecureCastOcrEngine::~SecureCastOcrEngine() = default;

// === STEP 1: OCR 엔진 생성 (초기화 시 1회) ===
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

        // === STEP 0 결과 반영: 한국어 언어팩 확인 ===
        if (check_ocr_language()) {
            impl_->engine = OcrEngine::TryCreateFromLanguage(ko);
        } else {
            // 한국어 언어팩이 없으면 사용자 프로필 언어 기반 OCR로 fallback
            show_language_pack_guide();
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

// ============================================================
// 4. 적응형 Dirty Rect Skip — 성능 최적화
// PDF에서는 SSIM 기반으로 설명하지만,
// 현재 구현에서는 FNV-1a 샘플링 해시 기반 Dirty Skip을 사용한다.
// ============================================================

std::vector<SecureCastOcrBox> SecureCastOcrEngine::analyze_bgra_frame(
    const uint8_t *pixels,
    int width,
    int height,
    int stride
) {
    if (!available_ || pixels == nullptr || width <= 0 || height <= 0 || stride <= 0) {
        return {};
    }

    // === STEP 4-1: 현재 프레임 해시 계산 ===
    uint64_t currentHash = compute_frame_hash(pixels, width, height, stride);

    // === STEP 4-2: 이전 프레임과 동일하면 OCR 생략 ===
    if (hasLastFrameHash_ && currentHash == lastFrameHash_) {
        return lastBoxes_;
    }

    // === STEP 4-3: 프레임이 바뀐 경우 OCR + PII 탐지 실행 ===
    auto lines = recognize_text(pixels, width, height, stride);
    auto boxes = detect_pii(lines);

    // === STEP 4-4: 결과 캐싱 ===
    lastFrameHash_ = currentHash;
    hasLastFrameHash_ = true;
    lastBoxes_ = boxes;

    return boxes;
}

// === STEP 4-5: FNV-1a 기반 간단 Dirty Skip 해시 ===
// 전체 프레임을 모두 읽지 않고 16px 간격으로 샘플링해서 비용을 낮춘다.
uint64_t SecureCastOcrEngine::compute_frame_hash(
    const uint8_t *pixels,
    int width,
    int height,
    int stride
) {
    uint64_t hash = 14695981039346656037ULL;

    constexpr uint64_t prime = 1099511628211ULL;
    constexpr int sampleStep = 16;

    for (int y = 0; y < height; y += sampleStep) {
        const uint8_t *row =
            pixels + static_cast<size_t>(y) * static_cast<size_t>(stride);

        for (int x = 0; x < width; x += sampleStep) {
            const uint8_t *px = row + static_cast<size_t>(x) * 4;

            // BGRA 중 B, G, R만 사용
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

// ============================================================
// 1. Windows.Media.Ocr — 텍스트 인식 구현
// ============================================================

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

        // === STEP 2: BGRA 프레임 데이터를 SoftwareBitmap으로 변환 ===
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

        // === STEP 2-1: OCR 실행 ===
        // PDF에서는 co_await 형태지만,
        // 현재 코드는 동기 호출 구조이므로 .get()으로 결과를 기다린다.
        auto result = impl_->engine.RecognizeAsync(bitmap).get();

        // === STEP 3: OCR 결과에서 텍스트와 좌표 추출 ===
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

            // === STEP 3-1: 한 줄 단위 Bounding Box 저장 ===
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

// ============================================================
// 2. 정규표현식 패턴 매칭
// ============================================================

// === STEP 2-0: OCR 오류 보정 ===
// OCR이 숫자 0을 O/o로, 숫자 1을 I/l로 잘못 읽는 경우를 보정한다.
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

// === STEP 2-1: OCR 결과에 대해 개인정보 패턴 매칭 실행 ===
// PDF에서는 Google RE2를 권장하지만,
// 현재 프로젝트 코드는 std::regex 기반이므로 기존 구조를 유지한다.
// RE2를 설치했다면 std::regex_search 부분을 RE2::PartialMatch로 바꾸면 된다.
std::vector<SecureCastOcrBox> SecureCastOcrEngine::detect_pii(
    const std::vector<SecureCastOcrLine>& lines
) {
    // 1) 주민등록번호: 6자리-7자리, 뒷자리 첫 글자 1~4
    static const std::regex rrn(
        R"(\d{6}[-–]\s?[1-4]\d{6})"
    );

    // 2) 전화번호: 010-XXXX-XXXX, 02-XXX-XXXX, +82 형식 포함
    static const std::regex phone(
        R"((?:\+82[-\s]?)?(?:010|011|016|017|018|019|02|0\d{2})[-.\s]?\d{3,4}[-.\s]?\d{4})"
    );

    // 3) 이메일
    static const std::regex email(
        R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})"
    );

    // 4) 신용카드 번호
    static const std::regex card(
        R"(\d{4}[-\s]?\d{4}[-\s]?\d{4}[-\s]?\d{4})"
    );

    // 5) IP 주소
    static const std::regex ip(
        R"(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})"
    );

    // 6) 계좌번호
    static const std::regex account(
        R"(\d{3,4}[-\s]?\d{2,4}[-\s]?\d{4,6}[-\s]?\d{0,3})"
    );

    // 7) OCR 보정이 필요한 숫자 후보
    static const std::regex numericLikeCandidate(
        R"([0-9OoIl\-\s\.–+]{6,})"
    );

    std::vector<SecureCastOcrBox> boxes;

    for (const auto& line : lines) {
        const std::string& rawText = line.text;
        const char *type = nullptr;

        // === STEP 2-2: 이메일은 문자 기반이므로 먼저 검사 ===
        if (std::regex_search(rawText, email)) {
            type = "EMAIL";
        } else {
            std::string numericText = rawText;

            // === STEP 2-3: 숫자형 후보는 OCR 오류 보정 후 검사 ===
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

        // === STEP 2-4: 패턴에 걸린 경우 Bounding Box 생성 ===
        if (type != nullptr) {
            SecureCastOcrBox box{
                type,
                line.x,
                line.y,
                line.w,
                line.h
            };

            // === STEP 3으로 전달: 휴리스틱 필터 적용 ===
            if (heuristic_filter(box, rawText)) {
                boxes.push_back(box);
            }
        }
    }

    return boxes;
}

// ============================================================
// 3. 휴리스틱 필터 — 2단계 보정
// ============================================================

bool SecureCastOcrEngine::heuristic_filter(
    const SecureCastOcrBox& box,
    const std::string& rawText
) {
    // 현재 함수에는 sourceWindow(HWND)가 없기 때문에,
    // PDF의 프로세스 이름 / 윈도우 타이틀 기반 필터는 아직 직접 적용하지 못한다.
    // 대신 OCR 텍스트 주변 문맥 기반 필터를 먼저 구현한다.

    (void)box;

    // === 규칙 1: 고위험 문맥 키워드가 있으면 유지 ===
    // 주소, 전화, 계좌, 카드, 결제, 주민번호, 이메일 관련 단어가 있으면 블러한다.
    static const std::regex highRiskContext(
        R"((주소|전화|휴대폰|계좌|카드|결제|주민|이메일|email|mail|tel|phone|account|card))",
        std::regex_constants::icase
    );

    if (std::regex_search(rawText, highRiskContext)) {
        return true;
    }

    // === 규칙 2: 기본 정책 ===
    // 개인정보로 의심되면 보수적으로 마스킹한다.
    // 즉, 애매하면 블러한다.
    return true;
}
