# Bhop the Backrooms!

Native UE4SS mod that ports the GoldSrc movement formulas to Escape the Backrooms.
Valve's `pm_shared.c` is the behavioral reference. Unreal remains responsible
for collision, stairs, floor detection, prediction, and replication.

## Multiplayer

The host and every PC client must install the same mod version and use the same
physics settings. Run `bhop.status` in the console and compare the configuration
checksum before joining. Mismatched physics settings cause Unreal server
corrections and rubber-banding.

## Installation

Copy the `bhop` directory into:

`EscapeTheBackrooms/EscapeTheBackrooms/Binaries/Win64/ue4ss/Mods/`

The installed package must contain:

- `bhop/dlls/main.dll`
- `bhop/bhop.ini`
- `bhop/enabled.txt`

The mod targets Steam build `23657885`, Unreal Engine 4.27, and ETBCommunity
UE4SS `1.3.0`. Unsupported builds fail closed with a log message.

## Console commands

- `bhop.toggle`
- `bhop.autobhop`
- `bhop.speedcap`
- `bhop.duckroll`
- `bhop.rawmouse`
- `bhop.reload`
- `bhop.status`

`RawMouseInput` disables Unreal Engine's legacy mouse smoothing and clears its
accumulated samples. It also removes ETB's frame-time scaling from mouse input;
the existing 120 FPS sensitivity is preserved. It does not change controller
look input.

Swimming, ladders, balancing, pushing, death/pass-out, and scripted/custom
movement continue to use ETB's native movement.

Airborne crouching is enabled. Releasing crouch within the configured
duck-roll window while grounded performs GoldSrc's 18-unit swept origin pop
without adding jump velocity; chain these releases to duck roll.

## Build

### Requirements

- Escape the Backrooms Steam build `23657885`
- ETBCommunity UE4SS fork `1.3.0`
- Visual Studio 2022 with C++ desktop tools
- CMake 3.22 or newer

```powershell
cmake -S . -B build-native -G "Visual Studio 17 2022" `
  -DBHOP_BUILD_UE4SS_MOD=ON `
  -DUE4SS_ROOT=C:\path\to\UE4SS
cmake --build build-native --config Game__Shipping__Win64 `
  --target etb_bhop bhop_tests
ctest --test-dir build-native -C Game__Shipping__Win64 --output-on-failure
```

The build stages the DLL at `package/bhop/dlls/main.dll`. Copy the complete
`package/bhop` directory into UE4SS's `Mods` directory.

The native adapter validates Steam build `23657885`, resolves the
`CalcVelocity` virtual slot from Unreal's reflected native wrapper, and checks
the live `UFancyMovementComponent` target before installing the detour. It
fails closed if any check differs.
