#pragma once
#include <util/base.h>
#include <cstdint>

/**
 * @brief GPU 파이프라인의 건강 상태를 감시하는 클래스
 * 
 * 연속된 수확 실패 횟수와 타임아웃 발생 빈도를 추적하여 
 * 시스템이 데드락 상태에 빠졌는지 판단합니다.
 */
class PipelineHealth {
public:
    enum class Status {
        OK,         // 정상 작동 중
        DEGRADED,   // 일시적 지연 발생 중 (강제 해제 등)
        CRITICAL    // 심각한 정체 발생 (1초 이상 무응답 또는 반복적 강제 해제)
    };

    PipelineHealth();

    void onCollectSuccess();
    void onCollectFailure();
    void onForceRelease();

    Status evaluate() const;
    bool shouldReset() const;
    void reset();

    // 보안 마스킹 홀드 로직용 상태 조회
    bool isDegraded() const { return evaluate() == Status::DEGRADED; }
    bool isCritical() const { return evaluate() == Status::CRITICAL; }

    // 디버깅용 정보 제공
    int getConsecutiveFailures() const { return m_consecutiveFailures; }
    int getForceReleaseCount() const { return m_forceReleaseCount; }

private:
    uint64_t m_lastSuccessTime = 0;
    int      m_consecutiveFailures = 0;
    int      m_forceReleaseCount = 0;
    uint64_t m_windowStart = 0;
    // enqueueCopy/submitFrame이 실제로 호출된 적 없으면 timeout CRITICAL을 발동하지 않는다.
    // GpuReadback 파이프라인이 미연결 상태일 때 오탐 리셋 루프를 방지하기 위함.
    bool     m_hasEverSucceeded = false;

    static constexpr uint64_t CRITICAL_TIMEOUT_MS = 1000;
    static constexpr uint64_t FORCE_RELEASE_WINDOW_MS = 3000;
    static constexpr int      MAX_FORCE_RELEASES_PER_WINDOW = 10;
};
