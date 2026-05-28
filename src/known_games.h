// =============================================================================
// known_games.h — Game Mode v2 / T03: Tier 1 빌트인 게임 exe 목록
//
// 역할:
//   게임 모드 자동 진입의 primary trigger를 위한 "이건 거의 확실히 게임"
//   판단 기준. Foreground의 exe가 이 배열에 있으면 CPU 임계값(secondary)을
//   기다리지 않고 즉시 게임 모드 ON.
//
//   또한 진입 시점에 captured된 gameModeGameExe가 이 목록의 exe면 "그건
//   진짜 게임" — 자동 fg 블러에서 안전하게 제외할 수 있다.
//
// 선정 기준:
//   1. 한국에서 스트리밍 빈도가 높은 작품 우선
//   2. 글로벌 top-100 트위치/유튜브 시청 시간 상위 작품
//   3. exe가 명확하고 다른 비-게임 앱과 헷갈리지 않는 것
//      (예: "game.exe" 같은 모호한 이름은 제외 — 자동 enum/사용자 등록으로 보강)
//
// 카테고리 주석은 grep/검색 편의 위한 것 — 분기에는 영향 없음.
//
// 이 헤더는 Windows 전용 (wchar_t / .exe 기반). 사용 측은 #ifdef _WIN32로 감쌀 것.
//
// 매칭은 case-insensitive (window_tracker.cpp::iequals).
// exe basename만 비교 — 풀 경로 비교 안 함.
// =============================================================================

#pragma once

#include <wchar.h>

namespace securecast {

// Tier 1 빌트인 게임 리스트.
//
// 추가/제거 가이드:
//   - 추가: exe basename(소문자/대문자 무관)을 카테고리 구역 안에 알파벳순으로.
//   - 제거: 그 exe가 일반 사무용으로도 쓰일 가능성이 생기면 즉시 제거.
//     (false positive로 OCR이 꺼지면 PII 노출 위험 — 그쪽이 더 치명적)
//
// 현재 목록은 약 80개. Stage 2~3의 자동 enum(Steam/Epic/Battle.net/Riot 등)이
// 들어오면 이 정적 목록은 "enum이 놓치는 항목 보강"용으로 점점 줄어든다.
inline const wchar_t *const kKnownGameExes[] = {
    // ── FPS / Tactical Shooter ──────────────────────────────
    L"cs2.exe",                  // Counter-Strike 2 (Valve)
    L"csgo.exe",                 // CS:GO (legacy)
    L"valorant.exe",             // Valorant launcher
    L"valorant-win64-shipping.exe", // Valorant game process
    L"overwatch.exe",            // Overwatch 2 launcher
    L"overwatch2.exe",           //
    L"r5apex.exe",               // Apex Legends
    L"r5apex_dx12.exe",          // Apex Legends DX12
    L"easyanticheat.exe",        // EAC (참고: 많은 FPS의 anti-cheat — 단독 매칭 안 함)
    L"battlebit.exe",            // BattleBit Remastered
    L"escapefromtarkov.exe",     // Escape from Tarkov
    L"hunt.exe",                 // Hunt: Showdown
    L"siegelauncher.exe",        // Rainbow Six Siege launcher
    L"rainbowsix.exe",           // R6 Siege
    L"rainbowsix_vulkan.exe",    //
    L"thefinals.exe",            // THE FINALS
    L"deltaforceclient-win64-shipping.exe", // Delta Force
    L"sof2mp.exe",               // Soldier of Fortune 2 (legacy 한국 유저)
    L"sudden_attack.exe",        // Sudden Attack
    L"sa2_client.exe",           // Sudden Attack 2 variants

    // ── Battle Royale ────────────────────────────────────────
    L"fortniteclient-win64-shipping.exe", // Fortnite
    L"fortnitelauncher.exe",     //
    L"pubg.exe",                 // PUBG: Battlegrounds
    L"tslgame.exe",              // PUBG game process
    L"warzone.exe",              // Call of Duty: Warzone
    L"cod.exe",                  // Call of Duty (HQ launcher)
    L"modernwarfare.exe",        // CoD: MW
    L"naraka.exe",               // Naraka: Bladepoint
    L"narakathebladepoint.exe",  //

    // ── MOBA / Auto-battler ──────────────────────────────────
    L"league of legends.exe",    // LoL launcher
    L"leagueclient.exe",         // LoL launcher (Riot)
    L"leagueclientux.exe",       //
    L"riotclientservices.exe",   // Riot client (TFT/LoL/Valorant 공통)
    L"dota2.exe",                // Dota 2
    L"heroesofthestorm.exe",     // HotS
    L"smite.exe",                // SMITE

    // ── 한국 MMO / RPG (한국 스트리밍 메인) ──────────────────
    L"lostark.exe",              // Lost Ark
    L"lostarkclient.exe",        //
    L"maplestory.exe",           // 메이플스토리
    L"maplestory2.exe",          // 메이플스토리2
    L"baram.exe",                // 바람의 나라
    L"barams.exe",               //
    L"blade & soul.exe",         // 블레이드앤소울
    L"bnsr.exe",                 // 블소
    L"l2.bin",                   // 리니지2 (.bin 실행 — 일부 매칭 안 될 수 있음)
    L"lineagew.exe",             // 리니지W
    L"aion.bin",                 // 아이온
    L"darkanddarker.exe",        // Dark and Darker
    L"throneandliberty.exe",     // 쓰론 앤 리버티
    L"tlclient.exe",             //

    // ── 글로벌 MMO / RPG ─────────────────────────────────────
    L"wow.exe",                  // World of Warcraft
    L"wowclassic.exe",           //
    L"ffxiv_dx11.exe",           // FFXIV
    L"ffxiv.exe",                //
    L"eso.exe",                  // Elder Scrolls Online
    L"elderscrollsonline.exe",   //
    L"newworld.exe",             // New World
    L"genshinimpact.exe",        // 원신
    L"yuanshen.exe",             // 원신 (중국 빌드)
    L"starrail.exe",             // 붕괴: 스타레일
    L"zzz.exe",                  // 젠레스 존 제로
    L"wuwa.exe",                 // 명조: 워더링 웨이브

    // ── AAA 싱글/멀티 ────────────────────────────────────────
    L"gta5.exe",                 // GTA V
    L"gta_sa.exe",               // GTA SA
    L"rdr2.exe",                 // Red Dead Redemption 2
    L"cyberpunk2077.exe",        // Cyberpunk 2077
    L"witcher3.exe",             // The Witcher 3
    L"eldenring.exe",            // Elden Ring
    L"darksoulsiii.exe",         // Dark Souls III
    L"sekiro.exe",               // Sekiro
    L"helldivers2.exe",          // Helldivers 2
    L"bg3.exe",                  // Baldur's Gate 3
    L"bg3_dx11.exe",             //
    L"starfield.exe",            // Starfield
    L"diablo iv launcher.exe",   // Diablo IV
    L"diabloiv.exe",             //
    L"diablo iii64.exe",         // Diablo III
    L"hogwartslegacy.exe",       // Hogwarts Legacy
    L"mhrise.exe",               // Monster Hunter Rise
    L"mhwilds.exe",              // Monster Hunter Wilds
    L"mhworld.exe",              // Monster Hunter World

    // ── 인디 / 협동 ──────────────────────────────────────────
    L"terraria.exe",             // Terraria
    L"minecraft.exe",            // Minecraft (Java launcher)
    L"minecraftlauncher.exe",    //
    // ★ javaw.exe는 의도적으로 제외 — Minecraft Java가 이걸로 떠도 IntelliJ/
    //   다른 Java 앱과 겹쳐 OCR이 잘못 꺼지면 PII 노출 위험이 크다.
    L"deadbydaylight-win64-shipping.exe", // Dead by Daylight
    L"lethalcompany.exe",        // Lethal Company
    L"phasmophobia.exe",         // Phasmophobia
    L"palworld-win64-shipping.exe", // Palworld
    L"valheim.exe",              // Valheim
    L"projectzomboid.exe",       // Project Zomboid
    L"projectzomboid64.exe",     //
    L"7daystodie.exe",           // 7 Days to Die
    L"rustclient.exe",           // Rust

    // ── 콘솔 에뮬 (스트리밍 중 화면 노출 잦음) ──────────────
    L"ryujinx.exe",              // Switch 에뮬
    L"yuzu.exe",                 // (참고: 단종됐으나 잔존 설치 있음)
    L"cemu.exe",                 // Wii U
    L"rpcs3.exe",                // PS3
    L"pcsx2.exe",                // PS2
    L"pcsx2-qt.exe",             //
    L"duckstation-qt-x64-releaseltcg.exe", // PS1
    L"dolphin.exe",              // GC/Wii

    // 위 javaw.exe 같은 항목은 의도적으로 제외 — 이 배열은 "확실한 게임"만 등재.
};

inline constexpr size_t kKnownGameExesCount =
    sizeof(kKnownGameExes) / sizeof(kKnownGameExes[0]);

// ============================================================================
// 친화명 매핑 — 시스템 enum(Uninstall 레지스트리/실행 중 프로세스)에서 잡히지
// 않는 .exe들의 친화명을 위한 빌트인 fallback.
//
// 특히 다음 케이스에 필수:
//   - 백그라운드 서비스 (vgc.exe = Riot Vanguard, BEService.exe = BattlEye 등)
//   - 윈도우 없는 보조 프로세스 (riotclientservices.exe, EpicWebHelper.exe 등)
//   - file version 정보가 빈약하거나 일관성 없는 게임 본체
//
// 사용처:
//   - populate_user_game_picker가 entry 친화명 lookup 시 fallback
//   - 자동 검색 결과의 마이그레이션
//
// 매칭은 case-insensitive (window_tracker.cpp::iequals).
// 게임 본체와 보조 .exe 모두 포함 — game-mode trigger 여부와 무관하게
// "이 exe가 어느 앱에 속하는지" 표시 정보.
// ============================================================================
struct KnownExeName {
  const wchar_t *exe;
  const wchar_t *name;
};

inline constexpr KnownExeName kKnownExeNames[] = {
    // ── 게임 launcher / 서비스 / anti-cheat ───────────────────────
    {L"steam.exe", L"Steam"},
    {L"steamservice.exe", L"Steam Service"},
    {L"steamwebhelper.exe", L"Steam"},
    {L"EpicGamesLauncher.exe", L"Epic Games Launcher"},
    {L"EpicWebHelper.exe", L"Epic Online Services"},
    {L"EpicOnlineServices.exe", L"Epic Online Services"},
    {L"BattleNet.exe", L"Battle.net"},
    {L"Battle.net.exe", L"Battle.net"},
    {L"Agent.exe", L"Battle.net Agent"},
    {L"riotclientservices.exe", L"Riot Client"},
    {L"riotclientux.exe", L"Riot Client"},
    {L"RiotClientCrashHandler.exe", L"Riot Client"},
    {L"vgc.exe", L"Riot Vanguard"},
    {L"vgm.exe", L"Riot Vanguard"},
    {L"vgtray.exe", L"Riot Vanguard"},
    {L"EasyAntiCheat.exe", L"Easy Anti-Cheat"},
    {L"EasyAntiCheat_EOS.exe", L"Easy Anti-Cheat"},
    {L"BEService.exe", L"BattlEye"},
    {L"BEServer.exe", L"BattlEye"},
    {L"BattlEye.exe", L"BattlEye"},
    {L"GalaxyClient.exe", L"GOG Galaxy"},
    {L"upc.exe", L"Ubisoft Connect"},
    {L"UbisoftConnect.exe", L"Ubisoft Connect"},
    {L"BethesdaNetLauncher.exe", L"Bethesda.net"},
    {L"NexonLauncher.exe", L"Nexon Launcher"},

    // ── FPS / Shooter ────────────────────────────────────────────
    {L"cs2.exe", L"Counter-Strike 2"},
    {L"csgo.exe", L"Counter-Strike: GO"},
    {L"valorant.exe", L"Valorant"},
    {L"valorant-win64-shipping.exe", L"Valorant"},
    {L"overwatch.exe", L"Overwatch 2"},
    {L"overwatch2.exe", L"Overwatch 2"},
    {L"OverwatchLauncher.exe", L"Overwatch Launcher"},
    {L"r5apex.exe", L"Apex Legends"},
    {L"r5apex_dx12.exe", L"Apex Legends (DX12)"},
    {L"battlebit.exe", L"BattleBit Remastered"},
    {L"escapefromtarkov.exe", L"Escape from Tarkov"},
    {L"hunt.exe", L"Hunt: Showdown"},
    {L"siegelauncher.exe", L"Rainbow Six Siege Launcher"},
    {L"rainbowsix.exe", L"Rainbow Six Siege"},
    {L"rainbowsix_vulkan.exe", L"Rainbow Six Siege"},
    {L"thefinals.exe", L"THE FINALS"},

    // ── Battle Royale ────────────────────────────────────────────
    {L"fortniteclient-win64-shipping.exe", L"Fortnite"},
    {L"fortnitelauncher.exe", L"Fortnite Launcher"},
    {L"pubg.exe", L"PUBG: Battlegrounds"},
    {L"tslgame.exe", L"PUBG: Battlegrounds"},
    {L"warzone.exe", L"Call of Duty: Warzone"},
    {L"cod.exe", L"Call of Duty"},
    {L"modernwarfare.exe", L"CoD: Modern Warfare"},
    {L"naraka.exe", L"Naraka: Bladepoint"},

    // ── MOBA ─────────────────────────────────────────────────────
    {L"leagueclient.exe", L"League of Legends"},
    {L"leagueclientux.exe", L"League of Legends"},
    {L"league of legends.exe", L"League of Legends"},
    {L"dota2.exe", L"Dota 2"},
    {L"heroesofthestorm.exe", L"Heroes of the Storm"},

    // ── 한국 MMO / RPG ───────────────────────────────────────────
    {L"lostark.exe", L"Lost Ark"},
    {L"lostarkclient.exe", L"Lost Ark"},
    {L"maplestory.exe", L"메이플스토리"},
    {L"maplestory2.exe", L"메이플스토리 2"},
    {L"baram.exe", L"바람의 나라"},
    {L"blade & soul.exe", L"블레이드 & 소울"},
    {L"bnsr.exe", L"블레이드 & 소울"},
    {L"lineagew.exe", L"리니지W"},
    {L"darkanddarker.exe", L"Dark and Darker"},
    {L"throneandliberty.exe", L"Throne and Liberty"},
    {L"tlclient.exe", L"Throne and Liberty"},

    // ── 글로벌 MMO / RPG ─────────────────────────────────────────
    {L"wow.exe", L"World of Warcraft"},
    {L"wowclassic.exe", L"WoW Classic"},
    {L"ffxiv_dx11.exe", L"Final Fantasy XIV"},
    {L"ffxiv.exe", L"Final Fantasy XIV"},
    {L"eso.exe", L"Elder Scrolls Online"},
    {L"elderscrollsonline.exe", L"Elder Scrolls Online"},
    {L"newworld.exe", L"New World"},
    {L"genshinimpact.exe", L"원신"},
    {L"yuanshen.exe", L"원신"},
    {L"starrail.exe", L"붕괴: 스타레일"},
    {L"zzz.exe", L"젠레스 존 제로"},
    {L"wuwa.exe", L"명조: 워더링 웨이브"},

    // ── AAA 싱글/멀티 ────────────────────────────────────────────
    {L"gta5.exe", L"Grand Theft Auto V"},
    {L"gta_sa.exe", L"Grand Theft Auto: San Andreas"},
    {L"rdr2.exe", L"Red Dead Redemption 2"},
    {L"cyberpunk2077.exe", L"Cyberpunk 2077"},
    {L"witcher3.exe", L"The Witcher 3"},
    {L"eldenring.exe", L"Elden Ring"},
    {L"darksoulsiii.exe", L"Dark Souls III"},
    {L"sekiro.exe", L"Sekiro"},
    {L"helldivers2.exe", L"Helldivers 2"},
    {L"bg3.exe", L"Baldur's Gate 3"},
    {L"bg3_dx11.exe", L"Baldur's Gate 3"},
    {L"starfield.exe", L"Starfield"},
    {L"diablo iv launcher.exe", L"Diablo IV"},
    {L"diabloiv.exe", L"Diablo IV"},
    {L"diablo iii64.exe", L"Diablo III"},
    {L"hogwartslegacy.exe", L"Hogwarts Legacy"},
    {L"mhrise.exe", L"Monster Hunter Rise"},
    {L"mhwilds.exe", L"Monster Hunter Wilds"},
    {L"mhworld.exe", L"Monster Hunter World"},

    // ── 인디 / 협동 / 서바이벌 ──────────────────────────────────
    {L"terraria.exe", L"Terraria"},
    {L"minecraft.exe", L"Minecraft"},
    {L"minecraftlauncher.exe", L"Minecraft Launcher"},
    {L"deadbydaylight-win64-shipping.exe", L"Dead by Daylight"},
    {L"lethalcompany.exe", L"Lethal Company"},
    {L"phasmophobia.exe", L"Phasmophobia"},
    {L"palworld-win64-shipping.exe", L"Palworld"},
    {L"valheim.exe", L"Valheim"},
    {L"projectzomboid.exe", L"Project Zomboid"},
    {L"projectzomboid64.exe", L"Project Zomboid"},
    {L"7daystodie.exe", L"7 Days to Die"},
    {L"rustclient.exe", L"Rust"},

    // ── 콘솔 에뮬 ────────────────────────────────────────────────
    {L"ryujinx.exe", L"Ryujinx"},
    {L"yuzu.exe", L"yuzu"},
    {L"cemu.exe", L"Cemu"},
    {L"rpcs3.exe", L"RPCS3"},
    {L"pcsx2.exe", L"PCSX2"},
    {L"pcsx2-qt.exe", L"PCSX2"},
    {L"duckstation-qt-x64-releaseltcg.exe", L"DuckStation"},
    {L"dolphin.exe", L"Dolphin"},
};

inline constexpr size_t kKnownExeNamesCount =
    sizeof(kKnownExeNames) / sizeof(kKnownExeNames[0]);

} // namespace securecast
