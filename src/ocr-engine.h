#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ============================================================
// Role B — On-device AI engine
// 담당 기능: 위험 키워드 블러
// 구성: OCR + Google RE2 정규식 + PII 탐지 + Dirty Rect Skip
// ============================================================

// === OCR / PII 탐지 결과 Bounding Box ===
struct SecureCastOcrBox {
  // "RRN", "PHONE", "EMAIL", "CARD", "IP", "ACCOUNT", "NAME", "ADDRESS"
  const char *type;
  float x;
  float y;
  float w;
  float h;
};

// === Windows.Media.Ocr 결과 한 줄 정보 ===
struct SecureCastOcrLine {
  std::string text;
  float x;
  float y;
  float w;
  float h;
};

// analyze_bgra_frame()의 확장 반환 — 셀프힐링용 진단 정보 포함.
// fullRecognitionRan==true 일 때만 effectiveLineCount==0 판정으로 셀프힐링
// 에스컬레이션이 발동한다 (L1/L2 캐시 히트 경로의 오발동 차단).
struct OcrAnalysisResult {
  std::vector<SecureCastOcrBox> boxes; // PII로 매칭된 박스만
  // 이번 결과의 effective text lines:
  //   full OCR  → recognize_text() 결과 라인 수
  //   L2 partial → cached + 새로 인식한 라인 수 합
  //   L1 hit    → 캐시 라인 수 (변경 없음)
  int effectiveLineCount = 0;
  // **full-frame** recognize_text()가 실제로 실행되었을 때만 true.
  // L2 partial(crop OCR)이나 L1 hit(완전 캐시)에서는 false.
  bool fullRecognitionRan = false;
};

struct ProfileEMA {
  double bgra_ms = 0, recog_ms = 0, multipass_ms = 0;
  double dhash_ms = 0, detect_ms = 0, total_ms = 0;
  uint32_t sample_count = 0;
  std::chrono::steady_clock::time_point last_log;

  // 프레임 내 누적을 위한 임시 저장소
  struct {
    double bgra = 0, recog = 0, multipass = 0, dhash = 0, detect = 0;
  } acc;
};

class SecureCastOcrEngine {
public:
  SecureCastOcrEngine();
  ~SecureCastOcrEngine();

  // === OCR 엔진 초기화 ===
  // 주의: render thread에서 호출하지 말고 OCR worker thread에서 호출하는 것이
  // 안전하다.
  bool init();

#ifdef SC_ENABLE_TESTS
  // detect_pii를 단위 테스트에서 직접 호출하기 위한 진입점.
  // 프로덕션 빌드에는 포함되지 않는다.
  std::vector<SecureCastOcrBox>
  detect_pii_for_test(const std::vector<SecureCastOcrLine> &lines) {
    return detect_pii(lines);
  }
#endif

  // === OCR 사용 가능 여부 ===
  bool available() const;

  // 2-C: 직전 사이클 OCR 라인들의 평균 높이(px). 첫 사이클은 0 반환.
  // OCR worker 단독 접근이므로 동기화 불필요.
  float averageLineHeight() const { return avgLineHeight_; }

  // === 진행 중인 RecognizeAsync 취소 ===
  // stop_ocr_worker가 join() 전에 호출하여 종료 지연을 방지한다.
  // 완료된 op에 Cancel()은 no-op이므로 경쟁 조건 없이 안전하다.
  void cancel_current();

  // dHash 캐시 무효화 — 소스/창 전환 시 호출하여 오탐 방지.
  // OCR worker thread에서만 호출해야 한다.
  void clearDHashCache();

  // ========================================================
  // 전체 파이프라인 실행
  // Dirty Skip → OCR → RE2 PII 탐지
  //
  // 주의: 이 함수는 RecognizeAsync(...).get()을 내부에서 호출하므로
  // video_render thread에서 직접 동기 호출하면 프레임 드랍이 발생할 수 있다.
  // render thread가 아니라 별도 OCR worker thread에서 호출해야 한다.
  // ========================================================
  OcrAnalysisResult analyze_bgra_frame(const uint8_t *pixels, int width,
                                       int height, int stride);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  bool available_ = false;
  mutable ProfileEMA profile_;

  // ========================================================
  // Dirty Skip — dHash 기반 2-Level 캐시
  //
  // L1: ROI dHash — PII 박스 영역만 해시. 시계·커서 등 배경 노이즈 무시.
  //     hamming_distance ≤ 2 → 동일 프레임 간주 → OCR 생략.
  //
  // L2: 라인별 dHash — 변경된 라인만 crop OCR 재인식.
  //     변경 없는 라인은 이전 박스 재사용 (OCR 비용 1/5~1/10).
  //
  // lastFrameWidth_/Height_: 해상도 변경 시 좌표 오탐 방지.
  // ========================================================
  uint64_t lastRoiDhash_ = 0;
  bool hasLastRoiDhash_ = false;
  int lastFrameWidth_ = 0;
  int lastFrameHeight_ = 0;
  std::vector<SecureCastOcrBox> lastBoxes_;

  struct LineDHashCache {
    int x, y, w, h;
    uint64_t dhash;
    bool hasBox;
    SecureCastOcrBox box;
    std::string
        text; // 이전 OCR 라인 텍스트 — L2 partial 시 인접줄 컨텍스트 보존용
  };
  std::vector<LineDHashCache> lastLineDhashes_;
  int consecutiveSkips_ = 0;
  // 2-C: 직전 full OCR 사이클의 라인 평균 높이. 적응형 스케일 계산에 사용.
  float avgLineHeight_ = 0.0f;
  // 연속 L1 히트 허용 횟수: 2 = 실제 1회 스킵(++후 1<2=true, 이후 full OCR).
  // OCR 워커 주기 포함 시 최대 ~250ms stale.
  static constexpr int kMaxConsecutiveSkips = 2;

  // Full OCR이 0개 박스를 반환했을 때 lastBoxes_ 캐시를 보존한 연속 횟수.
  // 같은 화면(dHash 동일)에서 이전엔 PII를 찾았다면 Windows OCR 비결정성으로
  // 간주하고 kEmptyOcrTolerance회까지 캐시를 유지한다 — 정적 화면 깜빡임 차단.
  // dHash 변경·해상도 변경 시 0으로 리셋된다.
  int consecutiveEmptyOcr_ = 0;
  // Windows.Media.Ocr이 같은 정적 이미지에서 라인 수/패턴 매칭 결과를
  // 변동시키는 비결정성이 있어, 3사이클(~900ms)로는 box 1↔0 토글이 자주
  // 발생. 5사이클(~1.5s)로 상향하여 burst를 흡수.
  // STALE_OCR_FRAMES(25프레임≈833ms)보다 길어 ghost가 송출될 위험은 없다.
  static constexpr int kEmptyOcrTolerance = 5;
  // 캐시 보존 로그 throttle 카운터(인스턴스별). 30회마다 1회 출력.
  int preserveLogCounter_ = 0;

  // dHash: 9×8 샘플 격자 → 행별 인접 밝기 비교(8비트 × 8행) → 64비트
  uint64_t compute_dhash_region(const uint8_t *px, int stride, int x, int y,
                                int w, int h) const;
  // P0-C: 박스 밖 새 PII를 놓치지 않도록 항상 전체 프레임 coarse dHash 사용.
  uint64_t compute_roi_dhash(const uint8_t *px, int stride, int width,
                             int height) const;
  // 해밍 거리 (XOR 후 set bit 수)
  static int hamming_distance(uint64_t a, uint64_t b);
  // L2용: crop 영역만 OCR 실행, 반환 좌표는 원본 프레임 기준
  std::vector<SecureCastOcrLine> recognize_text_crop(const uint8_t *px,
                                                     int width, int height,
                                                     int stride, int cx, int cy,
                                                     int cw, int ch);

  // === Windows.Media.Ocr — 텍스트와 좌표 추출 ===
  std::vector<SecureCastOcrLine>
  recognize_text(const uint8_t *pixels, int width, int height, int stride);

  // === Google RE2 — 정규표현식 기반 PII 탐지 ===
  std::vector<SecureCastOcrBox>
  detect_pii(const std::vector<SecureCastOcrLine> &lines);

  // P3: 소형 글씨 다중 패스 OCR
  // lines에서 높이 < 20px인 라인을 최대 MAX_PASSES개까지 2× 업스케일 재OCR.
  // detect_pii에 넘기기 전에 호출한다. L2 캐시 갱신에는 원본 lines를 사용한다.
  std::vector<SecureCastOcrLine>
  multipass_small_text(const std::vector<SecureCastOcrLine> &lines,
                       const uint8_t *pixels, int width, int height,
                       int stride);

  std::vector<SecureCastOcrLine>
  split_batch_multipass(const std::vector<SecureCastOcrLine> &lines,
                        const std::vector<int> &smallIndices,
                        const uint8_t *pixels, int width, int height,
                        int stride);

  // 2× nearest-neighbor 업스케일된 batch가 이 픽셀 수를 넘으면 단일 OCR이
  // 위험하다고 판단해 분할/스킵한다. Windows.Media.Ocr이 ~16MP에서 stall
  // 빈도가 급증하는 경험치 기반.
  static constexpr size_t SC_MAX_OCR_BATCH_PX = 8000000u;
  // 소형 라인이 Union BBox 내에 차지하는 비율이 이 값 미만이면 sparse 레이아웃
  // 으로 간주, Y정렬 N분할로 폴백.
  static constexpr float SC_SPARSE_THRESHOLD = 0.3f;

  // === OCR 오류 보정: O/o → 0, I/l → 1 ===
  std::string normalize_numeric_candidate(const std::string &text);
};
