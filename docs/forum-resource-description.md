# OBS 포럼 Resources — Description

이 파일의 본문을 obsproject.com → Resources → Add Resource → **Plugins** 의
Description에 붙여넣습니다. 대괄호 `[ ]` 값(다운로드 링크·태그·VirusTotal 링크)은
게시 직전에 실제 값으로 채웁니다. OBS 포럼은 국제 사용자가 많아 영문 요약을 맨 앞에
두고 한국어 본문을 이어 붙였습니다.

---

**SecureCast — Real-time Privacy (PII) Auto-Blur for OBS**
Windows only · OBS Studio 32.x · GPLv2

SecureCast detects and blurs personal information (emails, phone numbers, Korean RRN,
card numbers, addresses, names) in your captured screen **before it goes out**, using
on-device OCR + regex with an N-frame render delay for zero-exposure. All processing is
local; nothing is sent over the network.
Source: https://github.com/jin030830/SecureCast

---

## 소개

SecureCast는 OBS 렌더링 파이프라인에서 **개인정보(PII)를 실시간으로 감지·블러**하여
방송 화면에 노출되지 않게 하는 네이티브 필터 플러그인입니다. 모든 처리는 **로컬**에서
이루어지며, 외부로 어떤 데이터도 전송하지 않습니다.

## 주요 기능

- **지능형 블러** — 온디바이스 OCR + RE2 정규식으로 이메일·전화번호·주민등록번호·
  카드번호·주소·이름 등 위험 정보만 선택적으로 마스킹합니다.
- **N-Frame Render Delay (Zero-Exposure)** — 팝업 등 돌발 노출도 검증이 끝날 때까지
  송출을 지연시켜 노출을 차단합니다.
- **앱 차단** — 카카오톡 등 지정한 앱 창을 캡처에서 가립니다(게임 모드용 목록 별도 지원).
- **게임 모드 자동 인식** — Steam·Epic·Battle.net 등 게임 실행 시 자동으로 전환합니다.
- **단축키** (OBS 설정 → 단축키에서 재지정 가능):
  - `Ctrl+Shift+F12` — 패닉(전체 가림 토글)
  - `Ctrl+Shift+O` — OCR PII 마스킹 켜기/끄기
  - `Ctrl+Shift+L` — 차단/허용 목록 설정 창 열기

## 요구사항

- **OBS Studio 32.x**
- **Windows 10 (build 19041 / 2004) 이상** — Windows 전용 플러그인입니다.
- **Microsoft Visual C++ 재배포 패키지** — OBS 설치 시 일반적으로 함께 설치됩니다.
- **한국어 OCR 언어팩** — 설치하지 않으면 한국어 PII 블러가 동작하지 않습니다(필수).

### 한국어 OCR 언어팩 설치
Windows **설정 → 시간 및 언어 → 언어 및 지역**에서 한국어를 선택 → **언어 옵션**
(또는 추가 기능)에서 **광학 문자 인식(OCR)** 을 설치합니다.

## 설치

1. 아래 Download에서 zip을 내려받습니다.
2. 압축을 풀어 `securecast` 폴더를 다음 경로에 넣습니다:
   `%ProgramData%\obs-studio\plugins\`
   (보통 `C:\ProgramData\obs-studio\plugins\securecast\` — `ProgramData`는 숨김 폴더입니다.)
3. 폴더 구조가 아래와 같아야 합니다:
   ```
   plugins\securecast\
    ├─ bin\64bit\   (securecast.dll, re2.dll, abseil_dll.dll)
    └─ data\        (effect, locale)
   ```
4. OBS를 재시작한 뒤, 소스를 우클릭 → 필터 → SecureCast를 추가합니다.

`bin\64bit`에 `re2.dll`·`abseil_dll.dll`이 빠지면 "Failed to load plugin" 오류가 납니다.

## 보안 · 백신 안내

전역 입력/창 이벤트 훅, 창·프로세스 열거, 미서명 DLL이라는 특성상 일부 백신이
**오탐(false positive)** 할 수 있습니다. SecureCast는 **소스 전체 공개(GPLv2)**,
**로컬 전용(외부 전송 코드 없음)**, **재현 가능한 CI 빌드**입니다. 백신이 격리하면
설치 폴더를 예외로 추가하거나, 아래 소스·VirusTotal 결과로 확인 후 허용해 주세요.
- VirusTotal: [게시 전 스캔 링크 추가]
- 자세한 내용: 저장소 README의 "보안 · 백신 안내" 참고.

## 라이선스 / 소스

- **GPLv2** (소스 공개 필수)
- Source: https://github.com/jin030830/SecureCast (빌드 태그: `[1.0.0]`)

---

## OBS 포럼 폼 입력값

| 항목 | 값 |
|---|---|
| Title | SecureCast — 실시간 개인정보 자동 블러 (PII Masking) |
| Compatible OBS | 32.x |
| Supported OS | Windows 10 (build 19041 / 2004)+ |
| Download | [GitHub Release zip 링크] |
| Source (GPLv2) | https://github.com/jin030830/SecureCast (+ 빌드 태그) |
| Screenshots | 블러 전/후(더미 PII: 예 010-0000-0000) + 게임 모드 배지/HUD |
