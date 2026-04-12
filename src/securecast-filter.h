#pragma once

#include <obs.h>
#include <obs-module.h>
#include <stdint.h>
#include <atomic>

// ----------------------------------------------------
// Global Configurations (Config)
// ----------------------------------------------------
constexpr int SC_MAX_BLUR_RECTS = 32;       // 최대 동시 마스킹 가능 영역 수
constexpr int SC_RING_BUFFER_SLOTS = 5;     // Bounded Exposure를 위한 송출 지연프레임 수

// ----------------------------------------------------
// Shared Types (Types)
// ----------------------------------------------------

enum class SecurityState {
    SAFE,       // 노출 위험 없음 (초록)
    PARTIAL,    // 위험 감지 후 마스킹 동작 중 (노랑)
    RISK        // 심각한 유출 위험 또는 프레임 오버플로우 (빨강)
};

// 화면 내 개별적인 블러/블랙아웃 처리 영역 구조체
struct BlurRect {
    int x;
    int y;
    int width;
    int height;
    int type; // 0: Blur, 1: Blackout
};

// AI Thread에서 생성한 마스킹 결과를 Render Thread에 전달하기 위한 락프리 버퍼 팩
struct MaskPayload {
    BlurRect rects[SC_MAX_BLUR_RECTS];
    int rectCount;
};

// ----------------------------------------------------
// Core Filter Context
// ----------------------------------------------------

// 필터 상태를 저장하는 최상위 컨텍스트 구조체
// 모든 Role이 참조하는 필터 인스턴스입니다.
struct SecureCastFilter {
    obs_source_t* context;
    
    // UI 및 운영 토글 상태
    bool isActive;
    bool isGameMode;
    SecurityState currentState;
    
    // TODO (Role C): N-Frame 버퍼 리소스 포인터, GPU->CPU 스테이징 텍스처 등 추가
    // TODO (Role C): Lock-free 통신을 위한 atomic 변수 추가
    
    // TODO (Role A): 윈도우 추적 매트릭스, 컴파일된 HLSL 셰이더 포인터 추가
    
    // TODO (Role B): AI 처리 모델 객체, OCR 엔진 포인터 추가
};
