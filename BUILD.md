# Building and validating RRTool

All paths below are relative to `RRTool_Public` unless a command first changes
directories. Build and inspect the user-mode overlay before studying privileged
components. The included binaries are not evidence that a new source build passed.

## Prerequisites

| Component | Required tools or inputs |
|---|---|
| Overlay | Windows x64, MSVC C/C++ tools and Windows SDK |
| Menu at runtime | Microsoft Edge WebView2 Runtime |
| WebView2 at build time | Bundled SDK in `2_src_port/WebView2SDK` |
| Driver | MSVC x64 plus Windows Driver Kit headers and matching libraries |
| Mapper | MSBuild, the v143 C++ toolset and Windows SDK |
| Offset tools | Python 3 and external dump inputs described below |

The overlay script searches VS 18 and VS 2022 Community/Professional in their
standard installation paths and calls `vcvars64.bat`. It does not search every
edition or custom path; adjust its detection list for another installation.
The driver script has its own detection list and selects an installed MSVC
toolset. It prefers WDK/SDK `10.0.26100.0`, with a fallback include-directory
search. Confirm that the selected headers and x64 libraries belong together.

## Build the overlay

From the project root in PowerShell:

```powershell
Push-Location .\2_src_port
cmd /c build_port.bat
Pop-Location
```

The output is `2_src_port/RobRunPort.exe`, copied to `1_run/RobRunPort.exe`. The
entry point is `WinMain` in `rr_esp.c`; `rr_main.c` belonged to an earlier
all-in-one attempt and is not part of this build. The script compiles the C
reader, drawing, aim, geometry, scan and process helpers, plus the C++ GPU and
WebView2 hosts. `rc.exe` embeds `menu.html` and `app.manifest` into `app.res`.

The build uses `/O2 /W3 /MT` for C and adds `/EHsc` for C++. The C/C++ runtime
is linked statically. The WebView2 loader is linked through
`WebView2Loader.dll.lib`, so **WebView2Loader.dll must remain beside the exe**.
WebView2 Runtime is a separate installation. The embedded menu needs no loose
HTML file at runtime.

Rebuild the launcher afterwards (`build_aio.bat`) so `RobRunAIO.exe` embeds the
new overlay. `1_run` keeps two rollback copies: `RobRunPort.exe.orig` is the
original binary before the menu edit, and `RobRunPort.exe.patched_menu` is that
original code with the current menu.

Two discovery timings were changed in `rr_game.c` so a fresh attach does not
stall: `game_init()` only uses the dictionary path and leaves the heap fallback
to the scan worker, and that fallback runs in bounded 3-second passes from a
rotating region cursor instead of blocking the worker for a whole sweep.

## Check the overlay offline

```powershell
Push-Location .\2_src_port
.\RobRunPort.exe --selftest --nohide --no-pause
.\RobRunPort.exe --menu --nohide
.\RobRunPort.exe --demo --nohide
Pop-Location
```

For a GUI executable, PowerShell may return before the process exits. To wait
for the self-test and inspect its exit code:

```powershell
$check = Start-Process .\2_src_port\RobRunPort.exe `
    -ArgumentList '--selftest','--nohide','--no-pause' `
    -WorkingDirectory .\2_src_port -WindowStyle Hidden -Wait -PassThru
$check.ExitCode
Get-Content .\2_src_port\selftest.log
```

Exit code zero and `SELFTEST OK` mean the built-in checks passed. They cover
configuration round trips, layout constants, projection and chain parsing.
They do not verify the current game, driver, rig or server behavior. Menu/demo
are interactive visual checks; see README for exercises. All modes use the
same manifest and therefore request elevation. No game or driver is needed
for these checks, and `--nohide` skips the startup visibility request.

`--probe`, write tests, memory searches and other diagnostics have different
requirements and side effects. Read their dispatch branches and implementations
in `rr_esp.c` before running them.

## Build the driver

```powershell
Push-Location .\3_src_driver
cmd /c build_driver.bat
Pop-Location
```

The script invokes MSVC with `/kernel` and links against WDK x64 libraries.
It writes `x64/Release/driver.sys`, then copies that file to
`3_src_driver/driver.sys`. It prints deployment commands but does not execute
them. Building a driver does not load it.

Study `WM_PACKET`, the IOCTL definitions and `WmDispatch()` together
with `rr_mem.c`: request packing, field meanings, size caps and reply layout
must agree. METHOD_BUFFERED reuses the system buffer for request and reply,
so the driver copies request fields before overwriting that storage.

The included mapper loads a resident image without a normal unload lifecycle.
Closing the overlay or rebuilding the source does not replace that image.
The unload request removes access through the symbolic link; it does not unmap
the driver. A fresh Windows boot is needed to clear the mapped image.

## Build the mapper

In a Visual Studio Developer PowerShell with the v143 toolset installed:

```powershell
MSBuild .\4_src_mapper\kd2_src\kdmapper.sln `
    /p:Configuration=Release /p:Platform=x64 /m
```

The Release x64 project places `kdmapper.exe` in
`4_src_mapper/kd2_src/build`. The runtime package names that artifact `kd2.exe`.
The solution retains legacy channel blocks and upstream dependencies; those
blocks are not a required overlay transport. Building is separate from mapping.

## Runtime artifact map

| Built or bundled file | Distribution location |
|---|---|
| `2_src_port/RobRunPort.exe` | `1_run/RobRunPort.exe` |
| `2_src_port/WebView2Loader.dll` | `1_run/WebView2Loader.dll` |
| `3_src_driver/driver.sys` | `1_run/driver.sys` and `1_run/kernel-mode.sys` |
| `4_src_mapper/kd2_src/build/kdmapper.exe` | `1_run/kd2.exe` |

Close an executable before replacing its deployed copy. Keep compatible driver
and client builds together. The launch scripts are executable examples to read:
`START.bat` checks files and launches the overlay, while `MAP-DRIVER.bat` performs
mapping. `START-MASQ.bat` copies and renames an executable; it does not copy all
runtime dependencies or verify successful operation. None is needed for offline
source lessons.

## Understand the offset inputs

`robrun_offsets.h` and `robrun_sdk_offsets.h` are already included, so a normal
overlay build needs neither Python nor external game files. Their hashes and
source references identify the snapshots from which they were generated.

The maintenance tools consume inputs that are **not included** in this public
folder. Supply the exact dump and matching binary rather than reusing sample
heap addresses or assuming that a matching field proves runtime compatibility.

### CSV dump to base header

`gen_offsets.py` needs `types.csv`, `fields.csv`, `methods.csv`, `manifest.json`
and the matching `GameAssembly.dll`. The dump manifest's binary hash is checked.

```powershell
python .\tools\gen_offsets.py `
    --root 'C:\Path\To\Game' `
    --dump 'C:\Path\To\Dump' `
    --header .\2_src_port\robrun_offsets.h
```

Always supply `--header`: the script retains a legacy default under the absent
`2_src_cheat` directory. The report goes to `offsets/robrun_offsets.json`.
Review every missing field and static-global candidate. Candidate vote counts
do not identify active instances by themselves.

### CppSDK to supplemental header

`sdk_offsets.py` needs `Models.hpp`, `Battle.hpp`, `Game.hpp`, `Global.hpp`
and the matching `DumpManifest.sha256` in that directory's parent. The manifest
entries use paths such as `CppSDK/Models.hpp`.

```powershell
python .\tools\sdk_offsets.py --sdk 'C:\Path\To\CppSDK'
python .\tools\sdk_offsets.py --sdk 'C:\Path\To\CppSDK' --check
python .\tools\audit_unityentry_offsets.py `
    --sdk 'C:\Path\To\CppSDK' `
    --binary 'C:\Path\To\Game\GameAssembly.dll'
```

The first command rewrites the supplemental header and JSON report. `--check`
compares them without writing. The audit compares managed fields across both
headers, checks source fingerprints, and optionally checks the binary hash.
Review mismatches before rebuilding. Native Unity offsets, generic payloads,
method implementations and live object identities need separate verification.

Rig headers were exported from offline assets by a tool not included here.
`offsets/robrun_rig.json` preserves the palette and weighted/optional bone
provenance. Do not manually reorder palette indices to address a runtime issue.

## Build the all-in-one launcher

`RobRunAIO.exe` is one executable for the whole start-up order: it refuses to
run while VBS, Hyper-V or Memory Integrity is active, extracts its payload, maps
the driver with `kd2.exe --auto`, waits for `ROB RUN.exe` and its main window,
starts the overlay, then supervises it until game data appears. Its manifest
requests elevation.

```powershell
Push-Location .\2_src_port
cmd /c build_aio.bat
Pop-Location
```

The script embeds the current `1_run` binaries (`kd2.exe`, `kernel-mode.sys`,
`RobRunPort.exe`, `WebView2Loader.dll`) as resources, so rebuild those first if
the driver or overlay changed. Output: `2_src_port/RobRunAIO.exe`, copied to
`1_run/RobRunAIO.exe`.

At runtime the payload is unpacked to `payload/` beside the launcher, or to
`%LOCALAPPDATA%\RRAIO\payload` when that folder is read-only. The overlay writes
`CoreRR.cfg` and its logs there; a `CoreRR.cfg` beside the launcher is copied in
once. A driver image matching the rejected 2026-11-07 hash is never mapped.

### Menu wait and reattach

The overlay is not started during the engine's boot. The launcher first waits
for the game's own top-level window, then for a configurable settle delay, so
the overlay starts on the main menu. After that it reads the overlay's
`status.log`: if the game is in a match, visible and not minimized, but no
snapshot with players appears within the timeout, the overlay is terminated and
restarted ("reattached"), with `robrun_runtime.txt` moved aside so discovery
runs a clean scan. The last instance is left running when the attempts run out.

Settings live in `RobRunAIO.ini` beside the launcher (all optional):

| Key | Default | Meaning |
|---|---|---|
| `menu_wait_window` | `1` | Wait for the game window before the delay starts |
| `menu_delay` | `30` | Seconds to wait after the window appears |
| `attach_timeout` | `60` | Seconds the overlay gets to report game data |
| `reattach_attempts` | `3` | Restarts before the launcher gives up |
| `hide_launcher` | `0` | Hide through the driver's process-list unlink (off: PatchGuard checks it) |
| `masquerade` | `1` | Run under a random benign name each launch |

### Masquerade

Process hiding by unlinking `ActiveProcessLinks` is not usable on this Windows
build: PatchGuard cross-checks `PsActiveProcessHead` against `KiProcessListHead`
and `PspCidTable`, so the unlink bugchecks `0x109`. The driver therefore
refuses veil requests by default (`SYSTEM\WmPort\Veil` = 0).

Instead, with `masquerade=1` the launcher copies itself to `%TEMP%` under a
random name from a small pool (`dllhost`, `taskhostw`, `RuntimeBroker`,
`SearchProtocolHost`, `backgroundTaskHost`, `WmiPrvSE`, `smartscreen`,
`ctfmon`, `sihost`), starts that copy with `--masq --ini <original ini>
--payload <payload folder>` and exits. The overlay is likewise copied to
`%TEMP%`, together with `WebView2Loader.dll` and (once) `CoreRR.cfg`, and is
started from there, so its `status.log`, `robrun_runtime.txt` and settings live
in `%TEMP%` as well; the launcher reads supervision data from that folder. The
previous copies are removed on the next launch (`masq.txt` beside the launcher,
`masq_overlay.txt` in the payload folder). The window title follows the chosen
name, and the overlay's window is a tool window, so neither appears in the Apps
list.

`%TEMP%` remains visible as the process path in Task Manager's Details column,
but neither the executable name nor the tool folder is.

### Menu edits

The overlay loads its menu from resource 101 (RCDATA) inside `RobRunPort.exe`,
not from a loose file. Edit `2_src_port/menu.html`, then rebuild:

```powershell
Push-Location .\2_src_port
cmd /c build_port.bat
cmd /c build_aio.bat
Pop-Location
```

`update_menu.ps1` remains available for patching an existing binary without a
rebuild (`.\update_menu.ps1 -Rebuild`): it replaces the resource, verifies the
read-back byte for byte, keeps `RobRunPort.exe.orig` and rebuilds the launcher.

Check the launcher without touching the kernel:

```powershell
$check = Start-Process .\1_run\RobRunAIO.exe -ArgumentList '--selftest' -Wait -PassThru
$check.ExitCode
Get-Content .\1_run\payload\aio.log
```

`--selftest` extracts the payload, hashes the driver image and reports the VBS
state, then exits without mapping a driver or waiting for the game. The manifest
still requests elevation.

| Flag | Effect |
|---|---|
| `--dry-run` | Detect the process and window and wait the menu delay, then exit |
| `--no-map` | Skip driver mapping but keep going (UI diagnostics) |
| `--no-menu-wait` | Start the overlay as soon as the process is found |
| `--menu-delay <s>` | Override `menu_delay` |
| `--attach-timeout <s>` | Override `attach_timeout` |
| `--reattach-attempts <n>` | Override `reattach_attempts` |
| `--payload <dir>` | Use a different payload folder |
| `--overlay <path>` | Start a specific overlay executable (diagnostics) |
| `--ignore-vbs` | Bypass the VBS/HVCI refusal (diagnostics only) |

## Build troubleshooting

| Error | Check |
|---|---|
| `vcvars64.bat` or `cl.exe` missing | C++ workload and script installation paths |
| `rc.exe` or Windows libraries missing | Windows SDK and initialized developer environment |
| `ntddk.h` or kernel libraries missing | WDK installation and selected SDK version |
| MSBuild cannot find v143 | Install that toolset or deliberately retarget the project |
| WebView2 header or loader missing | Bundled SDK path and x64 loader files |
| Python reports missing dump files | Explicit external input paths; dumps are not bundled |
| Hash or field audit fails | Matching source snapshot, binary and generated artifacts |

A successful compilation verifies source and link compatibility with the selected
toolchain. Offline checks verify their stated cases. Neither establishes live-game
compatibility or stability of a privileged operation.
