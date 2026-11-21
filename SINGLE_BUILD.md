# ROB RUN AIO - Single Executable Build Guide

## Overview

This guide shows how to build the complete ROB RUN AIO as a **single executable** that combines:

1. **Mapper** — Reads and maps the driver's resident image
2. **Overlay** — Handles snapshotting, rendering, and UI  
3. **Integration** — Bridges mapper and overlay via SmxRing driver

## File Structure

```
RRTool_Public/
├── 2_src_port/
│   ├── build_port.bat (updated)
│   ├── rr_single_aio.c (NEW - unified entry)
│   └── rr_esp.c, rr_game.c, rr_mem.c, ... (existing)
├── 3_src_driver/
│   └── build_driver.bat (existing)
├── 4_src_mapper/
│   └── kd2_src/ (existing)
└── SINGLE_BUILD.md (this file)
```

## Build Process

### Step 1: Build the Driver

```bash
cd 3_src_driver
build_driver.bat
# Output: x64/Release/driver.sys
```

### Step 2: Build the Unified Overlay

```bash
cd 2_src_port
build_port.bat
# Output: RobRunPort.exe
```

**What `build_port.bat` now compiles:**

1. `rr_main.c` (or `rr_single_aio.c`) — Unified entry point
2. `rr_esp.c` — Overlay entry and lifecycle
3. `rr_game.c` — Snapshot and roster management
4. `rr_mem.c` — SmxRing transport (mapper bridge)
5. `rr_unity.c` — Unity metadata
6. `rr_chain.c` — Chain/instance management
7. `rr_draw.c` — Rendering
8. `rr_aim.c` — Aim calculations
9. `rr_los_*.c` — Line-of-sight
10. `rr_spawn.c`, `rr_nodescan.c`, `rr_classscan.c`, `rr_find.c` — Game object finding
11. `gpu.cpp`, `menu_webview.cpp` — GPU/D2D rendering and WebView2 menu
12. `rr_aim.c` — Aim calculations
13. `rr_los_*.c` — Line-of-sight
14. `app.rc` — Resources

### Step 3: Build the Mapper (Optional - already integrated)

The mapper functionality is **already integrated** in `rr_mem.c` via IOCTL_BASE operations. No separate build needed for the unified version.

## Final Artifacts

| Component | File | Size | Purpose |
|-----------|------|------|---------|
| Unified Executable | `RobRunPort.exe` | ~2-3 MB | Mapper + Overlay combined |
| Kernel Driver | `driver.sys` | ~150 KB | SmxRing transport |
| WebView2 Loader | `WebView2Loader.dll` | ~1 MB | WebView2 runtime component |
| WebView2 Runtime | (separate) | ~100 MB | WebView2 runtime (Windows Update) |

## Deployment

1. Copy `RobRunPort.exe` and `driver.sys` to the target machine
2. Run `RobRunPort.exe`
3. The mapper reads `driver.sys` via SmxRing
4. The overlay handles game interaction

## How It Works

```
┌─────────────────┐
│  ROB RUN.exe    │
└────────┬────────┘
         │
         ▼
┌─────────────────────────────────────┐
│ SmxRing (kernel driver)             │
│ ───────────────────────────────     │
│ • Memory read/write                 │
│ • PEB/module access                 │
│ • Process hiding                    │
└──────────────┬──────────────────────┘
                │
        ┌───────┴──────┐
        ▼             ▼
┌──────────────┐ ┌─────────────────┐
│ Mapper       │ │ Overlay        │
│ (in exe)     │ │ (in exe)       │
│ • Reads      │ │ • Snapshot     │
│   driver     │ │ • Render       │
│ • Maps       │ │ • UI           │
│   sections   │ │ • Hotkeys      │
└──────────────┘ └─────────────────┘
```

## Key Integration Points

### 1. Mapper in `rr_mem.c`

The mapper functionality is in the `rr_mem.c` file. It uses IOCTL_BASE to read the driver's resident image and map it into memory. This is already integrated — no changes needed.

### 2. Overlay in `rr_esp.c`

The overlay handles the main game loop, snapshotting, and rendering. It uses the mapper to get game data via the driver.

### 3. Unified Entry in `rr_single_aio.c`

The new `rr_single_aio.c` (or `rr_main.c`) ties everything together:

- Starts system checks
- Loads the driver
- Waits for ROB RUN.exe
- Initializes mapper
- Initializes overlay
- Runs the unified loop

## Command Line Arguments

| Argument | Short | Description |
|----------|-------|-------------|
| `--no-ui` | `-n` | Run without UI (mapper only) |
| `--auto` | `-a` | Auto-start after game detection |
| `--help` | `-h` | Show help |

## Example Usage

```bash
# Full AIO with UI
RobRunPort.exe

# Mapper only (no UI)
RobRunPort.exe --no-ui

# Auto-start after game detected
RobRunPort.exe --auto

# Help
RobRunPort.exe --help
```

## Troubleshooting

### "Mapper cannot initialize"
- Ensure `driver.sys` is loaded (check device \Device\SmxRing*)
- Verify SmxRing ring token is registered
- Check for duplicate driver instances

### "Driver not loaded"
- Ensure `build_driver.bat` was run
- Check antivirus/firewall isn't blocking device access
- Verify SmxRing0 through SmxRing7 device names

### "Game detection failed"
- Ensure ROB RUN.exe is running
- Check process name matching (case-insensitive)
- Verify process is not in a suspended state

## Performance Notes

The unified executable runs efficiently because:

1. **Single process** — No inter-process communication overhead
2. **Integrated mapper** — Reads driver once, maps sections as needed
3. **Shared memory** — Mapper and overlay share the same address space
4. **No redundant copies** — One copy of each module

## Future Enhancements

Potential improvements for the unified build:

1. **Dynamic module loading** — Load mapper/overlay modules on demand
2. **Hot-code swapping** — Update code without restarting
3. **Plugin architecture** — Allow third-party modules
4. **Distributed mapping** — Multiple mapper instances for different drivers

## Conclusion

This unified build approach provides a clean, efficient AIO that combines mapper and overlay functionality into a single executable. The driver remains as a separate .sys file (required for kernel-mode operations), while the mapper and overlay work together seamlessly in one process.

The build process is simple:
1. Run `build_driver.bat` in 3_src_driver
2. Run `build_port.bat` in 2_src_port  
3. Deploy both files to target

Ready to go! 🚀
