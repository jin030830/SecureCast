// =============================================================================
// securecast-filter.h — 필터 인스턴스의 공유 타입 정의
//
// 역할:
//   하나의 OBS 소스에 부착된 SecureCast 필터 인스턴스가 들고 다니는 상태를
//   정의한다. 이 헤더는 Role A/B/C 코드 모두가 include해서 같은 구조체를 본다.
//
// 어디서 사용:
//   - securecast-filter.cpp: 인스턴스 생성/소멸/tick/render 콜백에서 직접 사용.
//   - window_tracker.cpp: trackerAccumulator를 받아 throttle 처리 (Role A).
//   - Role B: OCR worker 상태와 OCR 결과 전달 채널을 보관.
//   - Role C: N-Frame Delay ring buffer와 GPU readback 상태를 보관.
// =============================================================================

#pragma once

// ----------------------------------------------------
// C++ Standard Library Headers (MUST be included before OBS headers)
// ----------------------------------------------------
#include <stdint.h>
#include <atomic>
#include <array>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "gpu-readback.h"
#endif
#include "pixel-hash.h"
#include "pipeline-health.h"
#include "securecast-types.h"

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
constexpr int SC_RING_BUFFER_SLOTS = 5;     // Bounded Exposure: AI 검증을 위해 N프레임만큼 송출 지연 슬롯 수

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
// SC_RING_BUFFER_SLOTS 개의 슬롯(텍스처 핸들)을 순환배열로 관리하여
// 렌더 스레드가 블로킹 없이 N프레임 전의 프레임을 꺼낼 수 있게 한다.
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
// 현재 실제 OCR worker를 사용하므로 기본 렌더 루프에서는 start하지 않는다.
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
// [Role C] AtomicMaskChannel
//
// OCR Worker Thread → Render Thread 단방향 채널.
// 단일 producer / 단일 consumer 구조지만 payload 복사를 보호하기 위해 mutex를 둔다.
//
// 상태 전이:
//   OCR Thread: 결과 계산 완료 → write pending payload → m_ready.store(true)
//   Render Thread: m_ready.load() == true → 읽고 → m_ready.store(false)
// ----------------------------------------------------
class AtomicMaskChannel {
public:
    // OCR 스레드에서 호출 (produce)
    void publish(const MaskPayload& payload);

    // 렌더 스레드에서 호출 (consume). 새 데이터가 없으면 false 반환
    bool consume(MaskPayload& out);

private:
    std::mutex                     m_mutex;    // m_pending 접근 보호 (torn read 방지)
    alignas(64) MaskPayload        m_pending{};
    alignas(64) std::atomic<bool>  m_ready{false};
};

// ----------------------------------------------------
// Core Filter Context
// ----------------------------------------------------
// 모든 Role이 협업하며 참조하는 메인 필터 인스턴스 구조체
// ----------------------------------------------------
struct SecureCastFilter {
    // [PR #11] unique_ptr<SecureCastOcrEngine> 불완전 타입 지원을 위한 명시 선언
    SecureCastFilter();
    ~SecureCastFilter();
    SecureCastFilter(const SecureCastFilter&) = delete;
    SecureCastFilter& operator=(const SecureCastFilter&) = delete;

    obs_source_t* context = nullptr; // OBS 필터 컨텍스트 포인터

    // UI 및 운영 토글 상태
    bool          isActive     = true;
    bool          isGameMode   = false;
    SecurityState currentState = SecurityState::SAFE;

    // ----- [Role C] 렌더링 파이프라인 및 GPU Readback -----
    FrameRingBuffer   ringBuffer;
    MockAIWorker      mockWorker;
    AtomicMaskChannel maskChannel;
    MaskPayload       lastMask{};

#ifdef _WIN32
    GpuReadback       readback;
#endif
    PixelHashCache    fullScreenHash;
    std::vector<uint8_t> readbackBuffer;
    uint64_t          frameCounter = 0;
    PipelineHealth    health;

    int  logUnchangedFrames = 0;
    int  logStallCount      = 0;
    int  logEnqueueCount    = 0;
    int  logScanThrottle    = 0;

    // ----- [Role A] HLSL 마스킹 셰이더 및 윈도우 추적 -----
    gs_effect_t*  blurEffect = nullptr;
    float         trackerAccumulator = 0.0f;
    std::mutex    blacklistMutex;
    MaskPayload   blacklistMask{};
#ifdef _WIN32
    WinEventListener  winListener;
    TrackedWindowList windowList{};
    TrackedWindowList captureWindowList{};
    TrackedWindowList prevWindowList{};
    TrackedWindowList recentlySeenList{};
    LingeringWindow   lingeringWindows[SC_MAX_LINGERING]{};
    int               lingeringCount = 0;

    float    cpuSampleAccumulator = 0.0f;
    float    cpuUsage             = 0.0f;
    float    gameModeEntryTimer   = 0.0f;
    float    gameModeExitTimer    = 0.0f;
    FILETIME prevIdleTime         = {};
    FILETIME prevKernelTime       = {};
    FILETIME prevUserTime         = {};
#endif

    // ----- [Panic Button] Ctrl+Shift+F12 -----
    std::atomic<bool> panicMode{false};
    obs_hotkey_id     panicHotkeyId = OBS_INVALID_HOTKEY_ID;

    // ----- [Role B] OCR 엔진 (forward declaration + unique_ptr) -----
    std::unique_ptr<SecureCastOcrEngine> ocrEngine;

    // ----- [Role B] OCR용 stage surface -----
    // TODO: 추후 readbackBuffer(Slot 1)로 대체 권장 (GpuReadback과 중복 GPU 복사)
    gs_stagesurf_t* ocrStageSurface = nullptr;
    uint32_t        ocrStageWidth   = 0;
    uint32_t        ocrStageHeight  = 0;

    // ----- [Role B] Async OCR worker 상태 -----
    std::thread             ocrWorkerThread;
    std::mutex              ocrWorkerMutex;
    std::condition_variable ocrWorkerCv;
    std::atomic<bool>       ocrWorkerRunning{false};

    bool                 ocrFramePending = false;
    std::vector<uint8_t> ocrPendingPixels;
    int                  ocrPendingWidth  = 0;
    int                  ocrPendingHeight = 0;
    int                  ocrPendingStride = 0;
    uint32_t             ocrFrameCounter  = 0;

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

    // filter 인스턴스별 OCR throttle counter.
    // render 함수 내부 static counter를 쓰면 여러 filter 인스턴스가 값을 공유하므로 사용하지 않는다.
    uint32_t ocrFrameCounter = 0;
};
