// Offline test for the add-on's logic and load/unload lifecycle in Control. No game needed.
//
// The add-on only wakes up inside the game's executable, so this builds to Control_DX12.exe. It exports
// what the add-on looks up in the game's DLLs, under the real decorated names: the tweakable lookup, the
// VRAM query and two statistics. Its tweakables have the game's layout, and game_update() is the game's
// own pool logic as read from the disassembly, so the fix is tested against the behaviour it is there to
// counter. test_harness.h plays ReShade. build.bat test runs it.

#include <atomic>
#include <climits>
#include <cstddef>
#include "test_harness.h"

// ---- the game's side -------------------------------------------------------------------
namespace d
{
class BaseTweakable
{
public:
    static __declspec(dllexport) BaseTweakable *getTweakable(const char *name);

    void *vtable;
    void *text_begin;
    const char *name;
    char padding[0x90];
    int32_t type; // 0 bool, 1 int, 2 float
    volatile uint8_t changed; // "changed by code"
    union
    {
        volatile int32_t i[4]; // value, minimum, maximum, default
        volatile float f[4];
    } value;
};
static_assert(offsetof(BaseTweakable, name) == 0x10 && offsetof(BaseTweakable, type) == 0xA8 &&
                  offsetof(BaseTweakable, changed) == 0xAC && offsetof(BaseTweakable, value) == 0xB0,
              "not the game's layout");
} // namespace d

namespace rend
{
class TextureResource
{
    static __declspec(dllexport) std::atomic<int> sm_iNumMissingMips;
    static __declspec(dllexport) bool sm_bHeapBelowLimit;

public:
    static std::atomic<int> &missing_mips() { return sm_iNumMissingMips; }
    static bool &heap_below_limit() { return sm_bHeapBelowLimit; }
};
std::atomic<int> TextureResource::sm_iNumMissingMips{0};
bool TextureResource::sm_bHeapBelowLimit = false;
} // namespace rend

namespace d3d
{
class DeviceUtil
{
public:
    static __declspec(dllexport) void getVideoMemory(unsigned __int64 &usage, unsigned __int64 &budget);
};
} // namespace d3d

d::BaseTweakable g_pool, g_min, g_shrink, g_bias;
const char *g_hidden = nullptr; // a tweakable this "game version" does not have
bool g_dx12 = true;             // the DX11 renderer has no VRAM numbers and never shrinks its pool
uint64_t g_vram_usage = 0, g_vram_budget = 0; // bytes
uint64_t g_textures = 0;                      // bytes of streamed textures in use
int64_t g_original = 0;                       // the pool size the game remembers at its first update
int g_early_vram_queries = 0;                 // asked before the streamer ever ran (the device may not exist yet)

d::BaseTweakable *d::BaseTweakable::getTweakable(const char *wanted)
{
    for (BaseTweakable *t : {&g_pool, &g_min, &g_shrink, &g_bias})
        if (_stricmp(t->name, wanted) == 0 && !(g_hidden && _stricmp(g_hidden, wanted) == 0))
            return t;
    return nullptr;
}

void d3d::DeviceUtil::getVideoMemory(unsigned __int64 &usage, unsigned __int64 &budget)
{
    if (!g_bias.changed)
        ++g_early_vram_queries;
    usage = g_dx12 ? g_vram_usage : 0;
    budget = g_dx12 ? g_vram_budget : 0;
}

namespace
{
// A fresh game process: tweakables at their compiled-in defaults, nothing has run yet.
void reset_game()
{
    const auto set_up = [](d::BaseTweakable &t, const char *name, int32_t type) {
        std::memset(&t, 0, sizeof(t));
        t.name = name;
        t.type = type;
    };
    set_up(g_pool, "Texture Streaming:Target texture pool size MB", 1);
    set_up(g_min, "Texture Streaming:Min Pool Size MB", 1);
    set_up(g_shrink, "Texture Streaming:Reduce Pool Size After Free VRAM < MB", 1);
    set_up(g_bias, "Texture Streaming:Mip adjust [Display]", 2);
    const int32_t pool[4] = {1024, 0, 4095, 1024}, min[4] = {100, 32, 1024, 100}, shrink[4] = {64, 32, 1024, 64};
    const float bias[4] = {0.0f, 0.0f, 10.0f, 0.0f};
    for (int k = 0; k < 4; ++k)
    {
        g_pool.value.i[k] = pool[k];
        g_min.value.i[k] = min[k];
        g_shrink.value.i[k] = shrink[k];
        g_bias.value.f[k] = bias[k];
    }
    g_original = 0;
    g_vram_usage = g_vram_budget = g_textures = 0;
    rend::TextureResource::missing_mips() = 0;
    rend::TextureResource::heap_below_limit() = false;
}

// The settings code: Texture Resolution picks 512 / 1024 / 1664 / 2048 / 4096.
void game_settings(int32_t pool_mb)
{
    g_pool.value.i[0] = pool_mb;
    g_pool.changed = 1;
}

// What the game's shrink makes of the pool: (1 - usage/budget) * original + usage/budget * minimum.
int32_t shrunk(int64_t original, int32_t minimum)
{
    const float usage = static_cast<float>(g_vram_usage) * 9.5367431640625e-07f;
    const float budget = static_cast<float>(g_vram_budget) * 9.5367431640625e-07f;
    const float ratio = usage / budget;
    return static_cast<int32_t>((1.0f - ratio) * static_cast<float>(original) + static_cast<float>(minimum) * ratio);
}

// One TextureResourceStreamManager::update(): adjustMipBias(), then (DX12 only) the low-VRAM shrink. The real
// one waits two seconds between shrinks; this one may shrink at every update, which is harder on the add-on.
void game_update()
{
    const uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(g_pool.value.i[0]) << 20);
    const uint64_t free_pool = target < g_textures ? 0 : target - g_textures; // unsigned, as in the game
    float bias = g_bias.value.f[0];
    if (free_pool < (50ull << 20))
        bias += 0.1f;
    if (free_pool > (100ull << 20))
        bias += -0.1f;
    bias = max(bias, 0.0f);
    bias = min(bias, g_bias.value.f[2]);
    g_bias.value.f[0] = bias;
    g_bias.changed = 1;
    rend::TextureResource::heap_below_limit() = free_pool > (50ull << 20);

    if (!g_dx12)
        return;
    if (g_original == 0)
        g_original = g_pool.value.i[0];
    const float usage = static_cast<float>(g_vram_usage) * 9.5367431640625e-07f;
    const float budget = static_cast<float>(g_vram_budget) * 9.5367431640625e-07f;
    if (static_cast<int32_t>(budget - usage) < g_shrink.value.i[0])
    {
        g_pool.value.i[0] = shrunk(g_original, g_min.value.i[0]);
        g_pool.changed = 1;
    }
}

void game_updates(int count, DWORD pause_ms = 0)
{
    for (int k = 0; k < count; ++k)
    {
        game_update();
        if (pause_ms)
            Sleep(pause_ms);
    }
}

void set_ini(unsigned min_mb, const char *bias)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "[" CRSF_NAME "]\r\nMinPoolMB=%u\r\nBiasLimit=%s\r\nLogIntervalSec=1\r\n", min_mb, bias);
    replace_ini(buf);
}

int32_t pool()
{
    return g_pool.value.i[0];
}
} // namespace

int wmain()
{
    init_files();
    build_fake_menu();
    std::printf("Testing " CRSF_NAME " " CRSF_VERSION " inside a fake " CRSF_EXE "\n");

    std::printf("1. The game alone (DX12): the bug\n");
    reset_game();
    game_settings(2048);
    g_vram_budget = 7000 * MiB;
    g_vram_usage = 5000 * MiB;
    g_textures = 1900 * MiB;
    game_updates(30);
    check(pool() == 2048 && g_bias.value.f[0] == 0.0f, "with VRAM to spare the pool stays at the Texture Resolution size");
    g_vram_usage = 6960 * MiB;
    game_update();
    std::printf("        pool after one update with 40 MB of VRAM free: %d MB\n", pool());
    check(pool() > 100 && pool() < 130, "under 64 MB of free VRAM the game shrinks the pool to about 100 MB");
    game_updates(150);
    check(g_bias.value.f[0] == 10.0f, "and blurs textures up to the 10 mip limit");
    g_vram_usage = 5000 * MiB;
    game_updates(30);
    check(pool() < 130, "freeing VRAM does not bring the pool back");
    g_vram_usage = 7700 * MiB;
    game_update();
    check(pool() < 0, "far over its budget the game even computes a negative pool");

    std::printf("2. ReShade-style load/unload storm (unloaded before the first tick)\n");
    reset_game();
    bool storm_ok = true;
    for (int i = 0; i < 30; ++i)
    {
        HMODULE m = LoadLibraryW(g_addon.c_str());
        storm_ok = storm_ok && m != nullptr;
        Sleep(rnd(300));
        if (m)
            FreeLibrary(m);
        storm_ok = storm_ok && !loaded();
    }
    check(storm_ok, "30 load/unload cycles, module gone after each");
    check(g_register_calls == 30 && g_unregister_calls == 30, "registered and unregistered 30 times");
    check(g_last_api == 18, "registered with API version 18");
    check(GetFileAttributesW(g_log.c_str()) == INVALID_FILE_ATTRIBUTES, "no log written by instances that never ticked");
    check(g_min.value.i[0] == 100 && pool() == 1024, "and nothing written to the game");

    std::printf("3. Unload around the first tick\n");
    bool tick_ok = true;
    ULONGLONG worst = 0;
    for (int i = 0; i < 12; ++i)
    {
        HMODULE m = LoadLibraryW(g_addon.c_str());
        Sleep(985 + rnd(60));
        const ULONGLONG t0 = GetTickCount64();
        FreeLibrary(m);
        worst = max(worst, GetTickCount64() - t0);
        tick_ok = tick_ok && !loaded();
    }
    check(tick_ok, "12 unloads around the first tick, module gone after each");
    std::printf("        slowest FreeLibrary: %llu ms\n", worst);
    check(worst < 3000, "unload never stalls");
    check(!log_contains("no settings tab"), "a tick caught by the unload does not log that the tab is missing");

    std::printf("4. Normal run\n");
    reset_game();
    HMODULE m = LoadLibraryW(g_addon.c_str());
    Sleep(1500); // first tick happened; the game has not applied its settings or streamed anything yet
    check(GetFileAttributesW(g_ini.c_str()) != INVALID_FILE_ATTRIBUTES && ini_int(L"MinPoolMB") == 2048 &&
              ini_int(L"MaxPoolMB") == -12345,
          "default ini created, without a MaxPoolMB");
    check(log_contains("Found the game's texture pool tweakables through " CRSF_EXE), "tweakables found through the game's lookup");
    check(log_contains("Game values: pool 1024 MB, shrinking towards 100 MB once free VRAM is under 64 MB; blur limit 10.00 mips"),
          "the game's own values are read and logged");
    check(g_min.value.i[0] == 2048 && g_min.changed == 1, "minimum raised to 2048 MB and marked changed, as the game's setters do");
    check(pool() == 2048, "pool lifted from 1024 MB to the floor");
    check(g_shrink.value.i[0] == INT_MIN, "at the floor the game's shrink is switched off");
    check(g_bias.value.f[2] == 10.0f, "blur limit untouched");
    check(log_contains("waiting for the renderer..."), "stats wait until the game's streamer has run");
    game_settings(4096); // the game applies Texture Resolution
    Sleep(600);
    check(pool() == 4096 && g_shrink.value.i[0] == 64, "a pool above the floor is the game's: its shrink is back on");
    g_vram_budget = 7000 * MiB;
    g_vram_usage = 6000 * MiB;
    g_textures = 1900 * MiB;
    game_updates(10, 20);
    set_ini(2048, "-1"); // one stats line per second from here on
    Sleep(1200);
    check(log_contains("pool 4096 MB [min 2048] has room | bias 0.00 mips | 0 mips missing | VRAM 6000 of 7000 MB"),
          "stats show the pool, blur, missing mips and the game's VRAM numbers");
    check(g_early_vram_queries == 0, "the game's VRAM query is never called before its streamer runs");

    std::printf("5. The game's low-VRAM shrink against the floor\n");
    g_vram_usage = 6960 * MiB; // 40 MB free
    char fell[96];
    std::snprintf(fell, sizeof(fell), "Pool fell from 4096 to %d MB", shrunk(4096, 2048));
    game_update();
    Sleep(600);
    check(pool() == 2048, "the shrink now ends just above the floor, and the pool is set on it");
    check(g_shrink.value.i[0] == INT_MIN, "where the shrink is switched off again");
    check(log_contains(fell) && log_contains("Holding it at 2048 MB."), "the log says what happened");
    g_vram_usage = 7700 * MiB; // 700 MB over budget: the game's formula would go under the floor
    int32_t lowest = INT_MAX, highest = INT_MIN;
    for (int k = 0; k < 40; ++k)
    {
        game_update();
        lowest = min(lowest, pool());
        highest = max(highest, pool());
        Sleep(25);
    }
    check(lowest == 2048 && highest == 2048, "over budget the pool does not move at all");
    check(g_bias.value.f[0] == 0.0f, "and 1900 MB of textures stay at full resolution");
    g_textures = 2040 * MiB; // more than fits: the streamer blurs, 0.1 mips per update
    rend::TextureResource::missing_mips() = 12;
    game_updates(12);
    Sleep(1300);
    check(log_contains("pool 2048 MB [min 2048] full | bias 1.20 mips | 12 mips missing | VRAM 7700 of 7000 MB (700 MB over budget)"),
          "stats show a full pool, the blur and how far the game is over its budget");
    game_settings(512); // the player picks the lowest Texture Resolution
    Sleep(600);
    check(pool() == 2048, "a Texture Resolution under the floor is lifted to the floor");

    std::printf("6. Live ini changes\n");
    game_settings(2048);
    g_vram_usage = 6000 * MiB;
    set_ini(1536, "2");
    check(g_min.value.i[0] == 1536, "MinPoolMB=1536 applied");
    check(g_bias.value.f[2] == 2.0f, "BiasLimit=2 applied");
    check(pool() == 2048 && g_shrink.value.i[0] == 64, "the pool above the new floor is left to the game");
    g_vram_usage = 6960 * MiB;
    game_update();
    Sleep(600);
    check(pool() == 1536, "and the game's next shrink stops at the new floor");
    set_ini(0, "-1");
    check(g_min.value.i[0] == 100 && g_shrink.value.i[0] == 64, "MinPoolMB=0 restores the game's 100 MB and its shrink");
    check(g_bias.value.f[2] == 10.0f, "BiasLimit=-1 restores the game's 10");
    game_update();
    Sleep(600);
    check(pool() < 200, "with the floor off the game collapses its pool as before");
    set_ini(4000, "-1");
    check(g_min.value.i[0] == 4000 && pool() == 4000, "a floor above the pool lifts the pool");
    g_vram_usage = 6000 * MiB;
    set_ini(2048, "-1");
    check(g_min.value.i[0] == 2048 && pool() == 4000 && g_shrink.value.i[0] == 64,
          "lowering the floor does not lower the pool by itself");

    std::printf("7. Unload while running, reload in the same process\n");
    const int unreg_before = g_unregister_calls;
    const ULONGLONG t0 = GetTickCount64();
    FreeLibrary(m);
    const ULONGLONG unload_ms = GetTickCount64() - t0;
    check(!loaded(), "module gone");
    check(g_unregister_calls == unreg_before + 1, "unregistered from ReShade");
    check(unload_ms < 3000, "unload did not stall");
    m = LoadLibraryW(g_addon.c_str());
    Sleep(1600);
    set_ini(0, "-1");
    check(g_min.value.i[0] == 100 && g_shrink.value.i[0] == 64,
          "after reload, MinPoolMB=0 still restores the game's values (not our 2048)");
    set_ini(2048, "-1");
    FreeLibrary(m);

    std::printf("8. Older and unwilling ReShade\n");
    g_max_api = 12;
    m = LoadLibraryW(g_addon.c_str());
    check(m != nullptr && g_last_api == 12, "falls back to the API version ReShade accepts (12)");
    if (m)
        FreeLibrary(m);
    g_max_api = 0;
    m = LoadLibraryW(g_addon.c_str());
    check(m == nullptr && !loaded(), "refused by ReShade: add-on does not stay loaded");
    g_max_api = 18;

    std::printf("9. Loaded as .asi (no ReShade involved)\n");
    const std::wstring asi = g_dir + CRSF_NAME_W L".asi";
    CopyFileW(g_addon.c_str(), asi.c_str(), FALSE);
    const int reg_before = g_register_calls;
    m = LoadLibraryW(asi.c_str());
    Sleep(1600);
    set_ini(1800, "-1");
    check(m != nullptr && g_min.value.i[0] == 1800, "works without registering: MinPoolMB=1800 applied");
    check(g_register_calls == reg_before, "did not register with ReShade");
    if (m)
        FreeLibrary(m);
    check(GetModuleHandleW(CRSF_NAME_W L".asi") == nullptr, "module gone after unload");
    set_ini(2048, "-1");

    std::printf("10. Settings tab in the ReShade menu\n");
    reset_game();
    game_settings(2048);
    m = LoadLibraryW(g_addon.c_str());
    check(g_overlay != nullptr && g_overlay_title == CRSF_NAME, "tab registered with ReShade");
    frames(1);
    check(drew("Starting..."), "says it is starting before the first tick");
    Sleep(1600);
    frames();
    check(drew("Waiting for the renderer..."), "waits for the game's streamer");
    g_vram_budget = 7000 * MiB;
    g_vram_usage = 7150 * MiB;
    g_textures = 2040 * MiB;
    rend::TextureResource::missing_mips() = 12;
    game_updates(12);
    frames();
    check(drew("Pool: 2048 MB, full") && drew("Blur: 1.20 mips") && drew("12 mips still to load"),
          "shows the pool, the blur and the mips still to load");
    check(drew("VRAM: 7150 of 7000 MB in use") && drew("The game is 150 MB over its budget"),
          "shows the game's VRAM numbers and warns when it is over its budget");
    check(g_shown["Minimum pool"] == 2048, "slider shows the current minimum");
    check(g_shown.count("Maximum pool") == 0, "no Maximum pool slider: Texture Resolution is the ceiling");
    check(g_window_size.x == 680.0f && g_window_pos.x == 2560.0f - 680.0f - 40.0f && drew("Tip: drag"),
          "a floating window is placed on the right and explains docking");
    g_docked = true;
    g_window_pos = ImVec2();
    frames();
    check(g_window_pos.x == 0.0f && !drew("Tip: drag"), "a docked tab is left alone");
    g_docked = false;
    g_vram_usage = 6000 * MiB;
    frames();
    check(drew("VRAM: 6000 of 7000 MB in use") && !drew("over its budget"), "no warning inside the budget");
    check(g_wrap_depth == 0, "text wrap pushes and pops balance");
    g_script.edit = "Minimum pool";
    g_script.value = 1536;
    frames();
    Sleep(500);
    check(g_min.value.i[0] == 1536, "dragging the slider applies within a tick");
    check(ini_int(L"MinPoolMB") == 2048, "ini not written while dragging");
    g_script.release = "Minimum pool";
    frames();
    Sleep(500);
    check(ini_int(L"MinPoolMB") == 1536, "ini written when the slider is released");
    g_script.edit = "Minimum pool";
    g_script.value = 3008;
    g_script.release = "Minimum pool";
    frames();
    Sleep(500);
    check(pool() == 3008 && g_min.value.i[0] == 3008, "raising the slider lifts the pool with it");
    g_script.toggle = "Limit blur";
    frames();
    Sleep(500);
    check(g_bias.value.f[2] == 2.0f && ini_str(L"BiasLimit") == L"2.00", "Limit blur switches the cap on at 2 mips and saves it");
    g_script.edit = "Blur limit";
    g_script.value = 3.5;
    g_script.release = "Blur limit";
    frames();
    Sleep(500);
    check(g_bias.value.f[2] == 3.5f && ini_str(L"BiasLimit") == L"3.50", "blur limit slider applies and saves");
    g_script.click = "Defaults";
    frames();
    Sleep(500);
    check(g_min.value.i[0] == 2048 && g_bias.value.f[2] == 10.0f, "Defaults restores 2048 MB and the game's blur limit");
    check(ini_int(L"MinPoolMB") == 2048 && ini_str(L"BiasLimit") == L"-1", "Defaults is saved");
    check(ini_int(L"MaxPoolMB") == -12345, "and still no MaxPoolMB in the ini");
    set_ini(1792, "-1");
    frames();
    check(g_shown["Minimum pool"] == 1792, "an edit of the ini file shows up in the tab");
    check(log_contains("Config saved from the ReShade menu: MinPoolMB=1536 BiasLimit=-1.00"), "saves are logged");
    FreeLibrary(m);
    check(g_overlay == nullptr && !loaded(), "tab unregistered on unload");
    set_ini(2048, "-1");

    std::printf("11. ReShade without the ImGui 1.92.5 table\n");
    g_table_available = false;
    m = LoadLibraryW(g_addon.c_str());
    Sleep(1600);
    set_ini(1600, "-1");
    check(m != nullptr && g_overlay == nullptr, "loads without a settings tab");
    check(g_min.value.i[0] == 1600, "the fix still works through the ini");
    check(log_contains("no settings tab"), "log says why there is no tab");
    if (m)
        FreeLibrary(m);
    g_table_available = true;
    set_ini(2048, "-1");

    std::printf("12. The DX11 build: no shrink and no VRAM numbers\n");
    reset_game();
    g_dx12 = false;
    game_settings(1024);
    m = LoadLibraryW(g_addon.c_str());
    Sleep(1600);
    g_textures = 1900 * MiB;
    game_updates(5);
    frames();
    check(pool() == 2048, "the pool is still lifted to the floor");
    check(drew("Pool: 2048 MB") && drew("Blur: 0.00 mips") && !drew("VRAM:"), "tab shows pool and blur, without VRAM lines");
    Sleep(1200);
    check(log_contains("| VRAM n/a"), "stats lines say n/a");
    if (m)
        FreeLibrary(m);
    g_dx12 = true;

    std::printf("13. Unsupported game versions\n");
    reset_game();
    g_hidden = "Texture Streaming:Min Pool Size MB";
    m = LoadLibraryW(g_addon.c_str());
    Sleep(1600);
    frames(1);
    check(drew("This game version is not supported"), "a missing tweakable: tab says the game version is not supported");
    check(g_min.value.i[0] == 100 && pool() == 1024 && g_shrink.value.i[0] == 64, "nothing is written");
    check(log_contains("the game has no usable \"Texture Streaming:Min Pool Size MB\" (not found)"), "log says which one");
    if (m)
        FreeLibrary(m);
    check(!loaded(), "unloads cleanly");
    g_hidden = nullptr;
    g_bias.type = 1; // a tweakable of another kind under the same name
    m = LoadLibraryW(g_addon.c_str());
    Sleep(1600);
    check(g_min.value.i[0] == 100 && pool() == 1024, "a tweakable of the wrong type: nothing is written either");
    check(log_contains("the game has no usable \"Texture Streaming:Mip adjust [Display]\" (wrong type)"), "log says which one");
    if (m)
        FreeLibrary(m);
    g_bias.type = 2;

    std::printf("14. On/off key\n");
    reset_game();
    test_toggle_key([] { return g_min.value.i[0] == 2048 && g_bias.value.f[2] == 2.0f; },
                    [] { return g_min.value.i[0] == 100 && g_shrink.value.i[0] == 64 && g_bias.value.f[2] == 10.0f; });

    std::printf("15. Exit with the add-on loaded\n");
    m = LoadLibraryW(g_addon.c_str());
    Sleep(1300);
    check(m != nullptr, "loaded; the process now exits without unloading it");

    return finish();
}
