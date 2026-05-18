#pragma once

// UI 및 운영 토글 상태
enum class SecurityState {
    SAFE,       // 노출 위험 없음 (초록)
    PARTIAL,    // 위험 감지 후 마스킹 동작 중 (노랑)
    RISK        // 심각한 유출 위험 또는 프레임 오버플로우 (빨강)
};

// 화면 내 개별적인 블러/블랙아웃 처리 영역
struct BlurRect {
    int x;
    int y;
    int width;
    int height;
    int type;       // 0: Blur, 1: Blackout
};
