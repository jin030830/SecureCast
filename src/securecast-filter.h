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
#include <string>

#ifdef _WIN32
#include "gpu-readback.h"
#include "overlay-window.h"
#include "selection-overlay.h"
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
constexpr int SC_MAX_BLUR_RECTS = 32;
constexpr int SC_RING_BUFFER_SLOTS = 5;

// ----------------------------------------------------
// Shared Types (Types) - Moved to securecast-types.h
// ----------------------------------------------------

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
// ----------------------------------------------------
class FrameRingBuffer {
public:
    struct Slot {
        gs_texrender_t* texrender = nullptr;
        uint64_t        timestamp = 0;
#ifdef _WIN32
        // 이 프레임이 캡처된 시점의 창 좌표 스냅샷.
        // 렌더 시 delayedSlot->windowSnapshot을 사용해야 프레임 내용과 마스크 위치가 동기화됨.
        TrackedWindowList windowSnapshot{};
#endif

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
// ----------------------------------------------------
class MockAIWorker {
public:
    using ResultCallback = std::function<void(const MaskPayload&)>;

    MockAIWorker()  = default;
    ~MockAIWorker() { stop(); }

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
// ----------------------------------------------------
class AtomicMaskChannel {
public:
    void publish(const MaskPayload& payload);
    bool consume(MaskPayload& out);

private:
    std::mutex                    m_mutex;
    alignas(64) MaskPayload      m_pending{};
    alignas(64) std::atomic<bool> m_ready{false};
};

// ----------------------------------------------------
// Core Filter Context
// ----------------------------------------------------
struct SecureCastFilter {
    SecureCastFilter();
    ~SecureCastFilter();

    SecureCastFilter(const SecureCastFilter&) = delete;
    SecureCastFilter& operator=(const SecureCastFilter&) = delete;

    obs_source_t* context = nullptr; // OBS 필터 컨텍스트 포인터

    bool          isActive     = true;
    bool          isGameMode   = false;                // CPU 임계값 기반 자동 전환
    SecurityState currentState = SecurityState::SAFE;

    // ----- [Role C] -----
    FrameRingBuffer   ringBuffer;
    MockAIWorker      mockWorker;
    AtomicMaskChannel maskChannel;
    MaskPayload       lastMask{};

#ifdef _WIN32
    GpuReadback   readback;
    OverlayWindow overlay;   // [Role D] 스트리머 전용 보안 상태 HUD (OBS 캡처에서 제외됨)
#endif
    PixelHashCache       fullScreenHash;   // FNV-1a 기반으로 화면 변화(Smart Grid)를 감지하여 AI 동작을 제어하는 객체
    std::vector<uint8_t> readbackBuffer;  // Readback을 통해 수확한 픽셀 데이터를 저장하는 CPU 버퍼 (Slot 0 + Slot 1)
    uint64_t             frameCounter = 0; // GPU와 CPU 간의 프레임 정합성을 맞추기 위한 카운터
    PipelineHealth       health;           // GPU 스톨 또는 쿼리 실패 감지 시 자가 치유(Reset)를 담당하는 헬스 매니저

    // [C2-3 수정] 함수-scope static → 멤버 변수로 이동 (다중 필터 인스턴스 간 공유 방지)
    int  logUnchangedFrames = 0;  // 미변화 상태 로그 주기 카운터 (120프레임마다 1회)
    int  logStallCount      = 0;  // 파이프라인 포화 경고 로그 주기 카운터 (30프레임마다 1회)
    int  logEnqueueCount    = 0;  // enqueue 성공 로그 주기 카운터 (300프레임마다 1회)
    int  logScanThrottle    = 0;  // 블랙리스트 윈도우 스캔 로그 주기 카운터 (10틱 = 1.5초 주기)

    // ----- [Role A 담당: 윈도우 추적 및 블랙리스트] -----
    float         trackerAccumulator = 0.0f;
    gs_effect_t*  blurEffect = nullptr;      // 컴파일된 HLSL 셰이더
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

    // ----- [Game Mode] CPU 사용률 기반 자동 전환 -----
    float    cpuSampleAccumulator = 0.0f;
    float    cpuUsage             = 0.0f;
    float    gameModeEntryTimer   = 0.0f;
    float    gameModeExitTimer    = 0.0f;
    FILETIME prevIdleTime         = {};
    FILETIME prevKernelTime       = {};
    FILETIME prevUserTime         = {};
#endif

    // destroy 진입 즉시 true — 진행 중인 핫키 콜백이 해제된 멤버에 접근하지 못하도록
    std::atomic<bool> isDestroying{false};

    // ----- [Panic Button] Ctrl+Shift+F12 -----
    std::atomic<bool> panicMode{false};
    obs_hotkey_id     panicHotkeyId = OBS_INVALID_HOTKEY_ID;

#ifdef _WIN32
    // ----- [Role D] 수동 드래그 블러 선택 오버레이 -----
    SelectionOverlay  selectionOverlay;
    obs_hotkey_id     selectHotkeyId = OBS_INVALID_HOTKEY_ID;
#endif

    // ----- [Role D] UI 설정 -----
    mutable std::mutex settingsMutex;
    std::string  blacklistApps  = "";
    float        blurIntensity  = 5.0f;
    float        sensitivity    = 0.5f;

    // ----- [Role D] 알림 영역 자동 블러 -----
    // screenChanged 감지 시 우하단 알림 영역에 변화가 있으면 3초간 블러를 유지.
    // video_tick에서 쿨다운 카운트다운, video_render에서 all_rects에 주입.
    bool     notifBlurActive   = false;
    float    notifBlurCooldown = 0.0f;   // 3.0f에서 카운트다운, 0에 도달하면 해제
    BlurRect notifBlurRect{};            // 소스 픽셀 좌표 (변화 감지 시 갱신)

    // ----- [Role D] 수동 드래그 블러 -----
    // OBS 소스 프리뷰에서 좌클릭 드래그로 영역 지정 → 영구 블러.
    // 우클릭 또는 Properties의 "Clear" 버튼으로 전체 초기화.
    // settingsMutex로 UI 스레드(mouse 콜백) ↔ Render 스레드(video_render) 보호.
    static constexpr int SC_MAX_MANUAL_RECTS = 8;
    BlurRect manualRects[SC_MAX_MANUAL_RECTS]{};
    int      manualRectCount = 0;

    bool     dragActive  = false;  // 드래그 진행 중
    int32_t  dragStartX  = 0;
    int32_t  dragStartY  = 0;
    int32_t  dragCurX    = 0;
    int32_t  dragCurY    = 0;

    // 모니터→소스 좌표 변환용 캐시 (video_render에서 갱신, 원자적 접근)
    std::atomic<uint32_t> lastSourceW{0};
    std::atomic<uint32_t> lastSourceH{0};

    // ----- [Role B] OCR 엔진 -----
    std::unique_ptr<SecureCastOcrEngine> ocrEngine;

    // ----- [Role B/C] OCR용 GPU readback 재사용 리소스 -----
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

    uint32_t ocrFrameCounter = 0;

};
