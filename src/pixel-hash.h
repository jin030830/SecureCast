#pragma once
#include <cstdint>
#include <vector>

struct BlurRect;

/**
 * @brief 64x64 픽셀 데이터의 변경 여부를 감지하는 캐시 클래스
 * 
 * FNV-1a 64비트 해시를 사용하며, 최근 8프레임의 해시값을 링 버퍼에 유지하여
 * 미세한 픽셀 진동(Noise)에 의한 불필요한 AI 호출을 방지합니다.
 */
class PixelHashCache {
    static constexpr int RING_SIZE = 8;
    static constexpr int CHANGE_THRESHOLD_PERCENT = 5;  // 변화량 임계치 (%)
    static constexpr int PIXELS_64x64 = 64 * 64;
    static constexpr int PIXEL_DIFF_THRESHOLD = 30;     // 개별 픽셀 차이 임계값 (0~255)

    uint64_t m_hashRing[RING_SIZE] = {};
    int      m_ringIdx = 0;
    int      m_cacheHitCount = 0;   // 연속 캐시 히트 횟수 (통계용)
    
    // 변화량 측정용 이전 프레임 데이터
    uint8_t  m_prevData[PIXELS_64x64 * 4] = {};
    bool     m_hasPrevData = false;
    uint64_t m_lastUpdateMs = 0; // 마지막으로 기준점이 갱신된 시각 (ms)

public:
    PixelHashCache();

    /**
     * @brief FNV-1a 64비트 해시 계산
     */
    static uint64_t computeHash(const uint8_t* data, size_t size);

    /**
     * @brief 데이터가 이전 프레임들(링 버퍼)과 비교하여 변경되었는지 확인
     * @param minPixelChange 변화된 픽셀 개수 임계치 (0~4096)
     * @return true 변경됨 (AI 처리 필요), false 변경 없음 (캐시 히트)
     */
    bool hasChanged(const uint8_t* data, size_t size, int minPixelChange = 204, BlurRect* outChangedGrid = nullptr);

    /**
     * @brief 캐시 초기화 (BBox 위치 변경 시 등)
     */
    void reset();
};
