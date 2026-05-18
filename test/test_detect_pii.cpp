// =============================================================================
// test_detect_pii.cpp — SecureCastOcrEngine::detect_pii 단위 테스트
//
// 실행:
//   cmake --preset windows-x64 -DBUILD_TESTING=ON
//   cmake --build --preset windows-x64
//   ctest --preset windows-x64 --output-on-failure
//
// 또는 직접:
//   build_x64\RelWithDebInfo\test_pii.exe [corpus_path]
//   (인수 없으면 실행 파일 옆 pii_corpus.txt 를 찾는다)
// =============================================================================
// SC_ENABLE_TESTS 는 CMakeLists.txt의 target_compile_definitions로 주입된다.
#include "../src/ocr-engine.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// ─── 코퍼스 파싱 ────────────────────────────────────────────────────────────

struct CorpusCase {
    std::string label;               // 예상 타입 or "NONE"
    std::vector<SecureCastOcrLine> lines;
};

static std::vector<CorpusCase> load_corpus(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[corpus] cannot open: " << path << "\n";
        return {};
    }

    std::vector<CorpusCase> cases;
    std::string rawLine;
    int lineNo = 0;

    while (std::getline(f, rawLine)) {
        ++lineNo;
        // Windows \r\n 대비 후행 CR 제거
        if (!rawLine.empty() && rawLine.back() == '\r')
            rawLine.pop_back();
        // 빈 줄 / 주석
        if (rawLine.empty() || rawLine[0] == '#') continue;

        // TAB 분리: LABEL<TAB>LINES
        const auto tab = rawLine.find('\t');
        if (tab == std::string::npos) continue;

        CorpusCase c;
        c.label = rawLine.substr(0, tab);
        // 앞뒤 공백 제거
        while (!c.label.empty() && c.label.back() == ' ') c.label.pop_back();

        const std::string linesPart = rawLine.substr(tab + 1);

        // | 로 다중 라인 분리
        std::istringstream ss(linesPart);
        std::string seg;
        float y = 0.0f;
        while (std::getline(ss, seg, '|')) {
            SecureCastOcrLine l{};
            l.text = seg;
            l.x = 0; l.y = y; l.w = 800; l.h = 24;
            y += 28.0f;
            c.lines.push_back(l);
        }

        cases.push_back(std::move(c));
    }

    return cases;
}

// ─── 테스트 실행 ─────────────────────────────────────────────────────────────

struct Stats {
    int total = 0;
    int tp = 0; // 맞게 탐지
    int tn = 0; // 맞게 미탐지 (NONE)
    int fp = 0; // 오탐
    int fn = 0; // 미탐
    std::vector<std::string> failures;
};

static void run_corpus(SecureCastOcrEngine& engine,
                       const std::vector<CorpusCase>& cases,
                       Stats& stats)
{
    for (const auto& c : cases) {
        ++stats.total;

        auto boxes = engine.detect_pii_for_test(c.lines);

        const bool expectedNone = (c.label == "NONE");
        const bool gotSomething = !boxes.empty();

        // 탐지된 타입 목록 (쉼표 구분 문자열)
        std::string gotTypes;
        for (const auto& b : boxes) {
            if (!gotTypes.empty()) gotTypes += ',';
            gotTypes += b.type;
        }
        if (gotTypes.empty()) gotTypes = "NONE";

        // 결과 판정: expected 타입이 탐지된 박스 중 하나라도 있으면 TP
        bool pass = false;
        if (expectedNone && !gotSomething) {
            ++stats.tn;
            pass = true;
        } else if (!expectedNone && gotSomething) {
            for (const auto& b : boxes) {
                if (std::string(b.type) == c.label) {
                    ++stats.tp;
                    pass = true;
                    break;
                }
            }
            if (!pass) ++stats.fn;
        } else if (expectedNone && gotSomething) {
            ++stats.fp;
        } else {
            ++stats.fn;
        }

        if (!pass) {
            std::ostringstream msg;
            msg << "  FAIL expected=" << c.label
                << " got=" << gotTypes
                << "  text=[";
            for (size_t i = 0; i < c.lines.size(); ++i) {
                if (i) msg << " | ";
                msg << c.lines[i].text;
            }
            msg << "]";
            stats.failures.push_back(msg.str());
        }
    }
}

// ─── 지표 출력 ───────────────────────────────────────────────────────────────

static void print_stats(const Stats& s)
{
    const int detected = s.tp + s.fp;  // 탐지됐다고 말한 것
    const int relevant = s.tp + s.fn;  // 실제 PII 케이스

    const double precision = detected  > 0 ? 100.0 * s.tp / detected : 0.0;
    const double recall    = relevant  > 0 ? 100.0 * s.tp / relevant : 0.0;
    const double f1 = (precision + recall > 0)
        ? 2.0 * precision * recall / (precision + recall)
        : 0.0;
    const double accuracy = s.total > 0
        ? 100.0 * (s.tp + s.tn) / s.total
        : 0.0;

    std::cout << "\n========== SecureCast PII Detection Test ==========\n";
    std::cout << "  Cases  : " << s.total  << "\n";
    std::cout << "  TP     : " << s.tp     << "\n";
    std::cout << "  TN     : " << s.tn     << "\n";
    std::cout << "  FP     : " << s.fp     << "  (오탐)\n";
    std::cout << "  FN     : " << s.fn     << "  (미탐)\n";
    std::cout << "---------------------------------------------------\n";
    std::cout << "  Precision : " << precision << " %\n";
    std::cout << "  Recall    : " << recall    << " %\n";
    std::cout << "  F1        : " << f1        << " %\n";
    std::cout << "  Accuracy  : " << accuracy  << " %\n";
    std::cout << "===================================================\n";

    if (!s.failures.empty()) {
        std::cout << "\n[FAILURES]\n";
        for (const auto& f : s.failures)
            std::cout << f << "\n";
    }

    // PR 회귀 기준: F1 < 90% 이면 빌드 실패로 처리
    const double F1_THRESHOLD = 90.0;
    if (f1 < F1_THRESHOLD) {
        std::cout << "\nFAIL: F1 " << f1 << "% < " << F1_THRESHOLD << "% threshold\n";
        std::exit(1);
    }

    std::cout << "\nPASS\n";
}

// ─── main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    // 코퍼스 경로: 인수 > 실행파일 옆
    std::string corpusPath;
    if (argc >= 2) {
        corpusPath = argv[1];
    } else {
        // 실행파일 경로에서 디렉터리 추출 후 pii_corpus.txt 붙이기
        std::string exePath = argv[0];
        auto slash = exePath.find_last_of("/\\");
        std::string dir = (slash != std::string::npos)
            ? exePath.substr(0, slash + 1)
            : "";
        corpusPath = dir + "pii_corpus.txt";
    }

    auto cases = load_corpus(corpusPath);
    if (cases.empty()) {
        std::cerr << "No test cases loaded from: " << corpusPath << "\n";
        return 1;
    }
    std::cout << "Loaded " << cases.size() << " cases from " << corpusPath << "\n";

    // detect_pii는 WinRT OCR 없이 동작한다 (텍스트 직접 주입).
    // init()은 호출하지 않는다 — impl_->engine이 null이어도 detect_pii는 독립 동작.
    SecureCastOcrEngine engine;

    auto t0 = std::chrono::steady_clock::now();

    Stats stats;
    run_corpus(engine, cases, stats);

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "Elapsed: " << ms << " ms\n";

    print_stats(stats);
    return 0;
}
