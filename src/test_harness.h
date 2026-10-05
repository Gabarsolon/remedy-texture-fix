// What the offline tests share: a pass/fail counter, the add-on's files, and a stand-in for ReShade.
// The test executable exports the add-on registration functions and hands out an ImGui function table
// whose widgets are stubs driven by a small script.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#pragma warning(push, 0)
#include <imgui.h>
#include <reshade_overlay.hpp>
#pragma warning(pop)

#include "game.h"

constexpr uint64_t MiB = 1024ull * 1024ull;

namespace
{
int g_register_calls = 0, g_unregister_calls = 0;
uint32_t g_last_api = 0, g_max_api = 18;

std::wstring g_dir, g_addon, g_ini, g_log;
int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

// The add-on's files live next to the test executable. Starts from a clean slate.
void init_files()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_dir = path;
    g_dir.resize(g_dir.find_last_of(L'\\') + 1);
    g_addon = g_dir + CRSF_NAME_W L".addon64";
    g_ini = g_dir + CRSF_NAME_W L".ini";
    g_log = g_dir + CRSF_NAME_W L".log";
    DeleteFileW(g_ini.c_str());
    DeleteFileW(g_log.c_str());
}

void write_ini(const char *text)
{
    HANDLE h = CreateFileW(g_ini.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n;
    WriteFile(h, text, static_cast<DWORD>(std::strlen(text)), &n, nullptr);
    CloseHandle(h);
}

// Replaces the ini and gives the add-on a few ticks to pick it up.
void replace_ini(const char *text)
{
    Sleep(20); // make sure the timestamp moves
    write_ini(text);
    Sleep(700);
}

bool log_contains(const char *text)
{
    HANDLE h = CreateFileW(g_log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    std::string content(GetFileSize(h, nullptr), '\0');
    DWORD n = 0;
    ReadFile(h, content.data(), static_cast<DWORD>(content.size()), &n, nullptr);
    CloseHandle(h);
    return content.find(text) != std::string::npos;
}

bool loaded()
{
    return GetModuleHandleW(CRSF_NAME_W L".addon64") != nullptr;
}

uint32_t g_seed = 12345;
uint32_t rnd(uint32_t n)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (g_seed >> 8) % n;
}

// ---- fake ReShade menu ------------------------------------------------------------------
// Only the widgets the add-on uses are stubbed; calling anything else jumps to null and fails the test.
imgui_function_table g_table = {};
bool g_table_available = true;
void (*g_overlay)(void *) = nullptr;
std::string g_overlay_title;
void (*g_osd)(void *) = nullptr; // the overlay ReShade draws every frame, menu open or not
std::string g_window;            // window opened by the add-on itself in the last frame
ImVec2 g_message_pos;            // where it asked for that window

std::vector<std::string> g_texts;           // everything drawn in the last frame
std::map<std::string, double> g_shown;      // value each slider showed in the last frame
std::map<std::string, std::string> g_previews; // what each closed combo box showed in the last frame
std::string g_last_item;
struct
{
    std::string edit;       // slider to change this frame...
    double value = 0;       // ...to this
    std::string toggle;     // checkbox to click this frame
    std::string click;      // button to click this frame
    std::string pick;       // combo box entry to choose this frame
    std::string release;    // item that reports "released after edit" this frame
} g_script;

bool g_docked = false;
int g_wrap_depth = 0; // PushTextWrapPos / PopTextWrapPos must balance
ImVec2 g_window_pos, g_window_size;
char g_io_storage[sizeof(ImGuiIO)]; // ImGuiIO's constructor lives in imgui.cpp, which is not linked

void build_fake_menu()
{
    reinterpret_cast<ImGuiIO *>(g_io_storage)->DisplaySize = ImVec2(2560.0f, 1440.0f);
    g_table.GetIO = []() -> ImGuiIO & { return *reinterpret_cast<ImGuiIO *>(g_io_storage); };
    g_table.GetFontSize = []() { return 20.0f; };
    g_table.IsWindowDocked = []() { return g_docked; };
    g_table.SetWindowPos = [](const ImVec2 &pos, ImGuiCond) { g_window_pos = pos; };
    g_table.SetWindowSize = [](const ImVec2 &size, ImGuiCond) { g_window_size = size; };
    g_table.PushTextWrapPos = [](float) { ++g_wrap_depth; };
    g_table.PopTextWrapPos = []() { --g_wrap_depth; };
    g_table.TextUnformatted = [](const char *text, const char *) { g_texts.emplace_back(text); };
    g_table.SeparatorText = [](const char *label) { g_texts.emplace_back(label); };
    g_table.ProgressBar = [](float, const ImVec2 &, const char *overlay) { g_texts.emplace_back(overlay ? overlay : ""); };
    g_table.SetItemTooltipV = [](const char *, va_list) {};
    g_table.PushStyleColor2 = [](ImGuiCol, const ImVec4 &) {};
    g_table.PopStyleColor = [](int) {};
    g_table.SliderInt = [](const char *label, int *v, int, int, const char *, ImGuiSliderFlags) {
        g_last_item = label;
        g_shown[label] = *v;
        if (g_script.edit != label)
            return false;
        *v = static_cast<int>(g_script.value);
        g_script.edit.clear();
        return true;
    };
    g_table.SliderFloat = [](const char *label, float *v, float, float, const char *, ImGuiSliderFlags) {
        g_last_item = label;
        g_shown[label] = *v;
        if (g_script.edit != label)
            return false;
        *v = static_cast<float>(g_script.value);
        g_script.edit.clear();
        return true;
    };
    g_table.Checkbox = [](const char *label, bool *v) {
        g_last_item = label;
        if (g_script.toggle != label)
            return false;
        *v = !*v;
        g_script.toggle.clear();
        return true;
    };
    g_table.Button = [](const char *label, const ImVec2 &) {
        g_last_item = label;
        if (g_script.click != label)
            return false;
        g_script.click.clear();
        return true;
    };
    g_table.IsItemDeactivatedAfterEdit = []() {
        if (g_script.release.empty() || g_script.release != g_last_item)
            return false;
        g_script.release.clear();
        return true;
    };
    g_table.BeginCombo = [](const char *label, const char *preview, ImGuiComboFlags) {
        g_last_item = label;
        g_previews[label] = preview ? preview : "";
        return !g_script.pick.empty(); // open only in a frame that picks an entry
    };
    g_table.Selectable = [](const char *label, bool, ImGuiSelectableFlags, const ImVec2 &) {
        if (g_script.pick != label)
            return false;
        g_script.pick.clear();
        return true;
    };
    g_table.EndCombo = []() {};
    g_table.SameLine = [](float, float) {};
    g_table.SetNextWindowPos = [](const ImVec2 &pos, ImGuiCond, const ImVec2 &) { g_message_pos = pos; };
    g_table.SetNextWindowBgAlpha = [](float) {};
    g_table.Begin = [](const char *name, bool *, ImGuiWindowFlags) {
        g_window = name;
        return true;
    };
    g_table.End = []() {};
}

// One frame of the game with ReShade's menu closed: only the every-frame overlay is drawn.
void game_frame()
{
    g_texts.clear();
    g_window.clear();
    if (g_osd)
        g_osd(nullptr);
}

// The first frame takes the scripted input; the following ones hand it to the add-on's tick thread
// (the add-on only try-locks, so one frame can miss).
void frames(int count = 4)
{
    for (int i = 0; i < count && g_overlay; ++i)
    {
        if (i)
            Sleep(15);
        g_texts.clear();
        g_shown.clear();
        g_overlay(nullptr);
    }
}

bool drew(const char *text)
{
    for (const std::string &t : g_texts)
        if (t.find(text) != std::string::npos)
            return true;
    return false;
}

int ini_int(const wchar_t *key)
{
    return static_cast<int>(GetPrivateProfileIntW(CRSF_NAME_W, key, -12345, g_ini.c_str()));
}

std::wstring ini_str(const wchar_t *key)
{
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(CRSF_NAME_W, key, L"?", buf, 64, g_ini.c_str());
    return buf;
}

// ---- fake keyboard ----------------------------------------------------------------------
// The add-on's own imports are pointed at these, so keys are "pressed" without touching real input
// and its beeps are counted instead of played.
bool g_keys[256];
bool g_game_in_front = true;
int g_beeps = 0;
DWORD g_last_beep_hz = 0;

SHORT WINAPI fake_key_state(int vk)
{
    return g_keys[vk & 0xFF] ? static_cast<SHORT>(0x8000) : 0;
}
DWORD WINAPI fake_window_process(HWND, LPDWORD pid)
{
    if (pid)
        *pid = g_game_in_front ? GetCurrentProcessId() : 0;
    return 0;
}
BOOL WINAPI fake_beep(DWORD hz, DWORD)
{
    ++g_beeps;
    g_last_beep_hz = hz;
    return TRUE;
}

bool patch_import(HMODULE module, const char *dll, const char *name, void *replacement)
{
    auto *base = reinterpret_cast<uint8_t *>(module);
    const auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + reinterpret_cast<IMAGE_DOS_HEADER *>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name; ++imp)
    {
        if (_stricmp(reinterpret_cast<const char *>(base + imp->Name), dll) != 0)
            continue;
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + imp->OriginalFirstThunk);
        auto *slots = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            if (std::strcmp(reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData)->Name, name) != 0)
                continue;
            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof(ULONGLONG), PAGE_READWRITE, &old);
            slots->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(ULONGLONG), old, &old);
            return true;
        }
    }
    return false;
}

// Call right after loading the add-on: its key timer first runs a second later.
bool take_over_keys(HMODULE addon)
{
    return patch_import(addon, "USER32.dll", "GetAsyncKeyState", &fake_key_state) &&
           patch_import(addon, "USER32.dll", "GetWindowThreadProcessId", &fake_window_process) &&
           patch_import(addon, "KERNEL32.dll", "Beep", &fake_beep);
}

// Holds the key (with Ctrl, if asked) for `ms`, then gives the add-on time to act on it.
void press(int vk, bool ctrl = false, DWORD ms = 120)
{
    g_keys[VK_CONTROL] = ctrl;
    g_keys[vk] = true;
    Sleep(ms);
    g_keys[vk] = false;
    g_keys[VK_CONTROL] = false;
    Sleep(450);
}

// The on/off key and the "Fix on" box, the same for every game. `is_on` and `is_off` look at the fake
// game: the floor of 2048 MB and blur limit of 2 from the ini, or the game's own values.
template <typename On, typename Off> void test_toggle_key(On is_on, Off is_off)
{
    replace_ini("[" CRSF_NAME "]\r\nMinPoolMB=2048\r\nBiasLimit=2\r\nLogIntervalSec=1\r\nToggleKey=ctrl + f8\r\nToggleSound=1\r\n");
    HMODULE m = LoadLibraryW(g_addon.c_str());
    check(m && take_over_keys(m), "the add-on reads the keyboard through GetAsyncKeyState");
    Sleep(1600);
    check(is_on(), "on when the game starts");
    check(log_contains("ToggleKey=Ctrl+F8"), "key read from the ini, whatever its spelling");
    press(VK_F8);
    check(is_on(), "F8 alone does nothing when the key is Ctrl+F8");
    g_game_in_front = false;
    press(VK_F8, true);
    check(is_on(), "ignored while another window is in front");
    g_game_in_front = true;
    press(VK_F8, true);
    check(is_off(), "Ctrl+F8 switches the fix off: the game's own values are back");
    check(g_beeps == 2 && g_last_beep_hz < 880, "two beeps, falling");
    check(log_contains("Switched off"), "logged");
    game_frame();
    check(g_window == CRSF_NAME " message" && drew(CRSF_NAME ": OFF") && g_message_pos.x == 1280.0f,
          "OFF is shown at the top centre of the screen, with the menu closed");
    frames();
    check(drew("The fix is switched off"), "the tab says it is off");
    Sleep(2200);
    check(is_off(), "it stays off");
    game_frame();
    check(g_window.empty() && g_texts.empty(), "the message goes away after a moment");
    press(VK_F8, true, 800);
    check(is_on(), "a held key switches once: on again");
    check(g_beeps == 4 && g_last_beep_hz > 880, "two beeps, rising");
    game_frame();
    check(drew(CRSF_NAME ": ON"), "ON is shown");
    frames();
    check(!drew("The fix is switched off"), "the tab no longer says off");

    g_script.toggle = "Fix on";
    frames();
    Sleep(450);
    check(is_off() && g_beeps == 4, "the \"Fix on\" box switches it off too, silently");
    g_script.toggle = "Fix on";
    frames();
    Sleep(450);
    check(is_on(), "and on again");

    check(g_previews["On/off key"] == "F8", "the tab shows the key");
    g_script.pick = "F9";
    frames();
    Sleep(450);
    check(ini_str(L"ToggleKey") == L"Ctrl+F9", "picking another key in the tab saves it, modifiers kept");
    g_script.toggle = "Ctrl";
    frames();
    Sleep(450);
    check(ini_str(L"ToggleKey") == L"F9", "unticking Ctrl saves the bare key");
    g_script.toggle = "Beep";
    frames();
    g_script.toggle = "Message";
    frames();
    Sleep(450);
    press(VK_F9);
    check(is_off() && g_beeps == 4 && ini_int(L"ToggleSound") == 0, "the new key works, and without beeps once Beep is unticked");
    game_frame();
    check(g_window.empty() && ini_int(L"ToggleMessage") == 0, "and without a message once Message is unticked");
    g_script.click = "Defaults";
    frames();
    Sleep(450);
    check(ini_str(L"ToggleKey") == L"F9", "Defaults leaves the key alone");

    FreeLibrary(m);
    replace_ini("[" CRSF_NAME "]\r\nMinPoolMB=2048\r\nBiasLimit=2\r\nLogIntervalSec=1\r\nToggleKey=0x01\r\n");
    m = LoadLibraryW(g_addon.c_str());
    take_over_keys(m);
    Sleep(1600);
    check(is_on(), "off is not remembered: on again after a reload");
    press(VK_LBUTTON);
    check(is_on(), "a mouse button is not taken as the key");
    FreeLibrary(m);

    replace_ini("[" CRSF_NAME "]\r\nMinPoolMB=2048\r\nBiasLimit=2\r\nLogIntervalSec=1\r\nToggleKey=F9\r\nToggleMessage=0\r\n");
    m = LoadLibraryW(g_addon.c_str());
    take_over_keys(m);
    Sleep(1600);
    check(g_overlay != nullptr && g_osd == nullptr, "ToggleMessage=0: nothing is registered for drawing every frame");
    g_script.toggle = "Message";
    frames();
    check(drew("from the next start on"), "ticking Message then says it shows from the next start of the game");
    FreeLibrary(m);

    // With winmm in the process the tones go through it, not through Beep. The process is muted for this:
    // the test only wants to know that winmm accepts what the add-on hands it.
    const HMODULE winmm = LoadLibraryW(L"winmm.dll");
    using volume_fn = UINT(WINAPI *)(HANDLE, DWORD);
    const auto set_volume = winmm ? reinterpret_cast<volume_fn>(GetProcAddress(winmm, "waveOutSetVolume")) : nullptr;
    const HANDLE default_device = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(static_cast<UINT>(-1))); // WAVE_MAPPER
    if (set_volume && set_volume(default_device, 0) == 0)
    {
        replace_ini("[" CRSF_NAME "]\r\nMinPoolMB=2048\r\nBiasLimit=2\r\nLogIntervalSec=1\r\nToggleKey=F9\r\nToggleSound=1\r\n");
        m = LoadLibraryW(g_addon.c_str());
        take_over_keys(m);
        Sleep(1600);
        const int beeps_before = g_beeps;
        press(VK_F9);
        check(is_off() && g_beeps == beeps_before, "with winmm loaded the tones are played through it, and it accepts them");
        FreeLibrary(m);
        set_volume(default_device, 0xFFFFFFFF);
    }
    else
    {
        std::printf("  SKIP  sound through winmm (no audio device to mute)\n");
    }
    check(!loaded(), "unloads cleanly with the key timer running");
}

int finish()
{
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    std::fflush(stdout);
    return g_failures ? 1 : 0;
}
} // namespace

extern "C" __declspec(dllexport) bool ReShadeRegisterAddon(HMODULE, uint32_t api_version)
{
    if (api_version > g_max_api)
        return false;
    ++g_register_calls;
    g_last_api = api_version;
    return true;
}
extern "C" __declspec(dllexport) void ReShadeUnregisterAddon(HMODULE)
{
    ++g_unregister_calls;
}
extern "C" __declspec(dllexport) const imgui_function_table *ReShadeGetImGuiFunctionTable(uint32_t version)
{
    return g_table_available && version == IMGUI_VERSION_NUM ? &g_table : nullptr;
}
extern "C" __declspec(dllexport) void ReShadeRegisterOverlay(const char *title, void (*callback)(void *))
{
    if (title && std::strcmp(title, "OSD") == 0)
    {
        g_osd = callback;
        return;
    }
    g_overlay_title = title ? title : "";
    g_overlay = callback;
}
extern "C" __declspec(dllexport) void ReShadeUnregisterOverlay(const char *, void (*callback)(void *))
{
    if (g_overlay == callback)
        g_overlay = nullptr;
    if (g_osd == callback)
        g_osd = nullptr;
}
