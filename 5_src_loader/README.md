# FFLoader — public build

Key-free launcher for the public Rob Run release. It does **not** contact a
license server, store credentials, or ask for a key.

## What it does

1. Looks for `RobRunAIO.exe` beside itself, then `payload\RobRunAIO.exe`, then
   `RobRunPort.exe`.
2. Starts the payload elevated with `ShellExecuteEx` (`runas`), because the
   driver mapper and overlay need administrator rights.
3. Shows the result in the status line.

If no payload is found it reports that instead of failing silently.

## Build

```bat
build_loader.bat
```

Output: `FFLoader.exe` in this folder. The manifest requests administrator.

## Layout

```
FFLoader.exe
RobRunAIO.exe        <- payload, beside the loader
```

or keep the payload in a `payload\` subfolder.

## Branding

Uses the shared `brand\` assets and `brand_resources.h`: emblem PNG, Space
Grotesk + Inter, icon. No network dependencies.
