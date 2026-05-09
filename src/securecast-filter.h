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
//   - 향후 Role B (AI/OCR) / Role C (N-Frame Delay)도 이 구조체에 필드 추가 예정.
// =============================================================================

#pragma once

// ----------------------------------------------------
// C++ Standard Library Headers (MUST be included before OBS headers)
// ----------------------------------------------------
#include <stdint.h>
#include <atomic>
#include <array>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>

#ifdef _WIN32
#include "gpu-readback.h"
#endif
#include "pixel-hash.h"
#include "pipeline-health.h"
#include "securecast-types.h"
#include "visual-tracker.h"

// ocr-engine.h는 이 헤더에서 직접 include하지 않는다.
// WinRT/OCR 관련 의존성이 다른 translation unit으로 전파되는 것을 막기 위해
// SecureCastOcrEngine은 forward declaration + unique_ptr로 보관한다.
class SecureCastOcrEngine;


// ----------------------------------------------------
// OBS Headers
// ----------------------------------------------------
#include <obs.h>
#include <obs-module.h>

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
constexpr int SC_MAX_BLUR_RECTS = 32;       // 한 프레임에 동시에 마스킹 가능한 최대 영역 수
constexpr int SC_RING_BUFFER_SLOTS = 3;     // Bounded Exposure: AI 검증을 위해 N프레임만큼 송출 지연 슬롯 수 (P0-D: 5→3, 50ms 기저 지연 단축)

// ----------------------------------------------------
// Shared Types (Types) - Moved to securecast-types.h
// ----------------------------------------------------

// AI Thread에서 만든 마스킹 결과 → Render Thread로 넘기는 락프리 버퍼 팩(전달 페이로드)
struct MaskPayload {
    BlurRect rects[SC_MAX_BLUR_RECTS];
    int rectCount;
};

#ifdef _WIN32
// 창이 사라진 후 ring buffer에 남은 N프레임 동안 마스킹을 유지하는 잔영 항목.
struct LingeringWindow {
    TrackedWindow window;         // 마지막으로 알려진 창 정보 (bounds 포함)
    int           ticksRemaining; // SC_RING_BUFFER_SLOTS에서 매 tick 카운트다운
};
constexpr int SC_MAX_LINGERING = SC_MAX_TRACKED_WINDOWS;
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
        gs_texrender_t* texrender = nullptr; // OBS 안전 렌더 타겟 관리자
        uint64_t        timestamp = 0;
#ifdef _WIN32
        // 이 프레임이 캡처된 시점의 창 좌표 스냅샷.
        // 렌더 시 delayedSlot->windowSnapshot을 사용해야 프레임 내용과 마스크 위치가 동기화됨.
        TrackedWindowList windowSnapshot{};
#endif

        // gs_texrender에서 결과 텍스처를 꺼내는 헬퍼
        gs_texture_t* getTexture() const {
            return texrender ? gs_texrender_get_texture(texrender) : nullptr;
        }
        bool isReady() const { return getTexture() != nullptr; }
    };

    FrameRingBuffer()  = default;
    ~FrameRingBuffer() = default;

    bool initialize(uint32_t width, uint32_t height);
    void destroy();

    // gs_texrender_begin/end를 사용하여 안전하게 프레임을 캡처.
    // wlist: 이 프레임 캡처 시점의 창 좌표 스냅샷 (null 허용).
#ifdef _WIN32
    void pushFrame(uint64_t timestamp, obs_source_t* filter_context, const TrackedWindowList* wlist);
#else
    void pushFrame(uint64_t timestamp, obs_source_t* filter_context);
#endif

    const Slot* peekDelayedSlot() const;
    // framesBack=SC_RING_BUFFER_SLOTS이면 peekDelayedSlot()과 동일.
    // framesBack=SC_RING_BUFFER_SLOTS-1이면 한 프레임 더 최신 슬롯 (빠른 이동 합집합용).
    const Slot* peekSlotAtOffset(int framesBack) const;

    bool isInitialized() const { return m_initialized; }
    uint32_t getWidth()  const { return m_width;  }
    uint32_t getHeight() const { return m_height; }

private:
    std::array<Slot, SC_RING_BUFFER_SLOTS> m_slots{};
    int      m_head        = 0;
    int      m_frameCount  = 0;
    uint32_t m_width       = 0;
    uint32_t m_height      = 0;
    bool     m_initialized = false;
};

// ----------------------------------------------------
// [Role C] Mock AI Worker Thread
//
// 실제 AI/OCR 모듈이 완성되기 전까지 동작을 시뮬레이션한다.
// - 50ms sleep(AI 처리 시간 모의) 후
// - 화면 한가운데 고정 BlurRect를 MaskPayload에 담아 콜백으로 전달
// Role B의 실제 AI Worker로 교체될 예정
// ----------------------------------------------------
class MockAIWorker {
public:
    // resultCallback: AI 분석이 끝날 때마다 Render Thread에서 읽을 결과를 전달하는 함수
    using ResultCallback = std::function<void(const MaskPayload&)>;

    MockAIWorker()  = default;
    ~MockAIWorker() { stop(); }

    // 워커 스레드 시작. frameWidth/Height로 가짜 중앙 좌표를 계산한다.
    void start(uint32_t frameWidth, uint32_t frameHeight, ResultCallback callback);
    void stop();
    void setPaused(bool paused);

    bool isRunning() const { return m_running.load(); }
    bool isPaused()  const { return m_paused.load();  }

private:
    void workerLoop();

    std::thread             m_thread;
    std::atomic<bool>       m_running{false};
    std::atomic<bool>       m_paused{false};
    std::mutex              m_mutex;
    std::condition_variable m_cv;

    uint32_t        m_frameWidth  = 0;
    uint32_t        m_frameHeight = 0;
    ResultCallback  m_callback;
};

// ----------------------------------------------------
// [Role C] Lock-Free Result Slot
//
// MockAIWorker(AI Thread) → Render Thread 단방향 채널.
// 단일 producer / 단일 consumer 구조이므로
// std::atomic으로 충분히 data-race-free를 보장한다.
//
// 상태 전이:
//   AI Thread: 결과 계산 완료 → write pending payload → m_ready.store(true)
//   Render Thread: m_ready.load() == true → 읽고 → m_ready.store(false)
// ----------------------------------------------------
class AtomicMaskChannel {
public:
    // AI 스레드에서 호출 (produce)
    void publish(const MaskPayload& payload);

    // 렌더 스레드에서 호출 (consume). 새 데이터가 없으면 false 반환
    bool consume(MaskPayload& out);

private:
    std::mutex                    m_mutex;    // m_pending 접근 보호 (torn read 방지)
    alignas(64) MaskPayload      m_pending{};
    alignas(64) std::atomic<bool> m_ready{false};
};

// ----------------------------------------------------
// Core Filter Context
// ----------------------------------------------------
// 모든 Role이 협업하며 참조하는 메인 필터 인스턴스 구조체
// ----------------------------------------------------
struct SecureCastFilter {
    SecureCastFilter();
    ~SecureCastFilter();

    SecureCastFilter(const SecureCastFilter&) = delete;
    SecureCastFilter& operator=(const SecureCastFilter&) = delete;

    obs_source_t* context = nullptr; // OBS 필터 컨텍스트 포인터

    // UI 및 운영 토글 상태
    bool          isActive     = true;                  // 필터 활성화 여부
    bool          isGameMode   = false;                 // 게임 모드 활성화 여부
    SecurityState currentState = SecurityState::SAFE;   // 현재 보안 등급 (SAFE/PARTIAL/RISK)

    // ----- [Role C 담당: 렌더링 파이프라인 및 GPU Readback] -----
    FrameRingBuffer  ringBuffer;         // Bounded Exposure(송출 지연) 구현용 N-프레임 텍스처 버퍼
    MockAIWorker     mockWorker;         // [Role B 작업용] Role B가 AI/OCR을 연결하기 전까지 모의 데이터를 발생시키는 워커
    AtomicMaskChannel maskChannel;       // AI 스레드 -> 비디오 렌더 스레드로 마스크 데이터를 안전하게 전달하는 단방향 채널
    MaskPayload      lastMask{};         // AI가 마지막으로 검출하여 발행한 블러/블랙아웃 처리 영역 정보

#ifdef _WIN32
    GpuReadback      readback;           // GPU 텍스처를 CPU 메모리로 지연 없이 복사하는 다중 슬롯 텍스처 풀
#endif
    PixelHashCache       fullScreenHash;   // FNV-1a 기반으로 화면 변화(Smart Grid)를 감지하여 AI 동작을 제어하는 객체
    
    std::vector<uint8_t> readbackBuffer;  // Readback을 통해 수확한 픽셀 데이터를 저장하는 CPU 버퍼 (Slot 0 + Slot 1)
    uint64_t             frameCounter = 0; // GPU와 CPU 간의 프레임 정합성을 맞추기 위한 카운터
    PipelineHealth       health;           // GPU 스톨 또는 쿼리 실패 감지 시 자가 치유(Reset)를 담당하는 헬스 매니저

    // [C2-3 수정] 함수-scope static → 멤버 변수로 이동 (다중 필터 인스턴스 간 공유 방지)
    int  logUnchangedFrames = 0;  // 미변화 상태 로그 주기 카운터 (120프레임마다 1회)
    int  logStallCount      = 0;  // 파이프라인 포화 경고 로그 주기 카운터 (30프레임마다 1회)
    int  logEnqueueCount    = 0;  // enqueue 성공 로그 주기 카운터 (300프레임마다 1회)

    // ----- [Role A 담당: 윈도우 추적 및 블랙리스트] -----
    float         trackerAccumulator = 0.0f; // 윈도우 스캔 틱 조절(0.15초 단위)용 시간 누산기
    gs_effect_t*  blurEffect = nullptr;      // 컴파일된 HLSL 셰이더
    std::mutex    blacklistMutex;            // video_tick(비디오)과 video_render(렌더) 간의 동시 접근을 막는 뮤텍스
    MaskPayload   blacklistMask{};           // [우선순위 1] Role A가 추적한 블랙리스트 앱 좌표 (AI 처리 전에 최상단에 덮어씌움)
#ifdef _WIN32
    WinEventListener  winListener;
    TrackedWindowList windowList{};          // 현재 추적 중인 창 목록
    TrackedWindowList captureWindowList{};   // pushFrame 스냅샷: 직전 프레임 DWM 좌표 (캡처 레이턴시 보정)
    TrackedWindowList prevWindowList{};      // lingering 감지용 직전 스캔 결과
    TrackedWindowList recentlySeenList{};    // 과거에 추적했던 창 목록 (quick restore용, 닫힐 때까지 유지)
    LingeringWindow   lingeringWindows[SC_MAX_LINGERING]{};
    int               lingeringCount = 0;

    // ----- [Game Mode] CPU 사용률 기반 자동 전환 -----
    float    cpuSampleAccumulator = 0.0f; // 1초 샘플링 누산기
    float    cpuUsage             = 0.0f; // 최근 측정 시스템 CPU 사용률 (0~100)
    float    gameModeEntryTimer   = 0.0f; // ≥40% 지속 시간 누산 (3초 도달 시 진입)
    float    gameModeExitTimer    = 0.0f; // <30% 지속 시간 누산 (10초 도달 시 해제)
    FILETIME prevIdleTime         = {};   // GetSystemTimes 이전 샘플
    FILETIME prevKernelTime       = {};
    FILETIME prevUserTime         = {};
#endif

    // ----- [Panic Button] Ctrl+Shift+F12 -----
    std::atomic<bool> panicMode{false};
    obs_hotkey_id     panicHotkeyId = OBS_INVALID_HOTKEY_ID;

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
    gs_effect_t*    trackerGrayEffect_ = nullptr;  // downsample.effect GrayDownsample
    gs_texrender_t* trackerGrayRender_ = nullptr;  // full-res R8 gray render target
    gs_stagesurf_t* trackerGrayStage_  = nullptr;  // staging for CPU readback
    uint32_t        trackerGrayW_      = 0;
    uint32_t        trackerGrayH_      = 0;

    // ----- [Role B] OCR 엔진 -----
    // OCR 엔진은 render thread가 아니라 OCR worker thread 내부에서 init()한다.
    // forward declaration을 위해 unique_ptr로 보관하여 ocr-engine.h 의존성을 분리한다.
    std::unique_ptr<SecureCastOcrEngine> ocrEngine;

    // ----- [Role B/C] OCR용 GPU readback 재사용 리소스 -----
    // 매 OCR마다 gs_stagesurface_create/destroy를 반복하지 않기 위해 보관한다.
    gs_stagesurf_t* ocrStageSurface = nullptr;
    uint32_t        ocrStageWidth   = 0;
    uint32_t        ocrStageHeight  = 0;

    // ----- [Role B] Async OCR worker 상태 -----
    // OCR은 RecognizeAsync(...).get()으로 블로킹될 수 있으므로 video_render에서 직접 실행하지 않는다.
    std::thread             ocrWorkerThread;
    std::mutex              ocrWorkerMutex;
    std::condition_variable ocrWorkerCv;
    std::atomic<bool>       ocrWorkerRunning{false};

    // OCR 입력 프레임은 최신 1장만 유지한다. OCR이 render보다 느릴 때 큐 누적을 막기 위함이다.
    bool                 ocrFramePending = false;
    std::vector<uint8_t> ocrPendingPixels;
    int                  ocrPendingWidth  = 0;
    int                  ocrPendingHeight = 0;
    int                  ocrPendingStride = 0;

    // back-pressure: idle이면 즉시 새 프레임 수용, busy면 GPU readback 건너뜀
    std::atomic<bool> ocrWorkerIdle{true};

    // 직전 OCR 사이클의 박스 수. 변경 시에만 LOG_INFO, 매 사이클은 LOG_DEBUG.
    int lastLoggedOcrCount = -1;

    // [SC-tracker] 주기 로그 카운터 (150 readback ≈ 5초마다 1회 @ 30Hz)
    int trackerLogCounter = 0;

    // ----- [P1] 30Hz Visual Tracker Thread -----
    // NCC 연산(CPU-only)을 렌더 스레드에서 분리. GPU readback은 렌더 스레드,
    // update_all_gray()는 이 스레드가 담당. register_or_update()는 OCR 워커가 호출.
    std::thread             trackerThread_;
    std::mutex              trackerInputMutex_;
    std::condition_variable trackerInputCv_;
    // P0-4: BGRA→gray 변환을 렌더 스레드에서 한 번만 수행; gray 버퍼를 swap으로 전달
    std::vector<uint8_t>   trackerInputGray_;   // grayscale (stride = trackerInputW_)
    int                    trackerInputW_      = 0;
    int                    trackerInputH_      = 0;
    bool                   trackerInputReady_  = false;
    std::atomic<bool>      trackerThreadRunning_{false};
    int                    trackerFrameSkip_   = 0; // 30Hz gate: 2프레임마다 readback

};
