# 🛡️ SecureCast 통합 구현 가이드 (Full Version)

> **프로젝트 성격**: OBS Studio용 고성능 C++ 네이티브 보안 필터 플러그인
> **핵심 가치**: 실수할 수 있는 인간을 기술(N-Frame Delay + AI)로 완벽하게 보호합니다.

본 문서는 SecureCast 프로젝트의 4개 Role(A, B, C, D)에 대한 모든 상세 구현 지침을 하나로 통합한 문서입니다. 각 담당자는 자신의 영역뿐만 아니라 타 영역과의 데이터 흐름을 이해하는 용도로 본 문서를 활용하시기 바랍니다.

---

## 📑 목차
1. [🔧 Role A — 플러그인 코어 & 블러 렌더링](#-role-a--플러그인-코어--블러-렌더링)
2. [🧠 Role B — 온디바이스 AI 엔진](#-role-b--온디바이스-ai-엔진)
3. [⚙️ Role C — GPU 파이프라인 & 스레드 동기화](#-role-c--gpu-파이프라인--스레드-동기화)
4. [🎨 Role D — UI/UX & 시스템 연동](#-role-d--uiux--시스템-연동)

---

<br>

# 🔧 Role A — 플러그인 코어 & 블러 렌더링

> **담당 기능**: ① 블랙리스트 앱 차단, ⑤ 패닉 버튼, ⑦ 게임 모드 감지, 좌표 변환, HLSL 렌더링
> **난이도**: ★★★★★ (프로젝트에서 가장 어려운 파트)

## 1. OBS Filter 플러그인 기본 구조 (뼈대)

### 1.1 무엇을 해야 하나요?
OBS Studio는 플러그인을 **DLL 파일** 형태로 로드합니다. 우리는 OBS의 "비디오 필터(Video Filter)"를 만들어야 합니다. 필터란, OBS가 매 프레임(1초에 60번)마다 화면을 그릴 때 **"이 프레임을 가공해줘"**라고 우리 코드를 호출하는 구조입니다.

### 1.2 사용할 API / 함수

```cpp
// 1) 플러그인 진입점 — OBS가 DLL을 로드할 때 가장 먼저 호출
bool obs_module_load(void) {
    // 여기서 우리 필터를 OBS에 등록합니다
    obs_register_source(&securecast_filter_info);
    return true;
}

// 2) 필터 정보 구조체 — "우리 필터는 이런 녀석이에요"라고 OBS에 알려주는 명세서
struct obs_source_info securecast_filter_info = {
    .id           = "securecast_filter",        // 고유 ID
    .type         = OBS_SOURCE_TYPE_FILTER,     // "나는 필터야"
    .output_flags = OBS_SOURCE_VIDEO,           // "비디오를 건드릴 거야"
    .get_name     = securecast_get_name,        // 필터 이름 반환
    .create       = securecast_create,          // 필터 생성 시 초기화
    .destroy      = securecast_destroy,         // 필터 제거 시 정리
    .video_render = securecast_video_render,    // ⭐ 매 프레임마다 호출되는 핵심 함수
    .get_properties = securecast_get_properties // 설정 UI
};

// 3) 핵심 렌더링 함수 — 1초에 60번 호출됨
void securecast_video_render(void *data, gs_effect_t *effect) {
    // 여기서 블러/블랙아웃을 실제로 적용합니다
    // ① 부모 소스의 프레임을 가져온다
    // ② 블랙리스트 앱 좌표를 확인한다
    // ③ HLSL 셰이더로 해당 영역에 블러/블랙아웃을 그린다
    // ④ 결과를 OBS 인코더에 넘긴다
}
```

### 1.3 빌드 환경
- **CMake** + **Visual Studio 2022** (MSVC 컴파일러)
- OBS Studio 소스코드에서 `libobs` 헤더 파일 필요
- 빌드 결과물: `securecast.dll` → OBS 플러그인 폴더에 복사

### 1.4 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **OBS 크래시** | `video_render` 안에서 예외 발생 시 OBS 전체가 죽음 | 모든 코드를 `try-catch`로 감싸고, 실패 시 블러 없이 원본 프레임 패스스루 |
| **NULL 포인터** | 필터가 연결된 소스가 없는 상태에서 `video_render` 호출 | `obs_filter_get_parent()` 반환값을 반드시 NULL 체크 |
| **리소스 누수** | GPU 텍스처를 매 프레임 생성하면 VRAM 폭발 | `create` 시점에 한 번만 할당, `destroy`에서 반드시 해제 |

## 2. 윈도우 추적 (블랙리스트 앱 찾기)

### 2.1 무엇을 해야 하나요?
"카카오톡 창이 지금 화면 어디에 있는지"를 알아내야 합니다. Windows에는 현재 열려있는 모든 창을 순회하는 API가 있습니다.

### 2.2 사용할 API (순서대로)

```cpp
// === STEP 1: 모든 창을 순회하면서 블랙리스트 앱 찾기 ===

// EnumWindows: 현재 열린 모든 최상위 윈도우를 하나씩 순회
// → 콜백 함수가 각 윈도우마다 한 번씩 호출됩니다
EnumWindows(MyEnumCallback, 0);

BOOL CALLBACK MyEnumCallback(HWND hwnd, LPARAM lParam) {
    // 보이지 않는 창은 무시
    if (!IsWindowVisible(hwnd)) return TRUE;
    
    // === STEP 2: 이 창의 실행 파일 이름 가져오기 ===
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    WCHAR exePath[MAX_PATH];
    DWORD size = MAX_PATH;
    QueryFullProcessImageNameW(hProc, 0, exePath, &size);
    CloseHandle(hProc);
    
    // exePath 예시: "C:\\Program Files\\KakaoTalk\\KakaoTalk.exe"
    // 파일명만 추출: "KakaoTalk.exe"
    
    // === STEP 3: 블랙리스트에 있는지 확인 ===
    // blacklist = {"KakaoTalk.exe", "Discord.exe", "Slack.exe", ...}
    if (blacklist.contains(exeName)) {
        // 찾았다! 이 창의 좌표를 가져오자
    }
    
    return TRUE; // 다음 창으로 계속 순회
}
```

```cpp
// === STEP 4: 창의 정확한 좌표 가져오기 ===

// ❌ 잘못된 방법: GetWindowRect
// → 윈도우 그림자, 투명 테두리까지 포함해서 좌표가 실제보다 큼

// ✅ 올바른 방법: DwmGetWindowAttribute + DWMWA_EXTENDED_FRAME_BOUNDS
// → 실제로 보이는 영역만 정확하게 반환
RECT rect;
DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
// rect.left, rect.top, rect.right, rect.bottom → 픽셀 좌표
```

### 2.3 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **유령 창** | 안티치트(Vanguard), 게임 런처 등이 1x1 픽셀짜리 투명 창을 만듦 | 창 크기가 100x100 미만이면 무시하는 임계값 필터 적용 |
| **UWP 앱** | Windows Store 앱(UWP)은 `QueryFullProcessImageName`이 다른 경로 반환 | `ApplicationFrameHost.exe`일 경우 내부 자식 윈도우의 실제 프로세스를 추가 탐색 |
| **관리자 권한 창** | 관리자 권한으로 실행된 앱은 `OpenProcess`가 실패할 수 있음 | `PROCESS_QUERY_LIMITED_INFORMATION` 플래그 사용 (일반 권한에서도 동작) |
| **성능** | `EnumWindows`를 매 프레임(60fps) 호출하면 CPU 낭비 | 100~200ms 간격의 타이머로 호출 주기 제한 |

## 3. 좌표 변환 (Matrix Transform)

### 3.1 무엇을 해야 하나요?
Windows에서 가져온 좌표(예: 카카오톡 창이 모니터 왼쪽 위 100, 200에 있다)를 **OBS 캔버스 안에서의 좌표**로 변환해야 합니다. OBS는 화면을 캡처할 때 크기를 줄이거나, 위치를 옮기거나, 크롭(자르기)할 수 있기 때문입니다.

### 3.2 변환 과정 (3단계)

```
[Windows 절대 좌표] → [모니터 상대 좌표] → [OBS Source 좌표] → [최종 셰이더 UV 좌표]
```

```cpp
// === STEP 1: 모니터 기준 상대 좌표로 변환 ===
// Windows 좌표는 "전체 가상 데스크톱" 기준입니다
// 듀얼 모니터면 왼쪽 모니터가 (0,0)~(1920,1080),
// 오른쪽 모니터가 (1920,0)~(3840,1080) 이런 식
HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
MONITORINFO mi = { sizeof(mi) };
GetMonitorInfo(hMon, &mi);
// 모니터 왼쪽 위를 (0,0)으로 만들기
int relX = rect.left - mi.rcMonitor.left;
int relY = rect.top  - mi.rcMonitor.top;

// === STEP 2: DPI 보정 ===
// 4K 모니터에서 배율이 150%면, 좌표값이 1.5배 뻥튀기됨
UINT dpiX, dpiY;
GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
float scale = dpiX / 96.0f; // 96 DPI = 100% 배율
relX = (int)(relX / scale);
relY = (int)(relY / scale);

// === STEP 3: OBS Source 좌표로 변환 ===
// OBS에서 소스 아이템의 변환 행렬(위치, 크기, 회전)을 가져옴
struct matrix4 transform;
obs_sceneitem_get_box_transform(scene_item, &transform);
// 이 행렬의 역행렬(inverse)을 곱하면
// "Windows 좌표 → OBS 소스 내부 좌표"로 변환 가능
struct matrix4 inv;
matrix4_inv(&inv, &transform);
// (relX, relY)에 inv를 곱하면 OBS 소스 UV 좌표가 나옵니다
```

### 3.3 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **DPI 혼합** | 모니터1=100%, 모니터2=150%일 때 좌표가 틀어짐 | 각 모니터별로 `GetDpiForMonitor`를 개별 호출하여 보정 |
| **OBS 소스 크롭** | 사용자가 소스를 잘라서(Crop) 사용 중이면 좌표 오프셋 발생 | `obs_sceneitem_crop` 값을 가져와서 추가 보정 |
| **회전된 소스** | OBS에서 소스를 45도 회전하면 직사각형 BBox가 안 맞음 | 역행렬 변환이 회전까지 자동으로 처리하므로, 반드시 행렬 연산 사용 |
| **소스를 못 찾음** | OBS에 여러 씬/소스가 있을 때, 어느 소스가 캡처 소스인지 판별 | `obs_frontend_get_current_scene()`로 현재 활성 씬을 가져온 뒤, 그 안의 아이템을 순회 |

## 4. HLSL 셰이더 렌더링 (블러 & 블랙아웃)

### 4.1 무엇을 해야 하나요?
OBS는 GPU에서 화면을 그립니다. 우리가 "이 영역을 블러처리 해줘"라고 하려면 **HLSL(High-Level Shading Language)**라는 GPU 전용 프로그래밍 언어로 셰이더를 작성해야 합니다.

### 4.2 HLSL 셰이더 코드

```hlsl
// === securecast_blur.effect (OBS 셰이더 파일) ===

uniform float4x4 ViewProj;     // OBS 카메라 행렬
uniform texture2d image;        // 원본 프레임 텍스처

// 블러 영역 정보 (최대 32개까지 동시 처리)
uniform float4 blur_rects[32];  // (x, y, width, height) - UV 좌표 (0.0~1.0)
uniform int    blur_count;      // 현재 블러 영역 수
uniform int    blur_type[32];   // 0=Gaussian Blur, 1=Blackout(완전 검정)

sampler_state texSampler {
    Filter   = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

// 이 픽셀이 블러 영역 안에 있는지 확인
bool is_in_rect(float2 uv, float4 rect) {
    return uv.x >= rect.x && uv.x <= (rect.x + rect.z) &&
           uv.y >= rect.y && uv.y <= (rect.y + rect.w);
}

// Gaussian Blur — 주변 9개 픽셀의 평균을 계산하여 흐리게 만듦
float4 gaussian_blur(float2 uv, float2 texel_size) {
    float4 sum = float4(0, 0, 0, 0);
    float weights[9] = {0.05, 0.09, 0.12, 0.15, 0.18, 0.15, 0.12, 0.09, 0.05};
    for (int i = -4; i <= 4; i++) {
        sum += image.Sample(texSampler, uv + float2(i * texel_size.x * 3.0, 0)) * weights[i+4];
    }
    return sum;
}

float4 mainImage(VertData v_in) : TARGET {
    float2 uv = v_in.uv;
    
    for (int i = 0; i < blur_count; i++) {
        // BBox를 15% 팽창(Inflation) — 안전 마진
        float4 inflated = blur_rects[i];
        inflated.x -= inflated.z * 0.075;
        inflated.y -= inflated.w * 0.075;
        inflated.z *= 1.15;
        inflated.w *= 1.15;
        
        if (is_in_rect(uv, inflated)) {
            if (blur_type[i] == 1) {
                return float4(0, 0, 0, 1); // 블랙아웃: 완전 검은색
            }
            float2 texel = float2(1.0/1920.0, 1.0/1080.0);
            return gaussian_blur(uv, texel); // 가우시안 블러
        }
    }
    
    return image.Sample(texSampler, uv); // 블러 영역 밖: 원본 그대로
}
```

### 4.3 C++ 코드에서 셰이더 사용법

```cpp
void securecast_video_render(void *data, gs_effect_t *effect) {
    SecureCastFilter *filter = (SecureCastFilter *)data;

    // 1) 부모 소스가 없으면 아무것도 안 함
    obs_source_t *parent = obs_filter_get_parent(filter->source);
    if (!parent) return;

    // 2) 우리가 만든 셰이더 로드
    gs_effect_t *blur_effect = filter->blur_effect;

    // 3) 블러 영역 좌표를 셰이더에 전달
    gs_effect_set_int(gs_effect_get_param_by_name(blur_effect, "blur_count"), 
                      filter->active_blur_count);
    for (int i = 0; i < filter->active_blur_count; i++) {
        // blur_rects[i]에 UV 좌표 전달
    }

    // 4) 셰이더를 적용하며 렌더링
    obs_source_process_filter_begin(filter->source, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING);
    obs_source_process_filter_end(filter->source, blur_effect, 0, 0);
}
```

### 4.4 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **셰이더 컴파일 실패** | 사용자 GPU가 HLSL 5.0을 지원하지 않을 수 있음 | `gs_effect_create_from_file` 반환값 NULL 체크, 실패 시 소프트웨어 폴백 |
| **블러 강도 부족** | 9-tap Gaussian으로는 고해상도에서 텍스트가 읽힘 | Two-pass(가로→세로) Gaussian으로 확장하여 블러 반경 증가 |
| **BBox 경계 잘림** | 15% 팽창(Inflation) 시 화면 밖으로 나가면 GPU 에러 | `clamp(0.0, 1.0)`으로 UV 좌표를 제한 |

## 5. 게임 모드 감지

### 5.1 사용할 API

```cpp
// === 전체 화면 게임 감지 로직 ===
bool IsFullscreenGame(HWND hwnd) {
    // 방법 1: 윈도우 스타일 확인
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    bool noBorder = !(style & WS_OVERLAPPEDWINDOW); // 테두리 없는 창
    
    // 방법 2: 창 크기가 모니터 전체를 덮는지 확인
    RECT wndRect;
    GetWindowRect(hwnd, &wndRect);
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMon, &mi);
    
    bool coversScreen = (wndRect.left <= mi.rcMonitor.left &&
                         wndRect.top <= mi.rcMonitor.top &&
                         wndRect.right >= mi.rcMonitor.right &&
                         wndRect.bottom >= mi.rcMonitor.bottom);
    
    return noBorder && coversScreen;
}

// === Partial Sleep: 게임 모드 중에도 유지되는 감시 ===
// SetWinEventHook로 윈도우 변경 이벤트를 항상 수신
g_hook = SetWinEventHook(
    EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
    NULL, WinEventCallback, 0, 0, 
    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
);

void CALLBACK WinEventCallback(HWINEVENTHOOK, DWORD event, HWND hwnd, ...) {
    // 포그라운드 윈도우가 바뀌었다!
    // → 블랙리스트 앱인지 체크
    // → 맞으면 즉시 AI Thread Wake-up
    if (IsBlacklistedApp(hwnd)) {
        ExitGameMode(); // 게임 모드 해제 + 분석 재개
    }
}
```

### 5.2 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **Borderless Windowed** | 테두리 없는 창모드 게임은 일반 창과 구분이 어려움 | 화면 전체를 덮으면서 + 프로세스 이름이 알려진 게임이면 게임으로 판정 |
| **게임 내 오버레이** | Discord/Steam 오버레이는 게임 위에 별도 윈도우로 뜸 | `SetWinEventHook`이 감지 → 오버레이 창의 프로세스가 블랙리스트면 AI 즉시 깨움 |
| **Alt-Tab 순간** | 게임→데스크톱 전환 시 0.1초간 빈 화면 노출 가능 | 전환 감지 후 첫 5프레임은 Sticky Mask(이전 마스크 유지) 적용 |

## 6. 패닉 버튼

### 6.1 구현 방법

```cpp
// OBS 핫키 등록
obs_hotkey_id panic_hotkey = obs_hotkey_register_source(
    source, "securecast_panic", "SecureCast 패닉 버튼",
    PanicButtonCallback, data
);

void PanicButtonCallback(void *data, obs_hotkey_id id, obs_hotkey_t *key, bool pressed) {
    if (pressed) {
        SecureCastFilter *filter = (SecureCastFilter *)data;
        filter->panic_mode = !filter->panic_mode; // 토글
        // panic_mode == true면 video_render에서 전체 화면에 | KPI | 목표 수치 | 달성 방법 |
|---|---|---|
| **CPU 점유율 (정적 화면)** | **2.0% 미만** | 적응형 Dirty Rect Skip + 64x64 다운샘플 해싱 검증 |
| **CPU 점유율 (동적 화면)** | **최대 8.0%** | 하드웨어 가속 OCR 활용 및 불필요한 영역 연산 배제 |
| **탐지 재현율 (Recall)** | **95% 이상** | Windows.Media.Ocr + RE2 한국어 특화 패턴 (보수적 탐지 우선) |
| **오탐률 (False Positive)** | **15% 이내** | 휴리스틱 필터 및 BBox 문맥 2차 검증 |
| **방송 송출 지연 (Delay)** | **80ms 고정** | N-Frame Delay Queue (5프레임 @ 60fps) |
| **Data Privacy** | **100% On-device** | 로컬 처리 및 분석 후 즉시 메모리 해제 |
### 6.2 ⚠️ 주의사항
- 핫키가 게임의 키바인딩과 겹치지 않도록 기본값을 `Ctrl+Shift+F12` 같은 조합으로 설정
- 패닉 모드 진입/해제 시 시각적 피드백(화면 가장자리 빨간 테두리 등)을 줘야 스트리머가 현재 상태를 인지 가능

---

# 🧠 Role B — 온디바이스 AI 엔진

> **담당 기능**: ② 위험 키워드 블러 (OCR + 정규식 + 휴리스틱 필터)

## 1. Windows.Media.Ocr — 텍스트 인식

### 1.1 무엇을 해야 하나요?
화면에서 "글자가 어디에 있고, 무슨 내용인지"를 알아내야 합니다. 우리는 Windows 10/11에 기본 내장된 **Windows.Media.Ocr** API를 사용합니다. 별도 라이브러리 설치가 필요 없고, GPU 가속도 자동으로 됩니다.

### 1.2 C++에서 WinRT API 사용법

```cpp
// WinRT 헤더 (C++/WinRT)
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Globalization.h>

using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Globalization;

// === STEP 0: 언어팩 사전 검증 (플러그인 초기화 시 1회) ===
bool CheckOcrLanguage() {
    Language lang(L"ko"); // 한국어
    bool supported = OcrEngine::IsLanguageSupported(lang);
    if (!supported) {
        // ⚠️ 사용자에게 "Windows 설정 > 언어 > 한국어 언어 팩 설치" 안내
        ShowLanguagePackGuide();
        return false;
    }
    return true;
}

// === STEP 1: OCR 엔진 생성 (초기화 시 1회) ===
Language lang(L"ko");
OcrEngine engine = OcrEngine::TryCreateFromLanguage(lang);
// engine이 nullptr이면 언어팩 미설치 상태

// === STEP 2: 비트맵을 넘겨서 텍스트 추출 ===
// GPU에서 가져온 프레임 데이터를 SoftwareBitmap으로 변환해야 함
SoftwareBitmap bitmap = SoftwareBitmap::CreateCopyFromBuffer(
    pixelBuffer,                    // 픽셀 데이터 (IBuffer)
    BitmapPixelFormat::Bgra8,       // 포맷
    width, height                   // 크기
);

// OCR 실행 (비동기)
OcrResult result = co_await engine.RecognizeAsync(bitmap);

// === STEP 3: 결과에서 텍스트와 좌표 추출 ===
for (auto const& line : result.Lines()) {
    for (auto const& word : line.Words()) {
        // 텍스트 내용
        winrt::hstring text = word.Text();
        
        // 텍스트의 화면상 위치 (픽셀 좌표)
        auto bounds = word.BoundingRect();
        // bounds.X, bounds.Y, bounds.Width, bounds.Height
        
        // 이 text를 정규식으로 검사합니다 (STEP 4로)
    }
}
```

### 1.3 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **언어팩 미설치** | 한국어 OCR이 안 되면 아무것도 탐지 못함 | `IsLanguageSupported` 사전 체크 + UI 가이드 |
| **비동기 실행** | `RecognizeAsync`는 코루틴(비동기)으로 동작 | AI Thread에서만 호출, Fast-Path(렌더 스레드)에서 절대 호출 금지 |
| **Data Isolation** | 개인정보가 담긴 `SoftwareBitmap`이 메모리에 남을 위험 | 분석 완료 직후 링 버퍼 내 비트맵 데이터 `ZeroMemory` 처리 후 해제 |
| **인식률 향상** | "0"을 "O"로 잘못 인식하여 정규식 우회 시도 | 정규식 적용 전 `CommonOcrFixer`를 통해 유사 문자 정규화 처리 (Recall 향상) |
| **한영 혼합** | "배달주소: 서울특별시 Seoul" 같은 혼합 텍스트 | 한국어 엔진 + 영어 엔진 2개를 번갈아 실행하거나, 영어 우선 엔진 사용 |
| **인식 지연** | 고해상도(4K)에서 전체 화면 OCR은 200ms+ 소요 | 전체 프레임 대신 변화 영역(ROI)만 잘라서 분석 — Dirty Rect Skip |

## 2. Google RE2 — 정규표현식 패턴 매칭

### 2.1 무엇을 해야 하나요?
OCR이 추출한 텍스트에서 "이게 주민번호인지, 전화번호인지, 계좌번호인지"를 **규칙(패턴)**으로 찾아냅니다.

### 2.2 왜 RE2를 쓰나요?
일반 정규식 엔진(C++ std::regex)은 특정 패턴에서 **무한 루프에 빠지는 버그(ReDoS)**가 있습니다. Google RE2는 이 문제가 구조적으로 발생하지 않으며, 속도도 훨씬 빠릅니다.

### 2.3 구현 코드

```cpp
#include <re2/re2.h>

// === 정규식 패턴 정의 (초기화 시 1회 컴파일) ===

// 1) 주민등록번호: 6자리-7자리 (뒷자리 첫글자 1~4)
static const re2::RE2 PATTERN_RRN(R"(\d{6}[-–]\s?[1-4]\d{6})");

// 2) 전화번호: 010-XXXX-XXXX 또는 02-XXX-XXXX 등
static const re2::RE2 PATTERN_PHONE(
    R"((?:010|011|016|017|018|019|02|0\d{2})[-.\s]?\d{3,4}[-.\s]?\d{4})"
);

// 3) 이메일: xxx@xxx.xxx
static const re2::RE2 PATTERN_EMAIL(
    R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})"
);

// 4) 신용카드: 4자리-4자리-4자리-4자리
static const re2::RE2 PATTERN_CARD(
    R"(\d{4}[-\s]?\d{4}[-\s]?\d{4}[-\s]?\d{4})"
);

// 5) IP 주소: xxx.xxx.xxx.xxx
static const re2::RE2 PATTERN_IP(
    R"(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})"
);

// 6) 계좌번호: 은행별 자릿수 패턴 (예: 3-2-6, 3-3-6 등)
static const re2::RE2 PATTERN_ACCOUNT(
    R"(\d{3,4}[-\s]?\d{2,4}[-\s]?\d{4,6}[-\s]?\d{0,3})"
);

// === OCR 결과에 대해 패턴 매칭 실행 ===
struct PiiMatch {
    std::string type;   // "RRN", "PHONE", "EMAIL" 등
    float x, y, w, h;  // 화면 좌표 (BBox)
};

std::vector<PiiMatch> ScanForPII(const OcrResult& ocrResult) {
    std::vector<PiiMatch> matches;
    
    for (auto const& line : ocrResult.Lines()) {
        std::string lineText = winrt::to_string(line.Text());
        auto lineBounds = line.Words().GetAt(0).BoundingRect(); // 줄 전체 좌표
        
        if (RE2::PartialMatch(lineText, PATTERN_RRN)) {
            matches.push_back({"RRN", lineBounds.X, lineBounds.Y, 
                              lineBounds.Width, lineBounds.Height});
        }
        if (RE2::PartialMatch(lineText, PATTERN_PHONE)) {
            matches.push_back({"PHONE", ...});
        }
        // ... 나머지 패턴도 동일하게 검사
    }
    return matches;
}
```

### 2.4 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **오탐 (False Positive)** | 게임 점수 "123456-1234567"이 주민번호로 잡힘 | 휴리스틱 필터(2단계)에서 윈도우 타이틀/프로세스 컨텍스트로 2차 판별 |
| **미탐 (False Negative)** | OCR이 "0"을 "O"로 인식하면 패턴 매칭 실패 | OCR 후처리에서 O→0, l→1 등 일반적인 OCR 오류 보정 적용 |
| **성능** | 6개 패턴을 매번 돌리면 느릴 수 있음 | RE2는 컴파일된 패턴을 재사용하므로 초기화 시 한 번만 컴파일, 이후 매칭은 마이크로초 단위 |
| **국제 번호** | "+82-10-1234-5678" 같은 국제 형식 | 패턴에 `(?:\+82[-\s]?)?` 접두사 추가 |

## 3. 휴리스틱 필터 (2단계 보정)

### 3.1 무엇을 해야 하나요?
정규식이 "이건 전화번호 같다"고 잡아왔을 때, **진짜 전화번호인지 게임 점수인지**를 한 번 더 걸러냅니다.

### 3.2 구현 로직

```cpp
bool HeuristicFilter(const PiiMatch& match, HWND sourceWindow) {
    // === 규칙 1: 프로세스 이름 기반 화이트리스트 ===
    // 게임 프로세스에서 나온 숫자는 개인정보가 아닐 확률 높음
    std::string processName = GetProcessName(sourceWindow);
    std::vector<std::string> gameWhitelist = {
        "League of Legends.exe", "VALORANT.exe", "Overwatch.exe"
    };
    if (contains(gameWhitelist, processName) && match.type != "RRN") {
        return false; // 주민번호가 아닌 경우 게임 내 숫자는 무시
    }
    
    // === 규칙 2: 윈도우 타이틀 컨텍스트 ===
    std::wstring title = GetWindowTitle(sourceWindow);
    // "배달의민족", "주소", "결제" 같은 키워드가 타이틀에 있으면 고위험
    if (containsAny(title, {"배달", "주소", "결제", "계좌", "카카오"})) {
        return true; // 무조건 블러 (높은 위험)
    }
    
    // === 규칙 3: 주변 텍스트 컨텍스트 ===
    // 매칭된 텍스트 주변에 "주소:", "Tel:", "계좌번호" 같은 레이블이 있으면 진짜
    // 이 부분은 V2에서 ONNX로 고도화 예정
    
    return true; // 기본적으로 보수적으로 판단 (의심되면 블러)
}
```

### 3.3 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **화이트리스트 관리** | 게임이 너무 많아서 모든 게임을 등록할 수 없음 | 게임 모드(Partial Sleep) 시에는 오탐 필터가 자동으로 완화됨 |
| **보수적 판단 vs 편의성** | 너무 보수적이면 화면 곳곳에 블러가 걸려 시청 경험 저하 | 사용자가 "민감도" 슬라이더를 조절할 수 있게 설정 UI 제공 |

## 4. 적응형 Dirty Rect Skip (성능 최적화)

### 4.1 무엇을 해야 하나요?
매 프레임(60fps) 전체 화면을 OCR 돌리면 CPU가 폭발합니다. **"화면에서 바뀐 부분만"** 골라서 OCR을 실행해야 합니다.

### 4.2 구현 로직

```cpp
// === 이전 프레임과 현재 프레임 비교 (FNV-1a Hash 기반) ===
// 1) 64x64로 다운샘플링된 프레임 데이터의 64-bit 해시 계산
uint64_t curr_hash = ComputeFNV1aHash(curr_64x64_data, 4096);

// 2) 해시 비교
if (curr_hash == prev_hash) {
    // 완전히 동일함 (변화 없음) → OCR 스킵 및 이전 마스킹 캐시 재사용 (CPU 절약)
    return;
} else {
    // 화면에 변화가 감지됨 (스크롤, 앱 전환 등)
    // 픽셀 차분을 통해 구체적인 변화 영역(Dirty Rect)을 추출
    auto changedRects = DetectChangedRegions(prev_64x64_data, curr_64x64_data);
    for (auto& rect : changedRects) {
        RunROI_OCR(rect); // 해당 영역만 OCR 수행
    }
    prev_hash = curr_hash;
}
```

### 4.3 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **스크롤/동영상** | 미세하게 화면이 움직이거나 영상이 재생되면 매 프레임 해시가 달라져 캐시 효율 하락 | 전체 화면 해싱과 더불어, 고정된 UI 영역(채팅창, 결제창) BBox별 개별 해싱 2중 체크 |
| **깜빡이는 UI** | 커서 깜빡임, 광고 배너 등이 지속적으로 변화를 유발 | 변화량 임계치를 설정하여 "의미 있는 변화"만 반응하도록 조절 |

---

# ⚙️ Role C — GPU 파이프라인 & 스레드 동기화

> **담당 기능**: N-Frame Ring Buffer, GPU→CPU Readback, Lock-free 동기화, Frame Freeze

## 1. N-Frame Ring Buffer

### 1.1 무엇을 해야 하나요?
"방송 화면을 3~5프레임만큼 늦게 내보내는 버퍼"를 만들어야 합니다. 이 버퍼 덕분에 AI가 미리 검사할 시간을 벌 수 있습니다. **링 버퍼(Ring Buffer)**란, 원형으로 되어 있어서 끝에 다다르면 처음으로 돌아가는 배열입니다.

### 1.2 구현 코드

```cpp
// === Ring Buffer 구조체 ===
struct RingBuffer {
    static const int SLOT_COUNT = 5;  // 5프레임 지연
    gs_texture_t* slots[SLOT_COUNT];  // GPU 텍스처 배열
    int write_idx = 0;                // 새 프레임을 넣는 위치
    int read_idx  = 0;                // 인코더에 보낼 프레임 위치
    bool is_full  = false;            // 버퍼가 다 찬 상태인지
};

// === 초기화: GPU 텍스처 5개 미리 할당 ===
void InitRingBuffer(RingBuffer* rb, uint32_t width, uint32_t height) {
    obs_enter_graphics(); // OBS GPU 컨텍스트 진입
    for (int i = 0; i < RingBuffer::SLOT_COUNT; i++) {
        rb->slots[i] = gs_texture_create(
            width, height, GS_RGBA, 1, NULL, GS_RENDER_TARGET
        );
        // GS_RENDER_TARGET: 이 텍스처에 렌더링(복사)할 수 있게 설정
    }
    obs_leave_graphics();
}

// === 프레임 저장 (video_render에서 매 프레임 호출) ===
void PushFrame(RingBuffer* rb, gs_texture_t* current_frame) {
    // 현재 프레임을 write_idx 슬롯에 복사
    gs_copy_texture(rb->slots[rb->write_idx], current_frame);
    
    // write_idx를 한 칸 앞으로 (원형으로 순환)
    rb->write_idx = (rb->write_idx + 1) % RingBuffer::SLOT_COUNT;
    
    // 버퍼가 다 찼는지 체크
    if (rb->write_idx == rb->read_idx) {
        rb->is_full = true;
    }
}

// === 가장 오래된 프레임 꺼내기 (인코더에 전달) ===
gs_texture_t* PopFrame(RingBuffer* rb) {
    if (!rb->is_full) {
        return NULL; // 아직 5프레임이 안 쌓임 → 대기
    }
    
    gs_texture_t* old_frame = rb->slots[rb->read_idx];
    rb->read_idx = (rb->read_idx + 1) % RingBuffer::SLOT_COUNT;
    rb->is_full = false; // 한 칸 비었으니까
    
    return old_frame; // 이 프레임에 블러를 적용한 뒤 인코더에 전달
}
```

### 1.3 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **VRAM 부족** | 1080p 기준 슬롯당 ~8MB, 5슬롯 = ~40MB | 4K에서는 슬롯당 ~32MB → 슬롯 수를 3으로 줄이거나, 해상도 축소 후 저장 |
| **초기 빈 프레임** | 방송 시작 직후 5프레임이 안 쌓여서 PopFrame이 NULL 반환 | 버퍼가 채워질 때까지는 프레임을 바로 패스스루(지연 없이 전달) |
| **게임 모드 전환** | 게임 모드 진입 시 Ring Buffer를 비활성화해야 함 | `bypass` 플래그를 두고, true이면 PushFrame/PopFrame을 생략하고 원본 프레임 직통 전달 |

## 2. GPU→CPU Readback (비동기 복사)

### 2.1 무엇을 해야 하나요?
AI(OCR)는 CPU에서 동작하는데, 화면 프레임은 GPU에 있습니다. **GPU의 데이터를 CPU로 가져오는 것**을 Readback이라고 합니다. 이 과정이 느리면 전체 파이프라인이 병목됩니다.

### 2.2 구현 코드 (D3D11 기반)

```cpp
// === STEP 1: 스테이징 텍스처 생성 (CPU가 읽을 수 있는 임시 텍스처) ===
D3D11_TEXTURE2D_DESC stagingDesc = {};
stagingDesc.Width  = 64;  // 원본 BBox 영역을 64x64로 축소
stagingDesc.Height = 64;
stagingDesc.Format = DXGI_FORMAT_R8_UNORM; // 그레이스케일 (1바이트/픽셀)
stagingDesc.Usage  = D3D11_USAGE_STAGING;  // CPU 접근 가능
stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
stagingDesc.MipLevels = 1;
stagingDesc.ArraySize = 1;
stagingDesc.SampleDesc.Count = 1;

ID3D11Texture2D* stagingTexture;
device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);

// === STEP 2: GPU에서 BBox 영역을 64x64로 축소 복사 ===
// 원본 텍스처의 특정 영역(BBox)을 잘라서 스테이징 텍스처에 복사
D3D11_BOX srcBox = { bboxX, bboxY, 0, bboxX + bboxW, bboxY + bboxH, 1 };
// GPU 셰이더로 다운샘플링 후 CopySubresourceRegion으로 복사
context->CopySubresourceRegion(stagingTexture, 0, 0, 0, 0, 
                                downsampledTexture, 0, nullptr);

// === STEP 3: CPU에서 데이터 읽기 ===
D3D11_MAPPED_SUBRESOURCE mapped;
HRESULT hr = context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
if (SUCCEEDED(hr)) {
    // mapped.pData에 64x64 그레이스케일 데이터 (4096 bytes)
    uint8_t* pixels = (uint8_t*)mapped.pData;
    
    // 픽셀 데이터 기반 해시 계산
    uint64_t hash = ComputeFNV1aHash(pixels, 64 * 64);
    
    context->Unmap(stagingTexture, 0);
}
```

### 2.3 초고속 픽셀 해싱 (FNV-1a Hash)

```cpp
// === FNV-1a 64-bit 해시 함수 ===
// O(N)의 시간 복잡도로 4096 바이트 배열을 한 번만 순회하며, 
// 곱셈과 XOR 연산만 사용하므로 CPU 부하가 극히 낮습니다. (SSIM 대비 압도적 속도)
uint64_t ComputeFNV1aHash(const uint8_t* data, size_t size) {
    uint64_t hash = 14695981039346656037ULL; // FNV_offset_basis
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;        // FNV_prime
    }
    return hash;
}

// 사용 예시
uint64_t current_hash = ComputeFNV1aHash(pixels, 4096);

if (current_hash == previous_hash) {
    // 변화 없음! 무거운 OCR 과정을 완전히 건너뜁니다 (Cache Hit)
    UsePreviousMaskCache();
} else {
    // 변화 감지! 새롭게 AI 분석을 수행합니다 (Cache Miss)
    RunNewAnalysis();
    previous_hash = current_hash;
}
```

### 2.4 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **Map 호출 블로킹** | `context->Map()`은 GPU가 복사를 완료할 때까지 CPU를 멈춤 | 비동기 쿼리(`D3D11_QUERY_EVENT`)를 사용하여 완료 여부 폴링 |
| **PCIe 대역폭** | 4K 전체 프레임을 매번 Readback하면 대역폭 포화 | 64x64로 다운샘플링하여 4KB만 전송 (전체 프레임 대비 99.9% 감소) |
| **멀티 GPU** | 노트북에서 내장/외장 GPU가 2개인 경우 텍스처 공유 불가 | OBS가 사용하는 GPU와 동일한 D3D11 디바이스를 반드시 사용 |

## 3. Lock-free Double Buffering (스레드 동기화)

### 3.1 무엇을 해야 하나요?
AI Thread가 "이 영역을 블러해!"라는 결과(좌표 목록)를 Render Thread에 전달해야 합니다. 두 스레드가 **동시에 같은 메모리를 건드리면 크래시**가 나므로, 안전한 전달 방법이 필요합니다.

### 3.2 구현 코드

```cpp
// === Atomic Double Buffer ===
// 버퍼 2개를 번갈아 사용하는 방식
struct MaskData {
    std::vector<BlurRect> rects; // 블러 영역 목록
    int count;
};

struct AtomicDoubleBuffer {
    MaskData buffers[2];               // 버퍼 2개
    std::atomic<int> active_index{0};  // 현재 Render가 읽고 있는 버퍼 번호
};

// === AI Thread: 분석 결과를 "비활성" 버퍼에 쓰기 ===
void AI_WriteResults(AtomicDoubleBuffer* db, std::vector<BlurRect> newRects) {
    // 현재 Render가 읽고 있는 버퍼의 반대쪽에 쓴다
    int writeIdx = 1 - db->active_index.load(std::memory_order_acquire);
    
    db->buffers[writeIdx].rects = std::move(newRects);
    db->buffers[writeIdx].count = db->buffers[writeIdx].rects.size();
    
    // 원자적으로 인덱스 교체 → Render Thread가 다음 프레임부터 새 데이터를 읽음
    db->active_index.store(writeIdx, std::memory_order_release);
}

// === Render Thread: "활성" 버퍼에서 읽기 ===
const MaskData& Render_ReadMasks(AtomicDoubleBuffer* db) {
    int readIdx = db->active_index.load(std::memory_order_acquire);
    return db->buffers[readIdx];
    // 여기서 읽는 동안 AI가 반대쪽 버퍼에 쓰고 있으므로 충돌 없음!
}
```

### 3.3 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **ABA 문제** | 인덱스가 0→1→0으로 돌아오면서 이전 데이터를 읽을 수 있음 | 우리는 단순 인덱스 교체만 하므로 ABA 문제 없음 (값 자체가 아닌 위치만 교체) |
| **메모리 오더링** | CPU가 명령어 순서를 바꿔서 실행하면 데이터가 꼬일 수 있음 | `memory_order_acquire/release` 사용하여 순서 보장 |
| **벡터 재할당** | `std::vector`가 크기를 늘리면서 메모리를 재할당하면 읽기 중인 쪽이 크래시 | 최대 블러 영역 수(32개)를 고정 크기 배열로 사용하거나, 쓰기 완료 후에만 swap |

## 4. Frame Freeze (비상 안전 장치)

### 4.1 무엇을 해야 하나요?
AI가 분석을 끝내지 못했는데 인코더가 "프레임 줘!"라고 할 때, **빈 프레임이나 망가진 프레임 대신 "이전에 안전했던 프레임"을 복제**하여 내보냅니다.

### 4.2 구현 코드

```cpp
void securecast_video_render(void *data, gs_effect_t *effect) {
    SecureCastFilter *f = (SecureCastFilter *)data;
    
    gs_texture_t* frame_to_send = PopFrame(&f->ring_buffer);
    
    if (frame_to_send == NULL) {
        // Ring Buffer가 아직 안 찼거나 GPU 부족으로 프레임 준비 실패
        if (f->last_safe_frame != NULL) {
            // ★ Frame Freeze: 마지막 안전 프레임을 복제하여 송출
            frame_to_send = f->last_safe_frame;
            // 이렇게 하면 화면이 잠시 "멈춘 것처럼" 보이지만,
            // 검은 화면이나 깨진 화면보다 100배 낫습니다!
        } else {
            // 최초 프레임이라 아직 안전 프레임도 없음
            // → 원본 그대로 패스스루 (어쩔 수 없음)
            obs_source_skip_video_filter(f->source);
            return;
        }
    } else {
        // 정상적으로 블러 적용 후 인코더에 전달
        ApplyBlurMasks(frame_to_send, f);
        f->last_safe_frame = frame_to_send; // 이 프레임을 "마지막 안전 프레임"으로 저장
    }
    
    RenderFrame(frame_to_send);
}
```

### 4.3 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **장시간 Freeze** | GPU가 5초 이상 멈추면 시청자에게 "방송 끊김"으로 보임 | Freeze가 500ms(30프레임) 이상 지속되면 패닉 모드 자동 전환 (전체 블러) |
| **오디오와 불일치** | 화면은 멈췄는데 소리는 계속 나옴 | A/V Sync 가이드에서 "일시적 불일치는 플랫폼 딜레이(2~5초)에 흡수됨"을 안내 |

---

# 🎨 Role D — UI/UX & 시스템 연동

> **담당 기능**: ③ 알림 차단, ④ 수동 블러, ⑥ 위험 경고, 보안 상태 UI, 다중 모니터 QA

## 1. OBS 설정 UI (Properties)

### 1.1 무엇을 해야 하나요?
사용자가 OBS 필터 설정 창에서 "블랙리스트 앱 목록", "블러 강도", "게임 모드 ON/OFF" 등을 조절할 수 있는 UI를 만들어야 합니다. OBS는 자체 UI 프레임워크(Properties API)를 제공합니다.

### 1.2 구현 코드

```cpp
// OBS Properties API — 설정 UI 정의
obs_properties_t* securecast_get_properties(void *data) {
    obs_properties_t *props = obs_properties_create();
    
    // === 1) 블랙리스트 앱 목록 (텍스트 입력) ===
    obs_properties_add_text(props, "blacklist_apps",
        "차단할 앱 목록 (쉼표 구분)",
        OBS_TEXT_DEFAULT);
    // 기본값: "KakaoTalk.exe, Discord.exe, Slack.exe"
    
    // === 2) 블러 강도 슬라이더 ===
    obs_properties_add_int_slider(props, "blur_strength",
        "블러 강도", 1, 20, 1);
    // 1=약함, 20=매우 강함
    
    // === 3) 게임 모드 토글 ===
    obs_properties_add_bool(props, "game_mode_enabled",
        "게임 모드 (전체 화면 게임 시 AI 자동 축소)");
    
    // === 4) 민감도 슬라이버 ===
    obs_properties_add_int_slider(props, "sensitivity",
        "탐지 민감도", 1, 10, 1);
    // 1=느슨함(오탐 적음), 10=엄격함(미탐 적음)
    
    // === 5) 패닉 버튼 핫키 ===
    // 핫키는 obs_hotkey_register_source로 별도 등록
    
    // === 6) A/V Sync 오프셋 안내 버튼 ===
    obs_properties_add_button(props, "av_sync_guide",
        "🔊 A/V 싱크 설정 가이드 보기",
        ShowAVSyncGuideCallback);
    
    // === 7) 언어팩 확인 버튼 ===
    obs_properties_add_button(props, "check_language",
        "🌐 OCR 언어팩 확인",
        CheckLanguagePackCallback);
    
    return props;
}

// === 설정값 저장/로드 ===
void securecast_update(void *data, obs_data_t *settings) {
    SecureCastFilter *f = (SecureCastFilter *)data;
    
    const char *apps = obs_data_get_string(settings, "blacklist_apps");
    f->blur_strength = (int)obs_data_get_int(settings, "blur_strength");
    f->game_mode_enabled = obs_data_get_bool(settings, "game_mode_enabled");
    f->sensitivity = (int)obs_data_get_int(settings, "sensitivity");
    
    // 블랙리스트 파싱 (쉼표로 분리)
    ParseBlacklist(f, apps);
}

// === 기본값 설정 ===
void securecast_defaults(obs_data_t *settings) {
    obs_data_set_default_string(settings, "blacklist_apps",
        "KakaoTalk.exe, Discord.exe, Slack.exe");
    obs_data_set_default_int(settings, "blur_strength", 10);
    obs_data_set_default_bool(settings, "game_mode_enabled", true);
    obs_data_set_default_int(settings, "sensitivity", 5);
}
```

### 1.3 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **빈 블랙리스트** | 사용자가 실수로 목록을 비우면 아무것도 차단 안 됨 | 빈 값이면 기본 목록으로 자동 복원 + 경고 메시지 |
| **오타** | "KakaoTalk.exe" 대신 "kakotalk.exe" 입력 | 대소문자 무시(case-insensitive) 비교 구현 |
| **UWP 앱** | Windows Store 앱은 .exe 이름이 다를 수 있음 | 미리 표에 실제 프로세스 이름을 조사하여 README에 명시 |

## 2. 보안 상태 UI (SAFE / PARTIAL / RISK)

### 2.1 무엇을 해야 하나요?
스트리머에게 현재 보안 상태를 **색깔로** 알려줘야 합니다. 시청자에게는 보이지 않고, 스트리머 본인에게만 보여야 합니다.

### 2.2 상태 정의

```
🟢 SAFE     — AI 분석 정상, 위험 탐지 없음
🟡 PARTIAL  — 게임 모드 중(Partial Sleep), 또는 AI 분석 지연 중
🔴 RISK     — 위험 정보 탐지됨, 또는 시스템 오류 발생
```

### 2.3 구현 방법

```cpp
// === 상태 전이 규칙 ===
enum SecurityState { SAFE, PARTIAL, RISK };

SecurityState DetermineState(SecureCastFilter *f) {
    // RISK 조건 (최우선)
    if (f->active_blur_count > 0)     return RISK;   // 위험 정보 감지 중
    if (f->ocr_engine_failed)         return RISK;   // OCR 엔진 오류
    if (f->frame_freeze_active)       return RISK;   // Frame Freeze 가동 중
    
    // PARTIAL 조건
    if (f->game_mode_active)          return PARTIAL; // 게임 모드
    if (f->ai_analysis_delayed)       return PARTIAL; // AI 분석 지연
    
    // 기본은 SAFE
    return SAFE;
}

// === OBS 소스 위에 색상 테두리 표시 ===
// OBS에서 스트리머 화면에만 별도 소스(투명 오버레이)를 추가하여
// 상태에 따라 테두리 색상을 변경
void RenderStatusBorder(SecureCastFilter *f) {
    SecurityState state = DetermineState(f);
    
    float r, g, b;
    switch (state) {
        case SAFE:    r=0.2f; g=0.9f; b=0.3f; break; // 초록
        case PARTIAL: r=1.0f; g=0.8f; b=0.0f; break; // 노랑
        case RISK:    r=1.0f; g=0.1f; b=0.1f; break; // 빨강
    }
    
    // 화면 가장자리에 3픽셀 두께의 테두리 렌더링
    DrawBorder(r, g, b, 3.0f);
}
```

### 2.4 시청자에게 안 보이게 하는 방법
OBS에는 **"프리뷰에만 표시되는 소스"** 또는 **"스튜디오 모드의 프리뷰 전용 씬"** 기능이 있습니다. 또는, 우리 플러그인이 별도의 Win32 투명 윈도우를 스트리머 모니터에 띄우는 방법을 사용합니다.

```cpp
// Win32 투명 오버레이 윈도우 생성 (시청자에게 안 보임)
HWND CreateOverlayWindow() {
    WNDCLASS wc = {};
    wc.lpfnWndProc = OverlayWndProc;
    wc.lpszClassName = L"SecureCastOverlay";
    RegisterClass(&wc);
    
    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"SecureCastOverlay", L"",
        WS_POPUP, 0, 0, 100, 100, NULL, NULL, NULL, NULL
    );
    
    // WS_EX_TRANSPARENT: 마우스 클릭이 통과
    // WS_EX_TOOLWINDOW: 작업 표시줄에 안 뜸
    // WS_EX_LAYERED: 투명도 지원
    
    // SetWindowDisplayAffinity로 캡처 방지
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    // → 이 창은 OBS 캡처에 잡히지 않음! (스트리머만 볼 수 있음)
    
    return hwnd;
}
```

### 2.5 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **WDA_EXCLUDEFROMCAPTURE 미지원** | Windows 10 2004 미만에서는 이 API가 없음 | OS 버전 체크 후 미지원 시 OBS 내부 프리뷰 기반 방식으로 폴백 |
| **경고가 너무 자주** | 오탐이 많으면 RISK 상태가 계속 뜨면서 스트리머 피로감 | 민감도 설정과 연동하여 임계값 조절 |

## 3. 알림 영역 자동 블러 (Layout Shift 감지)

### 3.1 무엇을 해야 하나요?
Windows 알림(우측 하단 토스트)이 뜨면 해당 영역을 자동으로 블러합니다.

### 3.2 구현 방법

```cpp
// Windows 알림 영역은 보통 우측 하단 고정 위치에 뜸
// 화면 크기 기준으로 알림 영역 좌표를 계산

struct NotificationGuard {
    // 알림이 뜨는 영역 (화면 우측 하단)
    // Windows 11 기준: 화면 너비의 우측 약 370px, 높이 약 100px
    float relX = 0.78f;  // UV 기준 좌표
    float relY = 0.90f;
    float relW = 0.22f;
    float relH = 0.10f;
    
    bool isActive = false;
    int cooldown = 0; // 알림이 사라진 뒤 일정 시간 동안 유지
};

// 알림 감지 로직: 해당 영역의 픽셀 변화를 모니터링
void CheckNotificationArea(SecureCastFilter *f) {
    // 우측 하단(알림 영역)의 픽셀 해시(FNV-1a) 계산
    // 팝업 알림이 뜨면 픽셀 색상이 완전히 바뀌므로 해시가 즉시 달라집니다.
    uint64_t curr_notif_hash = ComputeAreaHash(f->curr_frame, 
                                  f->notif.relX, f->notif.relY,
                                  f->notif.relW, f->notif.relH);
    
    if (curr_notif_hash != f->prev_notif_hash) {
        // 알림 영역의 픽셀 변화 감지 (팝업 발생)
        f->notif.isActive = true;
        f->notif.cooldown = 180; // 3초(60fps × 3) 동안 블러 유지
        f->prev_notif_hash = curr_notif_hash;
    }
    
    if (f->notif.cooldown > 0) {
        f->notif.cooldown--;
    } else {
        f->notif.isActive = false;
    }
}
```

### 3.3 ⚠️ 엣지 케이스 & 리스크

| 리스크 | 설명 | 대응법 |
|---|---|---|
| **알림 위치 변경** | Windows 설정에서 알림 위치를 바꿀 수 있음 (드묾) | 기본 위치(우측 하단)만 지원, 향후 사용자 커스텀 영역 설정 추가 |
| **게임 UI와 겹침** | 게임 HP바나 미니맵이 우측 하단에 있으면 오탐 | 게임 모드 시 이 감지는 프로세스 컨텍스트와 결합하여 판단 |

## 4. 수동 드래그 블러

### 4.1 구현 방법

```cpp
// OBS Interaction API를 활용
// 사용자가 프리뷰 화면에서 마우스 드래그로 영역을 지정

// 마우스 이벤트 콜백 등록
.video_mouse_click = securecast_mouse_click,
.video_mouse_move  = securecast_mouse_move,

bool isDragging = false;
float dragStartX, dragStartY;

void securecast_mouse_click(void *data, const struct obs_mouse_event *event,
                             int32_t type, bool mouse_up, uint32_t click_count) {
    if (type == MOUSE_LEFT) {
        if (!mouse_up) {
            // 드래그 시작
            isDragging = true;
            dragStartX = event->x;
            dragStartY = event->y;
        } else {
            // 드래그 끝 → 블러 영역 확정
            isDragging = false;
            AddManualBlurRect(dragStartX, dragStartY, event->x, event->y);
        }
    }
}
```

### 4.2 ⚠️ 주의사항
- OBS의 Interaction API는 **프리뷰 창에서만** 동작하므로, 방송 중에는 핫키 기반으로 전환하거나 별도 제어판에서 조작하도록 안내
- 수동 블러 영역은 반드시 설정 파일에 저장하여 OBS 재시작 시에도 유지

## 5. 다중 모니터 QA 체크리스트

### 5.1 반드시 테스트해야 할 케이스

| # | 테스트 케이스 | 예상 문제 | 확인 포인트 |
|---|---|---|---|
| 1 | 모니터1: 1080p 100%, 모니터2: 1080p 100% | 기본 케이스 | 좌표 정확도 확인 |
| 2 | 모니터1: 1080p 100%, 모니터2: 4K 150% | DPI 불일치 | `GetDpiForMonitor`로 보정되는지 확인 |
| 3 | 모니터1: 4K 150%, 모니터2: 1080p 125% | 양쪽 다 비표준 | 블러가 정확한 위치에 적용되는지 확인 |
| 4 | 창을 모니터 경계에 걸치게 배치 | 좌표가 분리됨 | 두 모니터에 걸친 창 처리 확인 |
| 5 | OBS 소스를 크롭(Crop)한 상태 | 좌표 오프셋 | 크롭 보정이 적용되는지 확인 |
| 6 | OBS 소스를 회전한 상태 | 역행렬 필요 | 블러 영역도 함께 회전하는지 확인 |

## 6. 첫 실행 가이드 (Onboarding)

### 6.1 플러그인 최초 로드 시 안내할 내용

```
📋 SecureCast 초기 설정 가이드

1. ✅ OCR 언어팩 확인
   → [확인] 버튼을 눌러 한국어/영어 언어팩이 설치되어 있는지 확인하세요.
   → 미설치 시: Windows 설정 > 시간 및 언어 > 언어 및 지역 > 한국어 > 언어 팩 설치

2. ✅ 블랙리스트 앱 설정
   → 기본값: 카카오톡, 디스코드, 슬랙
   → 필요한 앱을 추가/제거하세요.

3. ✅ A/V 싱크 설정 (선택)
   → OBS > 오디오 고급 속성 > 해당 소스의 싱크 오프셋을 80ms로 설정
   → (N-Frame Delay = 5프레임 = 약 80ms)

4. ✅ 패닉 버튼 핫키 설정
   → OBS > 설정 > 핫키 > "SecureCast 패닉 버튼"에 원하는 키 할당
```
