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
#include <string>

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
// 컴파일 타임 상수. 런타임에 바꿀 일이 없으므로 constexpr로 둔다.
constexpr int SC_MAX_BLUR_RECTS = 32;       // 한 프레임에 동시에 마스킹 가능한 최대 영역 수
constexpr int SC_RING_BUFFER_SLOTS = 5;     // Bounded Exposure: AI 검증을 위해 N프레임만큼 송출 지연 슬롯 수 (고정 3~5)

// ----------------------------------------------------
// Shared Types (Types)
// ----------------------------------------------------

// UI 인디케이터 상태(초록/노랑/빨강).
// video_render에서 오버레이 표시할 때, video_tick의 위험 판단 결과를 반영.
enum class SecurityState {
    SAFE,       // 노출 위험 없음 (초록)
    PARTIAL,    // 위험 감지 후 마스킹 동작 중 (노랑)
    RISK        // 심각한 유출 위험 또는 프레임 오버플로우 (빨강)
};

// 화면 내 개별적인 블러/블랙아웃 처리 영역.
// Role A의 window_tracker / Role B의 OCR 결과가 이 구조체로 모이고,
// Role A의 HLSL 셰이더가 이 좌표를 받아 픽셀 처리한다.
struct BlurRect {
    int x;
    int y;
    int width;
    int height;
    int type; // 0: Blur, 1: Blackout
};

// AI Thread에서 만든 마스킹 결과 → Render Thread로 넘기는 락프리 버퍼 팩(전달 페이로드)

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
    // 각 슬롯이 보유하는 데이터: gs_texrender + 타임스탬프
    struct Slot {
        gs_texrender_t* texrender = nullptr; // OBS 안전 렌더 타겟 관리자
        uint64_t        timestamp = 0;

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

    // gs_texrender_begin/end를 사용하여 안전하게 프레임을 캡처
    void pushFrame(obs_source_t* source, uint64_t timestamp);

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

    // ----- [Role A] 담당 필드 -----
    float         trackerAccumulator = 0.0f; // window_tracker tick throttle 누산기
    // gs_effect_t* blurEffect = nullptr;  // 컴파일된 HLSL 셰이더

    // ----- [Role D] 담당 필드 -----
    std::string  blacklistApps  = "";    // 블랙리스트 앱 목록 (줄바꿈 구분)
    float        blurIntensity  = 5.0f; // 블러 강도 (1.0 ~ 10.0)
    float        sensitivity    = 0.5f; // 감지 민감도 (0.0 ~ 1.0)

    // ----- TODO: Role B 담당 필드 -----
    // void* ocrEngine = nullptr;           // Windows.Media.Ocr 엔진 포인터
};
