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

// ----------------------------------------------------
// OBS Headers
// ----------------------------------------------------
#include <obs.h>
#include <obs-module.h>

// ----------------------------------------------------
// Helpers & Macros
// ----------------------------------------------------
#ifndef obs_log
#define obs_log(level, format, ...) blog(level, "[SecureCast] " format, ##__VA_ARGS__)
#endif

// ----------------------------------------------------
// Global Configurations (Config)
// ----------------------------------------------------
constexpr int SC_MAX_BLUR_RECTS   = 32; // 최대 동시 마스킹 가능 영역 수
constexpr int SC_RING_BUFFER_SLOTS = 5; // Bounded Exposure N-Frame 지연 슬롯 수 (고정 3~5)

// ----------------------------------------------------
// Shared Types (Types)
// ----------------------------------------------------

enum class SecurityState {
    SAFE,       // 노출 위험 없음 (초록)
    PARTIAL,    // 위험 감지 후 마스킹 동작 중 (노랑)
    RISK        // 심각한 유출 위험 또는 프레임 오버플로우 (빨강)
};

// 화면 내 개별적인 블러/블랙아웃 처리 영역 구조체
struct BlurRect {
    int x;
    int y;
    int width;
    int height;
    int type; // 0: Blur, 1: Blackout
};

// AI Thread → Render Thread 락프리 전달 페이로드
struct MaskPayload {
    BlurRect rects[SC_MAX_BLUR_RECTS];
    int rectCount;
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
    // 각 슬롯이 보유하는 데이터: OBS 텍스처 핸들 + 타임스탬프
    struct Slot {
        gs_texture_t* texture = nullptr; // GPU 텍스처 핸들 (OBS Graphics subsystem)
        uint64_t      timestamp = 0;     // obs_get_video_frame_time() 값

        bool isReady() const { return texture != nullptr; }
    };

    FrameRingBuffer()  = default;
    ~FrameRingBuffer() = default;

    // OBS gs_context가 잡힌 상태에서만 호출해야 함 (video_render 내부)
    // width, height: 현재 소스의 해상도
    bool initialize(uint32_t width, uint32_t height);
    void destroy();

    // 현재 프레임(currentVideoFrame)을 링 버퍼 HEAD에 복사해 넣는다.
    // obs_source_t* source: 상위 원본 소스 (텍스처 가져오기 대상)
    void pushFrame(obs_source_t* source, uint64_t timestamp);

    // N 슬롯 전의 "지연된" 프레임을 읽어온다. (nullptr 반환 시 아직 버퍼 미충족)
    const Slot* peekDelayedSlot() const;

    bool isInitialized() const { return m_initialized; }
    uint32_t getWidth()  const { return m_width;  }
    uint32_t getHeight() const { return m_height; }

private:
    std::array<Slot, SC_RING_BUFFER_SLOTS> m_slots{};
    int      m_head        = 0;     // 다음에 쓸 인덱스
    int      m_frameCount  = 0;     // 누적 push 횟수 (충분한 지연이 쌓였는지 판단)
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
// 모든 Role이 참조하는 필터 인스턴스
// ----------------------------------------------------
struct SecureCastFilter {
    obs_source_t* context = nullptr;

    // UI 및 운영 토글 상태
    bool          isActive    = true;
    bool          isGameMode  = false;
    SecurityState currentState = SecurityState::SAFE;

    // ----- [Role C] 담당 필드 -----
    FrameRingBuffer  ringBuffer;    // N-Frame 지연 버퍼
    MockAIWorker     mockWorker;    // 가짜 AI 워커 스레드
    AtomicMaskChannel maskChannel; // AI → Render 락프리 채널
    MaskPayload      lastMask{};   // 마지막으로 적용된 마스킹 결과 (없으면 rectCount==0)

    // ----- TODO: Role A 담당 필드 -----
    // gs_effect_t* blurEffect = nullptr;  // 컴파일된 HLSL 셰이더

    // ----- TODO: Role B 담당 필드 -----
    // void* ocrEngine = nullptr;           // Windows.Media.Ocr 엔진 포인터
};
