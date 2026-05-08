#include "ocr-engine.h"
#include <initializer_list>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

// === Google RE2 사용 ===
// std::regex 대신 RE2를 강제로 사용한다.
#include <re2/re2.h>

#ifdef _WIN32
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Security.Cryptography.h>
#endif

// ============================================================
// Role B — 온디바이스 AI 엔진
// 담당 기능: ② 위험 키워드 블러
// 구성: OCR + Google RE2 정규식 + Dirty Rect Skip
// ============================================================

#ifdef _WIN32
struct SecureCastOcrEngine::Impl {
    winrt::Windows::Media::Ocr::OcrEngine engine{nullptr};
    int lastFrameWidth{-1};
    int lastFrameHeight{-1};
};
#else
struct SecureCastOcrEngine::Impl {
    int lastFrameWidth{-1};
    int lastFrameHeight{-1};
};
#endif

// ============================================================
// 1. Windows.Media.Ocr — 텍스트 인식
// ============================================================

#ifdef _WIN32
namespace {

// === STEP 0: 언어팩 사전 검증 (플러그인 초기화 시 1회) ===
bool check_ocr_language()
{
    using winrt::Windows::Globalization::Language;
    using winrt::Windows::Media::Ocr::OcrEngine;

    Language ko(L"ko");
    return OcrEngine::IsLanguageSupported(ko);
}

// === STEP 0-1: 언어팩 미설치 안내 ===
void show_language_pack_guide()
{
    // TODO: 실제 UI 또는 로그 시스템과 연결
    // 예: "Windows 설정 > 언어 > 한국어 언어 팩 설치" 안내
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
        hasLastFrameHash_ = false;
        lastBoxes_.clear();
        impl_->lastFrameWidth = -1;
        impl_->lastFrameHeight = -1;

        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (const winrt::hresult_error&) {
            // 이미 초기화된 경우 계속 진행
        }

        using winrt::Windows::Globalization::Language;
        using winrt::Windows::Media::Ocr::OcrEngine;

        Language ko(L"ko");

        if (check_ocr_language()) {
            impl_->engine = OcrEngine::TryCreateFromLanguage(ko);
        } else {
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
// 현재 구현: FNV-1a 전체 픽셀 해시 기반 Dirty Skip
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

    if (stride < width * 4) {
        return {};
    }

    // === STEP 4-1: 현재 프레임 해시 계산 ===
    const uint64_t currentHash = compute_frame_hash(pixels, width, height, stride);

    // === STEP 4-2: 같은 해상도 + 같은 해시인 경우에만 OCR 생략 ===
    // 해상도가 바뀌었는데 이전 좌표를 재사용하면 마스크 위치가 어긋난다.
    const bool sameResolution =
        impl_->lastFrameWidth == width &&
        impl_->lastFrameHeight == height;

    if (hasLastFrameHash_ && sameResolution && currentHash == lastFrameHash_) {
        return lastBoxes_;
    }

    // === STEP 4-3: 변경된 프레임에 대해서만 OCR + PII 탐지 ===
    auto lines = recognize_text(pixels, width, height, stride);
    auto boxes = detect_pii(lines);

    // === STEP 4-4: 결과 캐싱 ===
    lastFrameHash_ = currentHash;
    hasLastFrameHash_ = true;
    impl_->lastFrameWidth = width;
    impl_->lastFrameHeight = height;
    lastBoxes_ = boxes;

    return boxes;
}

// === STEP 4-5: FNV-1a 기반 Dirty Skip 해시 ===
uint64_t SecureCastOcrEngine::compute_frame_hash(
    const uint8_t *pixels,
    int width,
    int height,
    int stride
) {
    constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
    constexpr uint64_t kFnvPrime = 1099511628211ULL;

    auto mixByte = [&](uint64_t& hash, uint8_t value) {
        hash ^= static_cast<uint64_t>(value);
        hash *= kFnvPrime;
    };

    auto mixUint32 = [&](uint64_t& hash, uint32_t value) {
        mixByte(hash, static_cast<uint8_t>(value & 0xffU));
        mixByte(hash, static_cast<uint8_t>((value >> 8U) & 0xffU));
        mixByte(hash, static_cast<uint8_t>((value >> 16U) & 0xffU));
        mixByte(hash, static_cast<uint8_t>((value >> 24U) & 0xffU));
    };

    uint64_t hash = kFnvOffset;

    // 해상도와 stride까지 해시에 포함해 캐시 충돌 가능성을 낮춘다.
    mixUint32(hash, static_cast<uint32_t>(width));
    mixUint32(hash, static_cast<uint32_t>(height));
    mixUint32(hash, static_cast<uint32_t>(stride));

    const size_t rowBytes = static_cast<size_t>(width) * 4U;

    // 기존 16픽셀 샘플링은 작은 텍스트 변경을 놓칠 수 있다.
    // 보안 우선 정책이므로 실제 화면 영역의 BGRA 전체 바이트를 해시한다.
    for (int y = 0; y < height; ++y) {
        const uint8_t *row =
            pixels + static_cast<size_t>(y) * static_cast<size_t>(stride);

        for (size_t i = 0; i < rowBytes; ++i) {
            mixByte(hash, row[i]);
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

    if (stride < width * 4) {
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
        auto result = impl_->engine.RecognizeAsync(bitmap).get();

        // === STEP 3: 결과에서 텍스트와 좌표 추출 ===
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
// ============================================================
// 2. Google RE2 — 정규표현식 패턴 매칭
// ============================================================

namespace {

static bool contains_any(
    const std::string& text,
    std::initializer_list<const char*> tokens
) {
    for (const char* token : tokens) {
        if (text.find(token) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static bool ends_with(const std::string& text, const std::string& suffix)
{
    if (text.size() < suffix.size()) {
        return false;
    }

    return text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool decode_utf8_next(
    const std::string& text,
    size_t& index,
    uint32_t& codepoint,
    size_t& charStart,
    size_t& charLength
) {
    if (index >= text.size()) {
        return false;
    }

    charStart = index;

    const unsigned char c0 = static_cast<unsigned char>(text[index]);

    if (c0 < 0x80) {
        codepoint = c0;
        charLength = 1;
        index += 1;
        return true;
    }

    if ((c0 & 0xE0) == 0xC0 && index + 1 < text.size()) {
        const unsigned char c1 = static_cast<unsigned char>(text[index + 1]);
        codepoint = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
        charLength = 2;
        index += 2;
        return true;
    }

    if ((c0 & 0xF0) == 0xE0 && index + 2 < text.size()) {
        const unsigned char c1 = static_cast<unsigned char>(text[index + 1]);
        const unsigned char c2 = static_cast<unsigned char>(text[index + 2]);
        codepoint = ((c0 & 0x0F) << 12) |
                    ((c1 & 0x3F) << 6) |
                    (c2 & 0x3F);
        charLength = 3;
        index += 3;
        return true;
    }

    if ((c0 & 0xF8) == 0xF0 && index + 3 < text.size()) {
        const unsigned char c1 = static_cast<unsigned char>(text[index + 1]);
        const unsigned char c2 = static_cast<unsigned char>(text[index + 2]);
        const unsigned char c3 = static_cast<unsigned char>(text[index + 3]);
        codepoint = ((c0 & 0x07) << 18) |
                    ((c1 & 0x3F) << 12) |
                    ((c2 & 0x3F) << 6) |
                    (c3 & 0x3F);
        charLength = 4;
        index += 4;
        return true;
    }

    codepoint = c0;
    charLength = 1;
    index += 1;
    return false;
}

static bool is_hangul_syllable(uint32_t codepoint)
{
    return codepoint >= 0xAC00 && codepoint <= 0xD7A3;
}

static std::string extract_hangul_syllables_utf8(const std::string& text)
{
    std::string out;

    size_t index = 0;
    while (index < text.size()) {
        uint32_t codepoint = 0;
        size_t charStart = 0;
        size_t charLength = 0;

        if (!decode_utf8_next(text, index, codepoint, charStart, charLength)) {
            continue;
        }

        if (is_hangul_syllable(codepoint)) {
            out.append(text.data() + charStart, charLength);
        }
    }

    return out;
}

static int count_hangul_syllables(const std::string& text)
{
    int count = 0;

    size_t index = 0;
    while (index < text.size()) {
        uint32_t codepoint = 0;
        size_t charStart = 0;
        size_t charLength = 0;

        if (!decode_utf8_next(text, index, codepoint, charStart, charLength)) {
            continue;
        }

        if (is_hangul_syllable(codepoint)) {
            count++;
        }
    }

    return count;
}

static int count_ascii_digits(const std::string& text)
{
    int count = 0;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            count++;
        }
    }
    return count;
}

static bool has_name_label(const std::string& text)
{
    return contains_any(text, {
        "이름",
        "성명",
        "성함",
        "고객명",
        "사용자명",
        "name",
        "Name",
        "NAME"
    });
}

static bool has_adjacent_name_label(
    const std::vector<SecureCastOcrLine>& lines,
    int index
) {
    const int start = std::max(0, index - 1);
    const int end = std::min(static_cast<int>(lines.size()) - 1, index + 1);

    for (int i = start; i <= end; ++i) {
        if (i == index) {
            continue;
        }

        if (has_name_label(lines[i].text)) {
            return true;
        }
    }

    return false;
}

static bool looks_like_korean_address_text(const std::string& text)
{
    if (contains_any(text, {
        "주소",
        "주소지",
        "거주지",
        "소재지",
        "사는곳",
        "배송지"
    })) {
        return true;
    }

    if (contains_any(text, {
        "서울",
        "부산",
        "대구",
        "인천",
        "광주",
        "대전",
        "울산",
        "세종",
        "경기",
        "강원",
        "충북",
        "충남",
        "충청",
        "전북",
        "전남",
        "전라",
        "경북",
        "경남",
        "경상",
        "제주"
    })) {
        return true;
    }

    const std::string hangulOnly = extract_hangul_syllables_utf8(text);
    const int hangulCount = count_hangul_syllables(hangulOnly);

    if (hangulCount < 2) {
        return false;
    }

    // 주소에서 자주 나오는 행정구역/도로명 접미사: 서울시, 강남구, 역삼동, 테헤란로, 세종대로, 12길 등
    if (ends_with(hangulOnly, "시") ||
        ends_with(hangulOnly, "군") ||
        ends_with(hangulOnly, "구") ||
        ends_with(hangulOnly, "동") ||
        ends_with(hangulOnly, "읍") ||
        ends_with(hangulOnly, "면") ||
        ends_with(hangulOnly, "로") ||
        ends_with(hangulOnly, "길")) {
        return true;
    }

    return false;
}

static bool starts_with_common_korean_surname(const std::string& hangulOnly)
{
    static const std::vector<std::string> surnames = {
        "김", "이", "박", "최", "정", "강", "조", "윤", "장", "임",
        "한", "오", "서", "신", "권", "황", "안", "송", "류", "홍",
        "전", "고", "문", "양", "손", "배", "백", "허", "유", "남",
        "심", "노", "하", "곽", "성", "차", "주", "우", "구", "민",
        "진", "지", "엄", "채", "원", "천", "방", "공", "현", "함",
        "변", "염", "여", "추", "도", "소", "석", "선", "설", "마",
        "길", "연", "위", "표", "명", "기", "반", "왕", "금", "옥",
        "육", "인", "맹", "제", "모", "남궁", "황보", "제갈", "선우"
    };

    for (const auto& surname : surnames) {
        if (hangulOnly.rfind(surname, 0) == 0) {
            return true;
        }
    }

    return false;
}

static bool looks_like_korean_name_candidate(const std::string& text)
{
    if (looks_like_korean_address_text(text)) {
        return false;
    }

    if (contains_any(text, {
        "주소",
        "전화",
        "휴대폰",
        "핸드폰",
        "이메일",
        "메일",
        "계좌",
        "카드",
        "결제",
        "주민",
        "번호",
        "서울",
        "부산"
    })) {
        return false;
    }

    const std::string hangulOnly = extract_hangul_syllables_utf8(text);
    const int hangulCount = count_hangul_syllables(hangulOnly);

    // 한국 이름은 보통 2~4글자이며 복성도 포함한다.
    if (hangulCount < 2 || hangulCount > 4) {
        return false;
    }

    if (!starts_with_common_korean_surname(hangulOnly)) {
        return false;
    }

    return true;
}

static bool has_nearby_pii_context(
    const std::vector<SecureCastOcrLine>& lines,
    int index
) {
    const int start = std::max(0, index - 2);
    const int end = std::min(static_cast<int>(lines.size()) - 1, index + 2);

    for (int i = start; i <= end; ++i) {
        const std::string& text = lines[i].text;

        if (text.find("@") != std::string::npos) {
            return true;
        }

        if (looks_like_korean_address_text(text)) {
            return true;
        }

        if (contains_any(text, {
            "주소",
            "전화",
            "휴대폰",
            "핸드폰",
            "이메일",
            "메일",
            "계좌",
            "카드",
            "주민",
            "010",
            "+82"
        })) {
            return true;
        }

        // 7자리 이상 숫자가 근처에 있으면 전화/계좌/주민번호/카드 가능성이 높음.
        if (count_ascii_digits(text) >= 7) {
            return true;
        }
    }

    return false;
}

} // namespace

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

// === STEP 2-1: OCR 결과에 대해 RE2 패턴 매칭 실행 ===
std::vector<SecureCastOcrBox> SecureCastOcrEngine::detect_pii(
    const std::vector<SecureCastOcrLine>& lines
) {
    // === 정규식 패턴 정의: 초기화 시 1회 컴파일 ===

    // 1) 주민등록번호: 6자리-7자리, 뒷자리 첫 글자 1~4
    static const re2::RE2 PATTERN_RRN(
        R"(\d{6}[-–]\s?[1-4]\d{6})"
    );

    // 2) 전화번호: 010-XXXX-XXXX, 02-XXX-XXXX, +82 형식 포함
    static const re2::RE2 PATTERN_PHONE(
        R"((?:\+82[-\s]?)?(?:010[-\s]?\d{4}[-\s]?\d{4}|011[-\s]?\d{4}[-\s]?\d{4}|016[-\s]?\d{4}[-\s]?\d{4}|017[-\s]?\d{4}[-\s]?\d{4}|018[-\s]?\d{4}[-\s]?\d{4}|019[-\s]?\d{4}[-\s]?\d{4}|02[-\s]?\d{3}[-\s]?\d{4}|0\d{2}[-\s]?\d{3}[-\s]?\d{4}|05\d{2}[-\s]?\d{3}[-\s]?\d{4}))"
    );

    // 3) 이메일
    static const re2::RE2 PATTERN_EMAIL(
        R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})"
    );

    // 4) 신용카드 번호
    static const re2::RE2 PATTERN_CARD(
        R"(\d{4}[-\s]?\d{4}[-\s]?\d{4}[-\s]?\d{4})"
    );

    // 5) IP 주소
    static const re2::RE2 PATTERN_IP(
        R"(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})"
    );

    // 6) 계좌번호
    static const re2::RE2 PATTERN_ACCOUNT(
        R"(\d{3,4}[-\s]?\d{2,4}[-\s]?\d{4,6}[-\s]?\d{0,3})"
    );

    // 7) OCR 보정이 필요한 숫자 후보
    static const re2::RE2 PATTERN_NUMERIC_LIKE_CANDIDATE(
        R"([0-9OoIl\-\s\.–+]{6,})"
    );

    std::vector<SecureCastOcrBox> boxes;

    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const auto& line = lines[i];
        const std::string& rawText = line.text;

        const char *type = nullptr;

        // === STEP 2-2: 이메일은 문자 기반이므로 먼저 검사 ===
        if (re2::RE2::PartialMatch(rawText, PATTERN_EMAIL)) {
            type = "EMAIL";
        }

        // === STEP 2-2-A: 주소 탐지 ===
        // 예: 주소:서울시, 서울시 강남구, 역삼동, 테헤란로 등
        if (type == nullptr && looks_like_korean_address_text(rawText)) {
            type = "ADDRESS";
        }

        // === STEP 2-3: 숫자형 개인정보 탐지 ===
        if (type == nullptr) {
            std::string numericText = rawText;

            if (re2::RE2::PartialMatch(rawText, PATTERN_NUMERIC_LIKE_CANDIDATE)) {
                numericText = normalize_numeric_candidate(rawText);
            }

            if (re2::RE2::PartialMatch(numericText, PATTERN_RRN)) {
                type = "RRN";
            } else if (re2::RE2::PartialMatch(numericText, PATTERN_PHONE)) {
                type = "PHONE";
            } else if (re2::RE2::PartialMatch(numericText, PATTERN_CARD)) {
                type = "CARD";
            } else if (re2::RE2::PartialMatch(numericText, PATTERN_IP)) {
                type = "IP";
            } else if (re2::RE2::PartialMatch(numericText, PATTERN_ACCOUNT)) {
                type = "ACCOUNT";
            }
        }

        // === STEP 2-3-A: 이름 탐지 ===
        // 1) "이름/성명/성함" 라벨이 명확하면 같은 줄 또는 인접 줄의 이름 후보까지 마스킹
        // 2) 라벨이 없으면 한국 이름 후보 + 근처 이메일/전화/주소/계좌 문맥을 요구
        if (type == nullptr) {
            const bool explicitNameLabel = has_name_label(rawText);
            const bool nameCandidate = looks_like_korean_name_candidate(rawText);
            const bool adjacentNameLabel = has_adjacent_name_label(lines, i);

            if (explicitNameLabel) {
                // 같은 줄에 라벨과 실제 이름이 같이 나온 경우: "이름: 김철수"
                const std::string hangulOnly = extract_hangul_syllables_utf8(rawText);
                if (count_hangul_syllables(hangulOnly) >= 3) {
                    type = "NAME";
                }
            } else if (nameCandidate && (adjacentNameLabel || has_nearby_pii_context(lines, i))) {
                // OCR이 "이름:"과 "김철수"를 서로 다른 라인으로 분리하는 경우를 처리한다.
                type = "NAME";
            }
        }

        // === STEP 2-4: 패턴에 걸린 경우 Bounding Box 생성 ===
        if (type != nullptr) {
            boxes.push_back(SecureCastOcrBox{
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

// ============================================================
// 3. 휴리스틱 필터 — 2단계 보정
// ============================================================

bool SecureCastOcrEngine::heuristic_filter(
    const SecureCastOcrBox& box,
    const std::string& rawText
) {
    (void)rawText;

    // detect_pii()에서 이미 타입별 판정을 끝냈다.
    // 여기서 전부 true를 반환하면 필터 의미가 사라지므로, 알려진 타입만 통과시킨다.
    return std::strcmp(box.type, "RRN") == 0 ||
           std::strcmp(box.type, "PHONE") == 0 ||
           std::strcmp(box.type, "EMAIL") == 0 ||
           std::strcmp(box.type, "CARD") == 0 ||
           std::strcmp(box.type, "IP") == 0 ||
           std::strcmp(box.type, "ACCOUNT") == 0 ||
           std::strcmp(box.type, "NAME") == 0 ||
           std::strcmp(box.type, "ADDRESS") == 0;
}
