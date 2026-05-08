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
#endif
#include "pixel-hash.h"
#include "pipeline-health.h"
#include "securecast-types.h"

// ----------------------------------------------------
// OBS Headers
// ----------------------------------------------------
#include <obs.h>
#include <obs-module.h>

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

// ----------------------------------------------------
// [Role C] N-Frame Ring Buffer
// ----------------------------------------------------
class FrameRingBuffer {
public:
    struct Slot {
        gs_texrender_t* texrender = nullptr;
        uint64_t        timestamp = 0;

        gs_texture_t* getTexture() const {
            return texrender ? gs_texrender_get_texture(texrender) : nullptr;
        }
        bool isReady() const { return getTexture() != nullptr; }
    };

    FrameRingBuffer()  = default;
    ~FrameRingBuffer() = default;

    bool initialize(uint32_t width, uint32_t height);
    void destroy();
    void pushFrame(uint64_t timestamp, obs_source_t* filter_context);
    const Slot* peekDelayedSlot() const;

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

    bool isRunning() const { return m_running.load(); }

private:
    void workerLoop();

    std::thread             m_thread;
    std::atomic<bool>       m_running{false};
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
    obs_source_t* context = nullptr;

    bool          isActive     = true;
    // bool        isGameMode  = false;                 // [v2] 게임 모드 — 현재 스코프 외
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
    PixelHashCache       fullScreenHash;
    std::vector<uint8_t> readbackBuffer;
    uint64_t             frameCounter = 0;
    PipelineHealth       health;

    int  logUnchangedFrames = 0;
    int  logStallCount      = 0;
    int  logEnqueueCount    = 0;
    int  logScanThrottle    = 0;

    // ----- [Role A] -----
    float      trackerAccumulator = 0.0f;
    std::mutex blacklistMutex;
    MaskPayload blacklistMask{};

    // ----- [Role D] -----
    mutable std::mutex settingsMutex;
    std::string  blacklistApps  = "";
    float        blurIntensity  = 5.0f;
    float        sensitivity    = 0.5f;

    // ----- [Role B 연계 포인트] -----
    // void* ocrEngine = nullptr;
};
