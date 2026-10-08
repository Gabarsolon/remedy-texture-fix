// Offline check against the real game files: map them as images and run the add-on's own lookup on them.
// Nothing is executed from the game.
#include "CRStreamingFix.cpp"

namespace
{
// What a new ini starts from, for the memory sizes cards really report, and for this machine's own card.
bool check_card_presets()
{
    const struct
    {
        uint64_t vram_mb, min_mb, max_mb;
    } cases[] = {{0, 2048, 0},      {1990, 704, 0},     {3962, 1536, 0},     {4096, 1536, 0},      {5980, 2048, 0},
                 {7949, 2048, 0},   {8192, 2048, 0},    {10018, 2048, 0},    {12282, 3072, 6144},  {16311, 4096, 8192},
                 {24564, 4096, 8192}};
    bool ok = true;
    for (const auto &c : cases)
    {
        const PoolDefaults d = defaults_for_vram(c.vram_mb);
        if (d.min_mb != c.min_mb || d.max_mb != c.max_mb)
        {
            std::printf("FAIL a card with %llu MB starts at %llu/%llu MB, expected %llu/%llu\n", c.vram_mb, d.min_mb, d.max_mb,
                        c.min_mb, c.max_mb);
            ok = false;
        }
    }
    std::printf("%s presets for card sizes from 2 to 24 GB\n", ok ? "OK  " : "FAIL");

    // A first ini on a 12 GB card: the card's values replace the 8 GB ones the text is written with.
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    g_dir = temp;
    DeleteFileW(ini_path().c_str());
    g_default_min_mb = 3072;
    g_default_max_mb = kHasMaxPool ? 6144 : 0;
    g_default_over_mb = 1024;
    write_default_ini();
    g_default_over_mb = 960; // so the value read back is the one in the file
    const Config written = read_config();
    const bool ini_ok = written.min_pool_mb == 3072 && written.max_pool_mb == g_default_max_mb &&
                        written.bias_limit == -1.0f && written.toggle_key == 0 && written.toggle_message &&
                        !written.toggle_sound && !written.auto_pool && written.auto_over_mb == (kHasAuto ? 1024 : 960);
    std::printf("%s a first ini on a 12 GB card starts at %llu MB, the other settings at their defaults\n",
                ini_ok ? "OK  " : "FAIL", written.min_pool_mb);
    DeleteFileW(ini_path().c_str());
    g_default_min_mb = 2048;
    g_default_max_mb = 0;
    ok = ok && ini_ok;

    // The add-on only asks a dxgi the game has already loaded. Here the test loads it.
    wchar_t path[MAX_PATH + 16];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\dxgi.dll");
    if (LoadLibraryW(path))
    {
        g_create_factory = reinterpret_cast<create_factory_fn>(system_function(L"dxgi.dll", "CreateDXGIFactory1"));
        const uint64_t vram = detect_vram_mb();
        std::printf("%s this machine's card: %llu MB, a new ini would start at %llu MB\n", g_create_factory ? "OK  " : "FAIL",
                    vram, defaults_for_vram(vram).min_mb);
        ok = ok && g_create_factory;
        if constexpr (kHasAuto)
        {
            const VideoMemory memory = read_video_memory();
            std::printf("%s this machine's card: Windows gives this process a VRAM budget of %llu MB, it uses %llu MB\n",
                        memory.valid || !vram ? "OK  " : "FAIL", memory.budget / kMiB, memory.usage / kMiB);
            ok = ok && (memory.valid || !vram);
        }
    }
    return ok;
}

// Tooltips are broken into lines by the add-on, because ImGui draws them as one.
bool check_tooltip_wrap()
{
    const std::string wrapped = wrap_text("The add-on sets the minimum pool by itself. It raises it while textures are "
                                          "blurred and the game's VRAM budget has room.", 40);
    const bool ok = wrapped == "The add-on sets the minimum pool by\nitself. It raises it while textures are\n"
                               "blurred and the game's VRAM budget has\nroom." &&
                    wrap_text("short", 72) == "short" && wrap_text("", 72).empty() &&
                    wrap_text("a-word-longer-than-the-line next", 10) == "a-word-longer-than-the-line\nnext";
    std::printf("%s tooltips are broken into lines that fit\n", ok ? "OK  " : "FAIL");
    return ok;
}

// Automatic mode on made-up readings. Nothing here asks the graphics card.
bool check_automatic()
{
    if constexpr (!kHasAuto)
        return true;
    bool ok = true;
    const auto expect = [&ok](const char *what, uint64_t got, uint64_t want) {
        if (got != want)
        {
            std::printf("FAIL automatic pool %s: %llu MB, expected %llu\n", what, got, want);
            ok = false;
        }
    };

    // An 8 GB card: budget 7100 MB, the game holds 6000 MB besides textures, 960 MB over is allowed.
    // 2060 MB fit, which is 2048 in steps of 64.
    AutoExcess none;
    expect("goes up a step at a time while textures are blurred", auto_pool_mb({1024, 7100, 6000, 960, 4096, true}, none), 1280);
    expect("goes up only when a whole step fits", auto_pool_mb({1920, 7100, 6000, 960, 4096, true}, none), 1920);
    expect("stays put while textures are sharp", auto_pool_mb({1024, 7100, 6000, 960, 4096, false}, none), 1024);
    expect("never goes over the maximum pool", auto_pool_mb({1408, 16000, 3000, 1024, 1664, true}, none), 1664);
    expect("comes down to a lowered maximum pool at once", auto_pool_mb({4096, 16000, 3000, 1024, 3072, false}, none), 3072);
    // The 6 GB card of a user report: budget 5202 MB, 4393 MB used with about 1700 MB of textures.
    expect("on a 6 GB card with room", auto_pool_mb({2048, 5202, 2693, 704, 4096, true}, none), 2304);

    // Over what fits: nothing happens until it has been so for kAutoLowerWindows windows in a row.
    const auto windows = [](AutoInput in, AutoExcess &excess, uint32_t count) {
        uint64_t pool = in.pool_mb;
        for (uint32_t i = 0; i < count; ++i)
            pool = auto_pool_mb(in, excess);
        return pool;
    };
    AutoExcess excess;
    expect("leaves a dip alone", windows({3072, 7100, 6000, 960, 4096, true}, excess, kAutoLowerWindows - 1), 3072);
    expect("comes down when it lasts", auto_pool_mb({3072, 7100, 6000, 960, 4096, true}, excess), 2048);
    ok = ok && excess.windows == 0;
    windows({3072, 7100, 6000, 960, 4096, true}, excess, kAutoLowerWindows - 1);
    expect("a window that fits", auto_pool_mb({3072, 7100, 5000, 960, 4096, true}, excess), 3072);
    expect("starts the count again", windows({3072, 7100, 6000, 960, 4096, true}, excess, kAutoLowerWindows - 1), 3072);
    // 1920, then 2112 MB fit in the windows before the last one: it comes down to the most of them.
    excess = AutoExcess();
    windows({3072, 7100, 6100, 960, 4096, true}, excess, kAutoLowerWindows - 2);
    auto_pool_mb({3072, 7100, 5900, 960, 4096, true}, excess);
    expect("comes down to the most that fitted meanwhile", auto_pool_mb({3072, 7100, 6000, 960, 4096, true}, excess), 2112);
    excess = AutoExcess();
    expect("leaves a small difference alone", windows({2304, 7100, 6000, 960, 4096, true}, excess, 2 * kAutoLowerWindows), 2304);
    expect("with nothing allowed over, follows the game's own rule",
           windows({2048, 7100, 6000, 0, 4096, true}, excess, kAutoLowerWindows), 1088);
    expect("never goes under 256 MB", windows({2048, 7100, 9000, 0, 4096, true}, excess, kAutoLowerWindows), 256);

    const struct
    {
        uint64_t vram_mb, over_mb;
    } cards[] = {{0, 960}, {1990, 256}, {3962, 448}, {5980, 704}, {7899, 960}, {12282, 1024}, {24564, 1024}};
    for (const auto &c : cards)
        expect("allowance for a card", over_budget_for_vram(c.vram_mb), c.over_mb);

    // Tick by tick: 6500 MB besides textures leave 1560 MB, so 1536.
    AutoState a;
    Config c;
    c.auto_pool = true;
    c.min_pool_mb = 2048;
    c.auto_over_mb = 960;
    const VideoMemory memory = {true, 7100 * kMiB, 8400 * kMiB};
    const AutoSample game = {true, 1900 * kMiB, true};
    uint64_t pool = 0;
    ULONGLONG now = 1000;
    for (; now < 1000 + kAutoWindowMs; now += 250)
        pool = auto_tick(a, c, 4096, memory, game, now);
    expect("starts from MinPoolMB", pool, 2048);
    const int window_ticks = static_cast<int>(kAutoWindowMs / 250);
    pool = auto_tick(a, c, 4096, memory, game, now);
    ok = ok && a.decided;
    for (int i = 0; i < window_ticks * static_cast<int>(kAutoLowerWindows - 1) - 1; ++i)
        pool = auto_tick(a, c, 4096, memory, game, now += 250);
    expect("waits while it has not been over for long", pool, 2048);
    pool = auto_tick(a, c, 4096, memory, game, now += 250);
    expect("comes down after enough windows over", pool, 1536);
    ok = ok && a.decided;

    const VideoMemory other_card = {true, 7100 * kMiB, 100 * kMiB}; // less in use than the pool alone holds
    for (int i = 0; i < 9; ++i)
        pool = auto_tick(a, c, 4096, other_card, game, now += 250);
    expect("keeps its value when the readings cannot be the game's", pool, 1536);
    ok = ok && !a.decided;
    for (int i = 0; i < 9; ++i)
        pool = auto_tick(a, c, 0, memory, game, now += 250);
    expect("keeps its value while the game's maximum pool is not known", pool, 1536);
    ok = ok && !a.decided;

    c.min_pool_mb = 1024;
    expect("starts over when MinPoolMB changes", auto_tick(a, c, 4096, memory, game, now += 250), 1024);
    for (int i = 0; i < 8 * 4; ++i)
        pool = auto_tick(a, c, 4096, memory, game, now += 250);
    expect("climbs back to what fits", pool, 1536);

    std::printf("%s automatic pool on made-up readings\n", ok ? "OK  " : "FAIL");
    return ok;
}
} // namespace

#if defined(CRSF_BACKEND_TWEAKABLES)

// Control: the add-on needs four exports of the game's DLLs and four tweakables by name. With the DLLs
// only mapped, their initialisers have not run, so the tweakables cannot be looked up; the names are
// searched for in the renderer image instead.
namespace
{
bool image_contains(HMODULE image, const char *text)
{
    const auto base = reinterpret_cast<const uint8_t *>(image);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    const size_t size = nt->OptionalHeader.SizeOfImage, len = std::strlen(text) + 1;
    MEMORY_BASIC_INFORMATION info;
    for (size_t at = 0; at < size && VirtualQuery(base + at, &info, sizeof(info)); at += info.RegionSize)
    {
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            continue;
        const auto *p = static_cast<const uint8_t *>(info.BaseAddress);
        for (size_t i = 0; i + len <= info.RegionSize; ++i)
            if (p[i] == static_cast<uint8_t>(text[0]) && std::memcmp(p + i, text, len) == 0)
                return true;
    }
    return false;
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        std::printf("usage: test_locate <path to " CRSF_EXE " or " CRSF_EXE_ALT ">\n");
        return 2;
    }
    g_log = GetStdHandle(STD_OUTPUT_HANDLE);
    std::wstring dir = argv[1];
    const size_t slash = dir.find_last_of(L"\\/");
    const std::wstring exe = dir.substr(slash == std::wstring::npos ? 0 : slash + 1);
    dir.resize(slash == std::wstring::npos ? 0 : slash + 1);
    const bool dx11 = _wcsicmp(exe.c_str(), CRSF_EXE_ALT_W) == 0;
    const wchar_t *flavour = dx11 ? L"_rmdwin7_f.dll" : L"_rmdwin10_f.dll";

    if (HMODULE image = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES))
    {
        char name[64];
        std::snprintf(name, sizeof(name), "%ls", exe.c_str());
        log_game_version(reinterpret_cast<uintptr_t>(image), name);
    }
    HMODULE renderer = nullptr;
    for (const wchar_t *part : {L"rl", L"renderer", L"d3d"})
    {
        const std::wstring path = dir + part + flavour;
        HMODULE image = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!image)
        {
            std::printf("FAIL could not map %ls (error %lu)\n", path.c_str(), GetLastError());
            return 1;
        }
        if (part[1] == L'e')
            renderer = image;
    }

    on_attach();
    bool ok = g_get_tweakable && g_get_video_memory && g_missing_mips && g_heap_below_limit;
    std::printf("%s tweakable lookup %s (in %s), VRAM query %s, missing mips %s, pool room flag %s\n", ok ? "OK  " : "FAIL",
                g_get_tweakable ? "found" : "MISSING", g_tweakable_module, g_get_video_memory ? "found" : "MISSING",
                g_missing_mips ? "found" : "MISSING", g_heap_below_limit ? "found" : "MISSING");
    for (const char *name : {"Texture Streaming:Target texture pool size MB", "Texture Streaming:Min Pool Size MB",
                             "Texture Streaming:Reduce Pool Size After Free VRAM < MB", "Texture Streaming:Mip adjust [Display]"})
    {
        const bool present = image_contains(renderer, name);
        std::printf("%s the renderer registers \"%s\"\n", present ? "OK  " : "FAIL", name);
        ok = ok && present;
    }
    ok = check_card_presets() && ok;
    ok = check_automatic() && ok;
    ok = check_tooltip_wrap() && ok;
    return ok ? 0 : 1;
}

#else

namespace
{
// Addresses and stats offsets found by hand (disassembly) in the builds the add-on was developed against.
// If the executable is one of them, the result has to match exactly.
struct KnownBuild
{
    unsigned version[4];
    uint32_t heap_ptr, mgr_ptr, bias_limit, high, low, rate;
    uint32_t heap_bytes, tiles, left_lo, left_hi;
};
#if defined(CRSF_GAME_AW2)
constexpr KnownBuild kKnown[] = {
    {{0, 559, 302, 8}, 0x3A34698, 0x397FEA8, 0x3867D50, 0x3867C78, 0x3867C98, 0x3867CD8, 0x210, 0x170, 0x1D8, 0x1E0}};
#else
constexpr KnownBuild kKnown[] = {
    {{0, 563, 737, 9}, 0x5C36B48, 0x5D2B470, 0x5D2B688, 0x5D2B508, 0x5D2B530, 0x5D2B5A0, 0x1E0, 0x140, 0x1A8, 0x1B0},
    {{0, 564, 208, 5}, 0x5D07328, 0x5E00C70, 0x5E00E18, 0x5E00DC8, 0x5E00DF0, 0x5E00E88, 0x218, 0x140, 0x1E0, 0x1E8}};
#endif
} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
    {
        std::printf("usage: test_locate <path to " CRSF_EXE ">\n");
        return 2;
    }
    g_log = GetStdHandle(STD_OUTPUT_HANDLE);
    HMODULE image = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!image)
    {
        std::printf("could not map the executable (error %lu)\n", GetLastError());
        return 1;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    log_game_version(base, CRSF_EXE);
    Targets t;
    bool ok = locate(base, t);
    const auto rva = [base](uintptr_t address) { return static_cast<uint32_t>(address ? address - base : 0); };
    std::printf("%s heap_ptr=+%X mgr_ptr=+%X bias_limit=+%X high=+%X low=+%X rate=+%X\n", ok ? "OK  " : "FAIL",
                rva(t.heap_ptr), rva(t.mgr_ptr), rva(t.bias_limit), rva(t.high_threshold), rva(t.low_threshold),
                rva(t.bias_rate));
    const StatsLayout &st = t.stats;
    const bool stats_ok = st.used_known && st.left_known;
    std::printf("%s stats: heap_bytes=+%X tiles=+%X (%s) left=+%X/+%X (%s)\n", stats_ok ? "OK  " : "FAIL", st.heap_bytes,
                st.tiles, st.used_known ? "found" : "NOT FOUND", st.left_lo, st.left_hi,
                st.left_known ? "found" : "NOT FOUND");
    ok = ok && stats_ok;

    unsigned version[4] = {};
    exe_version(base, version);
    bool known = false;
    for (const KnownBuild &k : kKnown)
    {
        if (std::memcmp(k.version, version, sizeof(version)) != 0)
            continue;
        known = true;
        const bool same = rva(t.heap_ptr) == k.heap_ptr && rva(t.mgr_ptr) == k.mgr_ptr &&
                          rva(t.bias_limit) == k.bias_limit && rva(t.high_threshold) == k.high &&
                          rva(t.low_threshold) == k.low && rva(t.bias_rate) == k.rate && st.heap_bytes == k.heap_bytes &&
                          st.tiles == k.tiles && st.left_lo == k.left_lo && st.left_hi == k.left_hi;
        std::printf("%s the addresses and offsets found by hand in this build\n", same ? "OK   matches" : "FAIL differs from");
        ok = ok && same;
    }
    if (!known)
        std::printf("note: not a build with known addresses, nothing to compare against\n");
    ok = check_card_presets() && ok;
    ok = check_automatic() && ok;
    ok = check_tooltip_wrap() && ok;
    return ok ? 0 : 1;
}

#endif
