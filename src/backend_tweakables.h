// Control (2019). Included by CRStreamingFix.cpp, inside its anonymous namespace.
//
// Why textures go blurry (from disassembly of renderer_rmdwin10_f.dll, game 0.0.518.2177):
//   * The pool is a tweakable, "Texture Streaming:Target texture pool size MB". Texture Resolution
//     sets it to 512 / 1024 / 1664 / 2048 / 4096.
//   * TextureResource::adjustMipBias() raises a global mip bias ("Mip adjust") by 0.1 per update while
//     the textures in use leave less than 50 MB of the pool free, up to 10 mips, and lowers it again
//     once more than 100 MB is free.
//   * In the DX12 renderer, TextureResourceStreamManager::update() shrinks the pool when the game's
//     free VRAM (DXGI budget - usage) drops under "Reduce Pool Size After Free VRAM < MB" (64):
//         pool = (1 - usage/budget) * original + usage/budget * "Min Pool Size MB"      (100)
//     That is about 100 MB, and nothing raises it again until Texture Resolution is changed. Far
//     enough over the budget, the result drops under the minimum and can go negative.
//   * The DX11 renderer has no such code: its pool stays at the Texture Resolution size.
//
// The engine looks tweakables up by name (d::BaseTweakable::getTweakable, exported by the game's rl
// DLL), so nothing is found by signature: the add-on asks the game for the ones it needs and writes
// their values. The minimum becomes the floor, and a pool at the floor is held there.

constexpr bool kHasMaxPool = false; // the ceiling is the game's Texture Resolution setting

#define CRSF_INI_MIN_POOL                                                                                  \
    "; Smallest texture pool in MB. In DX12 the game shrinks its pool to about 100 MB once free VRAM\r\n" \
    "; runs out and never grows it back, which is what makes textures blurry.\r\n"                        \
    "; Higher = sharper, but past your free VRAM Windows starts paging to system RAM (stutter).\r\n"      \
    "; Above the pool your Texture Resolution setting asks for (512 to 4096), it raises the pool.\r\n"    \
    "; 0 = leave the game's value.\r\n"
#define CRSF_INI_MAX_POOL_BLOCK ""
constexpr bool kHasAuto = false; // automatic mode is not there for this game
#define CRSF_INI_AUTO_BLOCK ""

// d::BaseTweakable, as the game's own getters and setters use it.
constexpr uintptr_t kTweakType = 0xA8;    // 0 bool, 1 int, 2 float
constexpr uintptr_t kTweakChanged = 0xAC; // "changed by code", set by every setter in the game
constexpr uintptr_t kTweakValue = 0xB0;   // numbers: value, then minimum, maximum, default
constexpr uintptr_t kTweakMax = 0xB8;

constexpr int32_t kSnapMB = 64;               // the game's shrink ends about this close above the floor
constexpr int32_t kNeverShrink = -2147483647 - 1; // free VRAM is never under this

struct Targets
{
    uintptr_t pool = 0;     // "Texture Streaming:Target texture pool size MB" (int)
    uintptr_t min_pool = 0; // "Texture Streaming:Min Pool Size MB" (int)
    uintptr_t shrink = 0;   // "Texture Streaming:Reduce Pool Size After Free VRAM < MB" (int)
    uintptr_t bias = 0;     // "Texture Streaming:Mip adjust [Display]" (float); its maximum is the blur limit
};

// The game's own values, read before the first write. Kept in a process environment variable
// too, so they survive ReShade unloading and reloading the add-on within one run.
struct Baseline
{
    bool valid = false;
    int32_t game_min = 0;    // MB
    int32_t game_shrink = 0; // MB of free VRAM under which the game shrinks its pool
    float game_bias_limit = 10.0f;

    uint64_t game_min_mb() const { return static_cast<uint64_t>(game_min); }
    uint64_t game_max_mb() const { return 0; }
};

bool read_i32(uintptr_t addr, int32_t &out)
{
    return read_mem(addr, out);
}

bool read_u8(uintptr_t addr, uint8_t &out)
{
    return read_mem(addr, out);
}

bool write_i32(uintptr_t addr, int32_t value)
{
    return write_mem(addr, value);
}

bool write_u8(uintptr_t addr, uint8_t value)
{
    return write_mem(addr, value);
}

using get_tweakable_fn = void *(__cdecl *)(const char *);
using get_video_memory_fn = void(__cdecl *)(uint64_t &, uint64_t &);

get_tweakable_fn g_get_tweakable = nullptr;       // d::BaseTweakable::getTweakable
get_video_memory_fn g_get_video_memory = nullptr; // d3d::DeviceUtil::getVideoMemory (usage, budget); zeros in DX11
uintptr_t g_missing_mips = 0;                     // rend::TextureResource::sm_iNumMissingMips
uintptr_t g_heap_below_limit = 0;                 // rend::TextureResource::sm_bHeapBelowLimit
char g_tweakable_module[64] = "";                 // file name of the module the lookup lives in
bool g_dx11 = false;                              // that module is the DX11 build's (..._rmdwin7_f.dll)
int32_t g_last_pool = 0;                          // pool after the previous apply

// Runs in DllMain, where asking the loader is safe. The game's DLLs are imports of its executable,
// so they are all loaded by now.
void on_attach()
{
    if (HMODULE m = find_module_exporting("?getTweakable@BaseTweakable@d@@SAPEAV12@PEBD@Z"))
    {
        g_get_tweakable =
            reinterpret_cast<get_tweakable_fn>(GetProcAddress(m, "?getTweakable@BaseTweakable@d@@SAPEAV12@PEBD@Z"));
        char path[MAX_PATH] = {};
        GetModuleFileNameA(m, path, MAX_PATH);
        const char *name = std::strrchr(path, '\\');
        std::snprintf(g_tweakable_module, sizeof(g_tweakable_module), "%s", name ? name + 1 : path);
        char lower[sizeof(g_tweakable_module)];
        std::snprintf(lower, sizeof(lower), "%s", g_tweakable_module);
        _strlwr_s(lower);
        g_dx11 = std::strstr(lower, "rmdwin7") != nullptr;
    }
    if (HMODULE m = find_module_exporting("?getVideoMemory@DeviceUtil@d3d@@SAXAEA_K0@Z"))
        g_get_video_memory =
            reinterpret_cast<get_video_memory_fn>(GetProcAddress(m, "?getVideoMemory@DeviceUtil@d3d@@SAXAEA_K0@Z"));
    if (HMODULE m = find_module_exporting("?sm_iNumMissingMips@TextureResource@rend@@0U?$atomic@H@std@@A"))
        g_missing_mips = reinterpret_cast<uintptr_t>(
            GetProcAddress(m, "?sm_iNumMissingMips@TextureResource@rend@@0U?$atomic@H@std@@A"));
    if (HMODULE m = find_module_exporting("?sm_bHeapBelowLimit@TextureResource@rend@@0_NA"))
        g_heap_below_limit =
            reinterpret_cast<uintptr_t>(GetProcAddress(m, "?sm_bHeapBelowLimit@TextureResource@rend@@0_NA"));
}

// Calls into the game are guarded like the memory reads: its objects can be gone during shutdown.
uintptr_t call_get_tweakable(const char *name)
{
    __try
    {
        return reinterpret_cast<uintptr_t>(g_get_tweakable(name));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

bool call_get_video_memory(uint64_t &usage, uint64_t &budget)
{
    __try
    {
        g_get_video_memory(usage, budget);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool locate(uintptr_t, Targets &t)
{
    if (!g_get_tweakable)
    {
        log_line("ERROR: no loaded module has the game's tweakable lookup. Unsupported game version, doing nothing.");
        return false;
    }

    const struct
    {
        const char *name;
        uintptr_t *object;
        int32_t type;
    } wanted[] = {
        {"Texture Streaming:Target texture pool size MB", &t.pool, 1},
        {"Texture Streaming:Min Pool Size MB", &t.min_pool, 1},
        {"Texture Streaming:Reduce Pool Size After Free VRAM < MB", &t.shrink, 1},
        {"Texture Streaming:Mip adjust [Display]", &t.bias, 2},
    };
    for (const auto &w : wanted)
    {
        int32_t type = -1;
        *w.object = call_get_tweakable(w.name);
        if (!*w.object || !read_i32(*w.object + kTweakType, type) || type != w.type)
        {
            log_line("ERROR: the game has no usable \"%s\" (%s). Unsupported game version, doing nothing.", w.name,
                     *w.object ? "wrong type" : "not found");
            return false;
        }
    }

    // The same objects must also look like the ones this was written against. The ranges are wide: after a
    // reload of the add-on these hold what it wrote earlier, and the game's own shrink can go negative.
    int32_t pool = 0, min_pool = 0;
    float bias_limit = 0.0f;
    if (!read_i32(t.pool + kTweakValue, pool) || !read_i32(t.min_pool + kTweakValue, min_pool) ||
        !read_f32(t.bias + kTweakMax, bias_limit) || pool < -65536 || pool > 65536 || min_pool < 1 || min_pool > 65536 ||
        !(bias_limit >= 0.0f && bias_limit <= 100.0f))
    {
        log_line("ERROR: the game's texture pool tweakables hold unexpected values (pool %d, minimum %d, blur limit "
                 "%.2f). Unsupported game version, doing nothing.",
                 pool, min_pool, bias_limit);
        return false;
    }

    log_line("Found the game's texture pool tweakables through %s%s", g_tweakable_module,
             g_get_video_memory ? "" : " (no VRAM numbers: the game's VRAM query was not found)");
    return true;
}

// ---------------------------------------------------------------------------------------
// baseline (the game's own values)

void save_baseline(const Baseline &b)
{
    wchar_t buf[128];
    std::swprintf(buf, 128, L"%d %d %.9g", b.game_min, b.game_shrink, static_cast<double>(b.game_bias_limit));
    SetEnvironmentVariableW(CRSF_NAME_W L"_BASELINE", buf);
}

bool load_baseline(Baseline &b)
{
    wchar_t buf[128] = {};
    if (GetEnvironmentVariableW(CRSF_NAME_W L"_BASELINE", buf, 128) == 0)
        return false;
    int mn = 0, shrink = 0;
    double bias = 0;
    if (swscanf_s(buf, L"%d %d %lf", &mn, &shrink, &bias) != 3)
        return false;
    b.game_min = mn;
    b.game_shrink = shrink;
    b.game_bias_limit = static_cast<float>(bias);
    b.valid = true;
    return true;
}

// ---------------------------------------------------------------------------------------
// the fix

// What the game's own setters do: store the value and mark the tweakable as changed by code.
bool set_tweakable(uintptr_t object, int32_t value)
{
    return write_i32(object + kTweakValue, value) && write_u8(object + kTweakChanged, 1);
}

// Called with g_lock held.
void apply(const Targets &t, const Config &c, Baseline &b, bool &announce)
{
    int32_t pool = 0, min_pool = 0, shrink = 0;
    float bias_limit = 0.0f;
    if (!read_i32(t.pool + kTweakValue, pool) || !read_i32(t.min_pool + kTweakValue, min_pool) ||
        !read_i32(t.shrink + kTweakValue, shrink) || !read_f32(t.bias + kTweakMax, bias_limit))
        return;

    if (!b.valid && !load_baseline(b))
    {
        // Tweakables are set up when the game's DLLs load, long before any add-on, so these are the game's.
        b.game_min = min_pool;
        b.game_shrink = shrink;
        b.game_bias_limit = bias_limit;
        b.valid = true;
        save_baseline(b);
        if (g_dx11)
            log_line("Game values: pool %d MB, which the DX11 renderer never shrinks; blur limit %.2f mips", pool,
                     b.game_bias_limit);
        else
            log_line("Game values: pool %d MB, shrinking towards %d MB once free VRAM is under %d MB; blur limit %.2f mips",
                     pool, b.game_min, b.game_shrink, b.game_bias_limit);
    }

    const bool floor_on = c.min_pool_mb != 0;
    const int32_t floor = floor_on ? static_cast<int32_t>(c.min_pool_mb) : b.game_min;
    int32_t want_pool = pool;
    int32_t want_shrink = b.game_shrink;
    if (floor_on && pool <= floor + kSnapMB)
    {
        // At the floor there is nothing left for the game to shrink. Its formula would only wobble around
        // the floor, or drop under it while the game is over its VRAM budget, so the shrink is switched off.
        want_pool = floor;
        want_shrink = kNeverShrink;
        if (g_last_pool > floor + kSnapMB)
            log_line("Pool fell from %d to %d MB (the game's low-VRAM shrink, or a lower Texture Resolution). "
                     "Holding it at %d MB.",
                     g_last_pool, pool, floor);
    }
    g_last_pool = want_pool;

    if (min_pool != floor && set_tweakable(t.min_pool, floor))
        announce = true;
    if (shrink != want_shrink)
        set_tweakable(t.shrink, want_shrink);
    if (pool != want_pool && set_tweakable(t.pool, want_pool))
        announce = true;

    const float want_bias = c.bias_limit >= 0.0f ? c.bias_limit : b.game_bias_limit;
    if (bias_limit != want_bias && write_f32(t.bias + kTweakMax, want_bias))
        announce = true;

    static int32_t logged_floor = -1, logged_pool = -1;
    static float logged_bias = -1.0f;
    if (announce)
    {
        announce = false;
        if (floor != logged_floor || want_pool != logged_pool || want_bias != logged_bias)
        {
            logged_floor = floor;
            logged_pool = want_pool;
            logged_bias = want_bias;
            log_line("Applied: pool floor %d MB (game %d), pool %d MB, blur limit %.2f mips (game %.2f)", floor,
                     b.game_min, want_pool, want_bias, b.game_bias_limit);
        }
    }
}

// What the streamer is doing right now, read straight from the game.
struct Live
{
    bool renderer = false;  // the streamer has run: it marks the blur tweakable as changed at every update
    int32_t pool = 0, min_pool = 0;
    float bias = 0.0f;
    bool has_missing = false;
    int32_t missing_mips = 0; // mips the streamer wants and has not loaded
    bool has_room = false;
    bool room = false; // more than the game's 50 MB margin of the pool is free
    bool vram_valid = false;
    uint64_t vram_usage = 0, vram_budget = 0; // as the game's own shrink sees them (DX12 only)
};

Live read_live(const Targets &t)
{
    Live v;
    uint8_t ran = 0;
    if (!read_u8(t.bias + kTweakChanged, ran) || !ran || !read_i32(t.pool + kTweakValue, v.pool) ||
        !read_i32(t.min_pool + kTweakValue, v.min_pool) || !read_f32(t.bias + kTweakValue, v.bias))
        return v;
    v.renderer = true;

    uint8_t room = 0;
    if (g_missing_mips)
        v.has_missing = read_i32(g_missing_mips, v.missing_mips);
    if (g_heap_below_limit && read_u8(g_heap_below_limit, room))
    {
        v.has_room = true;
        v.room = room != 0;
    }
    // Asked only once the streamer runs: it makes the same call at every update, so the device is there.
    if (g_get_video_memory)
        v.vram_valid = call_get_video_memory(v.vram_usage, v.vram_budget) && v.vram_budget != 0;
    return v;
}

AutoSample auto_sample(const Live &)
{
    return {};
}

uint64_t auto_ceiling_mb(const Config &, const Baseline &)
{
    return 0;
}

void log_stats(const Targets &t, const char * /*extra*/)
{
    const Live v = read_live(t);
    if (!v.renderer)
    {
        log_line("waiting for the renderer...");
        return;
    }

    char missing[48] = "";
    if (v.has_missing)
        std::snprintf(missing, sizeof(missing), " | %d mips missing", v.missing_mips);

    char vram[96] = "n/a";
    if (v.vram_valid)
    {
        int n = std::snprintf(vram, sizeof(vram), "%llu of %llu MB", static_cast<unsigned long long>(v.vram_usage / kMiB),
                              static_cast<unsigned long long>(v.vram_budget / kMiB));
        if (v.vram_usage > v.vram_budget)
            std::snprintf(vram + n, sizeof(vram) - n, " (%llu MB over budget)",
                          static_cast<unsigned long long>((v.vram_usage - v.vram_budget) / kMiB));
    }

    log_line("pool %4d MB [min %d]%s | bias %.2f mips%s | VRAM %s", v.pool, v.min_pool,
             !v.has_room ? "" : v.room ? " has room" : " full", v.bias, missing, vram);
}

// The "Right now" part of the settings tab.
void draw_status(const Live &v)
{
    if (!v.renderer)
    {
        ImGui::TextUnformatted("Waiting for the renderer...");
        return;
    }

    char text[192];
    std::snprintf(text, sizeof(text), "Pool: %d MB%s", v.pool, !v.has_room ? "" : v.room ? ", has room" : ", full");
    ImGui::TextUnformatted(text);
    tooltip("The size the game keeps its streamed textures under. Texture Resolution sets it. In DX12 the game "
            "shrinks it when VRAM runs out, down to the minimum below.");

    std::snprintf(text, sizeof(text), "Blur: %.2f mips%s", v.bias, v.bias < 0.05f ? " (full resolution)" : "");
    ImGui::TextUnformatted(text);
    tooltip("Mip levels the streamer is dropping to make textures fit the pool. 0 is full resolution.");

    if (v.has_missing && v.missing_mips > 0)
    {
        std::snprintf(text, sizeof(text), "%d mips still to load", v.missing_mips);
        ImGui::TextUnformatted(text);
    }

    if (v.vram_valid)
    {
        std::snprintf(text, sizeof(text), "VRAM: %llu of %llu MB in use",
                      static_cast<unsigned long long>(v.vram_usage / kMiB),
                      static_cast<unsigned long long>(v.vram_budget / kMiB));
        ImGui::TextUnformatted(text);
        tooltip("What the game has in VRAM, and the budget Windows gives it. Without this add-on the game shrinks "
                "its pool when the two meet.");
        if (v.vram_usage > v.vram_budget)
        {
            std::snprintf(text, sizeof(text),
                          "The game is %llu MB over its budget. Windows pages the difference to system RAM.",
                          static_cast<unsigned long long>((v.vram_usage - v.vram_budget) / kMiB));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.25f, 1.0f));
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
            tooltip("Fine in small amounts. If the game stutters, lower the minimum pool or free VRAM another way "
                    "(ray tracing, render resolution, Texture Resolution).");
        }
    }
}
