# Game Mode v2 — 통합 테스트 시나리오

이 문서는 게임 모드 v2 기능(T01~T15) 전체가 정상 동작하는지 사람이 직접 OBS에서
확인할 때 사용한다. 각 시나리오는 **사전 조건 → 단계 → 기대 동작 → 실패 신호** 순서.

자동 단위 테스트는 `test/test_steam_parser.cpp` (VDF 파서)만 존재.
시각/타이밍 의존 동작은 모두 본 문서로 검증한다.

---

## 공통 사전 조건

- Windows 10 build ≥ 19041 (WDA_EXCLUDEFROMCAPTURE 지원)
- OBS Studio 32.x + `securecast.dll` 설치
- OBS 씬에 SecureCast 필터가 부착된 임의 소스 (Display Capture 등)
- OBS 로그 콘솔(`도움말 → 로그 파일 → 현재 로그 보기`) 열어 두기

이하 시나리오에서 "로그" 언급은 모두 OBS 로그 콘솔의 `[SecureCast]` prefix 항목.

---

## S1. Primary trigger — 알려진 게임 즉시 진입

### 사전 조건
- 게임 모드: OFF
- 빌트인 Tier 1 리스트의 게임 중 임의 1개 실행 가능 (예: `cs2.exe`,
  `valorant.exe`, `lostark.exe` 등 `src/known_games.h` 참조)

### 단계
1. CPU 부하 거의 없는 상태로 OBS 가동
2. Tier 1 게임 실행 → 게임 창이 foreground 되도록 클릭

### 기대 동작
- **1초 안에** 게임 모드 ON
- HUD에 보라색 `GAME` 배지 표시
- 로그: `Game mode ON (trigger=primary, game='<exe>')`
- 작업관리자에서 OBS 프로세스 CPU 사용량 약 10% 감소 (OCR 정지)

### 실패 신호
- CPU 임계값 3초 기다린 뒤 진입 → Primary가 작동 안 함
  → `sc_is_known_game()` 매칭 확인 / 사용자 게임 목록 확인
- `GAME` 배지 안 보임 → HUD `setState(state, gameMode)` 호출 확인
- CPU 사용량 변화 없음 → OCR worker skip 분기 미적용

---

## S2. Secondary trigger — CPU 임계값으로 진입

### 사전 조건
- 게임 모드: OFF
- Tier 1 / 사용자 목록에 **없는** 부하 큰 프로그램 (예: 벤치마크 툴, 영상 변환)
- 또는 known 게임이 fg를 안 잡는 경우 (예: 게임이 미니마이즈된 상태)

### 단계
1. 부하 프로그램으로 CPU 사용률을 **40% 이상** 3초 유지

### 기대 동작
- 3초 후 게임 모드 ON
- 로그:
  - fg가 known 게임이 아닌 경우: `Game mode ON (trigger=secondary CPU=…%) but fg '…' is not in game list — fg auto-blur disabled for safety` (WARNING)
  - fg가 known 게임이면: `(trigger=secondary CPU=…%, game='…')` (INFO)
- HUD에 `GAME` 배지 표시

### 실패 신호
- 진입은 됐지만 자동 fg 블러가 적용됨 → empty 가드 미적용
- 즉시 진입 → Primary가 잘못 잡음

---

## S3. Empty 가드 — alt+tab으로 진짜 게임 들어가기

### 사전 조건
- Secondary trigger로 게임 모드 진입한 상태 (`gameModeGameExe` 비어있음)
- HUD에 `GAME` 배지 표시 중

### 단계
1. Tier 1 게임 (예: cs2.exe)을 새로 실행
2. Alt+Tab으로 그 게임에 들어감

### 기대 동작
- 게임 자체는 **블러되지 않음** (송출 화면에 게임 그대로)
- 게임 모드는 그대로 ON 유지
- 일반 블랙리스트 앱이 같은 화면에 있으면 그것만 블러 (보호 누락 없음)

### 실패 신호
- 게임 화면 전체가 보라색 블러로 덮임 → empty 가드 미적용
  → `securecast_video_render`의 `gameModeGameExe.empty()` 분기 확인

---

## S4. Silent fg auto-blur — 게임 중 디스코드 등 띄우기

### 사전 조건
- Primary trigger로 게임 모드 진입 (`gameModeGameExe`에 게임 exe 캡처됨)
- 화이트리스트는 비어있음

### 단계
1. 게임 모드 중 Discord/카카오톡/브라우저 등 임의 앱을 foreground 가져옴

### 기대 동작
- 사용자 동의 dialog **안 뜸** (T02에서 폐기됨)
- 송출 화면에서 그 앱이 보라색 블러 처리됨
- 로그: dialog 관련 메시지 없음

### 실패 신호
- MessageBoxW가 뜸 → T02 dialog 코드 잔존
- 블러 안 됨 → render 측 자동 블러 블록 진입 실패

---

## S5. Whitelist — 노출 허용

### 사전 조건
- 게임 모드 진입 상태
- Properties → "게임 모드 노출 허용 앱"에 `Discord.exe` 추가

### 단계
1. Discord를 foreground 가져옴

### 기대 동작
- Discord 화면이 송출에 **그대로 노출** (블러 안 함)
- 로그에 해당 fg가 가렸다는 표시 없음
- "최근 가린 앱" 섹션에도 Discord 안 나타남

### 실패 신호
- 여전히 가려짐 → settings → `g_gmWhitelist` 동기화 끊김
- 대소문자 차이로 매칭 실패 (`Discord.exe` vs `discord.exe`) → T15에서 정규화
  검토 필요

---

## S6. Auto-detect — 자동 검색 결과 dialog

### 사전 조건
- Steam / Epic / Battle.net 중 최소 1개 설치 + 게임 1개 이상 보유

### 단계
1. Properties → "내 게임 목록" 그룹 → "자동 검색" 버튼 클릭

### 기대 동작
- 백그라운드 thread로 enum (3~10초)
- 완료 시 결과 dialog:
  - Source별 발견 카운트 (Steam N / Epic M / …)
  - 새로 추가할 게임 수 + 상세 25개까지 (`[Source]` 라벨)
  - YES/NO 버튼
- YES 클릭 → editable_list에 자동 등록 + 로그
  `auto-detect: added N game(s) to sc_user_games`
- 검색 도중 버튼 재클릭 → "이미 진행 중" MessageBox

### 실패 신호
- dialog 안 뜸 → worker thread 시작 실패 / `g_autodetect_running` 상태 누락
- YES 후 목록 갱신 안 됨 → `obs_source_update` 호출 누락
- 같은 게임 중복 추가됨 → dedup 미동작

---

## S7. HUD 게임 배지

### 사전 조건
- 게임 모드 OFF 상태에서 HUD 정상 표시 (`SecureCast` + `SAFE/CAUTION/RISK`)

### 단계
1. 게임 모드 진입 (S1 또는 S2)
2. 진입 직후 HUD 관찰
3. 게임 모드 종료 (CPU 30% 이하 5초)

### 기대 동작
- 진입: HUD 폭이 ~260px로 확장 + 우측에 보라색 `GAME` 배지 표시
- 종료: 5초 hysteresis 후 폭이 ~200px로 축소 + 보라 배지 사라짐

### 실패 신호
- 진입했는데 배지 안 뜸 → setState 호출에 `isGameMode` 두 번째 인자 빠짐
- 배지가 잔상으로 남음 → WM_PAINT 전체 재칠 누락

---

## S8. Ctrl+Shift+L — 블랙리스트 UI 단축키

### 사전 조건
- OBS 핫키 설정에서 `SecureCast — 블랙리스트/화이트리스트 UI 열기`가
  `Ctrl+Shift+L`로 바인딩되어 있음

### 단계
1. 게임 모드 OFF 또는 ON 상관없이 OBS 메인 창 활성
2. `Ctrl+Shift+L` 누름

### 기대 동작
- SecureCast 필터의 Properties dialog 즉시 표시
- 게임 모드 중에도 정상 작동

### 실패 신호
- 아무 일 안 일어남 → 핫키 바인딩 충돌 또는 OBS 메인 창 비활성
- Properties가 열리지만 다른 source의 것 → `filter->context` 전달 확인

---

## S9. Multi-instance refcount

### 사전 조건
- OBS 씬에 SecureCast 필터가 2개 인스턴스 부착됨 (예: Display Capture +
  Window Capture 둘 다에 SecureCast 적용)

### 단계
1. 인스턴스 A 게임 모드 ON (Tier 1 게임 띄움) → B도 자동 ON (CPU↑)
2. 인스턴스 A 의 필터를 OBS에서 제거
3. 인스턴스 B만 남아 게임 모드 유지되는지 확인

### 기대 동작
- A 제거 시 A의 `securecast_destroy`에서 refcount −1
- `g_gameMode_RefCount > 0` 유지 → 글로벌 게임 모드 ON 상태 그대로
- B의 OCR은 계속 정지 / B의 자동 fg 블러 계속 동작

### 실패 신호
- A 제거 직후 B의 OCR이 재개되어 CPU↑ → refcount underflow 또는
  destroy에서 정리 누락 (`sc_set_global_game_mode(false)` 호출 확인)

---

## S10. 게임 모드 종료 — CPU 하강

### 사전 조건
- 게임 모드 ON 상태 (Primary 또는 Secondary 무관)

### 단계
1. 부하 프로그램/게임 종료
2. 시스템 CPU 사용률 30% 이하로 떨어진 상태로 5초 대기

### 기대 동작
- 5초 후 게임 모드 OFF
- HUD: `GAME` 배지 사라짐 + 폭 축소
- OCR worker 재개 → CPU 일부 사용량 복귀
- 로그: `Game mode OFF (CPU: …%, hysteresis=5s)`

### 실패 신호
- 진동 (ON↔OFF 반복) → CPU 사용률이 임계값 근처에서 변동 →
  Properties에서 CPU 임계값/hysteresis 조정
- 종료 안 됨 → `gameModeExitTimer`가 누적 안 됨

---

## 회귀 테스트 체크리스트 (PR 머지 전 빠른 검증)

| # | 항목 | 통과 |
|---|---|---|
| 1 | known 게임 fg → 1초 안에 GAME 배지 | ☐ |
| 2 | CPU 40%×3s → GAME 배지 + WARNING 로그(fg 미식별 시) | ☐ |
| 3 | Primary 진입 후 게임 alt+tab → 게임 자체 블러 안 됨 | ☐ |
| 4 | Secondary 진입 후 게임 alt+tab → 게임 자체 블러 안 됨 (empty 가드) | ☐ |
| 5 | 게임 모드 중 Discord 띄움 → 송출에서 블러 처리 | ☐ |
| 6 | Properties → 자동 검색 → 결과 dialog → YES → 목록 갱신 | ☐ |
| 7 | Ctrl+Shift+L → Properties 즉시 표시 | ☐ |
| 8 | CPU 떨어지고 5초 → GAME 배지 사라짐 | ☐ |
| 9 | 자동 검색 중 버튼 재클릭 → "이미 진행 중" | ☐ |
| 10 | `ctest -R steam_parser` 53/53 PASS | ☐ |

---

## 알려진 한계 (테스트 시 의도된 동작)

- **UWP 게임 매칭률**: `ApplicationFrameHost.exe` 호스팅 게임(일부 Forza 등)은
  fg.exe 매칭이 안 됨. 자동 fg 블러는 작동하지 않을 수 있음 — v2 plan에 명시.
- **Installed Programs false positive**: 휴리스틱이라 비-게임이 섞일 수
  있음. 사용자가 editable_list에서 제거.
- **Whitelist case-sensitivity**: 현재 매칭은 case-sensitive
  (`Discord.exe` ≠ `discord.exe`). 정규화는 T15 향후 작업.
- **CPU 진동 시 게임 모드 깜빡임**: 임계값 근처에서 진동 → Properties에서
  사용자가 임계값/hysteresis 조정 가능 (T07).
