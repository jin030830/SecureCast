#include "pipeline-health.h"
#include <util/platform.h>
#include <util/base.h>

PipelineHealth::PipelineHealth() {
    reset();
}

void PipelineHealth::onCollectSuccess() {
    m_hasEverSucceeded = true;
    m_lastSuccessTime = os_gettime_ns() / 1000000;
    m_consecutiveFailures = 0;
}

void PipelineHealth::onCollectFailure() {
    m_consecutiveFailures++;
    if (m_consecutiveFailures % 30 == 0) {
        blog(LOG_INFO, "[SecureCast] GPU query continues to stall (consecutive failures: %d)", m_consecutiveFailures);
    }
}

void PipelineHealth::onForceRelease() {
    uint64_t now = os_gettime_ns() / 1000000;
    
    // 슬라이딩 윈도우 초기화 체크 (3초 경과 시 카운트 리셋)
    if (now - m_windowStart > FORCE_RELEASE_WINDOW_MS) {
        m_forceReleaseCount = 0;
        m_windowStart = now;
    }
    
    m_forceReleaseCount++;
}

PipelineHealth::Status PipelineHealth::evaluate() const {
    uint64_t now = os_gettime_ns() / 1000000;
    uint64_t elapsedSinceSuccess = (m_lastSuccessTime > 0) ? (now - m_lastSuccessTime) : 0;

    // 1. 1초 이상 수확 성공이 없는 경우 (GPU 완전 정체 의심)
    // GpuReadback이 한 번이라도 실제 성공한 뒤에만 체크한다.
    // 파이프라인이 아직 연결되지 않은 상태(enqueueCopy 미호출)에서는 오탐 리셋을 막기 위함.
    if (m_hasEverSucceeded && m_lastSuccessTime > 0 && elapsedSinceSuccess > CRITICAL_TIMEOUT_MS) {
        return Status::CRITICAL;
    }

    // 2. 짧은 시간 내에 너무 잦은 강제 해제가 발생하는 경우 (파이프라인 포화 반복)
    if (m_forceReleaseCount >= MAX_FORCE_RELEASES_PER_WINDOW) {
        return Status::CRITICAL;
    }

    // 3. 현재 지연이 진행 중인 상태 (임계값 5회로 완화하여 가짜 DEGRADED 완화)
    if (m_consecutiveFailures >= 5) {
        return Status::DEGRADED;
    }

    return Status::OK;
}

bool PipelineHealth::shouldReset() const {
    return evaluate() == Status::CRITICAL;
}

void PipelineHealth::reset() {
    m_lastSuccessTime = os_gettime_ns() / 1000000;
    m_windowStart = m_lastSuccessTime;
    m_consecutiveFailures = 0;
    m_forceReleaseCount = 0;
}
