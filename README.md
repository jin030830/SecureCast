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

### 빌드 후 설치 방법 (OBS 연동)

빌드가 완료된 후, 생성된 바이너리와 리소스 파일들을 OBS Studio 설치 경로에 복사해야 합니다. 
(기본 설치 경로: `C:\Program Files\obs-studio`)

#### 1. 플러그인 및 의존성 복사 (총 3개의 `.dll` 파일)
빌드 출력 폴더에 함께 생성된 다음 3개의 DLL 파일을 OBS 플러그인 폴더로 복사합니다.
*   **복사 대상:** 
    *   `securecast.dll` (본 플러그인)
    *   `re2.dll` (RE2 라이브러리)
    *   `abseil_dll.dll` (또는 `absl_*.dll`, Abseil 라이브러리)
*   **대상 경로:** `C:\Program Files\obs-studio\obs-plugins\64bit\`

#### 2. 데이터/효과 파일 복사 (`.effect` 파일)
**※ 중요:** `C:\Program Files\obs-studio\data\obs-plugins\` 경로 아래에 **`securecast` 폴더가 없다면 반드시 직접 생성해야 합니다.**
*   **복사할 소스:** 프로젝트 루트의 `data/` 폴더 내 모든 파일 (예: `blur.effect`, `downsample.effect` 등)
*   **대상 경로:** `C:\Program Files\obs-studio\data\obs-plugins\securecast\`

> **⚠️ 점검:** `obs-plugins/64bit` 경로에 `re2.dll`이나 `abseil` 관련 의존 라이브러리가 누락될 경우 OBS 시작 시 "Failed to load plugin" 오류가 발생하므로, 위 3종의 DLL이 모두 정상 복사되었는지 확인해 주세요.
