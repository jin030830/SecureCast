#pragma once

#ifdef _WIN32
#include <util/base.h>
#include <graphics/graphics.h>
#include <d3d11.h>
#include <vector>
#include <cstdint>
#include "securecast-types.h" // BlurRect 사용

/**
 * @brief Role B(AI/OCR) 전달용 결과 구조체
 * CPU 메모리에 복사된 64x64 픽셀 데이터와 해당 영역의 메타데이터를 포함합니다.
 * 
 * [Role B 연계] 이 구조체에 담긴 hashData를 OCR 엔진의 입력값으로 사용하세요.
 */
struct ReadbackResult {
    std::vector<uint8_t> ownedData;  // 64x64 BGRA 데이터 깊은 복사용 (소유권 확보)
    const uint8_t*       hashData;   // AI 분석용 다운샘플링 결과 포인터 (ownedData.data() 또는 원본)
    size_t               hashSize;   // 데이터 크기 (64*64*4 바이트)
    BlurRect             bbox;       // 원본 영상에서의 좌표 (Role A가 전달한 값)
    uint64_t             frameIndex; // 분석 대상 프레임 번호
};

/**
 * @brief GPU 리소스 관리 슬롯
 */
struct PoolSlot {
    gs_texrender_t*  rt      = nullptr; // 다운샘플용 렌더 타겟
    ID3D11Texture2D* staging = nullptr; // CPU 읽기용 스테이징 텍스처
    int              unusedFrames = 0;
};

enum class CollectResult {
    OK,
    PENDING,
    SOFT_RECOVERED,
    FAILED
};

class GpuReadback {
    ID3D11Device*        m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    gs_effect_t*         m_downsampleEffect = nullptr;

    static constexpr int SLOT_COUNT = 3; // Triple Buffering
    std::vector<PoolSlot> m_pool[SLOT_COUNT];
    int                   m_totalSlots = 0;

    ID3D11Query*  m_queries[SLOT_COUNT] = {nullptr, nullptr, nullptr};
    int           m_activeQuery = 0;  // 다음에 제출할 슬롯 (Write Pointer)
    int           m_collectQuery = 0; // 다음에 수확할 슬롯 (Read Pointer)
    int           m_pendingCount = 0; // 현재 GPU에 제출되어 대기 중인 슬롯 수
    bool          m_querySubmitted[SLOT_COUNT] = {false, false, false};
    UINT64        m_submitTime[SLOT_COUNT] = {0, 0, 0};
    bool          m_forceReleasedLast = false;
    static constexpr UINT64 STALL_TIMEOUT_MS = 150;
    int           m_queryFailCount = 0;

public:
    GpuReadback() = default;
    ~GpuReadback() { destroy(); }

    bool initialize();
    void destroy();
    void destroyImmediate(); // [보안 보상] 스핀 대기 없이 즉각 리소스를 해제합니다.

    /**
     * @brief 비동기 쿼리 수확 (Non-blocking)
     * @return true 수확 성공, false 아직 진행 중이거나 실패
     */
    CollectResult tryCollectPreviousFrame();
    void releaseFrame(); // 읽기가 완료된 슬롯 비우기
    bool isPipelineFull(); // 모든 슬롯이 작업 중인지 확인
    
    // [보완] 교착 해제 및 보안 보상용 메서드
    bool isOldestSlotStalled() const;
    void forceReleaseOldestSlot();
    bool wasForceReleased() const;
    void setForceReleasedFlag(bool forced);

    /**
     * @brief 스테이징 버퍼의 데이터를 CPU 메모리로 복사
     * [Role B 연계] 이 함수를 통해 추출된 out 버퍼는 AI 분석의 소스 데이터가 됩니다.
     */
    bool readStagingBuffer(int idx, uint8_t* out, size_t expectedPitch, int targetH);

    /**
     * @brief BBox 개수에 맞춰 리소스 풀 리사이징
     */
    void resizePool(const std::vector<std::pair<int, int>>& slotSizes);

private:
    void releaseSlot(PoolSlot& slot);
};

#endif // _WIN32
