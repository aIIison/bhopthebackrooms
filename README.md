# Bhop the Backrooms

GoldSrc movement for Escape the Backrooms, implemented as a native UE4SS mod.

The movement math follows Valve's [`pm_shared.c`](https://github.com/ValveSoftware/halflife/blob/master/pm_shared/pm_shared.c) ground acceleration and
friction, air acceleration and strafing, jump behavior, the CS 1.6
mega-bunny cap, ladders, and water movement. Unreal still handles collision,
stairs, moving platforms, prediction, replication, and server correction.

## Features

- GoldSrc ground and air movement
- Hold-to-bhop with the CS 1.6 speed cap
- Air crouching and duck rolling
- Interaction while jumping or crouching
- GoldSrc ladder movement, ladder strafing, and jump-off velocity
- GoldSrc water movement, including swimming, sinking, and sharking
- Frame-rate-independent mouse input based on ETB's 120 FPS sensitivity
- Native fallback for scripted movement and level-transition ladders
- Deterministic physics configuration checksum for multiplayer

## Requirements

- Escape the Backrooms Steam build `23657885`
- Unreal Engine 4.27 game build
- [ETBCommunity UE4SS](https://github.com/ETBCommunity/UE4SS) `1.3.0`
- The same mod version and physics configuration on the host and every client

Unsupported executable builds fail closed instead of installing an uncertain
hook.

## Installation

Copy [`package/bhop`](package/bhop) into:

```text
EscapeTheBackrooms/EscapeTheBackrooms/Binaries/Win64/ue4ss/Mods/
```

The resulting installation should contain:

```text
Mods/
└── bhop/
    ├── bhop.ini
    ├── enabled.txt
    └── dlls/
        └── main.dll
```

Restart the game after replacing `main.dll`.

## Multiplayer

Every participating PC must install the mod. Run `bhop.status` in the UE4SS
console and compare checksums before joining. Different physics settings make
client prediction disagree with the server and cause corrections or
rubber-banding.

Vanilla clients are not supported.

## Configuration

Settings live in `bhop/bhop.ini`. Movevar values use GoldSrc units and are
converted to Unreal centimeters at runtime.

The default configuration provides:

- `AutoBhop=true`
- `BunnyhopSpeedCap=true`
- `DuckRoll=true`
- `RawMouseInput=true`
- Canonical GoldSrc gravity, friction, acceleration, air acceleration, and
  speed values

`RawMouseInput` disables Unreal's legacy smoothing and removes ETB's
frame-time scaling from mouse input. Controller look input is unchanged.

## Console commands

| Command         | Action                                          |
| --------------- | ----------------------------------------------- |
| `bhop.toggle`   | Enable or disable custom movement               |
| `bhop.autobhop` | Toggle hold-to-bhop                             |
| `bhop.speedcap` | Toggle the CS 1.6 mega-bunny cap                |
| `bhop.duckroll` | Toggle duck rolling                             |
| `bhop.rawmouse` | Toggle corrected mouse input                    |
| `bhop.reload`   | Reload `bhop.ini`                               |
| `bhop.status`   | Show active settings, checksum, and hook status |

## Movement notes

Ordinary pool ladders use continuous GoldSrc movement without ETB's alignment
timeline. Cooked transition ladders remain native so
their scripted level changes still run. Scripted climbing, balancing, pushing,
ropes, death, and pass-out states also remain native.

Water movement uses a persistent collision-preserving state around Unreal's
water volumes. This prevents native waterline snapping while retaining
Unreal's collision, prediction, and replication.

Duck rolling reproduces GoldSrc's swept 18-unit origin pop: release crouch
within the configured window while grounded. It changes the hull origin
without adding jump velocity.

## Building

Install Visual Studio 2022 with the C++ desktop workload and CMake 3.22 or
newer. Clone the ETBCommunity UE4SS fork at its pinned `1.3.0` commit with
submodules initialized.

The fork currently references a private `ETBCommunity/UEPseudo` submodule.
Override it with the public upstream before initializing submodules:

```powershell
git clone https://github.com/ETBCommunity/UE4SS.git C:\path\to\UE4SS
git -C C:\path\to\UE4SS checkout 0a3cf5ebaf28c9385523b4a651542be6746ed1e9
git -C C:\path\to\UE4SS config submodule.deps/first/Unreal.url `
  https://github.com/Re-UE4SS/UEPseudo.git
git -C C:\path\to\UE4SS submodule update --init --recursive
```

Do not run `git submodule sync` afterward; it restores the inaccessible URL
from the fork's `.gitmodules`.

```powershell
cmake -S . -B build-native -G "Visual Studio 17 2022" `
  -DBHOP_BUILD_UE4SS_MOD=ON `
  -DUE4SS_ROOT=C:\path\to\UE4SS

cmake --build build-native --config Game__Shipping__Win64 `
  --target etb_bhop bhop_tests

ctest --test-dir build-native `
  -C Game__Shipping__Win64 `
  --output-on-failure
```

The build stages the distributable DLL at
`package/bhop/dlls/main.dll`.

## Project layout

```text
src/                Movement core and UE4SS integration
src/util/           Process validation and x64 hook-scanning utilities
tests/              Deterministic movement vectors
package/bhop/       Ready-to-install mod package
```
