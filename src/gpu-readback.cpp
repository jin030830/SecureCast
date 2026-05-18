#ifdef _WIN32
#include "gpu-readback.h"
#include <obs-module.h>
#include <util/dstr.h>

bool GpuReadback::initialize() {
  m_device = (ID3D11Device *)gs_get_device_obj();
  if (!m_device)
    return false;
  m_device->GetImmediateContext(&m_context);

  // downsample.effect 로드
  char *path = obs_module_file("downsample.effect");
  if (!path) {
    obs_module_t *mod = obs_current_module();
    const char *data_path = obs_get_module_data_path(mod);
    blog(LOG_ERROR,
         "[SecureCast] downsample.effect not found! I was looking in the "
         "module data path: %s",
         data_path ? data_path : "(null)");
    return false;
  }

  blog(LOG_INFO, "[SecureCast] Attempting to load effect from: %s", path);
  m_downsampleEffect = gs_effect_create_from_file(path, nullptr);
  bfree(path);

  if (!m_downsampleEffect) {
    blog(LOG_ERROR,
         "[SecureCast] gs_effect_create_from_file failed (downsample.effect)");
    return false;
  }

  // 비동기 쿼리 생성
  D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
  for (int i = 0; i < SLOT_COUNT; i++) {
    HRESULT hr = m_device->CreateQuery(&qd, &m_queries[i]);
    if (FAILED(hr)) {
      blog(LOG_ERROR, "[SecureCast] CreateQuery failed for query %d (hr=0x%08X)", i, hr);
      m_queries[i] = nullptr;
    }
    m_querySubmitted[i] = false;
    m_submitTime[i] = 0;
  }

  m_totalSlots = 0;
  m_activeQuery = 0;
  m_collectQuery = 0;
  m_pendingCount = 0;

  blog(LOG_INFO, "[SecureCast] GpuReadback initialized successfully");
  return true;
}

CollectResult GpuReadback::tryCollectPreviousFrame() {
  if (m_pendingCount == 0)
    return CollectResult::PENDING;

  int prev = m_collectQuery;
  if (!m_querySubmitted[prev] || !m_queries[prev] || !m_context)
    return CollectResult::PENDING;

  HRESULT hr = m_context->GetData(m_queries[prev], nullptr, 0, 0);
  if (hr == S_FALSE) {
    m_context->Flush(); // GPU가 작업을 빨리 끝내도록 명령 제출 강제
    m_queryFailCount++;
    if (m_queryFailCount % 30 == 0) {
      blog(LOG_INFO,
           "[SecureCast] GPU query still in progress (S_FALSE) for slot %d. "
           "count: %d",
           prev, m_queryFailCount);
    }
    // 드라이버 버그 등으로 인한 무한 정체 방지를 위해
    // 45회 연속 실패 시(약 0.75초) 강제로 슬롯을 해제하는 소프트 복구를
    // 트리거합니다.
    if (m_queryFailCount > 45) {
      blog(LOG_WARNING, "[SecureCast] Persistent query failure (45 stalls). "
                        "Forcing health soft recovery.");
      forceReleaseOldestSlot();
      m_queryFailCount = 0;
      return CollectResult::SOFT_RECOVERED;
    }
    return CollectResult::PENDING;
  }

  if (FAILED(hr)) {
    blog(LOG_ERROR, "[SecureCast] GPU query failed (0x%08X)", hr);
    m_querySubmitted[prev] = false;
    // [P1 Fix] 실패한 슬롯도 인출을 진행(m_collectQuery 진행 및 m_pendingCount 감소)시켜 파이프라인 영구 교착 예방
    m_collectQuery = (m_collectQuery + 1) % SLOT_COUNT;
    m_pendingCount--;
    return CollectResult::FAILED;
  }

  m_queryFailCount = 0;
  // [F1 Fix] m_forceReleasedLast = false; 라인을 삭제하여 releaseFrame() 이전까지 flag가 정상 보존되도록 함.
  return CollectResult::OK;
}

void GpuReadback::releaseFrame() {
  int prev = m_collectQuery;
  m_querySubmitted[prev] = false;
  m_forceReleasedLast = false;
  m_collectQuery = (m_collectQuery + 1) % SLOT_COUNT;
  m_pendingCount--;
}

bool GpuReadback::isOldestSlotStalled() const {
  int prev = m_collectQuery;
  if (!m_querySubmitted[prev])
    return false;

  UINT64 now = GetTickCount64();
  return (now - m_submitTime[prev]) > STALL_TIMEOUT_MS;
}

void GpuReadback::forceReleaseOldestSlot() {
  int prev = m_collectQuery;
  blog(LOG_WARNING,
       "[SecureCast] Force releasing stalled slot %d (pending=%d/%d, age=%llu "
       "ms)",
       prev, m_pendingCount, SLOT_COUNT, GetTickCount64() - m_submitTime[prev]);

  m_querySubmitted[prev] = false;
  m_collectQuery = (m_collectQuery + 1) % SLOT_COUNT;
  m_pendingCount--;
  m_forceReleasedLast = true;
}

bool GpuReadback::wasForceReleased() const { return m_forceReleasedLast; }

void GpuReadback::setForceReleasedFlag(bool forced) {
  m_forceReleasedLast = forced;
}

bool GpuReadback::isPipelineFull() { return m_pendingCount >= SLOT_COUNT; }

bool GpuReadback::readStagingBuffer(int idx, uint8_t *out, size_t expectedPitch,
                                    int targetH) {
  if (idx >= m_totalSlots || !m_pool[m_collectQuery][idx].staging)
    return false;

  D3D11_MAPPED_SUBRESOURCE mapped;
  HRESULT hr = m_context->Map(m_pool[m_collectQuery][idx].staging, 0,
                              D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr))
    return false;

  uint8_t *src = (uint8_t *)mapped.pData;

  for (int row = 0; row < targetH; row++) {
    memcpy(out + row * expectedPitch, src + row * mapped.RowPitch,
           expectedPitch);
  }

  m_context->Unmap(m_pool[m_collectQuery][idx].staging, 0);
  return true;
}

void GpuReadback::resizePool(
    const std::vector<std::pair<int, int>> &slotSizes) {
  if (!m_device || !m_context)
    return;
  int newSize = (int)slotSizes.size();

  // 대기 중인 쿼리 완료 보장 (spin-wait)
  for (int i = 0; i < SLOT_COUNT; i++) {
    if (m_querySubmitted[i] && m_queries[i] && m_context) {
      int spin = 0;
      while (m_context->GetData(m_queries[i], nullptr, 0, 0) == S_FALSE) {
        if (++spin > 10000)
          break;
      }
      m_querySubmitted[i] = false;
    }
  }
  m_activeQuery = 0;
  m_collectQuery = 0;
  m_pendingCount = 0;

  // [BUGFIX] 슬롯 수가 같아도 해상도가 달라질 수 있으므로,
  // 항상 기존 슬롯을 모두 해제하고 새 해상도로 재생성합니다.
  for (int p = 0; p < SLOT_COUNT; p++) {
    for (auto &slot : m_pool[p]) {
      releaseSlot(slot);
    }
    m_pool[p].clear();

    for (int i = 0; i < newSize; i++) {
      PoolSlot slot;
      slot.rt = gs_texrender_create(GS_BGRA, GS_ZS_NONE);

      D3D11_TEXTURE2D_DESC td = {};
      td.Width = slotSizes[i].first;
      td.Height = slotSizes[i].second;
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_STAGING;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &slot.staging);
      if (FAILED(hr)) {
        blog(LOG_ERROR,
             "[SecureCast] CreateTexture2D failed for slot[%d] size=%dx%d "
             "(hr=0x%08X)",
             i, slotSizes[i].first, slotSizes[i].second, hr);
        // [C2-6 수정] 실패한 슬롯도 push하되 staging=nullptr로 남겨둠.
        // readStagingBuffer에서 staging nullptr 가드가 있어 안전하게 스킵됨.
      }
      m_pool[p].push_back(slot);
    }
  }
  // [C2-6 수정] CreateTexture2D 실패 여부와 무관하게 newSize를 설정하되,
  // 실패한 슬롯은 readStagingBuffer에서 nullptr 체크로 방어됨.
  m_totalSlots = newSize;
  blog(
      LOG_INFO,
      "[SecureCast] resizePool complete: %d slots (Slot0: %dx%d, Slot1: %dx%d)",
      newSize, newSize > 0 ? slotSizes[0].first : 0,
      newSize > 0 ? slotSizes[0].second : 0,
      newSize > 1 ? slotSizes[1].first : 0,
      newSize > 1 ? slotSizes[1].second : 0);
}

void GpuReadback::releaseSlot(PoolSlot &slot) {
  if (slot.rt)
    gs_texrender_destroy(slot.rt);
  if (slot.staging)
    slot.staging->Release();
  slot.rt = nullptr;
  slot.staging = nullptr;
}

void GpuReadback::destroy() {
  for (int q = 0; q < SLOT_COUNT; q++) {
    if (m_querySubmitted[q] && m_context && m_queries[q]) {
      int spin = 0;
      while (m_context->GetData(m_queries[q], nullptr, 0, 0) == S_FALSE) {
        if (++spin > 100000)
          break;
      }
    }
    if (m_queries[q]) {
      m_queries[q]->Release();
      m_queries[q] = nullptr;
    }

    for (auto &slot : m_pool[q]) {
      releaseSlot(slot);
    }
    m_pool[q].clear();
  }
  m_totalSlots = 0;

  if (m_downsampleEffect) {
    gs_effect_destroy(m_downsampleEffect);
    m_downsampleEffect = nullptr;
  }

  if (m_context) {
    m_context->Release();
    m_context = nullptr;
  }
}

void GpuReadback::destroyImmediate() {
  // [Fail Secure] 스핀 대기 없이 즉시 모든 GPU 리소스와 쿼리를 해제합니다.
  // GPU가 정체되어 응답 불가인 상황에서 수초간 블로킹되는 현상을 방지합니다.
  for (int q = 0; q < SLOT_COUNT; q++) {
    if (m_queries[q]) {
      m_queries[q]->Release();
      m_queries[q] = nullptr;
    }
    m_querySubmitted[q] = false;
    m_submitTime[q] = 0;

    for (auto &slot : m_pool[q]) {
      releaseSlot(slot);
    }
    m_pool[q].clear();
  }
  m_totalSlots = 0;
  m_activeQuery = 0;
  m_collectQuery = 0;
  m_pendingCount = 0;

  if (m_downsampleEffect) {
    gs_effect_destroy(m_downsampleEffect);
    m_downsampleEffect = nullptr;
  }

  if (m_context) {
    m_context->Release();
    m_context = nullptr;
  }
}

#endif // _WIN32
