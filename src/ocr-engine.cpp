#include "ocr-engine.h"
#include <initializer_list>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// === Google RE2 사용 ===
// std::regex 대신 RE2를 강제로 사용한다.
#include <re2/re2.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Security.Cryptography.h>

// ============================================================
// Role B — 온디바이스 AI 엔진
// 담당 기능: ② 위험 키워드 블러
// 구성: OCR + Google RE2 정규식 + Dirty Rect Skip
// ============================================================

#ifndef _WIN32
#error "SecureCast requires Windows. See CMakeLists.txt for details."
#endif

struct SecureCastOcrEngine::Impl {
    winrt::Windows::Media::Ocr::OcrEngine engine{nullptr};
    int lastFrameWidth{-1};
    int lastFrameHeight{-1};

    // stop_ocr_worker가 RecognizeAsync 블로킹 중에 Cancel()을 보낼 수 있도록 보관.
    // opMutex로 worker/main 스레드 간 접근을 보호한다.
    using OcrAsyncOp = winrt::Windows::Foundation::IAsyncOperation<
        winrt::Windows::Media::Ocr::OcrResult>;
    std::mutex   opMutex;
    OcrAsyncOp   currentOp{nullptr};
};

// ============================================================
// 1. Windows.Media.Ocr — 텍스트 인식
// ============================================================

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

SecureCastOcrEngine::SecureCastOcrEngine()
    : impl_(std::make_unique<Impl>())
{
}

void SecureCastOcrEngine::cancel_current()
{
    std::lock_guard<std::mutex> lk(impl_->opMutex);
    if (impl_->currentOp) {
        impl_->currentOp.Cancel();
        impl_->currentOp = nullptr;
    }
}

SecureCastOcrEngine::~SecureCastOcrEngine() = default;

// === STEP 1: OCR 엔진 생성 (초기화 시 1회) ===
bool SecureCastOcrEngine::init()
{
    try {
        hasLastRoiDhash_ = false;
        consecutiveSkips_ = 0;
        lastBoxes_.clear();
        lastLineDhashes_.clear();
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
    if (!available_ || pixels == nullptr || width <= 0 || height <= 0 || stride <= 0)
        return {};
    if (stride < width * 4)
        return {};

    const bool sameRes =
        impl_->lastFrameWidth == width &&
        impl_->lastFrameHeight == height;

    // 해상도 변경 시 모든 캐시 상태 초기화
    if (!sameRes) {
        lastLineDhashes_.clear();
        hasLastRoiDhash_ = false;
        consecutiveSkips_ = 0;
    }

    // ── L1: ROI dHash ─────────────────────────────────────────────
    // PII 박스 영역만 해시 → 시계·커서 등 ROI 외부 변화는 완전히 무시.
    // hamming_distance ≤ 2 → 안티앨리어싱·압축 노이즈 허용 → OCR 생략.
    if (sameRes && hasLastRoiDhash_) {
        const uint64_t roiHash =
            compute_roi_dhash(pixels, stride, width, height, lastBoxes_);
        if (hamming_distance(roiHash, lastRoiDhash_) <= 2) {
            if (++consecutiveSkips_ < kMaxConsecutiveSkips)
                return lastBoxes_; // L1 hit
            // kMaxConsecutiveSkips 연속 히트: 새 텍스트 발견용 주기적 full OCR
        }
    }
    consecutiveSkips_ = 0;

    // ── L2: 라인별 dHash + crop OCR ────────────────────────────────
    // NAME은 인접 줄 컨텍스트가 필요 → 이전 결과에 NAME이 있으면 L2 스킵
    bool prevHadName = false;
    for (const auto& b : lastBoxes_)
        if (b.type && std::strcmp(b.type, "NAME") == 0) { prevHadName = true; break; }

    if (sameRes && !lastLineDhashes_.empty() && !prevHadName) {
        std::vector<SecureCastOcrBox> merged;
        bool anyChanged = false;

        for (auto& lc : lastLineDhashes_) {
            if (lc.x < 0 || lc.y < 0 ||
                lc.x + lc.w > width || lc.y + lc.h > height) continue;

            const uint64_t lineHash =
                compute_dhash_region(pixels, stride, lc.x, lc.y, lc.w, lc.h);

            if (hamming_distance(lineHash, lc.dhash) <= 2) {
                if (lc.hasBox) merged.push_back(lc.box); // 불변 라인 재사용
            } else {
                anyChanged = true;
                // 변경된 라인만 crop OCR
                auto cropLines = recognize_text_crop(
                    pixels, width, height, stride,
                    lc.x, lc.y, lc.w, lc.h);
                auto cropBoxes = detect_pii(cropLines);
                for (auto& b : cropBoxes) merged.push_back(b);
                // L2 캐시 갱신
                lc.dhash  = lineHash;
                lc.hasBox = !cropBoxes.empty();
                lc.box    = lc.hasBox ? cropBoxes[0] : SecureCastOcrBox{};
            }
        }

        if (anyChanged) {
            lastRoiDhash_ = compute_roi_dhash(pixels, stride, width, height, merged);
            lastBoxes_ = merged;
            return merged; // L2 hit (partial)
        }
        // 모든 라인 불변이지만 L1 실패 → ROI 외부에 새 텍스트 가능 → full OCR
    }

    // ── Full OCR ──────────────────────────────────────────────────
    auto lines = recognize_text(pixels, width, height, stride);
    auto boxes = detect_pii(lines);

    // L1 캐시 갱신 (VisualTracker가 좌표 추적 담당, OCR은 탐지/확인만 수행)
    lastRoiDhash_ = compute_roi_dhash(pixels, stride, width, height, boxes);
    hasLastRoiDhash_ = true;
    impl_->lastFrameWidth  = width;
    impl_->lastFrameHeight = height;
    lastBoxes_ = boxes;

    // L2 라인 캐시 재구성
    lastLineDhashes_.clear();
    for (const auto& line : lines) {
        LineDHashCache lc{};
        lc.x = std::max(0, (int)line.x);
        lc.y = std::max(0, (int)line.y);
        lc.w = std::min((int)line.w, width  - lc.x);
        lc.h = std::min((int)line.h, height - lc.y);
        if (lc.w <= 0 || lc.h <= 0) continue;
        lc.dhash  = compute_dhash_region(pixels, stride, lc.x, lc.y, lc.w, lc.h);
        lc.hasBox = false;
        // 이 라인의 bounding box 안에 속한 PII 박스를 연결
        for (const auto& b : boxes) {
            if ((int)b.x >= lc.x && (int)b.y >= lc.y &&
                (int)(b.x + b.w) <= lc.x + lc.w &&
                (int)(b.y + b.h) <= lc.y + lc.h) {
                lc.hasBox = true;
                lc.box    = b;
                break;
            }
        }
        lastLineDhashes_.push_back(lc);
    }

    return boxes;
}

// ============================================================
// Dirty Skip — dHash 기반 헬퍼 함수
// ============================================================

// 9×8 격자 샘플 → 행별 인접 픽셀 밝기 비교 → 64비트 dHash
// BGRA 포맷: B=p[0], G=p[1], R=p[2]; 그레이 = (B+G+R)/3
uint64_t SecureCastOcrEngine::compute_dhash_region(
    const uint8_t* px, int stride,
    int x, int y, int w, int h) const
{
    if (!px || w <= 0 || h <= 0) return 0;

    constexpr int COLS = 9, ROWS = 8;
    uint64_t hash = 0;

    for (int r = 0; r < ROWS; ++r) {
        uint8_t g[COLS];
        for (int c = 0; c < COLS; ++c) {
            int sx = x + (COLS > 1 ? c * (w - 1) / (COLS - 1) : 0);
            int sy = y + (ROWS > 1 ? r * (h - 1) / (ROWS - 1) : 0);
            sx = std::max(x, std::min(x + w - 1, sx));
            sy = std::max(y, std::min(y + h - 1, sy));
            const uint8_t* p = px + sy * stride + sx * 4;
            g[c] = static_cast<uint8_t>(((int)p[0] + p[1] + p[2]) / 3);
        }
        for (int c = 0; c < COLS - 1; ++c)
            hash = (hash << 1) | (g[c] > g[c + 1] ? 1u : 0u);
    }
    return hash; // 8행 × 8비트 = 64비트
}

// ROI 합산 dHash: 박스 있으면 각 박스 영역 dHash를 FNV로 혼합,
// 박스 없으면 전체 프레임 coarse dHash (새 텍스트 발견용)
uint64_t SecureCastOcrEngine::compute_roi_dhash(
    const uint8_t* px, int stride, int width, int height,
    const std::vector<SecureCastOcrBox>& boxes) const
{
    if (boxes.empty())
        return compute_dhash_region(px, stride, 0, 0, width, height);

    constexpr uint64_t kFnvPrime = 1099511628211ULL;
    uint64_t combined = 14695981039346656037ULL;
    for (const auto& b : boxes) {
        int bx = std::max(0, (int)b.x);
        int by = std::max(0, (int)b.y);
        int bw = std::min((int)b.w, width  - bx);
        int bh = std::min((int)b.h, height - by);
        if (bw <= 0 || bh <= 0) continue;
        combined ^= compute_dhash_region(px, stride, bx, by, bw, bh);
        combined *= kFnvPrime;
    }
    return combined;
}

int SecureCastOcrEngine::hamming_distance(uint64_t a, uint64_t b) {
    return static_cast<int>(__popcnt64(a ^ b));
}

// L2 crop OCR: 원본 프레임 (cx,cy,cw,ch) 영역을 복사 후 OCR 실행.
// 반환 라인 좌표에 (cx, cy) 오프셋 적용.
std::vector<SecureCastOcrLine> SecureCastOcrEngine::recognize_text_crop(
    const uint8_t* px, int width, int height, int stride,
    int cx, int cy, int cw, int ch)
{
    cx = std::max(0, cx); cy = std::max(0, cy);
    cw = std::min(cw, width - cx); ch = std::min(ch, height - cy);
    if (!px || cw <= 0 || ch <= 0) return {};

    std::vector<uint8_t> crop(static_cast<size_t>(cw) * ch * 4);
    for (int ry = 0; ry < ch; ++ry) {
        std::memcpy(crop.data() + static_cast<size_t>(ry) * cw * 4,
                    px + static_cast<size_t>(cy + ry) * stride + cx * 4,
                    static_cast<size_t>(cw) * 4);
    }

    auto lines = recognize_text(crop.data(), cw, ch, cw * 4);
    for (auto& l : lines) { l.x += cx; l.y += cy; }
    return lines;
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
        // op를 Impl에 보관해 두면 stop_ocr_worker가 Cancel()을 보낼 수 있다.
        // opMutex 구간을 짧게 유지하여 Cancel() 호출이 .get() 블로킹 외부에서 처리된다.
        auto op = impl_->engine.RecognizeAsync(bitmap);
        {
            std::lock_guard<std::mutex> lk(impl_->opMutex);
            impl_->currentOp = op;
        }
        auto result = op.get(); // hresult_canceled 포함 예외 → catch(...) → return {}
        {
            std::lock_guard<std::mutex> lk(impl_->opMutex);
            impl_->currentOp = nullptr;
        }

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
        // 한국어 기본
        "이름", "성명", "성함", "고객명", "사용자명",
        // 금융/거래 폼 (A-6)
        "예금주", "수취인", "발신인", "수신인",
        // 행정/의료 폼 (A-6)
        "보호자", "담당자", "신청인", "회원명", "환자명",
        // 영문 (A-6)
        "name", "Name", "NAME",
        "Full Name", "First Name", "Last Name",
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
    // 명시 라벨 → 단독으로 충분
    if (contains_any(text, {"주소", "주소지", "거주지", "소재지", "사는곳", "배송지"}))
        return true;

    // 시·도 광역권 키워드 AND 행정구역 접미사 — 둘 다 만족해야 ADDRESS
    // "서울 다녀옴" → 지역어는 있지만 접미사 없음 → false
    // "도서관 가는 길" → 지역어 없음 → false
    // "서울시 강남구 역삼동" → 지역어+접미사 → true
    const bool hasRegion = contains_any(text, {
        "서울", "부산", "대구", "인천", "광주", "대전", "울산", "세종",
        "경기", "강원", "충북", "충남", "충청", "전북", "전남", "전라",
        "경북", "경남", "경상", "제주"
    });

    // B-2: 지역어 + 도로명 주소 ("서울 테헤란로 152", "경기 강남대로 1번길")
    // 기존 AND 조건에서 제외한 '로/길'을 번지수가 있을 때만 허용한다.
    if (hasRegion) {
        static const re2::RE2 PAT_ROAD(R"([가-힣]{2,}(?:로|길)\s*\d+)");
        if (RE2::PartialMatch(text, PAT_ROAD))
            return true;
    }

    // B-3: 동·읍·면 + 번지 ("역삼동 736-1", "삼성동 167번지") — 지역어 없어도 충분한 시그널
    {
        static const re2::RE2 PAT_LOT(R"([가-힣]{2,}(?:동|읍|면)\s*\d+(?:-\d+)?(?:번지)?)");
        if (RE2::PartialMatch(text, PAT_LOT))
            return true;
    }

    if (!hasRegion)
        return false;

    const std::string hangulOnly = extract_hangul_syllables_utf8(text);
    if (count_hangul_syllables(hangulOnly) < 3)
        return false;

    return ends_with(hangulOnly, "시") ||
           ends_with(hangulOnly, "군") ||
           ends_with(hangulOnly, "구") ||
           ends_with(hangulOnly, "동") ||
           ends_with(hangulOnly, "읍") ||
           ends_with(hangulOnly, "면");
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

// 성씨로 시작하지만 이름이 아닌 일반 한국어 단어 목록 (hangulOnly 기준)
// starts_with_common_korean_surname을 통과한 뒤 최종 방어선으로 사용한다.
static const std::unordered_set<std::string> NAME_BLOCKLIST = {
    // 김
    "김치", "김밥", "김장", "김포",
    // 이
    "이번", "이런", "이상", "이미", "이후", "이전", "이용", "이름",
    "이거", "이것", "이하", "이메일", "이상한",
    // 박
    "박스", "박사", "박수", "박물관",
    // 최
    "최근", "최선", "최대", "최고", "최소", "최신", "최저", "최종",
    // 정
    "정말", "정도", "정보", "정리", "정확", "정상",
    // 강
    "강의", "강조", "강력", "강아지", "강제",
    // 조
    "조금", "조회", "조건",
    // 장
    "장소", "장점", "장기",
    // 임
    "임시", "임의", "임대",
    // 한
    "한국", "한글", "한번", "한식", "한방",
    // 오
    "오늘", "오후", "오전", "오류", "오랜",
    // 서
    "서울",
    // 신
    "신청", "신용", "신규", "신뢰", "신호", "신고",
    // 안
    "안녕", "안전", "안내",
    // 송
    "송금", "송신",
    // 홍
    "홍보",
    // 전
    "전화", "전송", "전달",
    // 고
    "고객", "고려", "고급", "고장",
    // 하
    "하루", "하단",
    // 성
    "성명", "성함", "성별", "성과",
    // 기
    "기능", "기록", "기간",
    // 선 — "선생"은 A-7 님/씨 충돌 차단
    "선택", "선명", "선생",
    // 지
    "지금", "지역", "지원",
    // 공
    "공지", "공개",
    // 원 — "원장"은 A-7 충돌 차단
    "원본", "원래", "원장",
    // 민
    "민원",
    // 방
    "방법", "방향",
    // 주 — "주임"은 A-7 충돌 차단
    "주소", "주민", "주임",
    // 구
    "구매", "구독",
    // 현
    "현재", "현황",
    // 노 — "노출" 추가
    "노트", "노래", "노출",
    // 차 — "차장"은 A-7 충돌 차단
    "차장",
    // 이사 (이 + 사 = 직급, A-7 충돌 차단)
    "이사",
    // 양
    "양쪽", "양호",
    // 윤
    "윤리",
};

// Luhn checksum (ISO/IEC 7812-1)
// 무작위 16자리 숫자의 약 90%가 이 체크를 통과하지 못한다.
static bool luhn_check(const std::string& digits)
{
    if (digits.size() != 16) return false;
    int sum = 0;
    for (int i = 0; i < 16; ++i) {
        int d = digits[15 - i] - '0'; // 오른쪽에서 i번째 자리
        if (i % 2 == 1) {             // 오른쪽 2번째, 4번째... 자리는 2배
            d *= 2;
            if (d > 9) d -= 9;        // 두 자리이면 각 자리 합 (= d-9)
        }
        sum += d;
    }
    return sum % 10 == 0;
}

// IIN (Issuer Identification Number) 검증 — 주요 카드사 prefix 화이트리스트
// Visa(4), Mastercard(51-55 / 2221-2720), Discover(6011/65/644-649),
// JCB(3528-3589), UnionPay(62)
static bool valid_card_iin(const std::string& digits)
{
    if (digits.size() < 6) return false;

    const int p1 = digits[0] - '0';
    const int p2 = p1 * 10 + (digits[1] - '0');
    const int p3 = p2 * 10 + (digits[2] - '0');
    const int p4 = p3 * 10 + (digits[3] - '0');
    const int p6 = p4 * 100 + (digits[4] - '0') * 10 + (digits[5] - '0');

    if (p1 == 4)                          return true; // Visa
    if (p2 >= 51 && p2 <= 55)             return true; // Mastercard (classic)
    if (p4 >= 2221 && p4 <= 2720)         return true; // Mastercard (new range)
    if (p4 == 6011)                       return true; // Discover
    if (p2 == 65)                         return true; // Discover
    if (p3 >= 644 && p3 <= 649)           return true; // Discover
    if (p6 >= 622126 && p6 <= 622925)     return true; // Discover/UnionPay
    if (p4 >= 3528 && p4 <= 3589)         return true; // JCB
    if (p2 == 62)                         return true; // UnionPay

    return false;
}

static bool valid_email(const std::string& e) {
    const auto at = e.find('@');
    if (at == std::string::npos || at < 1 || at > 64) return false;
    const auto dot = e.rfind('.');
    if (dot == std::string::npos || dot < at + 2 || dot >= e.size() - 2) return false;
    const auto tldLen = e.size() - dot - 1;
    if (tldLen < 2 || tldLen > 24) return false;
    if (e.find("..") != std::string::npos) return false;
    return true;
}

// OCR 분할 주소 결합용: 두 라인이 시각적으로 연속한지 판단.
// y 거리가 현재 줄 높이의 2.5배 이내 & 중심 x 거리가 너비의 80% 이내.
static bool lines_visually_adjacent(const SecureCastOcrLine& a,
                                    const SecureCastOcrLine& b)
{
    const float lineH = (a.h > 0.0f) ? a.h : 20.0f;
    if (b.y - a.y > 2.5f * lineH) return false;
    const float maxW = (a.w > b.w) ? a.w : b.w;
    const float dx   = (a.x + a.w * 0.5f) - (b.x + b.w * 0.5f);
    if ((dx < 0.0f ? -dx : dx) > maxW * 0.8f) return false;
    return true;
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

    // 1) 주민등록번호: 6자리-7자리, 뒷자리 첫 글자 1~8 (외국인 포함)
    // \s?[-–]\s?: OCR이 "850101 - 1234566" 처럼 구분자 앞뒤 공백을 추가하는 경우 허용.
    static const re2::RE2 PATTERN_RRN(
        R"(\b(\d{6})\s?[-–]\s?([1-8]\d{6})\b)"
    );

    // 2) 전화번호: 휴대폰(010/011/016~019) + 지역번호(02/03x/04x/05x/06x) + +82 정규화
    // +82 대안: \b 를 제거 — RE2에서 \b 는 ASCII word-char 기준이므로 `+` 앞에서 미작동.
    //           01X 대안은 \b 를 유지 (긴 숫자열 부분 매칭 방지).
    static const re2::RE2 PATTERN_PHONE(
        R"((?:\+82[-\s]?1[016-9]|\b01[016-9])[-\s]?\d{3,4}[-\s]?\d{4}\b|\b0(?:2[-\s]?\d{3,4}|[3-9]\d[-\s]?\d{3,4}|[3-9]\d{2}[-\s]?\d{3,4})[-\s]?\d{4}\b)"
    );

    // 3) 이메일: 전체를 그룹 1로 캡처 (valid_email 검증에 사용)
    static const re2::RE2 PATTERN_EMAIL(
        R"(([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}))"
    );

    // 4) 신용카드 번호: 정확히 4-4-4-4 구조 + 그룹 캡처 (Luhn/IIN 검증용)
    static const re2::RE2 PATTERN_CARD(
        R"(\b(\d{4})[-\s](\d{4})[-\s](\d{4})[-\s](\d{4})\b)"
    );

    // 5) IP 주소: 각 옥텟 캡처 (사설/예약 대역 제외는 아래 is_valid_public_ip로 검증)
    static const re2::RE2 PATTERN_IP(
        R"(\b(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})\b)"
    );

    // 6) 계좌번호: "계좌", "예금주" 라벨 인접 시에만 판정 (아래 로직에서 제어)
    static const re2::RE2 PATTERN_ACCOUNT(
        R"(\d{3,4}[-\s]?\d{2,4}[-\s]?\d{4,6}(?:[-\s]?\d{0,3})?)"
    );

    // 7) OCR 보정이 필요한 숫자 cluster 추출
    static const re2::RE2 PATTERN_NUMERIC_CLUSTER(
        R"([0-9OoIl](?:[0-9OoIl\-\s\.–+]*[0-9OoIl])?)"
    );

    // A-7) 이름 + 호칭 직접 패턴 (높은 신뢰도)
    // "김철수님", "이민준씨" 등 — 성씨 시작 2~4자 + 님/씨
    // 블랙리스트로 "고객님"(고객+님), "선생님" 등 호칭 구분.
    // \b 제거: RE2의 \b 는 ASCII word-char 기준이라 한글 뒤에서 미작동.
    static const re2::RE2 PATTERN_NAME_HONORIFIC(
        R"(([가-힣]{2,4})\s*(?:님|씨))"
    );

    std::vector<SecureCastOcrBox> boxes;

    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const auto& line = lines[i];
        const std::string& rawText = line.text;

        const char *type = nullptr;

        // === STEP 2-2: 이메일은 문자 기반이므로 먼저 검사 ===
        {
            std::string emailMatch;
            if (re2::RE2::PartialMatch(rawText, PATTERN_EMAIL, &emailMatch) &&
                valid_email(emailMatch)) {
                type = "EMAIL";
            }
        }

        // === STEP 2-2-H: 이름 + 호칭 직접 탐지 (A-7) ===
        // "김철수님", "이민준씨" 처럼 호칭이 명시된 경우 고신뢰도 NAME 확정.
        // 성씨 시작 + 블랙리스트 통과 조건으로 "고객님", "선생님" 등 일반 호칭 배제.
        if (type == nullptr) {
            std::string honorName;
            if (RE2::PartialMatch(rawText, PATTERN_NAME_HONORIFIC, &honorName)) {
                const std::string h = extract_hangul_syllables_utf8(honorName);
                const int hc = count_hangul_syllables(h);
                if (hc >= 2 && hc <= 4 &&
                    starts_with_common_korean_surname(h) &&
                    NAME_BLOCKLIST.find(h) == NAME_BLOCKLIST.end()) {
                    type = "NAME";
                }
            }
        }

        // === STEP 2-2-A: 주소 탐지 (단일 라인) ===
        // 예: 주소:서울시, 서울시 강남구, 역삼동, 테헤란로 등
        if (type == nullptr && looks_like_korean_address_text(rawText)) {
            type = "ADDRESS";
        }

        // === STEP 2-2-A2: 주소 탐지 (멀티라인 결합) ===
        // OCR이 "서울" / "강남구 역삼로 123" 처럼 줄을 쪼갠 경우:
        // 인접한 다음 줄과 합쳐서 재검사 → 두 줄 모두 박스 추가 후 i+1 스킵.
        // 단, line i에 지역어 또는 주소 라벨이 없으면 스킵 (이름+숫자 오탐 방지).
        if (type == nullptr && i + 1 < static_cast<int>(lines.size())) {
            if (contains_any(rawText, {
                    "주소", "주소지", "거주지", "소재지", "사는곳", "배송지",
                    "서울", "부산", "대구", "인천", "광주", "대전", "울산", "세종",
                    "경기", "강원", "충북", "충남", "충청", "전북", "전남", "전라",
                    "경북", "경남", "경상", "제주"
                }) &&
                lines_visually_adjacent(lines[i], lines[i + 1])) {
                const std::string combined = rawText + " " + lines[i + 1].text;
                if (looks_like_korean_address_text(combined)) {
                    boxes.push_back(SecureCastOcrBox{
                        "ADDRESS", line.x, line.y, line.w, line.h});
                    boxes.push_back(SecureCastOcrBox{
                        "ADDRESS",
                        lines[i + 1].x, lines[i + 1].y,
                        lines[i + 1].w, lines[i + 1].h});
                    ++i;
                    continue;
                }
            }
        }

        // === STEP 2-3: 숫자형 개인정보 탐지 ===
        // normalize는 라인 전체가 아닌 숫자 cluster 단위로만 적용한다.
        if (type == nullptr) {
            // (f) 숫자 cluster 추출 → normalize → 각 cluster에 대해 패턴 검사
            re2::StringPiece input(rawText);
            std::string cluster;
            std::string normalizedLine; // cluster를 붙여 재조합한 보정 텍스트
            while (RE2::FindAndConsume(&input, PATTERN_NUMERIC_CLUSTER, &cluster)) {
                normalizedLine += normalize_numeric_candidate(cluster) + " ";
            }
            if (normalizedLine.empty())
                normalizedLine = rawText;

            // (d) RRN: 패턴 매칭 후 체크섬 검증
            {
                std::string d1, d2;
                if (RE2::PartialMatch(normalizedLine, PATTERN_RRN, &d1, &d2)) {
                    std::string digits13 = d1 + d2;
                    static const auto rrn_checksum = [](const std::string& d) -> bool {
                        if (d.size() != 13) return false;
                        const int month = (d[2]-'0')*10 + (d[3]-'0');
                        const int day   = (d[4]-'0')*10 + (d[5]-'0');
                        if (month < 1 || month > 12) return false;
                        if (day   < 1 || day   > 31) return false;
                        static const int w[12] = {2,3,4,5,6,7,8,9,2,3,4,5};
                        int sum = 0;
                        for (int i = 0; i < 12; ++i) sum += (d[i]-'0') * w[i];
                        int check = (11 - sum % 11) % 10;
                        return check == (d[12]-'0');
                    };
                    if (rrn_checksum(digits13))
                        type = "RRN";
                }
            }

            // (b) PHONE: \b 경계 + prefix 화이트리스트 패턴으로 탐지
            if (type == nullptr &&
                RE2::PartialMatch(normalizedLine, PATTERN_PHONE)) {
                type = "PHONE";
            }

            // CARD: 4-4-4-4 구조 + Luhn checksum + IIN 검증
            // Luhn만으로 무작위 16자리의 ~90%를, IIN으로 추가 ~50%를 배제한다.
            if (type == nullptr) {
                std::string g1, g2, g3, g4;
                if (RE2::PartialMatch(normalizedLine, PATTERN_CARD,
                                      &g1, &g2, &g3, &g4)) {
                    const std::string digits = g1 + g2 + g3 + g4;
                    if (luhn_check(digits) && valid_card_iin(digits))
                        type = "CARD";
                }
            }

            // (a) IP: 옥텟 범위 + 사설/예약 대역 제외 검증
            if (type == nullptr) {
                std::string oa, ob, oc, od;
                re2::StringPiece ipInput(normalizedLine);
                while (RE2::FindAndConsume(&ipInput, PATTERN_IP, &oa, &ob, &oc, &od)) {
                    int a = std::stoi(oa), b = std::stoi(ob),
                        c = std::stoi(oc), d = std::stoi(od);
                    // 옥텟 범위 검증
                    if (a > 255 || b > 255 || c > 255 || d > 255) continue;
                    // 사설/예약 대역 제외 (RFC 1918, 루프백, 링크로컬, 브로드캐스트)
                    if (a == 10) continue;                          // 10.0.0.0/8
                    if (a == 172 && b >= 16 && b <= 31) continue;  // 172.16.0.0/12
                    if (a == 192 && b == 168) continue;            // 192.168.0.0/16
                    if (a == 127) continue;                         // 루프백
                    if (a == 169 && b == 254) continue;            // 링크로컬
                    if (a == 0 || a == 255) continue;              // 0.x.x.x / 브로드캐스트
                    type = "IP";
                    break;
                }
            }

            // (e) ACCOUNT: 라벨이 동일 라인 또는 ±2 라인 이내에 있을 때만 탐지.
            // OCR 오인식 변형(게좌/계자/계조)도 허용.
            if (type == nullptr &&
                RE2::PartialMatch(normalizedLine, PATTERN_ACCOUNT)) {
                auto hasLabel = [](const std::string& t) {
                    // 정상 라벨
                    if (t.find("\xEA\xB3\x84\xEC\xA2\x8C") != std::string::npos) return true; // 계좌
                    if (t.find("\xEC\x98\x88\xEA\xB8\x88\xEC\xA3\xBC") != std::string::npos) return true; // 예금주
                    if (t.find("\xEC\x9E\x85\xEA\xB8\x88") != std::string::npos) return true; // 입금
                    if (t.find("\xEC\x86\xA1\xEA\xB8\x88") != std::string::npos) return true; // 송금
                    if (t.find("\xEC\x9D\x80\xED\x96\x89") != std::string::npos) return true; // 은행
                    if (t.find("Account") != std::string::npos) return true;
                    if (t.find("ACCT")    != std::string::npos) return true;
                    // OCR 오인식 변형
                    if (t.find("\xEA\xB2\x8C\xEC\xA2\x8C") != std::string::npos) return true; // 게좌
                    if (t.find("\xEA\xB3\x84\xEC\x9E\x90") != std::string::npos) return true; // 계자
                    if (t.find("\xEA\xB3\x84\xEC\xA1\xB0") != std::string::npos) return true; // 계조
                    return false;
                };
                bool hasAccountLabel = hasLabel(rawText);
                for (int d = 1; d <= 2 && !hasAccountLabel; ++d) {
                    if (i - d >= 0 && hasLabel(lines[i - d].text)) hasAccountLabel = true;
                    if (i + d < static_cast<int>(lines.size()) && hasLabel(lines[i + d].text)) hasAccountLabel = true;
                }
                if (hasAccountLabel)
                    type = "ACCOUNT";
            }
        }

        // === STEP 2-3-A: 이름 탐지 (라벨 필수) ===
        // 라벨이 같은 줄 또는 ±1 줄에 없으면 탐지하지 않는다 (오탐 방지).
        if (type == nullptr) {
            const bool sameLineLabel = has_name_label(rawText);
            const bool adjLabel     = has_adjacent_name_label(lines, i);

            if (sameLineLabel || adjLabel) {
                // 같은 줄 라벨("이름: 김철수")이면 라벨 이후 텍스트만 검사한다.
                // 인접 줄 라벨이면 현재 라인 전체를 검사한다.
                std::string candidateText = rawText;
                if (sameLineLabel) {
                    // has_name_label 에 포함된 라벨 전부 — 탈루 시 후보 텍스트가 라벨 포함 채로
                    // hangulCount 범위를 초과해 NONE 으로 빠지는 문제 방지.
                    static const char* const LABEL_STRS[] = {
                        "이름", "성명", "성함", "고객명", "사용자명",
                        "예금주", "수취인", "발신인", "수신인",
                        "보호자", "담당자", "신청인", "회원명", "환자명",
                        "Full Name", "First Name", "Last Name",
                        "name", "Name", "NAME"
                    };
                    for (const char* lbl : LABEL_STRS) {
                        const auto pos = rawText.find(lbl);
                        if (pos != std::string::npos) {
                            candidateText = rawText.substr(pos + std::strlen(lbl));
                            break;
                        }
                    }
                }

                const std::string hangulOnly = extract_hangul_syllables_utf8(candidateText);
                const int hangulCount = count_hangul_syllables(hangulOnly);

                // 한글 2~4자 + 성씨 시작 + 같은 줄 숫자 7자리 미만 + 블랙리스트 제외
                if (hangulCount >= 2 && hangulCount <= 4 &&
                    starts_with_common_korean_surname(hangulOnly) &&
                    count_ascii_digits(rawText) < 7 &&
                    NAME_BLOCKLIST.find(hangulOnly) == NAME_BLOCKLIST.end()) {
                    type = "NAME";
                }
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

