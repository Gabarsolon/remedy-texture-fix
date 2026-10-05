# Remedy texture fix

ReShade add-ons that stop textures in Remedy's Northlight games from going blurry a few minutes into play on 6–8 GB graphics cards, with ray tracing or path tracing left on. They change nothing on disk.

| Game | Add-on | Download | Played on |
|---|---|---|---|
| Control Resonant | `CRStreamingFix.addon64` | [v1.3.0](https://github.com/Gabarsolon/remedy-texture-fix/releases/tag/v1.3.0) | game 0.564.208.5 and 0.563.737.9, see [Tested with](#tested-with) |
| Alan Wake 2 | `AW2StreamingFix.addon64` | [v1.0.0](https://github.com/Gabarsolon/remedy-texture-fix/releases/tag/aw2-v1.0.0) | game 0.559.302.8, see [Alan Wake 2](#alan-wake-2) |
| Control | `ControlStreamingFix.addon64` | [v1.0.0](https://github.com/Gabarsolon/remedy-texture-fix/releases/tag/control-v1.0.0) | game 0.0.518.2177 in DX12, see [Control](#control) |

All three are built from one source. Control Resonant and Alan Wake 2 work the same way. Control has an older streamer, and its own section. The rest of this page describes the Control Resonant add-on. For Alan Wake 2, read `AW2StreamingFix` for `CRStreamingFix` and `AlanWake2.exe` for `CONTROLResonant.exe`.

## The problem

Textures look fine when you load in, then turn to mush and stay that way. Changing Texture Resolution doesn't help. Turning off ray tracing or dropping the resolution does.

The cause, from disassembling `CONTROLResonant.exe` 0.563.737.9:

- **Pool size.** The texture streamer sizes its pool as the DXGI budget minus everything else the game process has in VRAM, clamped between 100 MiB and a ceiling. Path tracing, ray reconstruction, frame generation and anything injected into the process all come out of the pool.
- **Texture Resolution.** It only sets the ceiling: 1664 MiB on Low, 3 GiB on Medium, 4 GiB above that. When your leftover VRAM is below the ceiling, the setting does nothing.
- **Blur.** While textures need more than 95% of the pool, the engine adds 0.1 mips of blur per update, up to 10. It removes blur only once demand falls under 90%.

So on a small card the pool follows your free VRAM down, and the blur follows the pool.

In one test session (8 GB card, 1440p, path tracing, ray reconstruction and frame generation on) the game left a median of 1.1 GB for textures. Within three minutes that was down to 228 MB.

## What the add-on does

It sets a floor under the pool: 2048 MB by default instead of the game's 100 MB. In the same test session the pool held at 2048 MB and the blur stayed flat at about 2.2 mips.

It can also raise the ceiling and cap the blur. The settings and the live numbers are in a panel in the ReShade menu, and in `CRStreamingFix.ini` and `CRStreamingFix.log`.

It finds what it needs by code signature. If the signatures don't match your game version, it logs an error and does nothing. The numbers it shows come from fields that game updates move around, so it reads their positions out of the game's code too, and leaves out any number it cannot place.

## Install

1. Install [ReShade](https://reshade.me) **with full add-on support** for the game. RenoDX needs the same build.
2. Put `CRStreamingFix.addon64` next to `CONTROLResonant.exe`.
3. Start the game. `CRStreamingFix.ini` and `CRStreamingFix.log` appear in the same folder.

To remove it, delete the file.

It also runs without ReShade: rename it to `CRStreamingFix.asi` and load it with an ASI loader. The offline test covers that path, but it has not been tried in the game.

## In the ReShade menu

Open the ReShade menu and look for the **CRStreamingFix** window. It shows the pool, how full it is, the current blur and what the game would have left for textures by itself. Below that are the settings.

- Changes apply within a quarter of a second. No restart.
- A slider is saved to `CRStreamingFix.ini` when you let go of it.
- If the window isn't already a tab, it starts as a floating window on the right. Drag its title onto the ReShade tab bar to dock it.

The panel needs a ReShade build that carries ImGui 1.92.5, such as 6.8.0. On other builds the add-on still works; it just has no panel, and the log says so.

## Settings

The panel and `CRStreamingFix.ini` hold the same settings. The ini is re-read while the game runs, so editing it by hand works too.

| Key | Default | Meaning |
|---|---|---|
| `MinPoolMB` | 2048 | Pool floor. 0 leaves the game's 100 MB. |
| `MaxPoolMB` | 0 | Pool ceiling. 0 leaves the game's value. |
| `BiasLimit` | -1 | Largest mip bias the streamer may add. -1 leaves the game's 10. |
| `LogIntervalSec` | 5 | Seconds between stats lines. 0 turns them off. |
| `ToggleKey` | None | Key that switches the fix off and on while you play, such as `F8` or `Ctrl+F8`. |
| `ToggleMessage` | 1 | Show ON or OFF at the top of the screen for a moment when the key is used. Read when the game starts. |
| `ToggleSound` | 0 | 1 plays two beeps when the key is used: rising for on, falling for off. |

### Switching it off while playing

Since CRStreamingFix v1.2.0 (the Alan Wake 2 and Control add-ons get it with their next release) the panel has a **Fix on** box and an **On/off key**. Off hands every value back to the game, so the pool shrinks again and the blur returns; on puts the floor back. Off is never saved: the fix is on whenever the game starts.

The panel offers F1 to F12, Insert, Delete, Home, End, PageUp, PageDown, Pause, ScrollLock and the numpad digits, with Ctrl, Shift and Alt. In the ini, `ToggleKey` also takes a single letter or digit. The key only counts while the game window is in front, and it is read without hooking the game's input.

The ON/OFF message is drawn by ReShade, so it needs the same ReShade build as the panel. It is set up when the game starts: after ticking **Message** in a session that started without it, it shows from the next start on. With the message enabled, ReShade may show a small empty window named "OSD" while one of its own messages is up, such as the one after a screenshot. That is how ReShade handles add-on text outside its menu; untick **Message** and restart the game to be rid of it.

The beeps are played through the game's own audio, so they follow the game's volume.

If the fix stutters for you, try a lower `MinPoolMB` before reaching for the key. The stutter comes from the pool being larger than your free VRAM, and switching the fix back on has to load the textures again.

## Tuning

A stats line looks like this:

```
pool 2048 MB [min 2048, max 3072] | used 1933 MB | demand 1922 MB | bias 2.30 mips | VRAM left for textures 1118-1142 MB (pool is 930 MB above it)
```

- **VRAM left for textures** is the pool the game would have picked by itself.
- **bias** is how many mip levels of blur the streamer is applying. 0 is full resolution.
- **pool is N MB above it** means the game is using more VRAM than its budget, and Windows pages the difference to system RAM.

In the test session the pool sat about 950 MB above free VRAM with no stutter reported, but that was a short session. If you get stutter, lower `MinPoolMB`, or free VRAM another way: frame generation, path tracing quality, render resolution.

### Presets by graphics card memory

Since v1.3.0 the panel has a row of preset buttons, and a first run picks the one for your card by itself. An ini that already exists is left alone.

| Card | Minimum pool | Maximum pool | Where the numbers come from |
|---|---|---|---|
| 4 GB | 1536 MB | game's | One user on an RTX 3050 4 GB runs 1664 MB on the Low preset at 1080p with DLSS Performance. Another gets stutter in combat at 2048 MB. |
| 6 GB | 1792 MB | game's | Not tested yet: halfway between the 4 GB and 8 GB values. |
| 8 GB | 2048 MB | game's | What the add-on was made and played with, path tracing on. |
| 12 GB | 3072 MB | 6144 MB | Not tested yet: the same share of the card as on 8 GB. |
| 16 GB and more | 4096 MB | 8192 MB | One user on a 16 GB card runs minimum and maximum at 8192 MB. |

Treat them as starting points. How much VRAM is left for textures depends on your settings far more than on the card: path tracing, frame generation and a high render resolution all take from it. If it stutters, go down; if "VRAM left for textures" stays well above your pool, there is room to go up.

A raised maximum is harmless: the game only grows the pool into VRAM that is actually free.

Lowering `BiasLimit` without a bigger pool doesn't sharpen anything, because the pool is also a hard limit on what gets loaded.

## Tested with

Game 0.564.208.5 with v1.3.0, v1.2.0 and v1.1.1, and game 0.563.737.9 with the versions before those. ReShade 6.8.0, RTX 5060 Laptop 8 GB, alongside RenoDX and OptiScaler. That is the only setup the author has played it on, panel, on/off key and preset buttons included. On that card the add-on read 7899 MB and matched it to the 8 GB preset; writing a first ini for other card sizes is covered by the offline test only. What users report on other cards is in the preset table above.

Game 0.564.478.0 has not been played. Its executable was checked with `build.bat test`: the add-on finds everything it needs there (fit-to-pool `exe+0x2ED93D0`, heap pointer `exe+0x5D0F2E8`, manager pointer `exe+0x5E08C30`), and the fit-to-pool code is the same as in 0.564.208.5.

On game 0.564 use v1.1.1 or later. Older versions still apply the fix there, but the game update moved the fields behind "MB used" and "VRAM left for textures", so they show wrong numbers.

## Alan Wake 2

Alan Wake 2 runs the same texture streamer with the same numbers: a 100 MiB floor, ceilings of 1664 MiB, 3 GiB and 4 GiB from Texture Resolution, and the same fit-to-pool controller. `AW2StreamingFix.addon64` is this add-on built for `AlanWake2.exe`. Install and settings are as above, with `AW2StreamingFix` in the file names: put `AW2StreamingFix.addon64` next to `AlanWake2.exe`.

One test session showed the pool driving the blur directly. It ran on an 8 GB card at 1440p from a 720p render, with path tracing, ray reconstruction and frame generation on and Texture Resolution on High. The pool was moved with the add-on's sliders while standing in one spot:

| Pool | Textures loaded | Blur |
|---|---|---|
| 128 MB, forced, as when VRAM runs out | 0.15 GB | 10 mips, the game's limit |
| 2048 MB, the add-on's default floor | 1.9 GB | 1.2 to 1.5 mips |
| 8192 MB, forced | 2.9 GB | 0 |

Five minutes in, the game by itself had 1.4 GB left for textures, and that number was still falling.

Tested with game 0.559.302.8 and ReShade 6.8.0 on an RTX 5060 Laptop 8 GB, with no other mods. That is the only setup it has been played on, and the session was short. If it misbehaves for you, please open an issue with `AW2StreamingFix.log` attached.

## Control

Control (2019) has an older version of the same system. `ControlStreamingFix.addon64` is the add-on for `Control_DX12.exe` and `Control_DX11.exe`.

What goes wrong there, from disassembling the renderer DLLs of game 0.0.518.2177:

- **Pool size.** Texture Resolution sets the pool directly: 512, 1024, 1664, 2048 or 4096 MB.
- **The shrink, in DX12 only.** When the game has less than 64 MB of VRAM free, it moves the pool almost all the way to a minimum of 100 MB. Nothing raises it again until you change Texture Resolution. Far enough over its VRAM budget, the result drops below the minimum and can go negative.
- **Blur.** While the textures in use leave less than 50 MB of the pool free, the engine adds 0.1 mips of blur per update, up to 10.

The DX11 renderer has no shrink. Its pool stays at the Texture Resolution size.

The add-on makes the floor the game's minimum (2048 MB by default instead of 100 MB), and once the game has shrunk the pool that far it holds it there. Differences from the other two add-ons:

- A floor above the pool your Texture Resolution asks for raises the pool to the floor.
- There is no `MaxPoolMB`. The ceiling is Texture Resolution.
- The panel shows the pool, the blur and the game's VRAM use against its budget. The game does not keep a running total of loaded textures to show.
- In DX11 all it can do is raise the pool, and there are no VRAM numbers.

Install it like the others: ReShade with full add-on support, and `ControlStreamingFix.addon64` next to `Control_DX12.exe`.

One test session caught the shrink as it happened. It ran in DX12 on an 8 GB card at 1440p from a 720p render, with every ray tracing effect on and Texture Resolution at its highest setting:

| Floor | What happened |
|---|---|
| 64 MB, close to the game's own 100 MB | The game shrank its pool from 4096 MB to 80 MB. Five seconds later the blur was at 10 mips, the game's limit. |
| 2048 MB, the add-on's default | The pool held at 2048 MB with no blur and room to spare. The game sat 220 to 400 MB over its VRAM budget of about 6.7 GB. |

Tested with game 0.0.518.2177 (Epic) in DX12 and ReShade 6.8.0 on an RTX 5060 Laptop 8 GB, with no other mods. That is the only setup it has been played on, and the session was short. The DX11 build has not been played. The add-on was checked against that build's DLLs: it finds the game's settings there, and the game's own getter confirms what it writes.

## If you used the old patcher from this repo

`patch.py` and `patch.ps1` are gone. They patched the exe based on a misreading of the engine's tweakables. Put back the `CONTROLResonant.exe.bak` they made.

## How it works

Addresses are for 0.563.737.9. The add-on finds them by signature.

| What | Where |
|---|---|
| `StreamedTextureHeap` constructor: pool 1 GiB, min 100 MiB, max 3 GiB | `exe+0x1D03BD0` |
| Pool update from `QueryVideoMemoryInfo` | `exe+0x1D05670` |
| Ceiling from Texture Resolution | `exe+0x2F17DDB` |
| Fit-to-pool controller | `exe+0x2E8C3E0` |
| Mip selection per texture | `exe+0x2E8C9B0` |
| `StreamedTextureHeap*` | `exe+0x5C36B48`, then `+0x00` pool, `+0x08` min, `+0x10` max |
| Texture streaming manager | `exe+0x5D2B470`, then `+0x00` demand, `+0x08` bias |

Game 0.564.208.5 has the same code at other addresses: the fit-to-pool controller at `exe+0x2ED9400`, the pool update at `exe+0x1D2CAC0`, `StreamedTextureHeap*` at `exe+0x5D07328` and the texture streaming manager at `exe+0x5E00C70`. The heap's stats object grew by 0x38 bytes in that update, which moved the bytes-in-heaps field from `+0x1E0` to `+0x218` and the "VRAM left" pair from `+0x1A8` to `+0x1E0`.

Four times a second the add-on writes the floor (and the ceiling, if set) into the heap object, and the bias limit into the tweakable's value. When it is unloaded it leaves memory alone.

The same places in Alan Wake 2 0.559.302.8:

| What | Where |
|---|---|
| Pool update from `QueryVideoMemoryInfo` | `exe+0x2087D80` |
| Ceiling from Texture Resolution | `exe+0x2267E6B` |
| Fit-to-pool controller | `exe+0x22075A0` |
| `StreamedTextureHeap*` | `exe+0x3A34698`, then `+0x00` pool, `+0x08` min, `+0x10` max |
| Texture streaming manager | `exe+0x397FEA8`, then `+0x08` demand, `+0x10` bias |

Control 0.0.518.2177 keeps these settings as tweakables: objects the engine looks up by name. The game's `rl` DLL exports that lookup, so the Control add-on asks the game for the ones it needs and uses no signatures.

| What | Where |
|---|---|
| Pool shrink, `TextureResourceStreamManager::update` | `renderer_rmdwin10_f.dll+0x168ED0` |
| Blur, `TextureResource::adjustMipBias` | `renderer_rmdwin10_f.dll+0x159CD0` |
| Pool from Texture Resolution | `renderer_rmdwin10_f.dll+0x137744` |
| Tweakable lookup, `d::BaseTweakable::getTweakable` | exported by `rl_rmdwin10_f.dll` |
| A tweakable | `+0xA8` type, `+0xAC` changed by code, `+0xB0` value, then minimum, maximum, default |
| Pool, minimum, shrink threshold, blur | `Texture Streaming:Target texture pool size MB`, `:Min Pool Size MB`, `:Reduce Pool Size After Free VRAM < MB`, `:Mip adjust [Display]` |

There the add-on writes the floor into the minimum, lifts a pool that is under the floor, and switches the shrink off while the pool sits on the floor.

## Build

`build.bat` needs Visual Studio 2022 or its Build Tools (x64). All three add-ons come from one source:

- `src/CRStreamingFix.cpp`: what they share. Loading, the timer, settings and the tab in the ReShade menu.
- `src/backend_heap.h`: Control Resonant and Alan Wake 2.
- `src/backend_tweakables.h`: Control.
- `src/game.h`: names, versions and the few fixed offsets per game.

Commands:

- `build.bat` builds `build\CRStreamingFix.addon64`, `build\AW2StreamingFix.addon64` and `build\ControlStreamingFix.addon64`.
- `build.bat test` also runs the offline test for each. It fakes the game and ReShade, menu included, to exercise loading, unloading and every setting. The Control one also runs the game's pool logic as read from the disassembly, so the fix is tested against the shrink it is there to stop.
- `build.bat test "path\to\CONTROLResonant.exe" "path\to\AlanWake2.exe" "path\to\Control_DX12.exe"` also checks the add-ons against those games' own files. Any of them is enough. For Control Resonant and Alan Wake 2 it checks that the signatures resolve and that the stats fields are found. For Control it loads the game's `rl` and renderer DLLs, without starting the game, and asks the game's own getter whether the pool is at the floor.

The menu panel is drawn through the ImGui function table that ReShade gives add-ons, so no ImGui code is compiled in. The two headers that takes are in `third_party`.

## License

MIT. The headers in `third_party` keep their own licenses: Dear ImGui is MIT, and ReShade's `reshade_overlay.hpp` is BSD-3-Clause OR MIT. See `third_party/README.md`.
