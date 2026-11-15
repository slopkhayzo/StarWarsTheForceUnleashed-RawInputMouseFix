Star Wars: The Force Unleashed Raw Mouse Fix v@VERSION@
========================================================

Supported game builds
---------------------
Star Wars: The Force Unleashed - Ultimate Sith Edition for Windows x86, Steam
(SWTFU.exe, PE timestamp 0x4B75EA93, Steam build 6522), SHA-256:
1C156705548FCE9E33FA3423E75B5CBE36248319B96F4B4850BF2E8125692032

The Steam executable is protected by Steam's DRM (SteamStub), which decrypts
the game code when the game starts; the fix waits for that.

This hash identifies the reference build; it is not a hard allowlist. Before
writing a hook, the ASI checks the image layout and every code signature it
depends on. Any mismatch leaves the game unchanged and is written to
swtfu-raw-mouse-fix.log. Other releases (GOG, retail DVD) have not been tested;
if their code differs, the fix stays inactive and the log says why.

Choose an installation
----------------------
Both archives use the game folder's "scripts" subfolder for the plugin. The
game folder is steamapps\common\Star Wars The Force Unleashed (the folder with
SWTFU.exe). Close the game before copying files, then start it normally
through Steam.

Convenience archive with Ultimate ASI Loader:
  If the game has no ASI loader, copy everything in the archive into the game
  folder: winmm.dll, winmm.ini and the scripts folder (with
  SWTFURawMouseFix.asi and swtfu-raw-mouse-fix.ini). The bundled winmm.dll is
  Ultimate ASI Loader x86 v9.7.4; winmm.ini tells it to load plugins from the
  scripts folder only.

  If the game folder already has a winmm.dll, do not overwrite it: use the
  plugin-only archive instead.

Plugin-only archive:
  Requires a compatible 32-bit (x86) ASI loader already installed (for
  example Ultimate ASI Loader as dinput8.dll, which also loads the scripts
  folder). Copy the scripts folder into the game folder. Keep
  SWTFURawMouseFix.asi and swtfu-raw-mouse-fix.ini together.

With the SWTFU Advanced Configurator:
  Use the convenience archive as described above, next to the Configurator.
  Never replace the Configurator's dinput8.dll: it is its own loader, and it
  does not keep other plugins loaded, so the fix brings its own loader as
  winmm.dll. Tested with the Advanced Configurator v0.8 (1.0.21), including
  its frame-rate limits (60 and 120 FPS): the camera turns the same per mouse
  movement at any frame rate.

What it fixes
-------------
In keyboard-and-mouse mode the game turns the mouse into a virtual gamepad
stick. Every frame, the mouse movement is sorted into one of six speed bands,
and the camera then eases its turn speed toward that band's speed (about 10 %
of the way per frame when speeding up, 50 % when slowing down) through a
response curve. The result is mouse acceleration and lag: how far the camera
turns depends on how long and how fast you move the mouse, not on how far;
the start of every movement is slow, flicks are smeared, and everything
changes with the frame rate.

With the fix, the gameplay camera turns by the raw mouse counts: the same
movement always turns the camera by the same amount, at any speed and any
frame rate, with no lag. Everything else is the game's own code: the pitch
limits, camera collision, scripted cameras and cutscenes. The game's invert
settings still apply.

Usage
-----
Install and play. Settings are in scripts\swtfu-raw-mouse-fix.ini:

[Sensitivity]
  SensitivityX, SensitivityY: camera turn per mouse count, in thousandths of
    a degree, horizontal and vertical. Higher turns faster. Default 50.
  SensitivityMultiplier: scales both. Default 1.00.

  SensitivityX=50 is 0.05 degrees per count, or 7200 counts for a full turn:
    400 DPI: 45.7 cm    800 DPI: 22.9 cm    1600 DPI: 11.4 cm    3200 DPI: 5.7 cm
  The game's own mouse sensitivity slider no longer changes the camera while
  the fix is on.

[Fix]
  DirectCamera=0 brings back the game's own mouse handling.
  Hotkeys=1 (default) enables, in game:
    F8        switch between the fix and the game's own mouse handling
    F9 / F10  lower / raise SensitivityMultiplier by 10 % until the game
              closes (set it in the ini to keep it)
    F6        reload the ini
  Diagnostics=1 writes input statistics to the log every five seconds.

Safety and scope
----------------
Only the mouse turning the gameplay camera in keyboard-and-mouse mode is
changed. The gamepad, keyboard camera keys, menus and the cursor use the game's
own code. Force Grip (moving a held object) and lock-on target switching also
keep the game's own mouse handling: there the game treats any mouse movement
as a full stick push in that direction, by design. If something feels wrong,
F8 switches back to the game's own handling for a comparison; please report
where it happened.

The fix changes the running game only. It never modifies the game executable
or its data on disk.

Diagnostics
-----------
The fix writes swtfu-raw-mouse-fix.log beside the ASI, in the scripts folder
(or, if that folder is not writable, in %LOCALAPPDATA%\SWTFURawMouseFix).
Check it if the fix does not activate: executable-layout and signature
failures are reported there. Diagnostics=1 in the ini adds input statistics
every five seconds.

Uninstallation
--------------
Close the game and remove scripts\SWTFURawMouseFix.asi and
scripts\swtfu-raw-mouse-fix.ini. The scripts\swtfu-raw-mouse-fix.log file may
also be removed, and the scripts folder if it is then empty.

Remove winmm.dll and winmm.ini only when you installed them from this fix's
convenience archive and no other ASI plugin needs them.

Third-party component
---------------------
The explicitly named convenience archive includes Ultimate ASI Loader v9.7.4
(x86) by ThirteenAG under the MIT License, as winmm.dll. See
THIRD-PARTY-NOTICES.txt. The plugin-only archive contains no third-party loader
binary.

Misc
----
For more game fixes, visit:
https://slop-blog.enkhayzomachines.net/fixes :)
