// CRStreamingFix / AW2StreamingFix / ControlStreamingFix - texture streaming pool fix for Remedy's
// Northlight games. One source; game.h picks the game at build time.
//
// These games keep their streamed textures inside a pool, and blur them (a global mip bias) until what
// is in use fits. The pool is sized from the VRAM that is left over, so on 6-8 GB cards it ends up near
// its 100 MB minimum and the blur climbs to the limit. The add-on raises that minimum at runtime (and
// optionally caps the blur). Nothing on disk is patched.
//
// This file is what the games share: loading, the timer, settings and the tab in the ReShade menu.
// How a game's pool is found and changed is in its backend:
//   backend_heap.h         Control Resonant, Alan Wake 2   (addresses found by signature)
//   backend_tweakables.h   Control                         (settings the game looks up by name)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <mmsystem.h>
#include <psapi.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

// Dear ImGui through the function table ReShade hands to add-ons (no ImGui code is linked).
#pragma warning(push, 0)
#include <imgui.h>
#include <reshade_overlay.hpp>
#pragma warning(pop)

#include "game.h"

extern "C" __declspec(dllexport) const char *NAME = CRSF_NAME;
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Keeps " CRSF_GAME "'s texture streaming pool from shrinking to mush on low-VRAM GPUs. "
    "Settings: the " CRSF_NAME " tab, or " CRSF_NAME ".ini";

namespace
{
constexpr uint32_t kReShadeApiVersion = 18; // accepted by ReShade 6.8; older ReShade is offered lower versions
constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr DWORD kFirstTickMs = 1000; // ReShade loads and drops add-ons a few times at startup; skip those
constexpr DWORD kTickMs = 250;
constexpr DWORD kKeyTickMs = 30; // how often the on/off key is looked at
constexpr DWORD kMessageMs = 2500; // how long "ON" / "OFF" stays on screen
constexpr char kOverlayTitle[] = CRSF_NAME;
constexpr char kOsdTitle[] = "OSD"; // ReShade draws overlays of this name every frame, menu open or not

HMODULE g_module = nullptr;
uintptr_t g_exe_base = 0;
char g_exe_name[64] = "";
std::wstring g_dir; // folder of this DLL, with trailing backslash
HANDLE g_log = INVALID_HANDLE_VALUE;
HANDLE g_timer = nullptr;
HANDLE g_key_timer = nullptr;
volatile LONG g_busy = 0;
volatile LONG g_key_busy = 0;
volatile LONG g_paused = 0;       // the fix is switched off for now (the key, or the tab). Never saved.
volatile LONG g_toggle_key = 0;   // Config::toggle_key, toggle_sound and toggle_message as the other threads see them
volatile LONG g_toggle_sound = 0;
volatile LONG g_toggle_message = 1;
volatile LONG64 g_message_until = 0; // GetTickCount64() value up to which the on/off message is shown
bool g_osd_registered = false;

using play_sound_fn = BOOL(WINAPI *)(LPCWSTR, HMODULE, DWORD);
play_sound_fn g_play_sound = nullptr; // winmm's PlaySoundW, if the game has winmm loaded

using create_factory_fn = HRESULT(WINAPI *)(REFIID, void **);
create_factory_fn g_create_factory = nullptr; // dxgi's CreateDXGIFactory1, if the game has dxgi loaded

// Set in the first tick, before g_located: the graphics card and what the add-on starts from on it.
uint64_t g_vram_mb = 0; // 0 = not known
uint64_t g_default_min_mb = 2048, g_default_max_mb = 0;
volatile LONG g_located = 0;       // the backend found the game's pool, g_state.targets is final
volatile LONG g_locate_failed = 0; // unsupported game version
bool g_registered_with_reshade = false;
bool g_overlay_registered = false;
bool g_has_tab = false; // the settings tab was registered at load; unlike the flag above, never cleared
SRWLOCK g_lock = SRWLOCK_INIT; // guards g_state.cfg / base / announce / save_pending

struct Config
{
    uint64_t min_pool_mb = 2048;
    uint64_t max_pool_mb = 0;  // 0 = leave the game's value
    float bias_limit = -1.0f;  // < 0 = leave the game's value
    uint32_t log_interval_s = 5;
    uint32_t toggle_key = 0;   // virtual-key code plus kKeyCtrl / kKeyShift / kKeyAlt; 0 = no key
    bool toggle_sound = false; // two beeps when the key switches the fix
    bool toggle_message = true; // "ON" / "OFF" on screen when the key switches the fix
};

// ---------------------------------------------------------------------------------------
// logging

void log_line(const char *fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE)
        return;
    char buf[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    n += std::vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, args);
    va_end(args);
    if (n > static_cast<int>(sizeof(buf)) - 3)
        n = static_cast<int>(sizeof(buf)) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    DWORD written;
    WriteFile(g_log, buf, n, &written, nullptr);
}

void open_log()
{
    // Start a fresh log per game run, but keep appending if the add-on is reloaded within the run.
    wchar_t marker[2];
    const bool first_load = GetEnvironmentVariableW(CRSF_NAME_W L"_LOG", marker, 2) == 0;
    const std::wstring path = g_dir + CRSF_NAME_W L".log";
    g_log = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        first_load ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(g_log, 0, nullptr, FILE_END);
    SetEnvironmentVariableW(CRSF_NAME_W L"_LOG", L"1");
}

// ---------------------------------------------------------------------------------------
// what the backends build on

// Guarded memory access: game objects can be freed during shutdown.
template <typename T> bool read_mem(uintptr_t addr, T &out)
{
    __try
    {
        out = *reinterpret_cast<volatile T *>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

template <typename T> bool write_mem(uintptr_t addr, T value)
{
    __try
    {
        *reinterpret_cast<volatile T *>(addr) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool read_f32(uintptr_t addr, float &out)
{
    return read_mem(addr, out);
}

bool write_f32(uintptr_t addr, float value)
{
    return write_mem(addr, value);
}

// The first loaded module that exports `name` (and `also`, if given). Asks the loader, so only for DllMain.
HMODULE find_module_exporting(const char *name, const char *also = nullptr)
{
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        return nullptr;
    const DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
    for (DWORD i = 0; i < count; ++i)
        if (GetProcAddress(modules[i], name) && (!also || GetProcAddress(modules[i], also)))
            return modules[i];
    return nullptr;
}

void tooltip(const char *text)
{
    ImGui::SetItemTooltip("%s", text);
}

// Each backend provides: kHasMaxPool, the CRSF_INI_* texts, Targets, Baseline, Live, on_attach(), locate(),
// apply(), read_live(), log_stats() and draw_status().
#if defined(CRSF_BACKEND_TWEAKABLES)
#include "backend_tweakables.h"
#else
#include "backend_heap.h"
#endif

struct State
{
    bool started = false;
    bool announce = true;      // log the limits at the next apply
    bool save_pending = false; // cfg was changed in the ReShade menu and must be written to the ini
    bool paused = false;       // g_paused as last applied; tick thread only
    Targets targets;
    Config cfg;
    FILETIME cfg_time = {}; // tick thread only
    Baseline base;
    ULONGLONG last_stats = 0;
};
State g_state;

// FileVersion of the game executable, for the log. Read from the mapped image instead of through
// version.dll: the fixed part of a version resource starts with a signature and sits on a 4-byte boundary.
bool exe_version(uintptr_t base, unsigned version[4])
{
    __try
    {
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
        const auto *words = reinterpret_cast<const uint32_t *>(base + dir.VirtualAddress);
        for (size_t i = 0; i + 4 <= dir.Size / 4; ++i)
        {
            if (words[i] != 0xFEEF04BD) // VS_FIXEDFILEINFO: signature, struct version, file version high, low
                continue;
            version[0] = words[i + 2] >> 16;
            version[1] = words[i + 2] & 0xFFFF;
            version[2] = words[i + 3] >> 16;
            version[3] = words[i + 3] & 0xFFFF;
            return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return false;
}

void log_game_version(uintptr_t base, const char *exe_name)
{
    unsigned version[4];
    if (exe_version(base, version))
        log_line("Game executable: %s %u.%u.%u.%u", exe_name, version[0], version[1], version[2], version[3]);
}

// ---------------------------------------------------------------------------------------
// the on/off key

constexpr uint32_t kKeyCodeMask = 0xFF, kKeyCtrl = 0x100, kKeyShift = 0x200, kKeyAlt = 0x400;

struct KeyName
{
    uint8_t vk;
    const char *name;
};
// The keys offered in the tab. The ini also takes a single letter or digit, or a virtual-key code as a number.
constexpr KeyName kKeyNames[] = {
    {VK_F1, "F1"},           {VK_F2, "F2"},           {VK_F3, "F3"},
    {VK_F4, "F4"},           {VK_F5, "F5"},           {VK_F6, "F6"},
    {VK_F7, "F7"},           {VK_F8, "F8"},           {VK_F9, "F9"},
    {VK_F10, "F10"},         {VK_F11, "F11"},         {VK_F12, "F12"},
    {VK_INSERT, "Insert"},   {VK_DELETE, "Delete"},   {VK_HOME, "Home"},
    {VK_END, "End"},         {VK_PRIOR, "PageUp"},    {VK_NEXT, "PageDown"},
    {VK_PAUSE, "Pause"},     {VK_SCROLL, "ScrollLock"},
    {VK_NUMPAD0, "Numpad0"}, {VK_NUMPAD1, "Numpad1"}, {VK_NUMPAD2, "Numpad2"},
    {VK_NUMPAD3, "Numpad3"}, {VK_NUMPAD4, "Numpad4"}, {VK_NUMPAD5, "Numpad5"},
    {VK_NUMPAD6, "Numpad6"}, {VK_NUMPAD7, "Numpad7"}, {VK_NUMPAD8, "Numpad8"},
    {VK_NUMPAD9, "Numpad9"},
};

void key_name(uint32_t vk, char *out, size_t size)
{
    for (const KeyName &k : kKeyNames)
        if (k.vk == vk)
        {
            std::snprintf(out, size, "%s", k.name);
            return;
        }
    if (!vk)
        std::snprintf(out, size, "None");
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        std::snprintf(out, size, "%c", static_cast<char>(vk));
    else
        std::snprintf(out, size, "0x%02X", vk);
}

// "Ctrl+F8", "K", "None"
void key_text(uint32_t key, char *out, size_t size)
{
    char name[16];
    key_name(key & kKeyCodeMask, name, sizeof(name));
    std::snprintf(out, size, "%s%s%s%s", key & kKeyCtrl ? "Ctrl+" : "", key & kKeyShift ? "Shift+" : "",
                  key & kKeyAlt ? "Alt+" : "", name);
}

uint32_t key_code(const char *name)
{
    for (const KeyName &k : kKeyNames)
        if (_stricmp(k.name, name) == 0)
            return k.vk;
    if (name[0] && !name[1] && std::isalnum(static_cast<unsigned char>(name[0])))
        return static_cast<uint32_t>(std::toupper(static_cast<unsigned char>(name[0])));
    char *end = nullptr;
    const unsigned long code = std::strtoul(name, &end, 0);
    return end != name && *end == '\0' && code >= VK_BACK && code < 0xFF ? code : 0; // below VK_BACK: mouse buttons
}

// Anything that is not a key (including "None") gives 0.
uint32_t parse_key(const wchar_t *text)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%ls", text);
    uint32_t key = 0;
    for (char *part = buf; part;)
    {
        char *next = std::strchr(part, '+');
        if (next)
            *next++ = '\0';
        while (*part == ' ')
            ++part;
        for (size_t n = std::strlen(part); n && part[n - 1] == ' ';)
            part[--n] = '\0';
        if (_stricmp(part, "Ctrl") == 0)
            key |= kKeyCtrl;
        else if (_stricmp(part, "Shift") == 0)
            key |= kKeyShift;
        else if (_stricmp(part, "Alt") == 0)
            key |= kKeyAlt;
        else
            key = (key & ~kKeyCodeMask) | key_code(part);
        part = next;
    }
    return key & kKeyCodeMask ? key : 0;
}

bool key_down(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// The key with exactly its modifiers, so that F8 does not answer to Ctrl+F8.
bool toggle_key_down(uint32_t key)
{
    return key_down(static_cast<int>(key & kKeyCodeMask)) && key_down(VK_CONTROL) == ((key & kKeyCtrl) != 0) &&
           key_down(VK_SHIFT) == ((key & kKeyShift) != 0) && key_down(VK_MENU) == ((key & kKeyAlt) != 0);
}

bool game_in_front()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Two short tones as a WAV file in memory: 880 Hz for 70 ms, then `second_hz` for 90 ms.
constexpr uint32_t kWavRate = 22050;
constexpr uint32_t kWavSamples = kWavRate * 160 / 1000;
struct Wav
{
    uint8_t bytes[44 + kWavSamples * 2];
};

void build_wav(Wav &wav, double second_hz)
{
    uint8_t *p = wav.bytes;
    const auto put = [&p](const void *src, size_t size) {
        std::memcpy(p, src, size);
        p += size;
    };
    const auto put32 = [&put](uint32_t v) { put(&v, 4); };
    const auto put16 = [&put](uint16_t v) { put(&v, 2); };
    put("RIFF", 4);
    put32(36 + kWavSamples * 2);
    put("WAVEfmt ", 8);
    put32(16);
    put16(1); // PCM
    put16(1); // mono
    put32(kWavRate);
    put32(kWavRate * 2);
    put16(2);
    put16(16);
    put("data", 4);
    put32(kWavSamples * 2);
    const uint32_t first = kWavRate * 70 / 1000, fade = kWavRate * 8 / 1000;
    for (uint32_t i = 0; i < kWavSamples; ++i)
    {
        const bool second = i >= first;
        const uint32_t at = second ? i - first : i, length = second ? kWavSamples - first : first;
        const double gain = std::min({1.0, at / static_cast<double>(fade), (length - at) / static_cast<double>(fade)});
        const double wave = std::sin(6.283185307179586 * (second ? second_hz : 880.0) * at / kWavRate);
        put16(static_cast<uint16_t>(static_cast<int16_t>(wave * gain * 8000.0)));
    }
}

// Falling tones: off. Rising: on. Played through the game's own audio, so it follows the game's volume.
// Beep() is the fallback: it belongs to Windows' system sounds, which are silent on some setups.
void play_toggle_sound(bool now_off)
{
    static Wav on, off;
    static bool built = false;
    if (!built)
    {
        build_wav(on, 1175.0);
        build_wav(off, 587.0);
        built = true;
    }
    if (g_play_sound && g_play_sound(reinterpret_cast<LPCWSTR>((now_off ? off : on).bytes), nullptr,
                                     SND_MEMORY | SND_SYNC | SND_NODEFAULT))
        return;
    Beep(880, 60);
    Beep(now_off ? 587 : 1175, 90);
}

// Runs every kKeyTickMs. The key is polled, not hooked: nothing of the game's input is touched. It only
// flips g_paused; the next tick applies it.
VOID CALLBACK key_tick(PVOID, BOOLEAN)
{
    if (InterlockedCompareExchange(&g_key_busy, 1, 0) != 0)
        return; // the sound of the last press is still playing

    static bool was_down = false;
    const uint32_t key = static_cast<uint32_t>(g_toggle_key);
    const bool down = key && g_located && toggle_key_down(key) && game_in_front();
    if (down && !was_down)
    {
        const bool now_off = InterlockedExchange(&g_paused, g_paused ? 0 : 1) == 0;
        InterlockedExchange64(&g_message_until, static_cast<LONG64>(GetTickCount64() + kMessageMs));
        if (g_toggle_sound)
            play_toggle_sound(now_off);
    }
    was_down = down;

    InterlockedExchange(&g_key_busy, 0);
}

// Called by ReShade every frame, with its menu open or closed. Shows "ON" / "OFF" for a moment after
// the key was used, in a small window of its own at the top of the screen.
void draw_message(void * /*reshade::api::effect_runtime*/)
{
    if (!g_toggle_message || GetTickCount64() >= static_cast<ULONGLONG>(g_message_until))
        return;
    const bool off = g_paused != 0;
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, 2.0f * ImGui::GetFontSize()), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (ImGui::Begin(CRSF_NAME " message", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, off ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f) : ImVec4(0.5f, 1.0f, 0.5f, 1.0f));
        ImGui::TextUnformatted(off ? CRSF_NAME ": OFF" : CRSF_NAME ": ON");
        ImGui::PopStyleColor();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------
// presets by graphics card memory

struct Preset
{
    const char *label;    // the button in the tab
    uint32_t up_to_gb;    // cards up to this size start from it
    uint32_t min_pool_mb;
    uint32_t max_pool_mb; // 0 = leave the game's value
    const char *basis;    // where the numbers come from
};
// Only the 8 GB row was played by the author. The others rest on what users reported, or on nothing yet.
constexpr Preset kPresets[] = {
    {"4 GB", 5, 1536, 0, "One user runs 1664 MB on low settings; another gets stutter in combat at 2048."},
    {"6 GB", 7, 1792, 0, "Not tested yet: halfway between the 4 GB and 8 GB values."},
    {"8 GB", 10, 2048, 0, "What the add-on was made and tested with, path tracing on."},
    {"12 GB", 14, 3072, 6144, "Not tested yet: the same share of the card as on 8 GB."},
    {"16 GB+", 0xFFFFFFFF, 4096, 8192, "One user with 16 GB runs minimum and maximum at 8192 MB."},
};

struct PoolDefaults
{
    uint64_t min_mb, max_mb;
};

// What a card with this much memory starts from. 0 (not known) gives the 8 GB values.
PoolDefaults defaults_for_vram(uint64_t vram_mb)
{
    if (!vram_mb)
        return {2048, 0};
    const uint64_t gb = (vram_mb + 512) / 1024; // cards report a little less than their nominal size
    if (gb < 4)
        return {std::max<uint64_t>(256, vram_mb * 3 / 8 / 64 * 64), 0}; // the 4 GB preset's share of the card
    for (const Preset &p : kPresets)
        if (gb <= p.up_to_gb)
            return {p.min_pool_mb, p.max_pool_mb};
    return {2048, 0};
}

// Dedicated memory of the largest graphics card in MB, 0 if it cannot be asked. On a laptop that is the
// discrete card, which is the one the game runs on.
uint64_t detect_vram_mb()
{
    if (!g_create_factory)
        return 0;
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(g_create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory)
        return 0;
    uint64_t best = 0;
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            best = std::max<uint64_t>(best, desc.DedicatedVideoMemory / kMiB);
        adapter->Release();
    }
    factory->Release();
    return best;
}

// ---------------------------------------------------------------------------------------
// config

std::wstring ini_path()
{
    return g_dir + CRSF_NAME_W L".ini";
}

void write_default_ini()
{
    const std::wstring path = ini_path();
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return;
    static const char text[] =
        "; " CRSF_NAME " settings. Edits are picked up while the game runs.\r\n"
        "; The same settings are in the " CRSF_NAME " tab of the ReShade menu.\r\n"
        "[" CRSF_NAME "]\r\n"
        CRSF_INI_MIN_POOL
        "; When this file is first written, the value is picked for the memory of your graphics card:\r\n"
        "; 1536 for 4 GB, 1792 for 6 GB, 2048 for 8 GB, 3072 for 12 GB, 4096 for 16 GB and more.\r\n"
        "MinPoolMB=2048\r\n"
        CRSF_INI_MAX_POOL_BLOCK
        "; Largest mip bias the streamer may add when textures don't fit the pool (game default 10).\r\n"
        "; -1 = leave the game's value.\r\n"
        "BiasLimit=-1\r\n"
        "; Seconds between stats lines in " CRSF_NAME ".log. 0 = off.\r\n"
        "LogIntervalSec=5\r\n"
        "; Key that switches the fix off and on while playing, e.g. F8 or Ctrl+F8. None = no key.\r\n"
        "; Off lasts until the key is pressed again; the fix is always on when the game starts.\r\n"
        "ToggleKey=None\r\n"
        "; 1 = show ON / OFF on screen for a moment when the key is used. 0 = no message.\r\n"
        "; Read when the game starts: switching it on later needs a restart of the game.\r\n"
        "ToggleMessage=1\r\n"
        "; 1 = two beeps when the key is used (rising: on, falling: off). 0 = silent.\r\n"
        "ToggleSound=0\r\n";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD written;
    WriteFile(h, text, sizeof(text) - 1, &written, nullptr);
    CloseHandle(h);

    // The text above carries the 8 GB values; put this card's in their place.
    wchar_t buf[32];
    if (g_default_min_mb != 2048)
    {
        std::swprintf(buf, 32, L"%llu", static_cast<unsigned long long>(g_default_min_mb));
        WritePrivateProfileStringW(CRSF_NAME_W, L"MinPoolMB", buf, path.c_str());
    }
    if (g_default_max_mb != 0) // stays 0 in a game without a pool ceiling of its own
    {
        std::swprintf(buf, 32, L"%llu", static_cast<unsigned long long>(g_default_max_mb));
        WritePrivateProfileStringW(CRSF_NAME_W, L"MaxPoolMB", buf, path.c_str());
    }
}

Config read_config()
{
    const std::wstring path = ini_path();
    Config c;
    c.min_pool_mb = GetPrivateProfileIntW(CRSF_NAME_W, L"MinPoolMB", static_cast<INT>(g_default_min_mb), path.c_str());
    if constexpr (kHasMaxPool)
        c.max_pool_mb = GetPrivateProfileIntW(CRSF_NAME_W, L"MaxPoolMB", 0, path.c_str());
    c.log_interval_s = GetPrivateProfileIntW(CRSF_NAME_W, L"LogIntervalSec", 5, path.c_str());
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(CRSF_NAME_W, L"BiasLimit", L"-1", buf, 64, path.c_str());
    c.bias_limit = static_cast<float>(std::wcstod(buf, nullptr));
    GetPrivateProfileStringW(CRSF_NAME_W, L"ToggleKey", L"None", buf, 64, path.c_str());
    c.toggle_key = parse_key(buf);
    c.toggle_sound = GetPrivateProfileIntW(CRSF_NAME_W, L"ToggleSound", 0, path.c_str()) != 0;
    c.toggle_message = GetPrivateProfileIntW(CRSF_NAME_W, L"ToggleMessage", 1, path.c_str()) != 0;
    if (c.min_pool_mb > 16384)
        c.min_pool_mb = 16384;
    if (c.max_pool_mb > 16384)
        c.max_pool_mb = 16384;
    if (c.bias_limit > 20.0f)
        c.bias_limit = 20.0f;
    return c;
}

// Writes the values in place; comments and anything else in the file stay.
void write_config(const Config &c)
{
    const std::wstring path = ini_path();
    wchar_t buf[32];
    std::swprintf(buf, 32, L"%llu", static_cast<unsigned long long>(c.min_pool_mb));
    WritePrivateProfileStringW(CRSF_NAME_W, L"MinPoolMB", buf, path.c_str());
    if constexpr (kHasMaxPool)
    {
        std::swprintf(buf, 32, L"%llu", static_cast<unsigned long long>(c.max_pool_mb));
        WritePrivateProfileStringW(CRSF_NAME_W, L"MaxPoolMB", buf, path.c_str());
    }
    if (c.bias_limit < 0.0f)
        std::swprintf(buf, 32, L"-1");
    else
        std::swprintf(buf, 32, L"%.2f", static_cast<double>(c.bias_limit));
    WritePrivateProfileStringW(CRSF_NAME_W, L"BiasLimit", buf, path.c_str());
    std::swprintf(buf, 32, L"%u", c.log_interval_s);
    WritePrivateProfileStringW(CRSF_NAME_W, L"LogIntervalSec", buf, path.c_str());
    char key[32];
    key_text(c.toggle_key, key, sizeof(key));
    std::swprintf(buf, 32, L"%hs", key);
    WritePrivateProfileStringW(CRSF_NAME_W, L"ToggleKey", buf, path.c_str());
    WritePrivateProfileStringW(CRSF_NAME_W, L"ToggleSound", c.toggle_sound ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(CRSF_NAME_W, L"ToggleMessage", c.toggle_message ? L"1" : L"0", path.c_str());
}

FILETIME ini_time()
{
    WIN32_FILE_ATTRIBUTE_DATA d = {};
    GetFileAttributesExW(ini_path().c_str(), GetFileExInfoStandard, &d);
    return d.ftLastWriteTime;
}

void log_config(const char *what, const Config &c)
{
    char max_pool[40] = "";
    if constexpr (kHasMaxPool)
        std::snprintf(max_pool, sizeof(max_pool), " MaxPoolMB=%llu", static_cast<unsigned long long>(c.max_pool_mb));
    char key[32];
    key_text(c.toggle_key, key, sizeof(key));
    log_line("%s: MinPoolMB=%llu%s BiasLimit=%.2f LogIntervalSec=%u ToggleKey=%s", what,
             static_cast<unsigned long long>(c.min_pool_mb), max_pool, c.bias_limit, c.log_interval_s, key);
}

// What "off" applies: every value left to the game.
Config game_values(Config c)
{
    c.min_pool_mb = 0;
    c.max_pool_mb = 0;
    c.bias_limit = -1.0f;
    return c;
}

// ---------------------------------------------------------------------------------------
// the timer

// Runs on a thread pool thread every kTickMs. Must not take the loader lock: DllMain waits for it on unload.
VOID CALLBACK tick(PVOID, BOOLEAN)
{
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
        return; // previous tick still running (the first one may scan the executable)

    State &s = g_state;
    if (!s.started)
    {
        s.started = true;
        open_log();
        log_line(CRSF_NAME " " CRSF_VERSION " (%s)",
                 !g_registered_with_reshade ? "loaded without ReShade"
                 : g_has_tab                ? "ReShade add-on, settings tab available"
                                            : "ReShade add-on, no settings tab: this ReShade lacks the ImGui 1.92.5 table");
        log_game_version(g_exe_base, g_exe_name);
        if (locate(g_exe_base, s.targets))
        {
            g_vram_mb = detect_vram_mb();
            const PoolDefaults defaults = defaults_for_vram(g_vram_mb);
            g_default_min_mb = defaults.min_mb;
            if constexpr (kHasMaxPool)
                g_default_max_mb = defaults.max_mb;
            if (g_vram_mb)
                log_line("Graphics card: %llu MB of memory. A new ini starts with a minimum pool of %llu MB for it.",
                         static_cast<unsigned long long>(g_vram_mb), static_cast<unsigned long long>(g_default_min_mb));
            else
                log_line("Graphics card memory not known. A new ini starts with a minimum pool of 2048 MB.");
            write_default_ini();
            s.cfg = read_config();
            s.cfg_time = ini_time();
            log_config("Config", s.cfg);
            InterlockedExchange(&g_located, 1); // from here on the settings tab may touch g_state (under g_lock)
        }
        else
        {
            InterlockedExchange(&g_locate_failed, 1);
        }
    }

    if (g_located)
    {
        // File I/O happens outside the lock; the settings tab only ever try-locks, so it never waits on us.
        const FILETIME now_time = ini_time();
        const bool file_changed = CompareFileTime(&now_time, &s.cfg_time) != 0;
        Config from_file;
        if (file_changed)
            from_file = read_config();

        bool save = false;
        AcquireSRWLockExclusive(&g_lock);
        if (s.save_pending)
        {
            s.save_pending = false; // an edit in the menu wins over a simultaneous edit of the file
            save = true;
        }
        else if (file_changed)
        {
            s.cfg = from_file;
            s.announce = true;
        }
        const bool paused = g_paused != 0;
        if (paused != s.paused)
        {
            s.paused = paused;
            s.announce = true;
            log_line(paused ? "Switched off: the game's own values are back until it is switched on again."
                            : "Switched on.");
        }
        const Config cfg = s.cfg;
        apply(s.targets, paused ? game_values(cfg) : cfg, s.base, s.announce);
        ReleaseSRWLockExclusive(&g_lock);
        InterlockedExchange(&g_toggle_key, static_cast<LONG>(cfg.toggle_key));
        InterlockedExchange(&g_toggle_sound, cfg.toggle_sound ? 1 : 0);
        InterlockedExchange(&g_toggle_message, cfg.toggle_message ? 1 : 0);

        if (save)
        {
            write_config(cfg);
            s.cfg_time = ini_time();
            log_config("Config saved from the ReShade menu", cfg);
        }
        else if (file_changed)
        {
            s.cfg_time = now_time;
            log_config("Config reloaded", cfg);
        }

        const ULONGLONG now = GetTickCount64();
        if (cfg.log_interval_s && now - s.last_stats >= cfg.log_interval_s * 1000ull)
        {
            s.last_stats = now;
            log_stats(s.targets);
        }
    }

    InterlockedExchange(&g_busy, 0);
}

// ---------------------------------------------------------------------------------------
// settings tab in the ReShade menu

// Called by ReShade on its render thread while the tab is visible. It never blocks on the tick thread and
// does no file I/O: edits are handed over under a try-lock and applied and saved by the next tick.
void draw_overlay(void * /*reshade::api::effect_runtime*/)
{
    // ReShade only docks add-on windows as tabs when it builds its layout for the first time. On an existing
    // layout a new window floats at ImGui's default spot, small and under the main menu, so place it once.
    const bool floating = !ImGui::IsWindowDocked();
    if (floating)
    {
        const float em = ImGui::GetFontSize();
        const float width = 34.0f * em;
        ImGui::SetWindowSize(ImVec2(width, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::SetWindowPos(ImVec2(std::max(0.0f, ImGui::GetIO().DisplaySize.x - width - 2.0f * em), 3.0f * em),
                            ImGuiCond_FirstUseEver);
    }

    if (!g_located)
    {
        ImGui::TextUnformatted(g_locate_failed ? "This game version is not supported. See " CRSF_NAME ".log."
                                               : "Starting...");
        return;
    }

    State &s = g_state;
    static Config ui_cfg;
    static Baseline ui_base;
    static bool ui_synced = false, ui_dirty = false, ui_save = false;
    if (TryAcquireSRWLockExclusive(&g_lock))
    {
        if (ui_dirty)
        {
            s.cfg = ui_cfg;
            s.announce = true;
            ui_dirty = false;
        }
        else
        {
            ui_cfg = s.cfg; // picks up edits made in the ini file
        }
        if (ui_save)
        {
            s.save_pending = true;
            ui_save = false;
        }
        ui_base = s.base;
        ui_synced = true;
        ReleaseSRWLockExclusive(&g_lock);
    }
    if (!ui_synced)
    {
        ImGui::TextUnformatted("Starting...");
        return;
    }

    ImGui::PushTextWrapPos(0.0f); // wrap text lines at the window edge instead of clipping them
    const bool paused = g_paused != 0;
    ImGui::SeparatorText("Right now");
    if (paused)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
        ImGui::TextUnformatted("The fix is switched off. The game's own values are in use.");
        ImGui::PopStyleColor();
    }
    draw_status(read_live(s.targets));

    ImGui::SeparatorText("Settings");
    bool changed = false, released = false;
    char text[192];
    char format[64];

    bool fix_on = !paused;
    if (ImGui::Checkbox("Fix on", &fix_on))
        InterlockedExchange(&g_paused, fix_on ? 0 : 1);
    tooltip("Switch the fix off to compare, or for a part of the game where it stutters. This is not saved: "
            "the fix is on again the next time the game starts.");

    int min_mb = static_cast<int>(ui_cfg.min_pool_mb);
    if (min_mb)
        std::snprintf(format, sizeof(format), "%%d MB");
    else
        std::snprintf(format, sizeof(format), "game value (%llu MB)",
                      static_cast<unsigned long long>(ui_base.game_min_mb()));
    if (ImGui::SliderInt("Minimum pool", &min_mb, 0, 8192, format))
    {
        ui_cfg.min_pool_mb = static_cast<uint64_t>(std::clamp((min_mb + 32) / 64 * 64, 0, 16384));
        changed = true;
    }
    released |= ImGui::IsItemDeactivatedAfterEdit();
    tooltip("Smallest texture pool. Higher is sharper. Above your free VRAM, Windows pages textures to system RAM, "
            "which can stutter. 0 leaves the game's value.");

    if constexpr (kHasMaxPool)
    {
        int max_mb = static_cast<int>(ui_cfg.max_pool_mb);
        if (max_mb)
            std::snprintf(format, sizeof(format), "%%d MB");
        else
            std::snprintf(format, sizeof(format), "game value (%llu MB)",
                          static_cast<unsigned long long>(ui_base.game_max_mb()));
        if (ImGui::SliderInt("Maximum pool", &max_mb, 0, 8192, format))
        {
            ui_cfg.max_pool_mb = static_cast<uint64_t>(std::clamp((max_mb + 32) / 64 * 64, 0, 16384));
            changed = true;
        }
        released |= ImGui::IsItemDeactivatedAfterEdit();
        tooltip("Largest texture pool. 0 leaves the game's value, which follows Texture Resolution. "
                "It is raised to the minimum if the minimum is higher.");
    }

    if (g_vram_mb)
        std::snprintf(text, sizeof(text), "Presets by graphics card memory (yours: %llu GB):",
                      static_cast<unsigned long long>((g_vram_mb + 512) / 1024));
    else
        std::snprintf(text, sizeof(text), "Presets by graphics card memory:");
    ImGui::TextUnformatted(text);
    for (const Preset &p : kPresets)
    {
        if (&p != kPresets)
            ImGui::SameLine();
        if (ImGui::Button(p.label))
        {
            ui_cfg.min_pool_mb = p.min_pool_mb;
            if constexpr (kHasMaxPool)
                ui_cfg.max_pool_mb = p.max_pool_mb;
            changed = released = true;
        }
        int n = std::snprintf(text, sizeof(text), "Minimum pool %u MB", p.min_pool_mb);
        if constexpr (kHasMaxPool)
            if (p.max_pool_mb)
                n += std::snprintf(text + n, sizeof(text) - n, ", maximum pool %u MB", p.max_pool_mb);
        std::snprintf(text + n, sizeof(text) - n, ". %s", p.basis);
        tooltip(text);
    }

    bool limit_blur = ui_cfg.bias_limit >= 0.0f;
    if (ImGui::Checkbox("Limit blur", &limit_blur))
    {
        ui_cfg.bias_limit = limit_blur ? 2.0f : -1.0f;
        changed = released = true;
    }
    tooltip("Cap the mip levels the streamer may drop. Without a bigger pool this does not sharpen anything: "
            "textures that no longer fit just fail to load.");
    if (limit_blur)
    {
        float limit = ui_cfg.bias_limit;
        if (ImGui::SliderFloat("Blur limit", &limit, 0.0f, 10.0f, "%.1f mips"))
        {
            ui_cfg.bias_limit = std::clamp(limit, 0.0f, 20.0f);
            changed = true;
        }
        released |= ImGui::IsItemDeactivatedAfterEdit();
        std::snprintf(text, sizeof(text), "Game value: %.0f mips.", ui_base.game_bias_limit);
        tooltip(text);
    }

    int log_s = static_cast<int>(ui_cfg.log_interval_s);
    if (ImGui::SliderInt("Log interval", &log_s, 0, 60, log_s ? "%d s" : "off"))
    {
        ui_cfg.log_interval_s = static_cast<uint32_t>(std::clamp(log_s, 0, 3600));
        changed = true;
    }
    released |= ImGui::IsItemDeactivatedAfterEdit();
    tooltip("Seconds between stats lines in " CRSF_NAME ".log.");

    const uint32_t vk = ui_cfg.toggle_key & kKeyCodeMask;
    key_name(vk, text, sizeof(text));
    if (ImGui::BeginCombo("On/off key", text))
    {
        if (ImGui::Selectable("None", vk == 0))
        {
            ui_cfg.toggle_key = 0;
            changed = released = true;
        }
        for (const KeyName &k : kKeyNames)
            if (ImGui::Selectable(k.name, k.vk == vk))
            {
                ui_cfg.toggle_key = (ui_cfg.toggle_key & ~kKeyCodeMask) | k.vk;
                changed = released = true;
            }
        ImGui::EndCombo();
    }
    tooltip("A key that switches the fix off and on while you play, the same as \"Fix on\" above. "
            "A letter or digit key can be set as ToggleKey in " CRSF_NAME ".ini.");
    if (vk)
    {
        const struct
        {
            const char *label;
            uint32_t bit;
        } modifiers[] = {{"Ctrl", kKeyCtrl}, {"Shift", kKeyShift}, {"Alt", kKeyAlt}};
        for (const auto &modifier : modifiers)
        {
            bool held = (ui_cfg.toggle_key & modifier.bit) != 0;
            if (ImGui::Checkbox(modifier.label, &held))
            {
                ui_cfg.toggle_key ^= modifier.bit;
                changed = released = true;
            }
            ImGui::SameLine();
        }
        if (ImGui::Checkbox("Message", &ui_cfg.toggle_message))
            changed = released = true;
        tooltip("Show ON or OFF at the top of the screen for a moment when the key is used.");
        ImGui::SameLine();
        if (ImGui::Checkbox("Beep", &ui_cfg.toggle_sound))
            changed = released = true;
        tooltip("Two beeps when the key is used: rising for on, falling for off.");
        if (ui_cfg.toggle_message && !g_osd_registered)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
            ImGui::TextUnformatted("The message is set up when the game starts. It will show from the next start on.");
            ImGui::PopStyleColor();
        }
    }

    if (ImGui::Button("Defaults"))
    {
        const Config old = ui_cfg;
        ui_cfg = Config();
        ui_cfg.min_pool_mb = g_default_min_mb;
        ui_cfg.max_pool_mb = g_default_max_mb;
        ui_cfg.toggle_key = old.toggle_key; // the key is not one of the values the button is about
        ui_cfg.toggle_sound = old.toggle_sound;
        ui_cfg.toggle_message = old.toggle_message;
        changed = released = true;
    }
    tooltip("The preset for this graphics card, with the blur limit left to the game.");

    if (floating)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::TextUnformatted("Tip: drag this window's title onto the ReShade tab bar to dock it.");
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();

    if (changed)
        ui_dirty = true; // handed to the tick thread at the next frame's try-lock
    if (released)
        ui_save = true; // written to the ini by the tick thread
}

// ---------------------------------------------------------------------------------------
// loading

// ReShade add-on registration without the SDK: find the module that exports ReShadeRegisterAddon.
HMODULE find_reshade()
{
    return find_module_exporting("ReShadeRegisterAddon", "ReShadeUnregisterAddon");
}

bool has_addon_extension(const wchar_t *path)
{
    const wchar_t *ext = std::wcsrchr(path, L'.');
    return ext && (_wcsicmp(ext, L".addon64") == 0 || _wcsicmp(ext, L".addon") == 0);
}

using overlay_callback = void (*)(void *);
using overlay_fn = void(__cdecl *)(const char *, overlay_callback);

// The settings tab needs ReShade's ImGui function table for exactly the ImGui version this was built with.
// Without it the fix still runs; it is just configured through the ini only.
void register_overlay(HMODULE reshade)
{
    using table_fn = const imgui_function_table *(__cdecl *)(uint32_t);
    const auto get_table = reinterpret_cast<table_fn>(GetProcAddress(reshade, "ReShadeGetImGuiFunctionTable"));
    const auto reg = reinterpret_cast<overlay_fn>(GetProcAddress(reshade, "ReShadeRegisterOverlay"));
    if (!get_table || !reg)
        return;
    const imgui_function_table *table = get_table(IMGUI_VERSION_NUM);
    if (!table)
        return;
    imgui_function_table_instance() = table;
    reg(kOverlayTitle, &draw_overlay);
    g_overlay_registered = g_has_tab = true;

    // The on/off message. Overlays can only be registered here, at load, so the setting is read early.
    // With it off nothing is registered: ReShade shows an "OSD" overlay as a small window of that name
    // while one of its own messages is up (after a screenshot, for one), and nobody should get that unasked.
    if (GetPrivateProfileIntW(CRSF_NAME_W, L"ToggleMessage", 1, ini_path().c_str()) != 0)
    {
        reg(kOsdTitle, &draw_message);
        g_osd_registered = true;
    }
}

void unregister_overlay(HMODULE reshade)
{
    if (!g_overlay_registered)
        return;
    if (const auto unreg = reinterpret_cast<overlay_fn>(GetProcAddress(reshade, "ReShadeUnregisterOverlay")))
    {
        unreg(kOverlayTitle, &draw_overlay);
        if (g_osd_registered)
            unreg(kOsdTitle, &draw_message);
    }
    g_overlay_registered = g_osd_registered = false;
}

// A function of a Windows DLL the game has loaded: winmm's PlaySoundW for the on/off sound, dxgi's
// CreateDXGIFactory1 to ask for the card's memory. Only the DLL in the Windows folder counts, never a file
// of that name next to the game (a mod loader's, upscaler's or ReShade's stand-in), and nothing is loaded
// for it. Asks the loader, so only for DllMain.
FARPROC system_function(const wchar_t *dll, const char *name)
{
    wchar_t path[MAX_PATH + 32];
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return nullptr;
    path[length] = L'\\';
    wcscpy_s(path + length + 1, 31, dll);
    const HMODULE module = GetModuleHandleW(path);
    return module ? GetProcAddress(module, name) : nullptr;
}

bool process_is_exiting()
{
    using fn = BOOLEAN(NTAPI *)();
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
        if (const auto in_progress = reinterpret_cast<fn>(GetProcAddress(ntdll, "RtlDllShutdownInProgress")))
            return in_progress() != FALSE;
    return false;
}

void stop_timer(HANDLE &timer)
{
    if (!timer)
        return;
    // Wait for a running tick to finish before the DLL goes away. Ticks never need the loader lock.
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (done)
    {
        if (DeleteTimerQueueTimer(nullptr, timer, done) || GetLastError() == ERROR_IO_PENDING)
            WaitForSingleObject(done, 5000);
        CloseHandle(done);
    }
    else
    {
        DeleteTimerQueueTimer(nullptr, timer, INVALID_HANDLE_VALUE);
    }
    timer = nullptr;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;

        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        const wchar_t *exe_name = std::wcsrchr(path, L'\\');
        exe_name = exe_name ? exe_name + 1 : path;
        bool our_game = _wcsicmp(exe_name, CRSF_EXE_W) == 0;
#if defined(CRSF_EXE_ALT)
        our_game = our_game || _wcsicmp(exe_name, CRSF_EXE_ALT_W) == 0;
#endif
        if (!our_game)
            return TRUE; // some other process (another game, a setup tool): stay inert
        g_exe_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        std::snprintf(g_exe_name, sizeof(g_exe_name), "%ls", exe_name);

        GetModuleFileNameW(module, path, MAX_PATH);
        g_dir = path;
        g_dir.resize(g_dir.find_last_of(L'\\') + 1);

        if (has_addon_extension(path))
        {
            if (HMODULE reshade = find_reshade())
            {
                using register_fn = bool(__cdecl *)(HMODULE, uint32_t);
                const auto reg = reinterpret_cast<register_fn>(GetProcAddress(reshade, "ReShadeRegisterAddon"));
                for (uint32_t version = kReShadeApiVersion; version >= 1 && !g_registered_with_reshade; --version)
                    g_registered_with_reshade = reg(module, version);
                if (!g_registered_with_reshade)
                    return FALSE; // ReShade refused the add-on and would unload it anyway
                register_overlay(reshade);
            }
        }

        on_attach();
        g_play_sound = reinterpret_cast<play_sound_fn>(system_function(L"winmm.dll", "PlaySoundW"));
        g_create_factory = reinterpret_cast<create_factory_fn>(system_function(L"dxgi.dll", "CreateDXGIFactory1"));
        if (!CreateTimerQueueTimer(&g_timer, nullptr, tick, nullptr, kFirstTickMs, kTickMs, WT_EXECUTEDEFAULT))
            g_timer = nullptr;
        if (!CreateTimerQueueTimer(&g_key_timer, nullptr, key_tick, nullptr, kFirstTickMs, kKeyTickMs, WT_EXECUTEDEFAULT))
            g_key_timer = nullptr;
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // At process exit the other threads are already gone; there is nothing to wait for or hand back.
        if (reserved != nullptr || process_is_exiting())
            return TRUE;

        HMODULE reshade = g_registered_with_reshade ? find_reshade() : nullptr;
        if (reshade)
            unregister_overlay(reshade); // no more draw_overlay calls after this
        stop_timer(g_key_timer);
        stop_timer(g_timer);
        if (reshade)
        {
            using unregister_fn = void(__cdecl *)(HMODULE);
            if (const auto unreg = reinterpret_cast<unregister_fn>(GetProcAddress(reshade, "ReShadeUnregisterAddon")))
                unreg(module);
        }
        g_registered_with_reshade = false;
        if (g_log != INVALID_HANDLE_VALUE)
        {
            log_line("Unloaded.");
            CloseHandle(g_log);
            g_log = INVALID_HANDLE_VALUE;
        }
    }
    return TRUE;
}
