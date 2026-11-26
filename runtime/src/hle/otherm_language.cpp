// Metroid: Other M: voices, text and subtitles each in their own language (Options > Game; [game] voices,
// text_language, subtitles in Config.toml).
//
// The USA and Japanese discs hold the same files: every CRI Sofdec movie has an English and a Japanese audio track, the
// voice banks hold both, and message/message_all.dat has eight text sections (0 JP, 1 US, 2 DE, 3 FR, 4 ES, 5 IT,
// 6 US French, 7 US Spanish). Only the game's code chooses:
//  - Voice language (0 Japanese, 1 English): read through rel_805AD2A4 by the movie start (the Sofdec audio track),
//    the voice playback (rel_8079F3E4 / rel_8079F898 look the line up in that language each time) and the system
//    save. The USA build lists only English (0x80AFD108 {1}; the Japanese build {1, 0}), so its own Options menu,
//    not finding Japanese there, applies list[-1], which is the Japanese build's 0: the getter never reads the stored
//    value back, it answers the port's choice. Changes apply from the next line.
//  - Text language: the word at r13-0x7A48 (0x80331F98). Messages look it up each time, but the HUD and the menus
//    take their text when they are built; the game's own Language option rebuilds them (rel_80659378,
//    rel_80654D1C(mode): the whole HUD for the mode it is in), which the port can't do from outside that menu. So the
//    port's text language is the one the game started with (Started): fn_8013AA38, the game's own setter (boot, its
//    save, its Options), stores it, and each frame puts it back over the game's own writes (fn_80137B44 maps French
//    and Spanish to the US ones on a USA disc).
//  - Fonts: fn_80137B44 loads japanese_font (the only one with kana and kanji; it has the Latin letters too) for text
//    language 0, global_font otherwise, at boot and when the game changes the language. The code tap at 0x80137D08
//    answers 0 when the text or the subtitles are Japanese.
//  - Subtitles: the subtitle manager rel_805B16F8 loads the message_all.dat section of the language it reads at
//    0x805B1778 and reloads by itself when that changes; the code tap there gives it the subtitle language, at once.
//    Their on/off switch is the byte at 0x80BB3690 (rel_805B2F38, the game's Subtitles option).
// The per-frame part runs in rel_805AD090, which the main loop calls once a frame (it counts frames, then goes on to
// rel_805AD2BC).
#include "hle_stubs.h"

#include "abi_bridge.h"
#include "code_patches.h"
#include "memory.h"
#include "runtime_config.h"

#include <atomic>
#include <cstdint>

namespace {
constexpr uint32_t kVoiceJapanese = 0;
constexpr uint32_t kVoiceEnglish = 1;
constexpr uint32_t kTextLanguage = 0x80331F98u;    // r13-0x7A48
constexpr uint32_t kSubtitlesOn = 0x80BB3690u;     // byte
constexpr uint32_t kFrameCount = 0x80C78A20u;      // rel_805AD090's counter (lis r4, 0x80C8; -0x75E0(r4))
constexpr uint32_t kSubtitleLanguageTap = 0x805B1778u;  // before r3
constexpr uint32_t kFontChoiceTap = 0x80137D08u;        // after r3
constexpr int32_t kJapanese = 0;

// What the game started with: its text language (-1 its own) and whether the Japanese fonts loaded.
std::atomic<bool> g_started{false};
std::atomic<int32_t> g_startText{-1};
std::atomic<bool> g_startJapaneseFont{false};

// The game's first call into this file (game.rel's start, right after main.dol loaded the fonts).
void Start() {
    if (g_started.load(std::memory_order_acquire)) return;
    const int32_t text = RuntimeConfigFile::TextLanguage();
    g_startText.store(text, std::memory_order_relaxed);
    g_startJapaneseFont.store(text == kJapanese || RuntimeConfigFile::SubtitleLanguage() == kJapanese,
                              std::memory_order_relaxed);
    g_started.store(true, std::memory_order_release);
}

int32_t TextLanguage() {
    return g_started.load(std::memory_order_acquire) ? g_startText.load(std::memory_order_relaxed)
                                                     : RuntimeConfigFile::TextLanguage();
}

void UpdateTaps(int32_t text, int32_t subtitles) {
    CodePatches::SetPortTap(kSubtitleLanguageTap, false, 3, subtitles >= 0, uint32_t(subtitles));
    CodePatches::SetPortTap(kFontChoiceTap, true, 3, text == kJapanese || subtitles == kJapanese, 0);
}
} // namespace

// Before the game starts (main.cpp) and when Options changes a language (settings_overlay.cpp): main.dol loads the
// fonts long before the main loop runs.
void OthermLanguageApply()
{
    UpdateTaps(TextLanguage(), RuntimeConfigFile::SubtitleLanguage());
}

// Options > Game: the game is running, so a text language chosen now applies at the next start.
bool OthermLanguageStarted()
{
    return g_started.load(std::memory_order_acquire);
}

// The Japanese fonts loaded at the start (Japanese subtitles show right away only then).
bool OthermJapaneseFontLoaded()
{
    return g_startJapaneseFont.load(std::memory_order_relaxed);
}

// OTHERM_SELFTEST (main.cpp): the Config.toml codes round-trip, and the translation has both taps (code_taps.txt).
bool OthermLanguageSelfCheck()
{
    bool ok = RuntimeConfigFile::GameLanguageIndex("off", false) == -1 &&
              RuntimeConfigFile::GameLanguageIndex("bogus", true) == -1;
    for (int32_t i = -2; i < 8; ++i) {
        ok = ok && RuntimeConfigFile::GameLanguageIndex(RuntimeConfigFile::GameLanguageCode(i), true) == i;
    }
    ok = ok && CodePatches::SetPortTap(kSubtitleLanguageTap, false, 3, false, 0) &&
         CodePatches::SetPortTap(kFontChoiceTap, true, 3, false, 0);
    OthermLanguageApply();
    return ok;
}

// 0x805AD2A4 -> rel_805AD2A4(): the voice language the game plays.
extern "C" uint32_t OthermVoiceLanguage_HLE()
{
    return RuntimeConfigFile::JapaneseVoices() ? kVoiceJapanese : kVoiceEnglish;
}

PPC_NATIVE_OVERRIDE(805AD2A4, OthermVoiceLanguage_HLE, uint32_t, (), ());

// 0x8013AA38 -> fn_8013AA38(language): the game sets its text language.
extern "C" void OthermSetTextLanguage_HLE(uint32_t language)
{
    Start();
    const int32_t chosen = TextLanguage();
    Memory::Write32(kTextLanguage, chosen >= 0 ? uint32_t(chosen) : language);
}

PPC_NATIVE_OVERRIDE_VOID(8013AA38, OthermSetTextLanguage_HLE, (uint32_t language), (language));

// 0x805AD090 -> rel_805AD090(): once a frame from the main loop; the game's own body is the counter and the jump.
extern "C" void OthermLanguageFrame_HLE(CpuContext* ctx)
{
    Start();
    const int32_t text = TextLanguage();
    const int32_t subtitles = RuntimeConfigFile::SubtitleLanguage();
    if (text >= 0 && Memory::Read32(kTextLanguage) != uint32_t(text)) Memory::Write32(kTextLanguage, uint32_t(text));
    if (subtitles != -1) {
        const uint8_t on = subtitles >= 0 ? 1 : 0;
        if (Memory::Read8(kSubtitlesOn) != on) Memory::Write8(kSubtitlesOn, on);
    }
    UpdateTaps(text, subtitles);

    const uint32_t count = Memory::Read32(kFrameCount);
    Memory::Write32(kFrameCount, count + 1);
    ctx->gpr[0] = count + 1;
    ctx->gpr[3] = count;
    ctx->gpr[4] = 0x80C80000u;  // its lis
    InvokeDirectCpu<0x805AD2BCu>(ctx);  // its tail call: returns to the main loop
}

PPC_NATIVE_OVERRIDE_VOID(805AD090, OthermLanguageFrame_HLE, (CpuContext* ctx), (ctx));
