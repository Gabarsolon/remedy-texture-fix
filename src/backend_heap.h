// Control Resonant and Alan Wake 2. Included by CRStreamingFix.cpp, inside its anonymous namespace.
//
// Why textures go blurry (from disassembly of CONTROLResonant.exe 0.563.737.9; AlanWake2.exe
// 0.559.302.8 has the same code with a few offsets moved):
//   * The texture streamer (StreamedTextureHeap) sizes its pool as
//       pool = clamp(DXGI Budget - (process VRAM usage - streamer's own memory), min, max)
//     with min = 100 MiB (hard-coded) and max = 1664 / 3072 / 4096 MiB from the
//     Texture Resolution setting. Every MB used by anything else in the process
//     (path tracing, RR, frame generation, injected mods) comes out of the pool.
//   * "Texture Streaming:Fit to pool" then raises a global mip bias by 0.1 per update
//     while texture demand is above 95% of the pool (up to 10 mips), and only lowers it
//     again once demand drops under 90%. On 6-8 GB cards the pool stays small, so the
//     bias climbs over a few minutes and never recovers.
//
// The add-on writes a higher minimum (and optionally the maximum and the bias limit) into the
// game's objects. Addresses are found by signature, and it does nothing if one does not match.

constexpr bool kHasMaxPool = true;
constexpr bool kHasAuto = true;

#define CRSF_INI_MIN_POOL                                                                                    \
    "; Smallest texture streaming pool in MB. The game allows 100 MB and shrinks the pool to whatever\r\n"  \
    "; VRAM is left after everything else, which is what makes textures blurry on 6-8 GB cards.\r\n"        \
    "; Higher = sharper, but past your free VRAM Windows starts paging to system RAM (stutter).\r\n"        \
    "; 0 = leave the game's value.\r\n"
#define CRSF_INI_MAX_POOL_BLOCK                                                                                 \
    "; Largest pool in MB. The game sets 1664 / 3072 / 4096 from Texture Resolution Low / Medium / High.\r\n"  \
    "; 0 = leave the game's value.\r\n"                                                                        \
    "MaxPoolMB=0\r\n"
#define CRSF_INI_AUTO_BLOCK                                                                                     \
    "; 1 = the add-on sets the minimum pool by itself, starting from MinPoolMB. It raises it while\r\n"        \
    "; textures are blurred and the game's VRAM budget has room, and lowers it when the game goes further\r\n" \
    "; over the budget than AutoOverBudgetMB. 0 = MinPoolMB is used as it is.\r\n"                             \
    "AutoPool=0\r\n"                                                                                           \
    "; How far over its VRAM budget the game may go in automatic mode, in MB. Windows pages that much to\r\n"  \
    "; system RAM. Higher = sharper, and it can stutter. When this file is first written, the value is\r\n"    \
    "; an eighth of your graphics card's memory.\r\n"                                                          \
    "AutoOverBudgetMB=960\r\n"

// Where the heap's stats object keeps the numbers shown in the log and the settings tab. Only for
// display: the fix itself does not depend on them.
struct StatsLayout
{
    bool used_known = false;  // heap_bytes and tiles are usable
    bool left_known = false;  // left_lo and left_hi are usable
    uint32_t heap_bytes = 0;  // bytes in whole heaps
    uint32_t tiles = 0;       // 64 KiB tiles in use
    uint32_t left_lo = 0;     // lowest "VRAM left for textures" in the current sampling window
    uint32_t left_hi = 0;     // highest
};

struct Targets
{
    uintptr_t heap_ptr = 0;       // global: StreamedTextureHeap*
    uintptr_t mgr_ptr = 0;        // global: TextureStreamingManager*
    uintptr_t bias_limit = 0;     // float tweakable value "Fit to pool:Bias limit"
    uintptr_t high_threshold = 0; // float "Fit to pool:High memory threshold"
    uintptr_t low_threshold = 0;  // float "Fit to pool:Low memory threshold"
    uintptr_t bias_rate = 0;      // float "Fit to pool:Rate of bias change"
    StatsLayout stats;
};

// The game's own values, read before the first write. Kept in a process environment variable
// too, so they survive ReShade unloading and reloading the add-on within one run.
struct Baseline
{
    bool valid = false;
    uint64_t game_min = 0;
    uint64_t game_max = 0; // follows the Texture Resolution setting
    uint64_t our_max = 0;  // last max we wrote (0 = none), to tell our writes from the game's
    float game_bias_limit = 10.0f;

    uint64_t game_min_mb() const { return game_min / kMiB; }
    uint64_t game_max_mb() const { return game_max / kMiB; }
};

void on_attach()
{
}

bool read_u64(uintptr_t addr, uint64_t &out)
{
    return read_mem(addr, out);
}

bool write_u64(uintptr_t addr, uint64_t value)
{
    return write_mem(addr, value);
}

// ---------------------------------------------------------------------------------------
// signature scanning

struct Section
{
    uintptr_t begin = 0, end = 0;
};

bool find_section(uintptr_t base, const char *name, Section &out)
{
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (std::strncmp(reinterpret_cast<const char *>(sec->Name), name, 8) == 0)
        {
            out.begin = base + sec->VirtualAddress;
            out.end = out.begin + std::max(sec->Misc.VirtualSize, sec->SizeOfRawData);
            return true;
        }
    }
    return false;
}

// "48 8B ?? C3" style pattern
std::vector<int> parse_pattern(const char *pattern)
{
    std::vector<int> bytes;
    for (const char *p = pattern; *p;)
    {
        if (*p == ' ')
        {
            ++p;
            continue;
        }
        if (*p == '?')
        {
            bytes.push_back(-1);
            while (*p == '?')
                ++p;
            continue;
        }
        bytes.push_back(static_cast<int>(std::strtoul(p, const_cast<char **>(&p), 16)));
    }
    return bytes;
}

std::vector<uintptr_t> scan(const Section &s, const char *pattern, size_t max_hits = 16)
{
    const std::vector<int> pat = parse_pattern(pattern);
    std::vector<uintptr_t> hits;
    const auto *mem = reinterpret_cast<const uint8_t *>(s.begin);
    const size_t size = s.end - s.begin;
    if (pat.empty() || size < pat.size())
        return hits;
    for (size_t i = 0; i + pat.size() <= size; ++i)
    {
        if (pat[0] >= 0 && mem[i] != pat[0])
            continue;
        size_t j = 1;
        for (; j < pat.size(); ++j)
            if (pat[j] >= 0 && mem[i + j] != pat[j])
                break;
        if (j == pat.size())
        {
            hits.push_back(s.begin + i);
            if (hits.size() >= max_hits)
                break;
        }
    }
    return hits;
}

uintptr_t rel32_target(uintptr_t disp_addr)
{
    const int32_t disp = *reinterpret_cast<const int32_t *>(disp_addr);
    return disp_addr + 4 + disp;
}

bool bytes_match(uintptr_t addr, const char *pattern)
{
    const std::vector<int> pat = parse_pattern(pattern);
    const auto *mem = reinterpret_cast<const uint8_t *>(addr);
    for (size_t i = 0; i < pat.size(); ++i)
        if (pat[i] >= 0 && mem[i] != pat[i])
            return false;
    return true;
}

bool in_section(uintptr_t addr, const Section &s)
{
    return addr >= s.begin && addr < s.end;
}

// The four fit-to-pool tweakables are read with the same instructions in both games, at different offsets:
//   vsubss xmm0,xmm4,[High] ; vsubss xmm0,xmm4,[Low] ; vsubss xmm0,xmm7,[Rate] ; vminss xmm6,xmm6,[Bias limit]
bool locate_tweakables(uintptr_t f, uintptr_t high, uintptr_t low, uintptr_t rate, uintptr_t limit, Targets &t)
{
    if (!bytes_match(f + high, "C5 DA 5C 05") || !bytes_match(f + low, "C5 DA 5C 05") ||
        !bytes_match(f + rate, "C5 C2 5C 05") || !bytes_match(f + limit, "C5 CA 5D 35"))
    {
        log_line("ERROR: fit-to-pool body looks different than expected, doing nothing.");
        return false;
    }
    t.high_threshold = rel32_target(f + high + 4);
    t.low_threshold = rel32_target(f + low + 4);
    t.bias_rate = rel32_target(f + rate + 4);
    t.bias_limit = rel32_target(f + limit + 4);
    return true;
}

#if defined(CRSF_GAME_AW2)

// Alan Wake 2 (AlanWake2.exe 0.559.302.8). Tweakable names are stripped from this build and the
// accessors are inlined, so the controller loads the heap pointer directly.
bool locate_game(const Section &text, Targets &t, uintptr_t &f)
{
    //   ... ; mov rsi,rcx ; mov rax,[rip+heap] ; mov rcx,[rax] ; vxorps xmm1,xmm1,xmm1
    const auto fit = scan(text, "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 60 C5 F8 29 74 24 50 "
                                "C5 F8 29 7C 24 40 48 8B F1 48 8B 05 ?? ?? ?? ?? 48 8B 08 C5 F0 57 C9");
    if (fit.size() != 1)
    {
        log_line("ERROR: fit-to-pool signature matched %zu times (expected 1). Unsupported game version, doing nothing.",
                 fit.size());
        return false;
    }
    f = fit[0];
    t.heap_ptr = rel32_target(f + 0x24);
    if (!locate_tweakables(f, 0x58, 0x93, 0xCE, 0xF5, t))
        return false;

    // Texture streaming manager: the frame code starts the streaming update with
    //   mov rdx,[rip+manager] ; mov rcx,[rip+jobs] ; call start
    // "start" is a small stub the game has one of per kind of job. The streaming one is the stub whose
    // worker sits next to the fit-to-pool function (same class, so the linker keeps them together).
    std::vector<uintptr_t> starts;
    for (uintptr_t hit : scan(text, "48 89 54 24 10 55 48 83 EC 20 48 8B E9 E8 ?? ?? ?? ?? 8B C0 48 8D 15 ?? ?? ?? ?? "
                                    "48 8B 14 C2 48 85 D2 74 ?? 83 3A 02 75 ?? 48 8D 4C 24 38 E8 ?? ?? ?? ??", SIZE_MAX))
    {
        const uintptr_t worker = rel32_target(hit + 47);
        if ((worker > f ? worker - f : f - worker) < 0x4000)
            starts.push_back(hit);
    }
    // Every call site of that shape has to name the same object, or none is trusted.
    uintptr_t manager = 0;
    bool agree = true;
    if (starts.empty())
        return true;
    for (uintptr_t hit : scan(text, "48 8B 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ??", SIZE_MAX))
    {
        if (std::find(starts.begin(), starts.end(), rel32_target(hit + 15)) == starts.end())
            continue;
        const uintptr_t candidate = rel32_target(hit + 3);
        agree = agree && (manager == 0 || manager == candidate);
        manager = candidate;
    }
    t.mgr_ptr = agree ? manager : 0;
    return true;
}

#else

// Control Resonant (CONTROLResonant.exe 0.563.737.9 and 0.564.208.5).
bool locate_game(const Section &text, Targets &t, uintptr_t &f)
{
    //   ... ; call StreamedTextureHeap::get ; mov rcx,rax ; call StreamedTextureHeap::poolSize
    const auto fit = scan(text,
                          "48 8B C4 48 89 58 20 55 56 57 41 56 41 57 48 81 EC C0 00 00 00 C5 F8 29 70 C8 "
                          "C5 F8 29 78 B8 4C 8B F1 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B C8 C5 F0 57 C9");
    if (fit.size() != 1)
    {
        log_line("ERROR: fit-to-pool signature matched %zu times (expected 1). Unsupported game version, doing nothing.",
                 fit.size());
        return false;
    }
    f = fit[0];
    const uintptr_t heap_getter = rel32_target(f + 35);
    const uintptr_t pool_getter = rel32_target(f + 43);
    if (!in_section(heap_getter, text) || !in_section(pool_getter, text) ||
        !bytes_match(heap_getter, "48 8B 05 ?? ?? ?? ?? C3") || !bytes_match(pool_getter, "48 8B 01 C3"))
    {
        log_line("ERROR: StreamedTextureHeap accessors look different than expected, doing nothing.");
        return false;
    }
    t.heap_ptr = rel32_target(heap_getter + 3);
    if (!locate_tweakables(f, 0x5C, 0x97, 0xD2, 0xF9, t))
        return false;

    // Texture streaming manager: stats code does
    //   call getManager (mov rax,[rip+x]; ret) ; mov rcx,rax ; call getBias (vmovss xmm0,[rcx+8]; ret)
    for (uintptr_t hit : scan(text, "E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? C5 F8 28 C8"))
    {
        const uintptr_t get_mgr = rel32_target(hit + 1);
        const uintptr_t get_bias = rel32_target(hit + 9);
        if (in_section(get_mgr, text) && in_section(get_bias, text) && bytes_match(get_mgr, "48 8B 05 ?? ?? ?? ?? C3") &&
            bytes_match(get_bias, "C5 FA 10 41 08 C3"))
        {
            t.mgr_ptr = rel32_target(get_mgr + 3);
            break;
        }
    }
    return true;
}

#endif

uint32_t disp32(uintptr_t addr)
{
    return *reinterpret_cast<const uint32_t *>(addr);
}

// The stats fields are not at a fixed place: Control Resonant 0.564 moved them by 0x38 bytes. Both games
// update them in the heap's pool update, with the same instructions, so the offsets are taken from there:
//   mov rdx,[rax+HEAP_BYTES] ; mov rax,[heap+18] ; mov r,[rax+20]            (what the streamer holds)
//   ...
//   mov rcx,[heap+18] ; mov rax,[rcx+LEFT_LO] ; cmp ; cmovb ; mov [rcx+LEFT_LO],rax
//   mov rcx,[heap+18] ; mov rax,[rcx+LEFT_HI] ; cmp ; cmovb ; mov [rcx+LEFT_HI],rax
// Anything that does not look exactly like that leaves the field unknown, and it is not shown.
void locate_stats(const Section &text, StatsLayout &s)
{
    const auto window = scan(text, "48 8B ?? 18 48 8B 81 ?? ?? ?? ?? 48 3B ?? 48 0F 42 ?? 48 89 81 ?? ?? ?? ?? "
                                   "48 8B ?? 18 48 8B 81 ?? ?? ?? ?? 48 3B ?? 48 0F 42 ?? 48 89 81 ?? ?? ?? ??");
    if (window.size() != 1)
        return;
    const uintptr_t w = window[0];
    const uint32_t lo = disp32(w + 7), hi = disp32(w + 32);
    if (lo != disp32(w + 21) || hi != disp32(w + 46) || hi != lo + 8 || hi >= 0x1000 || lo % 8 != 0)
        return;
    s.left_lo = lo;
    s.left_hi = hi;
    s.left_known = true;

    Section before;
    before.begin = std::max(text.begin, w - 0x60);
    before.end = w;
    const auto held = scan(before, "48 8B 90 ?? ?? ?? ?? 48 8B ?? 18 ?? 8B 40 20");
    if (held.size() != 1)
        return;
    const uint32_t heap_bytes = disp32(held[0] + 3);
    if (heap_bytes >= 0x1000 || heap_bytes % 8 != 0 || heap_bytes == lo || heap_bytes == hi)
        return;

    // The tile count has no instruction of its own to recognise. Its offset is taken from game.h and only
    // trusted if the heap's code reads it right after the heap bytes, the way it does when it adds the two.
    char pattern[32];
    std::snprintf(pattern, sizeof(pattern), "48 8B ?? %02X %02X %02X %02X", heap_bytes & 0xFF, (heap_bytes >> 8) & 0xFF,
                  (heap_bytes >> 16) & 0xFF, heap_bytes >> 24);
    Section around; // the heap's other functions sit next to the pool update
    around.begin = w - text.begin > 0x4000 ? w - 0x4000 : text.begin;
    around.end = std::min(text.end, w + 0x4000);
    const uint32_t tiles = kLayout.stats_tiles;
    for (uintptr_t hit : scan(around, pattern, SIZE_MAX))
    {
        for (uintptr_t at = hit + 7; at + 7 <= around.end && at <= hit + 24; ++at)
        {
            if (bytes_match(at, "48 8B") && disp32(at + 3) == tiles)
            {
                s.heap_bytes = heap_bytes;
                s.tiles = tiles;
                s.used_known = true;
                return;
            }
        }
    }
}

bool locate(uintptr_t base, Targets &t)
{
    Section text, data;
    if (!find_section(base, ".text", text) || !find_section(base, ".data", data))
    {
        log_line("ERROR: could not read the section table of the game executable");
        return false;
    }

    uintptr_t f = 0;
    if (!locate_game(text, t, f))
        return false;

    const uintptr_t all[] = {t.heap_ptr, t.high_threshold, t.low_threshold, t.bias_rate, t.bias_limit};
    for (uintptr_t a : all)
    {
        if (!in_section(a, data))
        {
            log_line("ERROR: resolved address %p is outside .data, doing nothing.", reinterpret_cast<void *>(a));
            return false;
        }
    }
    if (t.mgr_ptr && !in_section(t.mgr_ptr, data))
        t.mgr_ptr = 0;

    char manager[64] = "not found (stats will lack bias/demand)";
    if (t.mgr_ptr)
        std::snprintf(manager, sizeof(manager), "exe+0x%llX", static_cast<unsigned long long>(t.mgr_ptr - base));
    log_line("Found fit-to-pool at exe+0x%llX, heap ptr exe+0x%llX, manager ptr %s",
             static_cast<unsigned long long>(f - base), static_cast<unsigned long long>(t.heap_ptr - base), manager);

    locate_stats(text, t.stats);
    char used[48] = "pool use not found", left[48] = "VRAM left not found";
    if (t.stats.used_known)
        std::snprintf(used, sizeof(used), "heap bytes +0x%X, tiles +0x%X", t.stats.heap_bytes, t.stats.tiles);
    if (t.stats.left_known)
        std::snprintf(left, sizeof(left), "VRAM left +0x%X/+0x%X", t.stats.left_lo, t.stats.left_hi);
    log_line("Stats fields: %s, %s%s", used, left,
             t.stats.used_known && t.stats.left_known ? "" : " (what is not found is left out of the stats)");
    return true;
}

// ---------------------------------------------------------------------------------------
// baseline (the game's own values)

void save_baseline(const Baseline &b)
{
    wchar_t buf[128];
    std::swprintf(buf, 128, L"%llu %llu %llu %.9g", static_cast<unsigned long long>(b.game_min),
                  static_cast<unsigned long long>(b.game_max), static_cast<unsigned long long>(b.our_max),
                  static_cast<double>(b.game_bias_limit));
    SetEnvironmentVariableW(CRSF_NAME_W L"_BASELINE", buf);
}

bool load_baseline(Baseline &b)
{
    wchar_t buf[128] = {};
    if (GetEnvironmentVariableW(CRSF_NAME_W L"_BASELINE", buf, 128) == 0)
        return false;
    unsigned long long mn = 0, mx = 0, our = 0;
    double bias = 0;
    if (swscanf_s(buf, L"%llu %llu %llu %lf", &mn, &mx, &our, &bias) != 4)
        return false;
    b.game_min = mn;
    b.game_max = mx;
    b.our_max = our;
    b.game_bias_limit = static_cast<float>(bias);
    b.valid = true;
    return true;
}

// ---------------------------------------------------------------------------------------
// the fix

// Called with g_lock held.
void apply(const Targets &t, const Config &c, Baseline &b, bool &announce)
{
    uint64_t heap = 0;
    if (!read_u64(t.heap_ptr, heap) || heap == 0)
        return; // renderer not created yet

    uint64_t pool = 0, min_pool = 0, max_pool = 0;
    float bias_limit = 0;
    if (!read_u64(heap + 0x00, pool) || !read_u64(heap + 0x08, min_pool) || !read_u64(heap + 0x10, max_pool) ||
        !read_f32(t.bias_limit, bias_limit))
        return;

    if (!b.valid && !load_baseline(b))
    {
        // The renderer exists, so the tweakables were initialised long ago and nothing here is ours yet.
        b.game_min = min_pool;
        b.game_max = max_pool;
        b.our_max = 0;
        b.game_bias_limit = bias_limit;
        b.valid = true;
        save_baseline(b);
        float hi = 0, lo = 0, rate = 0;
        read_f32(t.high_threshold, hi);
        read_f32(t.low_threshold, lo);
        read_f32(t.bias_rate, rate);
        log_line("Game values: pool min %llu MB, max %llu MB; fit-to-pool bias limit %.2f, +/-%.2f per update, "
                 "raise above %.0f%% of pool, lower below %.0f%%",
                 static_cast<unsigned long long>(b.game_min / kMiB), static_cast<unsigned long long>(b.game_max / kMiB),
                 b.game_bias_limit, rate, (1.0f - lo) * 100.0f, (1.0f - hi) * 100.0f);
    }
    else if (max_pool != b.our_max && max_pool != b.game_max)
    {
        b.game_max = max_pool; // the game set a new ceiling (Texture Resolution changed)
        save_baseline(b);
        announce = true;
    }

    const uint64_t want_min = c.min_pool_mb ? c.min_pool_mb * kMiB : b.game_min;
    uint64_t want_max = c.max_pool_mb ? c.max_pool_mb * kMiB : b.game_max;
    if (want_min > want_max)
        want_max = want_min; // e.g. Texture Resolution Low caps the pool at 1664 MB

    if (max_pool != want_max && write_u64(heap + 0x10, want_max))
    {
        b.our_max = want_max != b.game_max ? want_max : 0;
        save_baseline(b);
        announce = true;
    }
    if (min_pool != want_min && write_u64(heap + 0x08, want_min))
        announce = true;
    if (pool < want_min)
        write_u64(heap + 0x00, want_min); // take effect now instead of at the game's next pool update

    const float want_bias = c.bias_limit >= 0.0f ? c.bias_limit : b.game_bias_limit;
    if (bias_limit != want_bias && write_f32(t.bias_limit, want_bias))
        announce = true;

    if (announce)
    {
        announce = false;
        log_line("Applied: pool min %llu MB (game %llu), max %llu MB (game %llu), bias limit %.2f (game %.2f)",
                 static_cast<unsigned long long>(want_min / kMiB), static_cast<unsigned long long>(b.game_min / kMiB),
                 static_cast<unsigned long long>(want_max / kMiB), static_cast<unsigned long long>(b.game_max / kMiB),
                 want_bias, b.game_bias_limit);
    }
}

// What the streamer is doing right now, read straight from the game.
struct Live
{
    bool renderer = false; // heap object exists
    uint64_t pool = 0, min_pool = 0, max_pool = 0;
    bool used_valid = false; // how much of the pool is filled could be read
    uint64_t used = 0;
    bool manager = false; // demand and bias could be read
    uint64_t demand = 0;
    float bias = 0.0f;
    // What the game computes as free for textures (DXGI budget minus the rest of the process), as a
    // low-high range over its sampling window: the pool it would pick by itself.
    bool left_valid = false;
    uint64_t left_lo = 0, left_hi = 0;
};

Live read_live(const Targets &t)
{
    Live v;
    uint64_t heap = 0, st = 0, heap_bytes = 0, tiles = 0, mgr = 0;
    if (!read_u64(t.heap_ptr, heap) || heap == 0)
        return v;
    v.renderer = true;
    read_u64(heap + 0x00, v.pool);
    read_u64(heap + 0x08, v.min_pool);
    read_u64(heap + 0x10, v.max_pool);
    read_u64(heap + 0x18, st);
    if (st && t.stats.used_known && read_u64(st + t.stats.heap_bytes, heap_bytes) && read_u64(st + t.stats.tiles, tiles))
    {
        v.used = heap_bytes + (tiles << 16);
        v.used_valid = true;
    }
    if (st && t.stats.left_known && read_u64(st + t.stats.left_lo, v.left_lo) && read_u64(st + t.stats.left_hi, v.left_hi))
    {
        // Until the game has sampled once, the two fields hold 0 and ~0.
        v.left_valid = v.left_lo <= v.left_hi && v.left_hi <= (1ull << 50);
    }
    if (t.mgr_ptr && read_u64(t.mgr_ptr, mgr) && mgr)
    {
        // Shown only if it looks like a mip bias and a byte count, in case a game update moved the fields.
        v.manager = read_u64(mgr + kLayout.mgr_demand, v.demand) && read_f32(mgr + kLayout.mgr_bias, v.bias) &&
                    v.bias >= 0.0f && v.bias <= 32.0f && v.demand < (1ull << 40);
    }
    return v;
}

// "1118-1142 MB", or "4118 MB" when both ends are the same.
void format_left(char *out, size_t size, const Live &v)
{
    const unsigned long long lo = v.left_lo / kMiB, hi = v.left_hi / kMiB;
    if (lo == hi)
        std::snprintf(out, size, "%llu MB", lo);
    else
        std::snprintf(out, size, "%llu-%llu MB", lo, hi);
}

// For automatic mode: what the pool holds, and whether textures are being blurred to fit it.
AutoSample auto_sample(const Live &v)
{
    AutoSample a;
    a.valid = v.renderer && v.used_valid;
    a.held = v.used;
    a.starving = v.manager ? v.bias >= 0.05f : v.used / 9 >= v.pool / 10; // without the bias: 90% full
    return a;
}

// The largest pool automatic mode may set: the maximum pool, or the minimum it starts from if that is
// higher, as with a fixed minimum. 0 until the game's own maximum is known.
uint64_t auto_ceiling_mb(const Config &c, const Baseline &b)
{
    if (!b.valid)
        return 0;
    return std::max(c.max_pool_mb ? c.max_pool_mb : b.game_max_mb(), c.min_pool_mb);
}

// `extra` is appended to the line as it is.
void log_stats(const Targets &t, const char *extra)
{
    const Live v = read_live(t);
    if (!v.renderer)
    {
        log_line("waiting for the renderer...");
        return;
    }

    char left[96];
    if (!v.left_valid)
    {
        std::snprintf(left, sizeof(left), "n/a");
    }
    else
    {
        format_left(left, sizeof(left), v);
        if (v.pool > v.left_lo)
        {
            const size_t n = std::strlen(left);
            std::snprintf(left + n, sizeof(left) - n, " (pool is %llu MB above it)",
                          static_cast<unsigned long long>((v.pool - v.left_lo) / kMiB));
        }
    }

    char streamer[64];
    if (v.manager)
        std::snprintf(streamer, sizeof(streamer), "demand %4llu MB | bias %.2f mips",
                      static_cast<unsigned long long>(v.demand / kMiB), v.bias);
    else
        std::snprintf(streamer, sizeof(streamer), "demand n/a | bias n/a");

    char used[32] = "used n/a";
    if (v.used_valid)
        std::snprintf(used, sizeof(used), "used %4llu MB", static_cast<unsigned long long>(v.used / kMiB));

    log_line("pool %4llu MB [min %llu, max %llu] | %s | %s | VRAM left for textures %s%s",
             static_cast<unsigned long long>(v.pool / kMiB), static_cast<unsigned long long>(v.min_pool / kMiB),
             static_cast<unsigned long long>(v.max_pool / kMiB), used, streamer, left, extra);
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
    if (v.used_valid)
    {
        std::snprintf(text, sizeof(text), "%llu of %llu MB used", static_cast<unsigned long long>(v.used / kMiB),
                      static_cast<unsigned long long>(v.pool / kMiB));
        ImGui::ProgressBar(v.pool ? static_cast<float>(static_cast<double>(v.used) / static_cast<double>(v.pool)) : 0.0f,
                           ImVec2(-FLT_MIN, 0.0f), text);
        tooltip("The texture streaming pool and how much of it is filled.");
    }
    else
    {
        std::snprintf(text, sizeof(text), "Pool: %llu MB", static_cast<unsigned long long>(v.pool / kMiB));
        ImGui::TextUnformatted(text);
        tooltip("The texture streaming pool. How much of it is filled could not be read in this game version.");
    }

    if (v.manager)
    {
        std::snprintf(text, sizeof(text), "Blur: %.2f mips%s", v.bias, v.bias < 0.05f ? " (full resolution)" : "");
        ImGui::TextUnformatted(text);
        tooltip("Mip levels the streamer is dropping to make textures fit the pool. 0 is full resolution.");

        std::snprintf(text, sizeof(text), "Textures want %llu MB at this blur",
                      static_cast<unsigned long long>(v.demand / kMiB));
        ImGui::TextUnformatted(text);
    }

    if (v.left_valid)
    {
        char left[48];
        format_left(left, sizeof(left), v);
        std::snprintf(text, sizeof(text), "The game alone would give textures %s", left);
        ImGui::TextUnformatted(text);
        tooltip("VRAM budget minus everything else the game has in VRAM. Without this add-on, that is the pool.");
        if (v.pool > v.left_lo)
        {
            std::snprintf(text, sizeof(text), "Pool is %llu MB above that. Windows pages the difference to system RAM.",
                          static_cast<unsigned long long>((v.pool - v.left_lo) / kMiB));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.25f, 1.0f));
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
            tooltip("Fine in small amounts. If the game stutters, lower the minimum pool or free VRAM another way "
                    "(frame generation, path tracing quality, render resolution).");
        }
    }
}
