// test_tracker.cpp — VisualTrackerManager 단위 테스트
//
// 빌드/실행:
//   cmake --preset windows-x64 -DBUILD_TESTING=ON
//   cmake --build --preset windows-x64
//   ctest --preset windows-x64 --output-on-failure -R tracker

#include "../src/visual-tracker.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

// ──────────────────────────────────────────────────────────
// 헬퍼: BGRA 프레임 생성
//   배경은 bg_val(기본 30), 박스 영역에 수평 그라디언트(50→230)를 그린다.
//   그라디언트 덕분에 NCC에 필요한 분산이 확보된다.
// ──────────────────────────────────────────────────────────
static std::vector<uint8_t> make_frame(
    int W, int H,
    int rx, int ry, int rw, int rh,
    uint8_t bg_val = 30)
{
    std::vector<uint8_t> f(static_cast<size_t>(W * H * 4), bg_val);
    // alpha 채널 = 255
    for (int i = 3; i < W * H * 4; i += 4) f[i] = 255;

    for (int r = ry; r < ry + rh && r < H; ++r) {
        for (int c = rx; c < rx + rw && c < W; ++c) {
            // 수평 그라디언트: 50 (왼쪽) → 230 (오른쪽)
            const uint8_t v = static_cast<uint8_t>(
                50 + static_cast<int>((c - rx) * 180) / std::max(rw - 1, 1));
            uint8_t* p = f.data() + (r * W + c) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    return f;
}

// 균일 배경 프레임 (NCC = 0 → 트래커 kill 용)
static std::vector<uint8_t> make_blank_frame(int W, int H, uint8_t val = 30)
{
    std::vector<uint8_t> f(static_cast<size_t>(W * H * 4), val);
    for (int i = 3; i < W * H * 4; i += 4) f[i] = 255;
    return f;
}

static int g_passed = 0, g_failed = 0;

static void check(bool ok, const char* name)
{
    if (ok) {
        printf("[PASS] %s\n", name);
        ++g_passed;
    } else {
        printf("[FAIL] %s\n", name);
        ++g_failed;
    }
}

// ──────────────────────────────────────────────────────────
// 테스트 1: 정적 템플릿 — 50 프레임 연속 추적 후 위치 오차 ±3px
// ──────────────────────────────────────────────────────────
static void test_static_template()
{
    constexpr int W = 160, H = 120;
    constexpr int BX = 40, BY = 30, BW = 40, BH = 24;

    VisualTrackerManager mgr;
    auto frame = make_frame(W, H, BX, BY, BW, BH);

    VtOcrBox box{"RRN", (float)BX, (float)BY, (float)BW, (float)BH};
    mgr.register_or_update({box}, frame.data(), W, H, W * 4);

    bool ok = true;
    for (int f = 0; f < 50 && ok; ++f) {
        mgr.update_all(frame.data(), W, H, W * 4);
        auto boxes = mgr.active_boxes();
        if (boxes.empty()) { ok = false; break; }
        const float dx = std::abs(boxes[0].x - BX);
        const float dy = std::abs(boxes[0].y - BY);
        if (dx > 3.0f || dy > 3.0f) ok = false;
    }
    check(ok, "Test 1: Static template 50-frame tracking (±3px)");
}

// ──────────────────────────────────────────────────────────
// 테스트 2: 1px/프레임 이동 — 20 프레임 후 위치 오차 ±6px
// ──────────────────────────────────────────────────────────
static void test_moving_template()
{
    constexpr int W = 200, H = 150;
    constexpr int BW = 40, BH = 24;

    int tx = 20, ty = 20;
    VisualTrackerManager mgr;
    auto frame = make_frame(W, H, tx, ty, BW, BH);

    VtOcrBox box{"PHONE", (float)tx, (float)ty, (float)BW, (float)BH};
    mgr.register_or_update({box}, frame.data(), W, H, W * 4);

    bool ok = true;
    for (int f = 0; f < 20 && ok; ++f) {
        tx += 1; // 1px 오른쪽으로 이동
        frame = make_frame(W, H, tx, ty, BW, BH);
        mgr.update_all(frame.data(), W, H, W * 4);
    }

    auto boxes = mgr.active_boxes();
    if (boxes.empty()) {
        ok = false;
    } else {
        const float dx = std::abs(boxes[0].x - tx);
        const float dy = std::abs(boxes[0].y - ty);
        if (dx > 6.0f || dy > 6.0f) ok = false;
    }
    check(ok, "Test 2: Moving template 1px/frame (±6px after 20 frames)");
}

// ──────────────────────────────────────────────────────────
// 테스트 3: 템플릿 소멸 — FRAMES_LOST 이내에 트래커 제거
// ──────────────────────────────────────────────────────────
static void test_disappear()
{
    constexpr int W = 160, H = 120;
    constexpr int BX = 40, BY = 30, BW = 40, BH = 24;

    VisualTrackerManager mgr;
    auto frame = make_frame(W, H, BX, BY, BW, BH);

    VtOcrBox box{"EMAIL", (float)BX, (float)BY, (float)BW, (float)BH};
    mgr.register_or_update({box}, frame.data(), W, H, W * 4);

    // 균일 배경 프레임 → NCC = 0 everywhere → framesSinceMatch 증가
    auto blank = make_blank_frame(W, H);

    bool removed = false;
    for (int f = 0; f < VisualTrackerManager::FRAMES_LOST + 2; ++f) {
        mgr.update_all(blank.data(), W, H, W * 4);
        if (mgr.active_boxes().empty()) { removed = true; break; }
    }
    check(removed, "Test 3: Disappeared template removed within FRAMES_LOST+2 frames");
}

// ──────────────────────────────────────────────────────────
// 테스트 4: 다른 위치 신규 등록 — 트래커 2개 생성
// ──────────────────────────────────────────────────────────
static void test_new_tracker_at_different_position()
{
    constexpr int W = 320, H = 240;
    constexpr int BW = 40, BH = 24;

    VisualTrackerManager mgr;

    // 첫 번째 박스 등록
    auto frame1 = make_frame(W, H, 10, 10, BW, BH);
    VtOcrBox box1{"RRN", 10.0f, 10.0f, (float)BW, (float)BH};
    mgr.register_or_update({box1}, frame1.data(), W, H, W * 4);

    // 두 번째 박스는 완전히 다른 위치 (IoU = 0)
    // 두 패턴이 모두 있는 프레임
    auto frame2 = make_frame(W, H, 10, 10, BW, BH);
    // 두 번째 패턴 그리기 (200, 160)
    for (int r = 160; r < 160 + BH && r < H; ++r) {
        for (int c = 200; c < 200 + BW && c < W; ++c) {
            const uint8_t v = static_cast<uint8_t>(
                80 + static_cast<int>((c - 200) * 140) / std::max(BW - 1, 1));
            uint8_t* p = frame2.data() + (r * W + c) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }

    VtOcrBox box2{"PHONE", 200.0f, 160.0f, (float)BW, (float)BH};
    mgr.register_or_update({box1, box2}, frame2.data(), W, H, W * 4);

    const auto boxes = mgr.active_boxes();
    check(boxes.size() == 2,
          "Test 4: New tracker registered at different position (expect 2 trackers)");
    if (boxes.size() != 2)
        printf("         → got %zu tracker(s)\n", boxes.size());
}

// ──────────────────────────────────────────────────────────
// 테스트 5: clear() — 모든 트래커 제거
// ──────────────────────────────────────────────────────────
static void test_clear()
{
    constexpr int W = 160, H = 120;
    constexpr int BX = 20, BY = 20, BW = 40, BH = 24;

    VisualTrackerManager mgr;
    auto frame = make_frame(W, H, BX, BY, BW, BH);

    VtOcrBox box{"CARD", (float)BX, (float)BY, (float)BW, (float)BH};
    mgr.register_or_update({box}, frame.data(), W, H, W * 4);

    mgr.clear();
    const bool ok = mgr.active_boxes().empty();
    check(ok, "Test 5: clear() removes all trackers");
}

// ──────────────────────────────────────────────────────────
// 테스트 6: 센터-크롭 대형 박스
//
// 6a: active_boxes()는 캡 전 원본 bw/bh를 반환해야 한다.
// 6b: 50 프레임 연속 생존 (트래커가 잘못 제거되지 않아야 한다).
//
// 참고: 순수 선형 그라디언트는 NCC 불변(I→aI+b)이므로 위치 정확도는
//       Test 1/2에서만 검증하고, 여기서는 크래시/오버킬 없음을 확인한다.
// ──────────────────────────────────────────────────────────
static void test_large_box_center_crop()
{
    constexpr int W = 600, H = 400;
    const int BX = 50, BY = 80;
    const int BW = VisualTrackerManager::MAX_TMPL_W + 80; // 240 (캡 초과)
    const int BH = VisualTrackerManager::MAX_TMPL_H + 20; // 100 (캡 초과)

    VisualTrackerManager mgr;
    auto frame = make_frame(W, H, BX, BY, BW, BH);

    VtOcrBox box{"RRN", (float)BX, (float)BY, (float)BW, (float)BH};
    mgr.register_or_update({box}, frame.data(), W, H, W * 4);

    // 6a: bw/bh는 캡 전 원본 크기여야 함
    auto boxes = mgr.active_boxes();
    bool sizeOk = (!boxes.empty() &&
                   std::abs(boxes[0].w - BW) < 1.0f &&
                   std::abs(boxes[0].h - BH) < 1.0f);
    check(sizeOk, "Test 6a: Large box — active_boxes returns original bw/bh");

    // 6b: 50 프레임 연속 생존 (SCORE_LOST 이상의 NCC가 어딘가에서 발견되어야 함)
    bool survived = true;
    for (int f = 0; f < 50 && survived; ++f) {
        mgr.update_all(frame.data(), W, H, W * 4);
        if (mgr.active_boxes().empty()) { survived = false; break; }
    }
    check(survived, "Test 6b: Large box — center-crop tracker survives 50 frames");
}

int main()
{
    printf("=== VisualTrackerManager Unit Tests ===\n\n");

    test_static_template();
    test_moving_template();
    test_disappear();
    test_new_tracker_at_different_position();
    test_clear();
    test_large_box_center_crop();

    printf("\n%d/%d passed\n", g_passed, g_passed + g_failed);
    return (g_failed == 0) ? 0 : 1;
}
