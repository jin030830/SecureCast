// =============================================================================
// visible_subrects_cache.cpp — [Freeze T04] 구현
// =============================================================================

#ifdef _WIN32

// windows.h(헤더 경유 include)의 min/max 매크로가 std::min/std::max를 깨뜨리지
// 않도록 차단. visual-tracker.cpp와 동일 이유.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "visible_subrects_cache.h"
#include "win_event_listener.h" // WinEventListener::eventGeneration()

#include <algorithm>
#include <chrono>

namespace {
uint64_t steady_now_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
} // namespace

uint64_t VisibleSubrectsCache::make_key(HWND target, const RECT &b) {
  // HWND + bounds 4성분을 섞어 64-bit 키 생성. 해시 충돌은 저장된 bounds 정확
  // 비교로 거르므로(충돌 시 miss=재계산) 안전.
  uint64_t h = reinterpret_cast<uintptr_t>(target);
  auto mix = [&h](uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  };
  mix(static_cast<uint32_t>(b.left));
  mix(static_cast<uint32_t>(b.top));
  mix(static_cast<uint32_t>(b.right));
  mix(static_cast<uint32_t>(b.bottom));
  return h;
}

int VisibleSubrectsCache::query(HWND target, RECT target_bounds, RECT *out,
                                int maxOut) {
  if (!out || maxOut <= 0)
    return 0;

  const uint64_t gen = WinEventListener::eventGeneration();
  const uint64_t nowMs = steady_now_ms();
  const uint64_t key = make_key(target, target_bounds);

  // --- 1) shared-lock 빠른 경로: hit 시도 ---
  {
    std::shared_lock<std::shared_mutex> rlock(mutex_);
    // 캐시 전체가 현재 세대를 반영할 때만 신뢰 (세대 진행 시 아래에서 flush).
    if (cacheGen_ == gen) {
      auto it = cache_.find(key);
      if (it != cache_.end() && it->second.generation == gen &&
          nowMs < it->second.valid_until_ms &&
          rect_equal(it->second.bounds, target_bounds)) {
        // 방어적 clamp: visibleCount는 store 시 [0,16]이지만, 어떤 이유로든
        // 비정상 값이 들어와도 out/visibleRects 경계를 넘지 않도록 이중 보호.
        int n = it->second.visibleCount;
        if (n < 0)
          n = 0;
        if (n > SC_MAX_VISIBLE_SUBRECTS)
          n = SC_MAX_VISIBLE_SUBRECTS;
        if (n > maxOut)
          n = maxOut;
        for (int i = 0; i < n; ++i)
          out[i] = it->second.visibleRects[i];
        hits_.fetch_add(1, std::memory_order_relaxed);
        // n==0(완전 occlusion)도 유효 캐시 — 0 반환.
        return n;
      }
    }
  }

  // --- 2) miss → 재계산 (DWM 호출은 락 밖에서) ---
  RECT computed[SC_MAX_VISIBLE_SUBRECTS];
  const int n = sc_compute_visible_subrects(target, target_bounds, computed,
                                            SC_MAX_VISIBLE_SUBRECTS);
  misses_.fetch_add(1, std::memory_order_relaxed);

  // --- 3) unique-lock 저장 ---
  {
    std::unique_lock<std::shared_mutex> wlock(mutex_);
    // 세대가 진행됐으면 전체 flush — 전역 무효화 + 맵 무한 증가 방지
    // (창 이동 시 (HWND,bounds) 키가 매번 달라져 stale 항목이 쌓이는 것을 차단).
    if (cacheGen_ != gen) {
      cache_.clear();
      cacheGen_ = gen;
    }
    CachedSubrects &slot = cache_[key];
    slot.hwnd = target;
    slot.bounds = target_bounds;
    slot.visibleCount = n;
    const int store = std::min(n, SC_MAX_VISIBLE_SUBRECTS);
    for (int i = 0; i < store; ++i)
      slot.visibleRects[i] = computed[i];
    slot.valid_until_ms = nowMs + kTtlMs;
    slot.generation = gen;
  }

  const int outN = std::min(n, maxOut);
  for (int i = 0; i < outN; ++i)
    out[i] = computed[i];
  return outN;
}

void VisibleSubrectsCache::invalidate(HWND target) {
  std::unique_lock<std::shared_mutex> wlock(mutex_);
  for (auto it = cache_.begin(); it != cache_.end();) {
    if (it->second.hwnd == target)
      it = cache_.erase(it);
    else
      ++it;
  }
}

void VisibleSubrectsCache::invalidate_all() {
  std::unique_lock<std::shared_mutex> wlock(mutex_);
  cache_.clear();
}

#endif // _WIN32
