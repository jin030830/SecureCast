# SecureCast (OBS Privacy Filter Plugin)

OBS Studio 렌더링 파이프라인에서 개인정보 유출을 방지하는 Bounded Exposure 기반 네이티브 필터 플러그인입니다.

## 기능 스펙 (MVP)
* **어플리케이션 차단**: 카카오톡 등 지정 앱 캡처 오버레이
* **지능형 블러**: AI OCR 및 RE2 정규식 기반 위험 정보 선택적 마스킹 
* **N-Frame Render Delay**: 돌발 상황(팝업 등장 등)에서도 AI 검증 시까지 송출을 지연시켜 송출 노출 제로(Zero-Exposure) 보장

## 개발 환경 구축 가이드 (Windows 타겟)
본 프로젝트는 **CMake** 및 **C++20** 기반이며 최종 빌드는 Windows에서 수행합니다.

### 요구사항
- Visual Studio 2022 (Desktop Development with C++)
- CMake 3.28 이상
- OBS Studio 31.1.x

### 빌드 방법 (VS Developer Command Prompt)
```bash
# 1. CMake 설정 (Windows x64 프리셋 사용)
cmake --preset windows-x64

# 2. 빌드
cmake --build --preset windows-x64
```

---
*개발: SecureCast 팀 (OSSP Role A~D 통합 환경)*
