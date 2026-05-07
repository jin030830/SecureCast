#include "pixel-hash.h"
#include "securecast-types.h"
#include <cstring>
#include <cstdlib>  // std::abs (int형)
#include <util/platform.h>

PixelHashCache::PixelHashCache() {
    reset();
}

void PixelHashCache::reset() {
    for (int i = 0; i < RING_SIZE; i++) {
        m_hashRing[i] = 0;
    }
    m_ringIdx = 0;
    m_cacheHitCount = 0;
    m_hasPrevData = false;
    m_lastUpdateMs = 0;
    memset(m_prevData, 0, sizeof(m_prevData));
}

uint64_t PixelHashCache::computeHash(const uint8_t* data, size_t size) {
    if (!data || size == 0) return 0;

    // FNV-1a 64비트 상수 (고성능 비암호화 해시 알고리즘)
    uint64_t hash = 0xcbf29ce484222325ULL;
    const uint64_t prime = 0x100000001b3ULL;

    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= prime;
    }
    return hash;
}

bool PixelHashCache::hasChanged(const uint8_t* data, size_t size, int minPixelChange, BlurRect* outChangedGrid) {
    uint64_t currentHash = computeHash(data, size);
    uint64_t nowMs = os_gettime_ns() / 1000000;

    // [1단계: 해시 비교] 링 버퍼에서 일치하는 해시가 있는지 확인
    bool found = false;
    for (int i = 0; i < RING_SIZE; i++) {
        if (m_hashRing[i] == currentHash) {
            found = true;
            break;
        }
    }

    if (found) {
        m_cacheHitCount++;
        // 캐시 히트 시에는 현재 데이터를 기준점으로 삼아도 안전함 (이미 아는 상태이므로)
        memcpy(m_prevData, data, (size < sizeof(m_prevData)) ? size : sizeof(m_prevData));
        m_hasPrevData = true;
        m_lastUpdateMs = nowMs;
        return false;
    }

    // [2단계: 변화량 임계치 필터]
    bool significantChange = true;
    bool isStale = (m_hasPrevData && (nowMs - m_lastUpdateMs) > 1000); // 1초 이상 갱신 안됨

    if (m_hasPrevData && size == PIXELS_64x64 * 4) {
        int changedPixels = 0;
        int minX = 64, minY = 64, maxX = 0, maxY = 0;

        for (int y = 0; y < 64; y++) {
            for (int x = 0; x < 64; x++) {
                int idx = (y * 64 + x) * 4;
                int diffB = std::abs((int)data[idx]   - (int)m_prevData[idx]);
                int diffG = std::abs((int)data[idx+1] - (int)m_prevData[idx+1]);
                int diffR = std::abs((int)data[idx+2] - (int)m_prevData[idx+2]);
                
                if (diffR > PIXEL_DIFF_THRESHOLD || diffG > PIXEL_DIFF_THRESHOLD || diffB > PIXEL_DIFF_THRESHOLD) {
                    changedPixels++;
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
            }
        }
        // [Suggestion 반영] 해상도 변화 대응: 입력 버퍼 크기에 따른 적응형 임계값 연산
        int adaptiveThreshold = minPixelChange;
        if (minPixelChange == 204 && size > 0) {
            int pixelCount = (int)(size / 4);
            adaptiveThreshold = (pixelCount * CHANGE_THRESHOLD_PERCENT) / 100;
        }
        significantChange = (changedPixels >= adaptiveThreshold);
        
        if (significantChange && outChangedGrid) {
            outChangedGrid->x = minX;
            outChangedGrid->y = minY;
            outChangedGrid->width = (maxX >= minX) ? (maxX - minX + 1) : 64;
            outChangedGrid->height = (maxY >= minY) ? (maxY - minY + 1) : 64;
        }
    } else if (outChangedGrid) {
        outChangedGrid->x = 0;
        outChangedGrid->y = 0;
        outChangedGrid->width = 64;
        outChangedGrid->height = 64;
    }

    // 유의미한 변화가 있거나, 처음 데이터이거나, 1초 이상 갱신이 안 된 경우(Stale) 기준점 갱신
    if (significantChange || !m_hasPrevData || isStale) {
        memcpy(m_prevData, data, (size < sizeof(m_prevData)) ? size : sizeof(m_prevData));
        m_hasPrevData = true;
        m_lastUpdateMs = nowMs;

        // 새로운 해시값을 링 버퍼에 기록 (Stale 리셋 시에는 기록하지 않아도 무방하나 일관성을 위해 기록)
        m_hashRing[m_ringIdx] = currentHash;
        m_ringIdx = (m_ringIdx + 1) % RING_SIZE;
        m_cacheHitCount = 0;

        // 실제 유의미한 변화였을 때만 true 반환 (단순 Stale 리셋은 false)
        return significantChange;
    }

    // 변화가 미미하면 기준점을 갱신하지 않음 → 다음 프레임에서 변화가 누적되어 감지됨
    return false;
}
