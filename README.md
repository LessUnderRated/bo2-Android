<div align="center">

<picture>
  <source media="(prefers-color-scheme: light)" srcset="docs/assets/hero-public-light.svg">
  <img src="docs/assets/hero-public.svg" alt="bo2-mac: Call of Duty: Black Ops II statically recompiled from x86-32 Windows to native ARM64 macOS, from your own Steam copy. On an Apple M1: campaign 51 to 60 frames per second, Zombies 57 to 60, multiplayer against bots 53 to 60." width="100%">
</picture>

# bo2-mac

**Call of Duty: Black Ops II, statically recompiled from 32-bit x86 Windows to native ARM64 macOS.**<br>
Campaign, Zombies and multiplayer against bots, built on your Mac from your own Steam copy.

<b>English</b> · <a href="README.zh.md">中文</a>

<a href="#install"><img src="docs/assets/btn-play.svg" alt="Play it" height="40"></a>&nbsp;
<a href="#results"><img src="docs/assets/btn-results.svg" alt="Results" height="40"></a>&nbsp;
<a href="#pipeline"><img src="docs/assets/btn-pipeline.svg" alt="Pipeline" height="40"></a>&nbsp;
<a href="#findings"><img src="docs/assets/btn-findings.svg" alt="Findings" height="40"></a>&nbsp;
<a href="#roadmap"><img src="docs/assets/btn-roadmap.svg" alt="Roadmap" height="40"></a>

![source: x86-32 Windows](https://img.shields.io/badge/source-x86--32%20Windows-ff6a00?style=flat-square&labelColor=0b0b0c)
![target: ARM64 macOS](https://img.shields.io/badge/target-ARM64%20macOS-ff6a00?style=flat-square&labelColor=0b0b0c)
![lifter: Remill, LLVM 17](https://img.shields.io/badge/lifter-Remill%20%E2%86%92%20LLVM%2017-ff6a00?style=flat-square&labelColor=0b0b0c)
![renderer: D3D11 to Metal](https://img.shields.io/badge/renderer-D3D11%20%E2%86%92%20Metal-ff6a00?style=flat-square&labelColor=0b0b0c)
![Wine: none](https://img.shields.io/badge/Wine-none-ff6a00?style=flat-square&labelColor=0b0b0c)
![game files in this repository: none](https://img.shields.io/badge/game%20files-none-f4f1ea?style=flat-square&labelColor=0b0b0c)
![status: beta](https://img.shields.io/badge/status-beta-f4f1ea?style=flat-square&labelColor=0b0b0c)

<sub>[Abstract](#abstract) · [Method](#method) · [Limits](#limits) · [Repository](#repository) · [Legal](#legal)</sub><br>
<sub>Beta, 2026-11-03 · measured on one Apple M1 (4 performance + 4 efficiency cores, 16 GB)</sub>

</div>

<p align="center"><a href="https://github.com/adrbn/bo2-mac/raw/main/docs/assets/gameplay.mp4"><img src="docs/assets/preview.gif" alt="Campaign, Zombies and multiplayer running natively on macOS" width="560"></a><br>
<sub>Campaign (Angola), Zombies (Town) and multiplayer (Hijacked, against bots) on an Apple M1, recorded while playing; the number in the top right corner is frames per second, and the screen recording itself costs some, mostly in the campaign. <a href="https://github.com/adrbn/bo2-mac/raw/main/docs/assets/gameplay.mp4">80-second video (MP4, 20 MB)</a></sub></p>

> [!IMPORTANT]
> **This repository contains no game file and no translated game code.** It turns your own Steam copy of Black Ops
> II into an app for your Mac, on your Mac. The game's code ends up inside that app, so nobody can hand you a
> ready-made one: each player builds their own, and it may not be shared. Unofficial fan project, not affiliated with
> Activision, Treyarch or Microsoft. To play, buy the game. Beta: read [what has and has not been proven](docs/PRERELEASE.md).

---

<img src="docs/assets/rule-01.svg" alt="" width="100%">

## Abstract

bo2-mac takes the three executables of Black Ops II, the campaign (`t6sp.exe`), Zombies (`t6zm.exe`) and
multiplayer (`t6mp.exe`), and turns their x86 machine code into ARM64 machine code ahead of time.
[Remill](https://github.com/lifting-bits/remill) lifts each function to LLVM IR, a chain of our own passes rewrites
that IR, and the result is linked with a Win32 runtime written by hand, whose Direct3D 11 is drawn by Metal. There is
no Wine and no JIT: the game runs as native code, in one process. About 150,000 functions are translated across the
three executables. Only the short sequences that the copy protection hides run in a small x86 interpreter, and so does
any code the translation missed, so a hole is built to cost speed rather than crash the game.

On an Apple M1 the campaign runs at **51–60 frames per second**, and every mission loads and hands over to the next
one, down to the end credits. Zombies is played solo on **six maps** through the game's own menus at **57–60**.
Multiplayer is played against bots through its real menus, eleven game modes, at **53–60**. One app,
`Black Ops II.app`, opens the campaign, and the CAMPAIGN / ZOMBIES / MULTIPLAYER entries of each menu switch to the
other game.

Nothing is downloaded from anyone but you: the converter reads the executables of the copy you bought, checks them
byte for byte, and builds the app on your machine.

<img src="docs/assets/rule-02.svg" alt="" width="100%">

## Results

| | measured | conditions |
|---|---|---|
| Campaign | **51–60 fps** | every mission loads and chains to the end credits; not all played end to end |
| Zombies | **57–60 fps** (Die Rise 43–58) | TranZit, Mob of the Dead, Nuketown, Origins, Buried, Die Rise, solo, through the real menus |
| Multiplayer against bots | **53–60 fps** | eleven game modes through the real menus: a full round, the scoreboard, back to the lobby |
| Game world against real time | **≈ 1.0×** | the game's own server clock against wall time |
| Conversion on the player's Mac | **about 2.5 hours** on an M1 | the campaign is playable as soon as it is converted; Zombies and multiplayer follow |
| Disk | peak about **10 GB**, about **4 GB** kept | after the conversion, only the linked binaries stay |

Frame rate counts the drawables really presented to the screen, checked against the screen and the Metal HUD.

**What you can play**

<table>
<tr>
<td width="33%" valign="top"><img src="docs/assets/card-public-campaign.svg" alt="Campaign" width="100%"><br>
The real main menu: NEW STORY, RESUME STORY, REPLAY MISSION. From <code>angola</code> to the end credits, the
missions load and hand over to the next one. Strike Force missions start with a click.</td>
<td width="33%" valign="top"><img src="docs/assets/card-public-zombies.svg" alt="Zombies" width="100%"><br>
The six maps, solo: main menu, lobby, match, Game Over, scoreboard, then another match in the same process.
DLC maps play if your Steam account owns them.</td>
<td width="33%" valign="top"><img src="docs/assets/card-public-multi.svg" alt="Multiplayer" width="100%"><br>
Custom Games against bots from the real menus: CHOOSE CLASS, the HUD, a full round, the scoreboard
and back to the lobby. The match rules are this project's own script.</td>
</tr>
</table>

All three modes: keyboard and mouse, or a DualSense as XInput through GameController.framework; sound on; movement
keys follow their position on your keyboard (ZQSD on AZERTY, WASD on QWERTY), unless you bound them yourself in the
game's options.

<img src="docs/assets/rule-03.svg" alt="" width="100%">

## Pipeline

In five steps, all on the player's Mac:

1. **Check.** The converter accepts only the exact Steam build of each executable, checked by SHA-256. The
   launcher asks the Steam client on your Mac whether the signed-in account owns the game, and refuses otherwise.
2. **Catalogue and lift.** Ghidra lists the functions of your own executable; Remill v6.0.1 lifts each one to
   LLVM IR. Five patches to the lifter are carried in [tools/remill-patches/](tools/remill-patches/), among them the
   x87 rounding mode, and one that forks a running Remill for each function instead of reloading its x86 semantics.
3. **Fold.** Remill's IR is correct but slow: every guest memory access, flag and x87 push is a call or a store
   into a `State` structure. Our passes rewrite it before `clang -O2`: guest reads and writes are inlined, `lock`
   sequences become hardware atomics, the guest `ret` stays inside the lifted code, and the x87 stack is promoted to
   locals.
4. **Runtime.** [runtime/](runtime/) implements the Windows imports the game really calls, with exact stdcall
   arities (the Steam API ones taken from Valve's public headers). Guest memory is one 4 GiB mapping. Direct3D 11 is
   a fake-COM layer over Metal; DXBC shaders go through [HLSLcc](third_party/HLSLcc). XAudio2 goes to CoreAudio, and
   the videos are decoded natively with libvpx.
5. **Threads.** Guest threads start as coroutines on one host thread, which removes most sources of
   non-determinism and makes a run replayable. The fast configuration moves the server, the render backend, sound mixing and decoding, and the job
   workers onto real host threads. ARM is not TSO, so this needed real memory barriers.

**The copy protection is executed, not bypassed.** The game hides integrity checks behind hardware exceptions,
single-step traps and code it decrypts into the heap. A small x86 machine built on Unicorn
([x86_guest_vm.cpp](runtime/win32/x86_guest_vm.cpp), [heap_code.cpp](runtime/win32/heap_code.cpp)) runs exactly those
sequences and hands control back to the recompiled code. No check result is forced and no key is placed by hand.

**A hole in the translation is built to cost speed, not a crash.** When the game reaches x86 code that was never lifted, the
same machine runs its original bytes ([interp.cpp](runtime/win32/interp.cpp)) and gives control back to the
recompiled code at the next translated block, call or return. Each interpreted address is logged (`[INTERP]`).

**The online service is answered locally.** Zombies and multiplayer expect Demonware; a responder written from the
game's own requests ([dw_local.cpp](runtime/win32/dw_local.cpp)) answers the lobby, storage and ticket on your Mac.
Nothing leaves the machine.

<details>
<summary>Why not Wine, CrossOver, DXVK or DXMT?</summary>

They translate Windows calls at run time and keep the x86 code under Rosetta. Here the x86 code itself is gone: it is
ARM64 machine code before the game starts. DXVK and DXMT are PE DLLs that reach their native backend through Wine's
unixlib mechanism; this port has no Wine, and its Direct3D 11 is its own layer over 32-bit guest memory. Only shader
translation was needed from outside, and HLSLcc, a plain C++ library, covers it.
</details>

<img src="docs/assets/rule-04.svg" alt="" width="100%">

## Findings

Each entry is a symptom seen on screen, the cause that was measured, and the effect of the fix.

<details>
<summary><b>Thirteen findings</b>, from the x87 rounding mode to a false start inside one instruction</summary>


**F1 · The lifter ignored the x87 rounding mode.** In Zombies the prone view shook. `fistp` rounded to nearest where
the game had asked for truncation, inside the network codec of the view height. The height received by the client was
wrong on 8 samples out of 8, and on 0 out of 8 after a Remill patch and a re-lift of the codec.

**F2 · One reserved bit of the x87 control word.** A campaign mission froze in an effects loop, only with real
threads. The lifter stored bit 6 of the control word differently from real hardware. A one-line Remill patch and four
re-lifted runtime functions fixed it, with no slowdown.

**F3 · A CPU feature flag changes the algorithm.** Our `IsProcessorFeaturePresent` answered "no SSE2", so the C
runtime took its x87 `ceil`, which rounded to nearest under our runtime. An effect then sampled index −1 forever: two
missions froze.

**F4 · Our own log caused the stutter.** Hitches of 170–430 ms were the render thread blocked in `write()`: our log,
redirected to a file on a nearly full disk. Sending it through a pipe to a child process took the hitches per run from
12 and 8 to 0 and 0.

**F5 · Shader translation moved constants by 4 bytes.** HLSLcc placed a `float2` after a `float` at offset 24 where
Direct3D reads it at 20. 31 of 2,664 shaders were affected, among them the eyes and the reticles.

**F6 · The protection has to run.** The options menu froze 8 times out of 8. An integrity check rewrites its own return
address to point at code decrypted into the heap, which the runtime ignored. Running that code, as Windows does, gave
3 out of 3 without a freeze.

**F7 · Our own safety guards were the obstacle.** One guard of ours stopped the game at 28 seconds of play. The largest
gains of the project came from removing our own guards and false sharing, not from clever code.

**F8 · An empty API stub looks like a broken service.** Two stubs that returned nothing (`CryptGenRandom`,
`StringFromGUID2`) looked like a network failure in Zombies.

**F9 · Thread priorities are part of the program.** `SetThreadPriority` was a stub. Passing the mixer's request to
macOS cut the short audio buffers of a run from 908–1872 to 7–71, and the sound gaps with them.

**F10 · Native skinning, bit for bit.** CPU skinning rewritten in NEON gives the same bits as the original x86 and took
a heavy scene from 17.8 to 30.3 fps.

**F11 · Decals need the game's depth bias.** Scorch marks and blood appeared or vanished with a slight turn of the
camera. The game asks for a depth bias on decals; our layer left it at zero. Passing the game's values to Metal fixed it.

**F12 · The render thread was copying the level's geometry every frame.** In multiplayer and Zombies it copied 23–63 MB
of vertex data to the GPU per frame, most of it static. Keeping static buffers resident in Metal brought the copies to
3.5–5 MB per frame: multiplayer 51.8 → 60 fps, Zombies 43.1 → 58.2. The same change does nothing in the campaign,
where the pace is set by the scheduler thread. A speed-up helps only the mode whose bottleneck it touches.

**F13 · A false start inside one instruction cost half the frame rate in combat.** In the first conversions from a
fresh Steam copy, Zombies fell to 25 fps whenever the player fired. Hot functions were running in the interpreter: the
catalogue placed the start of the next function inside the first instruction of a root, so its bounded sweep
decoded zero bytes and the root was dropped without a word. Sweeping such roots without the bound, as the one-by-one
lifter already did, put them back in the translated code.

</details>

<img src="docs/assets/rule-05.svg" alt="" width="100%">

## Method

**Predict, then measure.**

- A prediction is written before the result is read: about 1,100 numbered ones by the time of this beta.
- At least three runs per configuration. Run-to-run noise is 30–64%, so a difference under about 40% cannot be seen at n = 3.
- A change must prove it is active: the log has to show it firing.
- A run that never reaches the level is a dead run, not a data point.
- The screen beats the log. When the screen contradicts a probe, the probe is wrong.

<details>
<summary><b>Record and replay, retractions, clean room, who does what</b></summary>

**Deterministic record and replay.** With every guest thread a coroutine on one host thread, most sources of
non-determinism are gone; the replay is checked against the recording, not assumed. Three boundaries capture all outside input: the import dispatcher, the window message drain, and the
consumed audio buffers. The journal is about 16 MB. It allows the same run twice with exactly one flag changed.

**The instrument is the thing that fails.** Nearly 200 findings were taken back along the way. The largest share
(31%) were an instrument that answered a different question; the next (17%), one that could not answer at all. So
every oracle here names its input and refuses to answer when that input is missing.

**Clean room for the match rules.** Multiplayer and Zombies take their match rules from a daily patch file that Steam
no longer ships. The replacement in [runtime/match_rules/](runtime/match_rules/) was written by a team that never saw the
original script: one team wrote a behavioural specification from the game, the other wrote the code from that
specification alone, and the two only spoke through written questions and answers, kept with their dates.

**Who does what.** AI agents supply the throughput: writing the shims, bisecting millions of calls per run, reading tens
of thousands of lines. The human supplies the goal, the corrections from what is really on screen, and refuses the
flattering conclusion. This is one game, months of work and heavy manual intervention.

</details>

<img src="docs/assets/rule-07.svg" alt="" width="100%">

## Limits

- **Local only.** Multiplayer and Zombies run against the responder on your Mac. There is no online play, no LAN and
  no co-op yet. League Play is refused.
- **Match rules are ours.** Written in a clean room (see [Method](#method)); behaviour may differ from the original in
  places.
- **Heat.** The Mac runs very hot during long sessions (111–116 °C reported on the M1). Play plugged in: on battery,
  macOS throttles the game hard. Plugging in avoids the slowdown, not the heat.
- **First game on each map.** In Zombies and multiplayer, shaders are translated for Metal while the map loads, so
  that first load is longer and the first minute can still hitch slightly. In the campaign they are translated when
  they first appear, so the loading cinematic stays smooth and the first pass through a place can hitch. Everything
  is cached: the next time, it loads fast and runs smooth.
- **Window.** The game runs in a 1280×720 window.
- **Other Macs.** Only one M1 has been measured. M2, M3 and M4 should be faster, but no number is claimed.
- **Campaign.** Every mission loads and chains; not every one has been played end to end.
- **Zombies.** Grief and Turned need a second player, and there is no co-op yet.
- **macOS audio.** When the system's `coreaudiod` is unhealthy the campaign stalls on its first video. Check that
  `afplay -t 1 /System/Library/Sounds/Tink.aiff` takes about 1 s; restarting the Mac fixes it.

<img src="docs/assets/rule-08.svg" alt="" width="100%">

## Install

You need an Apple silicon Mac, macOS 14 or later, at least **20 GB free** (the conversion peaks at about 10 GB and about
4 GB stays), Homebrew, and Steam for Mac signed in to the account that owns the game.

```bash
git clone https://github.com/adrbn/bo2-mac.git && cd bo2-mac
brew install llvm@17 cmake ninja libvpx unicorn
pip3 install capstone
scripts/make_app.sh --from-source
```

Open `Black Ops II.app`, point it at your game folder and let it convert: about 2.5 hours on an M1. The campaign is
playable as soon as it is converted: play it then, and Zombies and multiplayer are converted when you first pick them
in the game's menu. Otherwise the conversion goes on through all three. If the disk falls under
8 GB free, the conversion stops cleanly and resumes where it was.

**Updating, or if the conversion stops.** Run `git pull && scripts/make_app.sh --from-source` in the repository
folder: what is already converted is kept and the conversion resumes. If it still fails, open an issue with the log
it names.

The converter only accepts the exact Steam build, checked by SHA-256. The Steam executables are encrypted by Valve's
SteamStub wrapper and **this repository provides no way to remove it**: the converter expects the three decrypted
images in `~/.bo2-native/steam/` (`t6sp.exe.unpacked.exe`, `t6mp.exe.unpacked.exe`, `t6zm.exe.unpacked.exe`). You make
them yourself from the copy you bought, with a separate tool of your choice. Only the wrapper must go: the file names
above and the SHA-256 check tell you at once whether a file is right. Never share these files.

**DLC.** The launcher asks your Steam client which DLC your account owns, exactly as the game does on Windows. A DLC map
plays when you own it and its files are in your game folder.

<img src="docs/assets/rule-09.svg" alt="" width="100%">

## Repository

| path | what it holds |
|---|---|
| [runtime/win32/](runtime/win32/) | the Win32 runtime: kernel32, the scheduler, Direct3D 11, sound, input, the local service responder |
| [runtime/gfx/](runtime/gfx/) | the Metal backend: presentation, shader cache, render targets |
| [runtime/match_rules/](runtime/match_rules/) | the clean-room match rules for multiplayer and Zombies |
| [tools/](tools/) | the lift driver, the catalogue, the IR passes, the disassembly and analysis tools |
| [tools/remill-patches/](tools/remill-patches/) | the patches carried on top of Remill v6.0.1 |
| [scripts/](scripts/) | `make_app.sh`, the Steam installer and converter, the launchers |
| [third_party/HLSLcc](third_party/HLSLcc) | DXBC to Metal shader translation |
| [docs/](docs/) | the prerelease status, the toolchain |

<img src="docs/assets/rule-10.svg" alt="" width="100%">

## Roadmap

1. **This beta**: the three modes from a fresh Steam copy, on other Macs than the one that built them. Your reports
   (Mac model, frame rate, what broke) decide what comes next.
   First outside report, 2026-11-05: on an M2 Pro (16 GB), the conversion completed and the game started, after three
   fixes found on the way ([#4](https://github.com/adrbn/bo2-mac/issues/4), [#5](https://github.com/adrbn/bo2-mac/issues/5), [#6](https://github.com/adrbn/bo2-mac/issues/6)).
2. **No Terminal**: one app to download and open, then drop the game folder on its window. It carries its tools
   already built, so nothing is compiled on your Mac but the game itself (macOS 26 or later; macOS 14 and 15 keep the
   Terminal install).
3. **Fully translated**: the 46 code blocks that still run through the Unicorn interpreter are translated ahead of
   time, and every translated instruction is checked against the interpreter;
   any translated function can be sent back to the interpreter, to find a wrong one by halving.
4. **No shader wait**: shaders built before the first match, instead of at launch (about 3 minutes in multiplayer) and
   during play (freezes when a new effect appears). Started: shaders now compile in batches, and the ones from the
   last sessions compile in the background at launch. In multiplayer on an M1 without Xcode (a few tries, not a
   benchmark), shader compiling during the first two minutes of a match went from 18–25 s to 10–14 s on a first
   launch and about 5 s on the next one; freezes over 300 ms went from 15–22 to 13–15, then 7. The first launch
   still freezes about as often.
5. **Shorter conversion** (about 2.5 hours on an M1 today): the code the three modes share converted once instead of
   three times, an update converting only what changed, and less optimisation for code that rarely runs.
6. **Faster, without heat**: the campaign is bound by the scheduler thread, not by rendering; multiplayer with 11 bots
   is bound by the server thread. Native rewrites of the hottest functions come next, then frame rates above 60 for
   120 Hz screens.
7. **Controllers**: vibration and aim assist.
8. **Together**: real UDP sockets for Zombies co-op, Grief and Turned, and multiplayer over LAN.
9. **Later**: MetalFX, post-processing when the frame is presented, and replacement textures loaded from the player's own copy.

<img src="docs/assets/rule-11.svg" alt="" width="100%">

## Legal

For a copy the user owns, on hardware the user owns.

- No engine source and no leaked code are used.
- Neither the input binary nor the recompiled output is distributed. No game file is included: the player needs their
  own Steam copy of Call of Duty: Black Ops II. The converter only accepts the exact Steam build (SHA-256), and
  ownership is checked each time the game starts by the player's own Steam client: the game refuses to run when Steam
  is not signed in or the account does not own it.
- The copy protection is executed as the game wrote it; nothing in it is patched out. This repository provides no way
  to remove Valve's SteamStub wrapper.
- The online service the game expects is answered by a local responder written from the game's own requests: no
  Activision server is contacted, there is no online play, and game ownership is still checked by the player's Steam.
  The match rules were written in a clean room.
- The code written here is licensed under GPL-3.0 ([LICENSE](LICENSE)).
- Unicorn (GPL-2.0) is linked statically from Homebrew when you build ([tools/boot.py](tools/boot.py)). GPL-2.0 and
  GPL-3.0 code cannot be shipped together in one binary, so the prebuilt tools of roadmap step 2 cannot embed Unicorn
  as it is linked today. Building from source on your own Mac is not affected.

This is an unofficial fan project. Call of Duty and Black Ops are trademarks of Activision Publishing, Inc. Steam is a
trademark of Valve Corporation. This project is not affiliated with, endorsed by or sponsored by Activision, Treyarch,
Microsoft or Valve. To play, buy the game.

<div align="center"><img src="docs/assets/footer.svg" alt="" width="100%"></div>
