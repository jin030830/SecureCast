// =============================================================================
// visible_subrects_cache.h — [Freeze T04] sc_compute_visible_subrects 캐시
//
// 문제:
//   sc_compute_visible_subrects(owner, box, ...)는 owner 창 위 z-order의 모든
//   top-level 창을 GetWindow 루프로 순회하며 DwmGetWindowAttribute(CLOAKED/
//   EXTENDED_FRAME_BOUNDS)를 호출해 노출 영역을 차감한다. snapshot_for_push가
//   렌더 스레드에서 매 프레임(60fps) PII 박스마다 호출하므로, 정적 화면에서도
//   동일 결과를 위해 DWM 호출이 반복돼 비용이 크다.
//
// 해법:
//   같은 입력(owner HWND + target_bounds)이고 그 사이 어떤 top-level 창 이벤트도
//   없었다면 결과는 동일하다. 이를 캐시한다.
//
// 무효화 — 둘 다 만족해야 hit:
//   (1) [주] WinEventListener 이벤트 세대(generation). 창의 생성/소멸/표시/숨김/
//       z-order(REORDER)/포그라운드/위치(LOCATIONCHANGE) 변화 시 세대가 증가하며,
//       세대가 달라진 캐시는 즉시 stale로 간주. → "위 창이 옆으로 빠졌는데 stale
//       '가려짐'을 믿어 그 아래 PII가 노출"되는 구멍을 닫는다.
//   (2) [보조] TTL backstop(기본 200ms). 이벤트 누락(훅 실패 등) 대비 안전망.
//
// 동작 특성:
//   - 정적 화면: 이벤트 없음 → 세대 동일 → hit → DWM 호출 격감.
//   - 창 이동/전환: 세대 증가 → miss → 매 프레임 재계산(=정확). 이때가 바로
//     재계산이 필요한 시점이므로 캐시 무효화가 곧 올바른 동작.
//
// thread-safety:
//   query/invalidate가 여러 스레드에서 불릴 수 있어 shared_mutex로 보호. DWM
//   호출(재계산)은 락 밖에서 수행해 락 유지 시간을 최소화한다. 현재 호출처는
//   렌더 스레드(snapshot_for_push) 단독이나 향후 tracker 스레드 공유 대비.
// =============================================================================
#pragma once

#ifdef _WIN32

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <unordered_map>

#include "window_tracker.h" // SC_MAX_VISIBLE_SUBRECTS, sc_compute_visible_subrects

struct CachedSubrects {
  HWND hwnd = nullptr; // invalidate(HWND)용 + 진단
  RECT bounds{};       // 저장 시점 target_bounds (키 해시 충돌 시 정확 비교)
  RECT visibleRects[SC_MAX_VISIBLE_SUBRECTS]{};
  int visibleCount = 0;       // 0 = 완전 occlusion (유효한 캐시 값)
  uint64_t valid_until_ms = 0; // TTL backstop 만료 시각 (steady_clock ms)
  uint64_t generation = 0;     // 저장 시점 WinEventListener 이벤트 세대
};

class VisibleSubrectsCache {
public:
  // sc_compute_visible_subrects와 동일 시맨틱(+캐싱). out에 최대 maxOut개 채우고
  // 노출 사각형 개수를 반환. 0이면 완전히 가려진 상태.
  int query(HWND target, RECT target_bounds, RECT *out, int maxOut);

  // 특정 창 / 전체 캐시 무효화. 세대 기반 flush가 주 경로이므로 보통 불필요하나,
  // 명시적 무효화가 필요한 호출처(예: 창 추적 종료)를 위해 제공.
  void invalidate(HWND target);
  void invalidate_all();

  // 진단용(T07): 적중/미스 카운터.
  uint64_t hits() const { return hits_.load(std::memory_order_relaxed); }
  uint64_t misses() const { return misses_.load(std::memory_order_relaxed); }

  // TTL backstop (ms). 이벤트 누락 대비 안전망 — 세대 무효화가 주 경로.
  static constexpr uint64_t kTtlMs = 200;

private:
  static uint64_t make_key(HWND target, const RECT &b);
  static bool rect_equal(const RECT &a, const RECT &b) {
    return a.left == b.left && a.top == b.top && a.right == b.right &&
           a.bottom == b.bottom;
  }

  std::unordered_map<uint64_t, CachedSubrects> cache_;
  mutable std::shared_mutex mutex_;
  // 캐시 전체가 반영하는 세대. 현재 전역 세대와 다르면 전체 flush(메모리 bound +
  // 전역 무효화). 단일 uint64_t — unique_lock에서만 write, shared_lock에서 read.
  uint64_t cacheGen_ = 0;

  std::atomic<uint64_t> hits_{0};
  std::atomic<uint64_t> misses_{0};
};

#endif // _WIN32
