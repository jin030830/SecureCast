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

#include <obs.h>
#include <obs-module.h>
#include <stdint.h>
#include <atomic>

// ----------------------------------------------------
// Global Configurations (Config)
// ----------------------------------------------------
// 컴파일 타임 상수. 런타임에 바꿀 일이 없으므로 constexpr로 둔다.
constexpr int SC_MAX_BLUR_RECTS = 32;       // 한 프레임에 동시에 마스킹 가능한 최대 영역 수
constexpr int SC_RING_BUFFER_SLOTS = 5;     // Bounded Exposure: AI 검증을 위해 N프레임만큼 송출 지연

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

// AI Thread에서 만든 마스킹 결과 → Render Thread로 넘기는 락프리 버퍼 팩.
// Role C가 N-Frame 큐로 여러 슬롯을 굴리며 사용 예정.
struct MaskPayload {
    BlurRect rects[SC_MAX_BLUR_RECTS];
    int rectCount;
};

// ----------------------------------------------------
// Core Filter Context
// ----------------------------------------------------

// 사용자가 OBS에서 필터를 추가할 때마다 securecast_create가 이 구조체를 하나
// bzalloc으로 만들어 OBS가 보관한다. 이후 모든 콜백 (tick/render/destroy) 의
// data 포인터로 다시 들어온다.
//
// 즉, "필터 한 개 = 이 struct 한 개" 의 1:1 매핑.
struct SecureCastFilter {
    obs_source_t* context;          // libobs가 만든 source 핸들 (이 필터 자신)

    // UI 및 운영 토글 상태
    bool isActive;                  // 필터 ON/OFF (사용자 토글)
    bool isGameMode;                // 게임 모드 시 일부 검사 스킵 (Role A 향후)
    SecurityState currentState;     // 현재 위험 상태 (UI 인디케이터)

    // TODO (Role C): N-Frame 버퍼 리소스 포인터, GPU->CPU 스테이징 텍스처 등 추가
    // TODO (Role C): Lock-free 통신을 위한 atomic 변수 추가

    // TODO (Role A): 윈도우 추적 매트릭스, 컴파일된 HLSL 셰이더 포인터 추가
    // window_tracker가 video_tick마다 누산. 0.15초 넘으면 EnumWindows 1회 실행 후 0으로 리셋.
    // 60fps tick에서 매번 EnumWindows 돌리면 CPU 낭비라 throttle 용도.
    float trackerAccumulator;

    // TODO (Role B): AI 처리 모델 객체, OCR 엔진 포인터 추가
};
