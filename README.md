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
- OBS Studio 32.x
- Windows 10 (build 19041 / 2004) 이상 — Windows 전용 플러그인

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

OBS Studio 28+의 **신형 플러그인 레이아웃**을 사용합니다.

**설치 경로:** `%ProgramData%\obs-studio\plugins\securecast\`
(보통 `C:\ProgramData\obs-studio\plugins\securecast\` — `ProgramData`는 숨김 폴더)

배포 zip / 설치 폴더 구조:
```
securecast/
 ├─ bin/64bit/
 │    ├─ securecast.dll        (본 플러그인)
 │    ├─ re2.dll               (RE2 라이브러리)
 │    └─ absl_*.dll            (Abseil 런타임 — re2 의존)
 └─ data/
      ├─ locale/en-US.ini
      ├─ securecast_blur.effect
      └─ downsample.effect
```

- **로컬 빌드 시 위 경로로 자동 배포**됩니다 (POST_BUILD; `CMAKE_INSTALL_PREFIX = %ProgramData%\obs-studio\plugins`).
- 배포용 zip은 `cmake --install` 또는 CI(semver 태그 push)가 위 레이아웃으로 생성합니다.

> **⚠️ 점검:** `bin/64bit`에 `re2.dll`과 `absl_*.dll`(Abseil)이 빠지면 OBS 시작 시 "Failed to load plugin" 오류가 발생합니다 (1순위 실패 원인). 이 의존 DLL들이 함께 들어갔는지 반드시 확인하세요.

---

## 보안 · 백신 안내 (Security / Antivirus Notice)

SecureCast는 동작 특성상 일부 백신·보안 솔루션이 **오탐(false positive)** 으로 경고할 수 있습니다. 아래는 그 이유와 투명성 안내입니다.

**왜 오탐이 날 수 있나 (모두 정상 기능 수행을 위한 것):**
- **전역 입력 훅** (스크롤/모션 감지) 및 **WinEvent 훅** (창 생성/이동/최소화/포그라운드 감지) — 가려야 할 창과 화면 변화를 추적하기 위함.
- **창·프로세스 열거 및 레지스트리 읽기** — 차단 대상 앱 및 게임 런처(Steam/Epic 등) 자동 인식을 위함.
- **미서명(unsigned) DLL** — 코드 서명 인증서 미적용 시 휴리스틱 백신이 경고하기 쉬움.

**투명성 보장:**
- **소스 전체 공개 (GPLv2)** — 모든 동작을 코드로 검증할 수 있습니다: `https://github.com/jin030830/SecureCast` (배포 태그 기준).
- **모든 처리는 로컬에서만** 이루어집니다. 네트워크 통신·데이터 외부 전송 코드가 **없습니다** (소스에서 확인 가능).
- **재현 가능한 빌드** — GitHub Actions(CI)가 동일 산출물을 생성합니다.
- **VirusTotal 스캔 결과:** _(배포 시 링크 추가 예정: `https://www.virustotal.com/...`)_

**권장:** 백신이 격리하면 SecureCast 설치 폴더(`%ProgramData%\obs-studio\plugins\securecast\`)를 예외로 추가하거나, 위 소스/VirusTotal 결과로 확인 후 허용해 주세요. 신뢰가 중요하면 직접 빌드해 사용하실 수 있습니다.
