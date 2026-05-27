// =============================================================================
// securecast-filter.h — 필터 인스턴스의 공유 타입 정의
//
// 역할:
//   하나의 OBS 소스에 부착된 SecureCast 필터 인스턴스가 들고 다니는 상태를
//   정의. 이 헤더는 Role A/B/C 코드 모두가 include해서 같은 구조체를 본다.
//
// 어디서 사용:
//   - securecast-filter.cpp: 인스턴스 생성/소멸/tick/render 콜백에서 직접 사용.
//   - window_tracker.cpp: trackerAccumulator를 받아 throttle 처리 (Role A).
//   - 향후 Role B (AI/OCR) / Role C (N-Frame Delay)도 이 구조체에 필드 추가
//   예정.
// =============================================================================

#pragma once

// ----------------------------------------------------
// C++ Standard Library Headers (MUST be included before OBS headers)
// ----------------------------------------------------
#include <array>
#include <string>
#include <unordered_set>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdint.h>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "overlay-window.h"
#include "selection-overlay.h"
#endif
#include "securecast-types.h"
#include "visual-tracker.h"

// ocr-engine.h는 이 헤더에서 직접 include하지 않는다.
// WinRT/OCR 관련 의존성이 다른 translation unit으로 전파되는 것을 막기 위해
// SecureCastOcrEngine은 forward declaration + unique_ptr로 보관한다.
class SecureCastOcrEngine;

// ----------------------------------------------------
// OBS Headers
// ----------------------------------------------------
#include <obs-module.h>
#include <obs.h>

// ----------------------------------------------------
// Platform Headers
// ----------------------------------------------------
#ifdef _WIN32
#include "win_event_listener.h"
#include "window_tracker.h"
#endif

// ----------------------------------------------------
// Helpers & Macros
// ----------------------------------------------------

// ----------------------------------------------------
// Global Configurations (Config)
// ----------------------------------------------------
// 컴파일 타임 상수. 런타임에 바꿀 일이 없으므로 constexpr로 둔다.
constexpr int SC_MAX_BLUR_RECTS =
    32; // 한 프레임에 동시에 마스킹 가능한 최대 영역 수
constexpr int SC_RING_BUFFER_SLOTS =
    60; // Bounded Exposure: OCR 최대 레이턴시(≈1000ms) 대비 여유 확보를 위해 60슬롯으로 증가 (1초 지연)

// ----------------------------------------------------
// Shared Types (Types) - Moved to securecast-types.h
// ----------------------------------------------------

// AI Thread에서 만든 마스킹 결과 → Render Thread로 넘기는 락프리 버퍼 팩(전달
// 페이로드)
struct MaskPayload {
  BlurRect rects[SC_MAX_BLUR_RECTS];
  int rectCount;
};

#ifdef _WIN32
// 창이 사라진 후 ring buffer에 남은 N프레임 동안 마스킹을 유지하는 잔영 항목.
// fromPreview: 작업표시줄 hover/peek 가드(마우스가 작업표시줄→썸네일 영역으로
//   이동 시 발동)로 등록된 항목. 렌더 시 minimize cutoff(endNs) 대신
//   filter->previewActiveNs로 slot.timestamp 컷오프 — peek이 끝난 후 캡처된
//   슬롯에는 그려지지 않아 빈 영역에 잔상 박스가 남지 않음.
struct LingeringWindow {
  TrackedWindow window; // 마지막으로 알려진 창 정보 (bounds 포함)
  int ticksRemaining;   // SC_RING_BUFFER_SLOTS에서 매 tick 카운트다운
  bool fromPreview;     // taskbar 미리보기 가드용 (cutoff 분기)
};
constexpr int SC_MAX_LINGERING = SC_MAX_TRACKED_WINDOWS * 2;
#endif

// ----------------------------------------------------
// [Role C] N-Frame Ring Buffer
//
// 송출 지연(Bounded Exposure) 구현의 핵심 자료구조.
// SC_RING_BUFFER_SLOTS 개의 슬롯(텍스처 핸들)을
// 순환배열로 관리하여 렌더 스레드가 블로킹 없이
// N프레임 전의 "안전한" 프레임을 꺼낼 수 있게 한다.
//
// 슬롯 상태 다이어그램:
//   HEAD(쓰기) → [0][1][2][3][4] → TAIL(읽기)
//   Render Thread: push → HEAD++
//   Render Thread: pop  ← TAIL (HEAD - N 위치)
// ----------------------------------------------------
class FrameRingBuffer {
public:
  // 각 슬롯이 보유하는 데이터: gs_texrender + 타임스탬프
  struct Slot {
    gs_texrender_t *texrender = nullptr; // OBS 안전 렌더 타겟 관리자
    uint64_t timestamp = 0;
    uint64_t frameId = 0;
    uint64_t dependentOcrFrameId = 0; // 이 프레임 송출 전 완료되어야 할 대표 OCR 프레임 ID
#ifdef _WIN32
    // 이 프레임이 캡처된 시점의 창 좌표 스냅샷.
    // 렌더 시 출력 슬롯의 windowSnapshot을 사용해야 프레임 내용과 마스크 위치가
    // 동기화됨.
    TrackedWindowList windowSnapshot{};
    // 이 프레임 시점의 알림 영역 블러 rect (width 0 = 없음). windowSnapshot과
    // 같은 이유로 슬롯에 저장 — 지연 송출 프레임과 동기화하기 위함.
    BlurRect notifRect{};
#endif
    // [Window anchor v2] 이 프레임이 push될 시점의 트래커 박스 스냅샷
    // (tracker 좌표 공간; render에서 trackerCoordScale_로 환산).
    // 송출 프레임과 같이 지연되므로 텍스트와 블러가 동시에 움직임.
    std::vector<VtOcrBox> trackerSnapshot;

    // gs_texrender에서 결과 텍스처를 꺼내는 헬퍼
    gs_texture_t *getTexture() const {
      return texrender ? gs_texrender_get_texture(texrender) : nullptr;
    }
    bool isReady() const { return getTexture() != nullptr; }
  };

  FrameRingBuffer() = default;
  ~FrameRingBuffer() = default;

  bool initialize(uint32_t width, uint32_t height);
  void destroy();

  // gs_texrender_begin/end를 사용하여 안전하게 프레임을 캡처.
  // wlist: 이 프레임 캡처 시점의 창 좌표 스냅샷 (null 허용).
  // trackerSnap: 이 프레임 시점의 트래커 박스 스냅샷 (null 허용; 트래커 좌표 공간).
#ifdef _WIN32
  void pushFrame(uint64_t timestamp, obs_source_t *filter_context,
                 const TrackedWindowList *wlist,
                 const std::vector<VtOcrBox> *trackerSnap,
                 uint64_t dependentOcrFrameId);
#else
  void pushFrame(uint64_t timestamp, obs_source_t *filter_context,
                 const std::vector<VtOcrBox> *trackerSnap,
                 uint64_t dependentOcrFrameId);
#endif

#ifdef _WIN32
  // 최근 maxAgeNs(ns) 이내에 캡처된 슬롯들의 windowSnapshot에 win을 소급
  // 추가한다. 새 블랙리스트 창은 감지 지연 동안 캡처된 슬롯의 스냅샷에서
  // 빠져 있어, 그 슬롯이 송출될 때 마스킹 없이 노출된다. 이를 보정한다.
  void backfillRecentSnapshots(const TrackedWindow &win, uint64_t nowNs,
                               uint64_t maxAgeNs);
  // 최근 maxAgeNs(ns) 이내에 캡처된 슬롯들의 notifRect를 rect로 설정한다.
  // 알림 블러를 windowSnapshot과 동일하게 지연 송출 프레임과 동기화한다.
  void backfillRecentNotifRect(const BlurRect &rect, uint64_t nowNs,
                               uint64_t maxAgeNs);
#endif

  const Slot *peekDelayedSlot() const;
  // framesBack=SC_RING_BUFFER_SLOTS이면 peekDelayedSlot()과 동일.
  // framesBack=SC_RING_BUFFER_SLOTS-1이면 한 프레임 더 최신 슬롯 (빠른 이동
  // 합집합용).
  const Slot *peekSlotAtOffset(int framesBack) const;

  // [Bounded Exposure backfill]
  // OCR worker가 새 frameId를 claim한 직후 호출. 링 내 모든 슬롯 중 frameId가
  // newOcrFrameId 미만이고 dependentOcrFrameId가 그보다 작은 슬롯의 dependent를
  // newOcrFrameId로 갱신한다. 새 PII가 등장한 후 push된 슬롯들이 "이 슬롯
  // 이후의 OCR"이 완료될 때까지 unsafe로 게이트되도록 강제한다.
  //
  // 호출 thread: render thread 단독 (push/peek와 동일). 외부 동기화 불필요.
  void backfillDependentOcr(uint64_t newOcrFrameId);

  bool isInitialized() const { return m_initialized; }
  uint32_t getWidth() const { return m_width; }
  uint32_t getHeight() const { return m_height; }

private:
  std::array<Slot, SC_RING_BUFFER_SLOTS> m_slots{};
  int m_head = 0;
  int m_frameCount = 0;
  uint64_t m_nextFrameId = 1;
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  bool m_initialized = false;
};

// ----------------------------------------------------
// Core Filter Context
// ----------------------------------------------------
struct SecureCastFilter {
  SecureCastFilter();
  ~SecureCastFilter();

  SecureCastFilter(const SecureCastFilter &) = delete;
  SecureCastFilter &operator=(const SecureCastFilter &) = delete;

  obs_source_t *context = nullptr; // OBS 필터 컨텍스트 포인터

  // UI 및 운영 토글 상태
  bool isActive = true; // 필터 활성화 여부
  std::atomic<bool> isGameMode{
      false}; // CPU 임계값 기반 자동 전환 (render/tick 크로스 스레드)

  // ----- [Game Mode 세션 화이트리스트] -----
  // 게임 모드 ON 동안: OCR 중단 + foreground 변화 시 게임 외 모든 앱 자동 블러
  // + dialog로 사용자 허용 받음. 사용자 OK → 세션 화이트리스트 추가 → 그 앱은
  // 이 세션 동안 블러 안 함. 게임 모드 OFF 시 화이트리스트 폐기.
  std::mutex gameModeMutex;
  // 게임 모드 진입 시 캡처한 게임 프로세스 exe (이 exe는 블러 안 함)
  std::wstring gameModeGameExe;
  // 사용자 허용 받은 exe 목록 (lower-case)
  std::unordered_set<std::wstring> gameModeWhitelist;
  // dialog 중복 방지: dialog 중이거나 응답받은 exe (lower-case)
  std::unordered_set<std::wstring> gameModeDialogPromptedExes;
  // 직전 폴링 시 foreground HWND (변화 감지용)
  std::atomic<void *> gameModeLastFg{nullptr};
  // 설정: 블랙리스트 exe도 dialog로 물어볼지 여부 (기본 false = 항상 블러, 안 물음)
  std::atomic<bool> gameModeAskForBlacklist{false};
  SecurityState currentState =
      SecurityState::SAFE; // 현재 보안 등급 (SAFE/PARTIAL/RISK)

  // ----- [Role C 담당: 렌더링 파이프라인 (N-Frame Ring Buffer)] -----
  FrameRingBuffer
      ringBuffer; // Bounded Exposure(송출 지연) 구현용 N-프레임 텍스처 버퍼

#ifdef _WIN32
  OverlayWindow
      overlay; // [Role D] 스트리머 전용 보안 상태 HUD (OBS 캡처에서 제외됨)
#endif

  // ----- [Role D] UI 설정 -----
  mutable std::mutex
      settingsMutex; // GUI 스레드(update)와 렌더 스레드 간 data race 방지
  std::string blacklistApps = ""; // 줄바꿈 구분 앱 이름 목록

  // [C2-3 수정] 함수-scope static → 멤버 변수로 이동 (다중 필터 인스턴스 간
  // 공유 방지)
  int logScanThrottle =
      0; // 블랙리스트 윈도우 스캔 로그 주기 카운터 (10틱 = 1.5초 주기)

  // ----- [Role A 담당: 윈도우 추적 및 블랙리스트] -----
  float trackerAccumulator =
      0.0f; // 윈도우 스캔 틱 조절(0.15초 단위)용 시간 누산기
  gs_effect_t *blurEffect = nullptr; // 컴파일된 HLSL 셰이더
  gs_texrender_t *lastSafeRender_ =
      nullptr; // 마스크까지 합성된 마지막 안전 출력 프레임
  bool lastSafeReady_ = false;
  uint32_t lastSafeW_ = 0;
  uint32_t lastSafeH_ = 0;
  std::mutex blacklistMutex;   // video_tick(비디오)과 video_render(렌더) 간의
                               // 동시 접근을 막는 뮤텍스
  MaskPayload blacklistMask{}; // [우선순위 1] Role A가 추적한 블랙리스트 앱
                               // 좌표 (AI 처리 전에 최상단에 덮어씌움)
#ifdef _WIN32
  WinEventListener winListener;
  TrackedWindowList windowList{};        // 현재 추적 중인 창 목록
  TrackedWindowList captureWindowList{}; // pushFrame 스냅샷: 직전 프레임 DWM
                                         // 좌표 (캡처 레이턴시 보정)
  TrackedWindowList prevWindowList{};    // lingering 감지용 직전 스캔 결과
  TrackedWindowList recentlySeenList{};  // 과거에 추적했던 창 목록 (quick
                                         // restore용, 닫힐 때까지 유지)
  TrackedWindowList prevPushedWindowList{}; // 직전 프레임에 push된 스냅샷
                                            // (새 창 소급 보정 비교용)
  LingeringWindow lingeringWindows[SC_MAX_LINGERING]{};
  int lingeringCount = 0;
  // 작업표시줄 hover 미리보기가 마지막으로 보였던 OBS 시각(나노초).
  // fromPreview=true lingering의 컷오프 기준 — slot.timestamp가 이 값보다
  // 크면(=preview가 끝난 뒤 캡처된 프레임) 그리지 않아 잔상 방지.
  uint64_t previewActiveNs = 0;
  // 마지막으로 마우스가 썸네일 영역(작업표시줄 바로 위 ~400px 띠) 위에
  // 있었던 시각. peek 트리거 hysteresis용. mouse off 시에도 잠시 active 유지해
  // 부드러운 전환.
  uint64_t lastInThumbnailZoneTick = 0;
  // 마지막으로 마우스가 작업표시줄 본체에 있었던 시각. peek 트리거의 사전조건:
  // 썸네일이 실제로 떠 있다는 것은 직전에 마우스가 작업표시줄에 있었다는 뜻.
  // 이 없이 zone만으로 판정하면 단순히 화면 하단을 지나가도 발동돼버림.
  uint64_t lastOverTaskbarTick = 0;
  // 마지막으로 마우스가 블랙리스트/비-블랙리스트 작업표시줄 버튼 위에 있었던 시각.
  // 두 tick 중 큰(=최근) 쪽이 현재 hover 의도로 간주되며, hysteresis(1.5s) 안의
  // 신호만 유효. previewActive 동안에는 매 frame refresh되어 미리보기를 보는 동안
  // 의도가 만료되지 않는다(=메모장 peek 동안 카톡 블러가 끌려오는 회귀 차단).
  uint64_t lastHoverBlacklistTick = 0;
  uint64_t lastHoverNotBlacklistTick = 0;
  // 마지막으로 BL hover가 인식된 시점의 매칭 exe(예: "KakaoTalk.exe"). lingering
  // 등록 시 alive BL 중 이 exe와 일치하는 인스턴스만 통과시켜, 카톡 hover로 Discord
  // 까지 가려지는 회귀를 막는다. blRecent가 만료되면 stale 방지를 위해 무시한다.
  wchar_t lastHoverExe[64] = {0};

  // ----- [Game Mode] CPU 사용률 기반 자동 전환 -----
  float cpuSampleAccumulator = 0.0f; // 1초 샘플링 누산기
  float cpuUsage = 0.0f;             // 최근 측정 시스템 CPU 사용률 (0~100)
  float gameModeEntryTimer = 0.0f;   // ≥40% 지속 시간 누산 (3초 도달 시 진입)
  float gameModeExitTimer = 0.0f;    // <30% 지속 시간 누산 (10초 도달 시 해제)
  FILETIME prevIdleTime = {};        // GetSystemTimes 이전 샘플
  FILETIME prevKernelTime = {};
  FILETIME prevUserTime = {};
#endif

  // destroy 진입 즉시 true — 진행 중인 핫키 콜백이 해제된 멤버에 접근하지
  // 못하도록
  std::atomic<bool> isDestroying{false};

  // ----- [Panic Button] Ctrl+Shift+F12 -----
  std::atomic<bool> panicMode{false};
  obs_hotkey_id panicHotkeyId = OBS_INVALID_HOTKEY_ID;

#ifdef _WIN32
  // ----- [Role D] 수동 드래그 블러 선택 오버레이 -----
  SelectionOverlay selectionOverlay;
  obs_hotkey_id selectHotkeyId = OBS_INVALID_HOTKEY_ID;
#endif

  // ----- [Role D] 알림 영역 자동 블러 -----
  // 매 스캔 "현재 보이는" 토스트들의 union을 notifBlurRect에 반영한다.
  // 토스트가 사라지면 union이 즉시 줄어든다. 송출 동기화·지연 노출 방지는
  // video_render가 매 프레임 슬롯 notifRect에 기록하는 방식으로 처리한다.
  bool notifBlurActive = false;
  BlurRect notifBlurRect{};          // 현재+최근 스캔 토스트 union (화면 좌표)
  // 직전 N스캔의 토스트 union 기록. 현재 스캔과 합쳐, 토스트가 사라진 뒤에도
  // 블러가 N스캔만큼 더 유지돼 송출 화면에서 "팝업 먼저, 블러 그다음"이 되고
  // 스택 재배열 슬라이드 중 노출도 막는다.
  static constexpr int SC_NOTIF_LINGER_SCANS = 3; // 0.1초 스캔 × 3 ≈ 0.3초
  BlurRect notifScanHist[SC_NOTIF_LINGER_SCANS]{};
  float notifScanAccumulator = 0.0f; // 토스트 탐지 throttle 누산기

  // ----- [Role D] 수동 드래그 블러 -----
  // OBS 소스 프리뷰에서 좌클릭 드래그로 영역 지정 → 영구 블러.
  // 우클릭 또는 Properties의 "Clear" 버튼으로 전체 초기화.
  // settingsMutex로 UI 스레드(mouse 콜백) ↔ Render 스레드(video_render) 보호.
  // sc_manual_rects 키로 OBS 씬 컬렉션에 자동 저장/로드됨.
  static constexpr int SC_MAX_MANUAL_RECTS = 8;
  MaskPayload manualBlurMask{};

  bool dragActive = false; // 드래그 진행 중
  int32_t dragStartX = 0;
  int32_t dragStartY = 0;
  int32_t dragCurX = 0;
  int32_t dragCurY = 0;

  // 모니터→소스 좌표 변환용 캐시 (video_render에서 갱신, 원자적 접근)
  std::atomic<uint32_t> lastSourceW{0};
  std::atomic<uint32_t> lastSourceH{0};

  // ----- [Role B] Visual Tracker -----
  // OCR("what": ~250ms) 과 Tracker("where": render rate) 분리.
  // register_or_update() → render thread에서 OCR 결과 소비 시 호출
  // update_all_gray()    → tracker thread에서 30Hz로 호출
  // active_boxes()       → 매 render 프레임 블러 좌표 조회
  VisualTrackerManager trackerMgr;

  // ----- [Tier 1] GPU Grayscale Readback for 30Hz Tracker -----
  // 렌더 스레드에서 full BGRA readback(8MB) 대신:
  //   GPU: 전체 프레임 → R8 gray 셰이더 → trackerGrayRender_
  //   Stage: trackerGrayStage_ → map → 2MB gray (1 byte/pixel, 1080p 기준)
  // BGRA readback은 ~4fps OCR 경로에서만 발생.
  gs_effect_t *trackerGrayEffect_ = nullptr; // downsample.effect GrayDownsample
  gs_texrender_t *trackerGrayRender_ =
      nullptr;                                 // full-res R8 gray render target
  gs_stagesurf_t *trackerGrayStage_ = nullptr; // staging for CPU readback
  uint32_t trackerGrayW_ = 0;
  uint32_t trackerGrayH_ = 0;

  // ----- [Role B] OCR 엔진 -----
  // OCR 엔진은 render thread가 아니라 OCR worker thread 내부에서 init()한다.
  // forward declaration을 위해 unique_ptr로 보관하여 ocr-engine.h 의존성을
  // 분리한다.
  std::unique_ptr<SecureCastOcrEngine> ocrEngine;

  // ----- [Role B/C] OCR용 GPU readback 재사용 리소스 -----
  // 매 OCR마다 gs_stagesurface_create/destroy를 반복하지 않기 위해 보관한다.
  gs_stagesurf_t *ocrStageSurface = nullptr;
  uint32_t ocrStageWidth = 0;
  uint32_t ocrStageHeight = 0;

  // 1-G: 1440p+ 소스에서 GPU 2× 다운샘플 후 readback하는 half-size stagesurf.
  // CPU 다운스케일(8 MB) 대신 GPU 다운스케일(2 MB) 후 readback → OCR 메모리
  // 대역폭 4× 절감.
  gs_texrender_t *ocrDownRender_ = nullptr; // half-size BGRA render target
  gs_stagesurf_t *ocrDownStage_ = nullptr;  // half-size BGRA staging
  uint32_t ocrDownW_ = 0;
  uint32_t ocrDownH_ = 0;

  // ----- [Role B] Async OCR worker 상태 -----
  // OCR은 RecognizeAsync(...).get()으로 블로킹될 수 있으므로 video_render에서
  // 직접 실행하지 않는다.
  std::thread ocrWorkerThread;
  std::mutex ocrWorkerMutex;
  std::condition_variable ocrWorkerCv;
  std::atomic<bool> ocrWorkerRunning{false};

  // OCR 입력 프레임은 최신 1장만 유지한다. OCR이 render보다 느릴 때 큐 누적을
  // 막기 위함이다.
  bool ocrFramePending = false;
  uint64_t ocrPendingFrameId = 0;
  std::vector<uint8_t> ocrPendingPixels;
  int ocrPendingWidth = 0;
  int ocrPendingHeight = 0;
  int ocrPendingStride = 0;
#ifdef _WIN32
  // [Window anchor] OCR 프레임 push 시점의 windowList 스냅샷. OCR worker가
  // PII 박스→owner HWND 매칭에 사용한다 (video_tick과의 race 회피).
  TrackedWindowList ocrPendingWindowSnapshot{};
  // [Window anchor v6] OCR 프레임 캡처 시점의 모든 가시 top-level 창 enum.
  // WindowFromPoint 실시간 호출 대신 이걸로 owner를 매칭해야 OCR 실행 지연 동안
  // 창이 움직였어도 정확한 owner를 잡는다.
  TrackedWindowList ocrPendingAllWindows{};
#endif

  // back-pressure: idle이면 즉시 새 프레임 수용, busy면 GPU readback 건너뜀
  std::atomic<bool> ocrWorkerIdle{true};
  std::atomic<uint64_t> lastSubmittedOcrFrameId{0};
  std::atomic<uint64_t> lastCompletedOcrFrameId{0};
  int unverifiedFrameLogCounter = 0;

  // dHash 캐시 무효화 요청 — GUI 스레드가 set, OCR 워커가 다음 사이클 진입 시
  // clear. clearDHashCache()를 GUI 스레드에서 직접 호출하면 data race
  // 발생하므로 이 플래그를 경유해 워커 스레드에서 안전하게 실행한다.
  std::atomic<bool> ocrClearCachePending{false};

  // M8: OCR 엔진 초기화 영구 실패 여부. 렌더 루프에서 확인 후 RISK 상태로 전환.
  std::atomic<bool> ocrIsDown{false};

  // 직전 OCR 사이클의 박스 수. 변경 시에만 LOG_INFO, 매 사이클은 LOG_DEBUG.
  int lastLoggedOcrCount = -1;

  // [SC-tracker] 주기 로그 카운터 (150 readback ≈ 5초마다 1회 @ 30Hz)
  int trackerLogCounter = 0;

  // [backfill 빈도 제어] OCR claim 성공 4번에 1번만 backfill 호출.
  // 매번 호출 시 링 내 모든 슬롯 dependent를 새 frameId로 끌어올려 freeze가
  // 자주 발생하던 문제 완화. render thread 단독 사용이라 atomic 불필요.
  int ocrBackfillCounter_ = 0;

  // ----- [P1] 30Hz Visual Tracker Thread -----
  // NCC 연산(CPU-only)을 렌더 스레드에서 분리. GPU readback은 렌더 스레드,
  // update_all_gray()는 이 스레드가 담당. register_or_update()는 OCR 워커가
  // 호출.
  std::thread trackerThread_;
  std::mutex trackerInputMutex_;
  std::condition_variable trackerInputCv_;
  // P0-4: BGRA→gray 변환을 렌더 스레드에서 한 번만 수행; gray 버퍼를 swap으로
  // 전달
  std::vector<uint8_t> trackerInputGray_; // grayscale (stride = trackerInputW_)
  int trackerInputW_ = 0;
  int trackerInputH_ = 0;
  bool trackerInputReady_ = false;
  std::atomic<bool> trackerThreadRunning_{false};
  int trackerFrameSkip_ = 0; // 30Hz gate: 2프레임마다 readback

  // ----- [좌표계 동기화] OCR 다운스케일 ↔ 트래커 공간 정합 -----
  // use1GPath(OCR 절반 크기 제출)가 활성화된 경우, 트래커도 절반 해상도
  // 공간에서 추적하므로 렌더 시 박스 좌표를 trackerCoordScale_ 배율로
  // 원본 해상도로 복원해야 한다.
  // 1.0f = full-res 모드(기본), 2.0f = half-res OCR 최적화 모드.
  float trackerCoordScale_ = 1.0f;
};
