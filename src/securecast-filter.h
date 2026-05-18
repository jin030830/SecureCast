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
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdint.h>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "gpu-readback.h"
#include "overlay-window.h"
#include "selection-overlay.h"
#endif
#include "pipeline-health.h"
#include "pixel-hash.h"
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
    60; // Bounded Exposure: OCR 최대 레이턴시(≈1000ms) 대비 여유 확보를 위해
        // 60슬롯으로 증가 (1초 지연)

// ----------------------------------------------------
// OCR 다운스케일 정책 tier
//
// 입력 해상도에 따라 OCR 워커에 넘기는 BGRA 크기를 결정한다.
//   Full       : 원본 그대로 (≤1080p 기본 또는 셀프힐링 강등 시)
//   TwoThirds  : 1080p에서 1280×720 (0.667×) — Mitchell+USM 셰이더
//   Half       : 1440p+에서 절반 (0.5×) — 기존 bilinear 다운샘플
//
// enum 값이 작을수록 OCR 입력 해상도가 크다 = 더 안전한 인식.
// ocr_effective_tier()는 policy와 override 중 더 안전한 쪽(값이 작은 쪽)을
// 선택한다.
// ----------------------------------------------------
enum class OcrScaleTier : int8_t { Full = 0, TwoThirds = 1, Half = 2 };

// 셀프힐링 override.
//   Auto(-1)   : override 없음 → policy 그대로 사용
//   Full/TwoThirds/Half : policy를 무시하고 이 값과 policy 중 더 안전한 쪽을
//                        선택 (`ocr_effective_tier()` 참조).
enum class OcrTierOverride : int8_t {
  Auto = -1,
  Full = 0,
  TwoThirds = 1,
  Half = 2,
};

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
struct LingeringWindow {
  TrackedWindow window; // 마지막으로 알려진 창 정보 (bounds 포함)
  int ticksRemaining;   // SC_RING_BUFFER_SLOTS에서 매 tick 카운트다운
};
constexpr int SC_MAX_LINGERING = SC_MAX_TRACKED_WINDOWS;
#endif

// ----------------------------------------------------
// [Role C] OCR 트래커 박스 슬롯 스냅샷
//
// windowSnapshot이 창 좌표를 슬롯 단위로 저장해 송출과 동기화하듯,
// OCR/Visual Tracker 박스 좌표도 캡처 시점의 슬롯에 함께 저장한다.
// → 60프레임 후 그 슬롯이 송출될 때 저장된 박스를 사용 = 1프레임 오차.
//
// lingering 처리:
//   NCC lost 후에도 ticksRemaining > 0이면 마지막 위치를 유지.
//   SC_RING_BUFFER_SLOTS(60) 경과 후 슬롯이 자연 순환하여 소멸.
// ----------------------------------------------------
constexpr int SC_MAX_SLOT_OCR_BOXES =
    16; // VisualTrackerManager::MAX_TRACKERS(8)의 2배 여유

struct OcrBoxSnapshot {
  BlurRect rects[SC_MAX_SLOT_OCR_BOXES]{};
  int count = 0;
  // 각 박스의 lingering TTL. lost 후에도 > 0이면 박스 유지.
  int ticksRemaining[SC_MAX_SLOT_OCR_BOXES]{};
};

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
    uint64_t dependentOcrFrameId =
        0; // 이 프레임 송출 전 완료되어야 할 대표 OCR 프레임 ID
#ifdef _WIN32
    // 이 프레임이 캡처된 시점의 창 좌표 스냅샷.
    // 렌더 시 출력 슬롯의 windowSnapshot을 사용해야 프레임 내용과 마스크 위치가
    // 동기화됨.
    TrackedWindowList windowSnapshot{};
#endif
    // 이 프레임이 캡처된 시점의 OCR/Tracker 박스 스냅샷 (플랫폼 무관).
    // 송출 시 outputSlot->ocrBoxSnapshot을 사용해 박스 위치와 텍스처를 동기화.
    OcrBoxSnapshot ocrBoxSnapshot{};

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
  // wlist      : 이 프레임 캡처 시점의 창 좌표 스냅샷 (null 허용, Win32 전용).
  // ocrSnapshot: 이 프레임 캡처 시점의 OCR/Tracker 박스 스냅샷 (null 허용).
#ifdef _WIN32
  void pushFrame(uint64_t timestamp, obs_source_t *filter_context,
                 const TrackedWindowList *wlist, uint64_t dependentOcrFrameId,
                 const OcrBoxSnapshot *ocrSnapshot = nullptr);
#else
  void pushFrame(uint64_t timestamp, obs_source_t *filter_context,
                 uint64_t dependentOcrFrameId,
                 const OcrBoxSnapshot *ocrSnapshot = nullptr);
#endif

  const Slot *peekDelayedSlot() const;
  // framesBack=SC_RING_BUFFER_SLOTS이면 peekDelayedSlot()과 동일.
  // framesBack=SC_RING_BUFFER_SLOTS-1이면 한 프레임 더 최신 슬롯 (빠른 이동
  // 합집합용).
  const Slot *peekSlotAtOffset(int framesBack) const;

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
// [Role C] Mock AI Worker Thread
// ----------------------------------------------------
class MockAIWorker {
public:
  // resultCallback: AI 분석이 끝날 때마다 Render Thread에서 읽을 결과를
  // 전달하는 함수
  using ResultCallback = std::function<void(const MaskPayload &)>;

  MockAIWorker() = default;
  ~MockAIWorker() { stop(); }

  // 워커 스레드 시작. frameWidth/Height로 가짜 중앙 좌표를 계산한다.
  void start(uint32_t frameWidth, uint32_t frameHeight,
             ResultCallback callback);
  void stop();
  void setPaused(bool paused);

  bool isRunning() const { return m_running.load(); }
  bool isPaused() const { return m_paused.load(); }

private:
  void workerLoop();

  std::thread m_thread;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_paused{false};
  std::mutex m_mutex;
  std::condition_variable m_cv;

  uint32_t m_frameWidth = 0;
  uint32_t m_frameHeight = 0;
  ResultCallback m_callback;
};

// ----------------------------------------------------
// [Role C] Lock-Free Result Slot
// ----------------------------------------------------
class AtomicMaskChannel {
public:
  // AI 스레드에서 호출 (produce)
  void publish(const MaskPayload &payload);

  // 렌더 스레드에서 호출 (consume). 새 데이터가 없으면 false 반환
  bool consume(MaskPayload &out);

private:
  std::mutex m_mutex; // m_pending 접근 보호 (torn read 방지)
  alignas(64) MaskPayload m_pending{};
  alignas(64) std::atomic<bool> m_ready{false};
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
  SecurityState currentState =
      SecurityState::SAFE; // 현재 보안 등급 (SAFE/PARTIAL/RISK)

  // ----- [Role C 담당: 렌더링 파이프라인 및 GPU Readback] -----
  FrameRingBuffer
      ringBuffer; // Bounded Exposure(송출 지연) 구현용 N-프레임 텍스처 버퍼
  MockAIWorker mockWorker; // [Role B 작업용] Role B가 AI/OCR을 연결하기 전까지
                           // 모의 데이터를 발생시키는 워커
  AtomicMaskChannel maskChannel; // AI 스레드 -> 비디오 렌더 스레드로 마스크
                                 // 데이터를 안전하게 전달하는 단방향 채널
  MaskPayload lastMask{}; // AI가 마지막으로 검출하여 발행한 블러/블랙아웃 처리
                          // 영역 정보

#ifdef _WIN32
  GpuReadback readback; // GPU 텍스처를 CPU 메모리로 지연 없이 복사하는 다중
                        // 슬롯 텍스처 풀
  OverlayWindow
      overlay; // [Role D] 스트리머 전용 보안 상태 HUD (OBS 캡처에서 제외됨)
#endif
  PixelHashCache fullScreenHash; // FNV-1a 기반으로 화면 변화(Smart Grid)를
                                 // 감지하여 AI 동작을 제어하는 객체

  std::vector<uint8_t> readbackBuffer; // Readback을 통해 수확한 픽셀 데이터를
                                       // 저장하는 CPU 버퍼 (Slot 0 + Slot 1)
  uint64_t frameCounter =
      0;                 // GPU와 CPU 간의 프레임 정합성을 맞추기 위한 카운터
  PipelineHealth health; // GPU 스톨 또는 쿼리 실패 감지 시 자가 치유(Reset)를
                         // 담당하는 헬스 매니저

  // ----- [Role D] UI 설정 -----
  mutable std::mutex
      settingsMutex; // GUI 스레드(update)와 렌더 스레드 간 data race 방지
  std::string blacklistApps = ""; // 줄바꿈 구분 앱 이름 목록
  float blurIntensity = 5.0f;
  float sensitivity = 0.5f;

  // [C2-3 수정] 함수-scope static → 멤버 변수로 이동 (다중 필터 인스턴스 간
  // 공유 방지)
  int logUnchangedFrames =
      0; // 미변화 상태 로그 주기 카운터 (120프레임마다 1회)
  int logStallCount =
      0; // 파이프라인 포화 경고 로그 주기 카운터 (30프레임마다 1회)
  int logEnqueueCount = 0; // enqueue 성공 로그 주기 카운터 (300프레임마다 1회)
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
  LingeringWindow lingeringWindows[SC_MAX_LINGERING]{};
  int lingeringCount = 0;

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
  // screenChanged 감지 시 우하단 알림 영역에 변화가 있으면 3초간 블러를 유지.
  // video_tick에서 쿨다운 카운트다운, video_render에서 all_rects에 주입.
  bool notifBlurActive = false;
  float notifBlurCooldown = 0.0f; // 3.0f에서 카운트다운, 0에 도달하면 해제
  BlurRect notifBlurRect{};       // 소스 픽셀 좌표 (변화 감지 시 갱신)

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

  // OCR 전용 Mitchell+USM 다운스케일 셰이더 + 중간 텍스처.
  // 1080p TwoThirds(1280×720) 및 1440p Half 경로에서 사용.
  // 미로드 시 compute_policy()가 Full 반환으로 안전 폴백.
  gs_effect_t *ocrDownsampleEffect_ = nullptr;
  gs_texrender_t *ocrMidRender_ = nullptr; // pass1(Mitchell) → pass2(USM) 입력

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

  // OCR 워커 큐에 함께 실리는 트래커 등록용 데이터.
  // 1080p TwoThirds: ocrPendingPixels는 1280×720 BGRA, trackerGray는 원본
  //                  1920×1080 gray (좌표계 분리, 추가 readback 없음).
  // 1440p Half: ocrPendingPixels는 1280×720 BGRA, trackerGray는 half 1280×720.
  // Full: ocrPendingPixels는 원본 BGRA, trackerGray는 원본 gray.
  // ocrWorkerMutex로 ocrPendingPixels 등과 함께 한 lock으로 push/pop된다.
  std::vector<uint8_t> ocrPendingTrackerGray;
  int ocrPendingTrackerW = 0;
  int ocrPendingTrackerH = 0;
  OcrScaleTier ocrPendingTier = OcrScaleTier::Full;
  std::chrono::steady_clock::time_point ocrPendingEnqueueTs;

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
  // 1.0f = full-res 모드(기본 / TwoThirds 트래커는 원본 공간 추적),
  // 2.0f = half-res OCR 최적화 모드.
  float trackerCoordScale_ = 1.0f;

  // pushFrame 직전에 매 프레임 갱신되는 "살아있는" OCR 박스 스냅샷.
  // captureWindowList와 동일한 역할 — 이 값이 다음 슬롯에 봉인됨.
  // active_boxes()가 줄었을 때(lost) ticksRemaining > 0인 박스를 N프레임 유지.
  OcrBoxSnapshot liveOcrSnapshot_{};

  // ----- [OCR 다운스케일 tier + 셀프힐링] -----
  // policyTier : 해상도에서 결정되는 기본 정책 (compute_policy 결과).
  // overrideTier: 셀프힐링이 정책 위에 강제하는 안전 모드.
  //               Auto면 policy 그대로 사용. 그 외엔 policy와 min() 비교로
  //               더 안전한(더 큰 OCR 입력 해상도 = enum 값 작은) 쪽 채택.
  // zeroLineStreak: full OCR에서 effectiveLineCount==0이 연속된 사이클 수.
  // lastEscalateMs: 마지막 셀프힐링 발동 시각 (60초 후 자동 완화).
  std::atomic<OcrScaleTier> ocrPolicyTier_{OcrScaleTier::Full};
  std::atomic<OcrTierOverride> ocrOverrideTier_{OcrTierOverride::Auto};
  std::atomic<int> ocrZeroLineStreak_{0};
  std::atomic<int64_t> ocrLastEscalateMs_{0};
  // tier 전환 감지용 — 변경 시 trackerMgr.clear() 호출.
  std::atomic<OcrScaleTier> lastEffectiveTier_{OcrScaleTier::Full};

  // OCR 처리시간 누적 진단 로그용 (Step 9).
  double ocrEnqueueLatencyAccumMs_ = 0.0;
  int ocrEnqueueLatencyCount_ = 0;

  // ----- [OCR tier 헬퍼] -----
  static constexpr float ocr_scale_ratio(OcrScaleTier t) {
    return (t == OcrScaleTier::Half)        ? 0.5f
           : (t == OcrScaleTier::TwoThirds) ? (2.0f / 3.0f)
                                            : 1.0f;
  }
  static OcrScaleTier ocr_effective_tier(OcrScaleTier policy,
                                         OcrTierOverride ov) {
    if (ov == OcrTierOverride::Auto)
      return policy;
    int a = static_cast<int>(policy);
    int b = static_cast<int>(ov);
    // 값이 작을수록 안전(다운스케일 약함). 둘 중 더 안전한 쪽 선택.
    return static_cast<OcrScaleTier>(std::min(a, b));
  }
  // override를 policy 방향으로 한 단계 완화. policy 이상에 도달하면 Auto로
  // 전환. (+1 방향이 "덜 보수적": Full(0)→TwoThirds(1)→Half(2)→Auto(-1))
  static OcrTierOverride relax_override(OcrTierOverride ov,
                                        OcrScaleTier policy) {
    if (ov == OcrTierOverride::Auto)
      return ov;
    int next = static_cast<int>(ov) + 1;
    if (next >= static_cast<int>(policy))
      return OcrTierOverride::Auto;
    return static_cast<OcrTierOverride>(next);
  }
};
