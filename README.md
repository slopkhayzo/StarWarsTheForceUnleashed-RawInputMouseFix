# Star Wars: The Force Unleashed Raw Mouse Fix

> [!WARNING]
> This patch has been pretty much entirely been generated using AI;
> I won't and will never claim to have enough knowledge or expertise to do this
> kind of reverse-engineering by myself.
> this patch currently probably only works for the Steam version of the game (Ultimate Sith Edition).

An ASI plugin for the 32-bit Steam version of *Star Wars: The Force Unleashed -
Ultimate Sith Edition* that removes the mouse acceleration from the gameplay
camera and makes it independent of the frame rate. It works with and without
the SWTFU Advanced Configurator.

The patch changes only the running process; it never modifies the game
executable or game data on disk.

## What it fixes

In keyboard-and-mouse mode the game turns the mouse into a virtual gamepad
stick. Every frame the mouse movement is sorted into one of six speed bands
(0.25 to 2.0 times the sensitivity slider), and the camera eases its turn speed
toward that band's speed (10 % of the way per frame when speeding up, 50 % when
slowing down) through an exponent-7 response curve. So how far the camera
turns depends on how long and how fast the mouse moves, not on how far; every
movement starts slowly, flicks are smeared, and all of it changes with the
frame rate.

With the fix, the camera turns by the raw mouse counts after the game's own
turn: degrees = counts × sensitivity, the same at any speed and frame rate.
The speed bands see only the keyboard's part of the stick, and the game's
camera collision prediction gets the matching turn speed. Pitch limits,
camera collision, scripted cameras and cutscenes stay the game's own. Force
Grip and lock-on target switching keep the game's mouse handling (a direction,
by design).

## Install

Both archives put the plugin in the game folder's `scripts` subfolder:

| Archive | Contents |
| --- | --- |
| `…-with-Ultimate-ASI-Loader-v9.7.4.zip` | `winmm.dll` (Ultimate ASI Loader x86 v9.7.4), `winmm.ini` (`loadfromscriptsonly=1`), `scripts\SWTFURawMouseFix.asi`, `scripts\swtfu-raw-mouse-fix.ini` |
| `…-plugin-only.zip` | `scripts\SWTFURawMouseFix.asi`, `scripts\swtfu-raw-mouse-fix.ini` (needs an ASI loader already installed) |

The loader is `winmm.dll`, not `dinput8.dll`, so it can sit next to the SWTFU
Advanced Configurator, which ships its own `dinput8.dll` loader that does not
keep other plugins loaded. `README.txt` in the archives has the details,
settings and hotkeys.

## Compatibility

| Build | SWTFU.exe SHA-256 |
| --- | --- |
| Steam, build 6522 (PE timestamp `0x4B75EA93`, SteamStub 2.1) | `1C156705548FCE9E33FA3423E75B5CBE36248319B96F4B4850BF2E8125692032` |

The Steam executable's code is encrypted on disk and decrypted when the game
starts; the plugin waits for that before it checks anything. Before hooking it
checks the image base and size and every code signature it depends on
(`swtfu_layout.h`). Any mismatch fails closed: the game runs unmodified and the
reason is written to `swtfu-raw-mouse-fix.log`. A hook site that another
plugin has already patched is reported as a foreign patch, and one patched
after us is reported while the game runs.

Tested with the SWTFU Advanced Configurator v0.8 (1.0.21), at its 60 and
120 FPS limits.

## Build

Requirements: Windows, Visual Studio (or Build Tools) with the C++ x86 tools.

```bat
build-msvc.cmd
```

It builds `build\SWTFURawMouseFix.asi` (x86, static runtime) and runs the
tests: `counts-test`; the ASI must stay inactive in a foreign host and in a
renamed host with the wrong layout, and must survive being unloaded right
after loading (20 times). `tools\build_queued.ps1` also sets the paths of the
real executables for `layout-profile-test`: the Steam exe must be rejected
while encrypted, and a Steamless copy (analysis only, never shipped) must be
accepted. `build-msvc.cmd dev` adds the development tools (a
hardware-breakpoint trace of the code that reads the mouse, `[Trace]` in the
ini).

## Package

```powershell
.\package-release.ps1 -Version <x.y.z> -LoaderPath <x86 Ultimate ASI Loader v9.7.4 dinput8.dll>
```

It writes the plugin-only archive and the archive with Ultimate ASI Loader
(renamed to `winmm.dll`) to `dist\`, each with a `SHA256SUMS.txt` inside and a
`.sha256` file beside it. It refuses to overwrite a release, to package a
non-x86 or development build, a plugin whose version string differs, or while
`release-readme.txt` has TODO sections.

## Misc

For more game fixes, visit: https://slop-blog.enkhayzomachines.net/fixes :)
