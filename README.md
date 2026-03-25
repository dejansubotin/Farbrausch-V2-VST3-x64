# Farbrausch V2 x64 Ports

This repository contains a modernized Windows-focused port of the public-domain Farbrausch V2 synthesizer by Tammo "kb" Hinrichs and Leonard "paniq" Ritter.

The goal of this work is pragmatic:

- keep the original synth engine intact
- preserve the original V2 editor look and behavior where practical
- make the instrument usable on current 64-bit Windows systems
- provide a real VST3 build for Cubase-class hosts
- keep V2M import, record, export, and playback workflows usable

## What is V2?

V2 is the synthesizer system Farbrausch used across many intros and productions. The public release includes:

- the synth core
- the original VST2/Win32 editor
- bank and patch management
- RONAN speech support hooks
- V2M conversion and playback tools

This port does not reimplement the DSP. It keeps the original synthesis path and wraps it for modern x64 targets.

## What is in this port?

This tree currently builds four main deliverables:

- `Farbrausch V2 x64.exe`
  - native Windows standalone synth
  - original V2 GUI
  - live MIDI input support
  - V2M record/export support
- `Farbrausch V2.vst3`
  - Windows x64 VST3 instrument
  - original Win32/WTL editor hosted through a VST3 `IPlugView`
  - verified against Steinberg's validator
- `Farbrausch V2M Player x64.exe`
  - minimal Windows x64 V2M player
  - drag-and-drop loading
  - play, pause, stop
  - clickable seek bar
  - built-in audio visualization
- `v2record_smoke.exe`
  - small smoke-test utility for V2M recording/export

## Porting approach

### 1. Keep the original engine

The synth engine still comes from the original V2 sources. The port uses the existing:

- synth core in `synth_core.cpp`
- patch/global definitions in `sounddef.cpp`
- V2M playback core in `v2mplayer.cpp`

The work here focuses on wrappers, build system changes, and x64 compatibility.

### 2. Replace the VST2 shell, not the synth

The original plugin was VST2-only and 32-bit. The x64 VST3 build adds:

- a new VST3 component/controller wrapper in `vst3/v2vst3_plugin.cpp`
- a helper DLL in `vst3/helper_main.cpp` and `vst3/helper_backend.cpp`
- chunk/state bridging for banks, globals, appearance, and program maps

The helper keeps the original Win32 editor and synth-side integration isolated from the VST3 host wrapper.

### 3. Keep the original GUI

The VST3 plugin intentionally keeps the original V2 editor instead of rebuilding it in a new UI toolkit.

That means:

- the visual style is the original V2 editor
- the plugin is Windows-only
- the editor size is fixed to the real shared-view size used by the old GUI

The editor size path was corrected so Cubase no longer clips the bottom of the interface.

### 4. Fix x64-only issues directly

Examples of x64 port work in this tree:

- replacing x86 inline assembly in the V2M player with portable 64-bit-safe C++
- new x64-native standalone audio/MIDI backend
- VST3 x64 helper and wrapper layer
- modern CMake build definitions

## VST3 status

The VST3 build targets Windows 10/11 x64 hosts such as Cubase 12.

Known-good validation in this workspace:

- Steinberg validator normal suite: `47 passed, 0 failed`
- Steinberg validator extensive suite: `537 passed, 0 failed`

Additional fixes made during the port:

- per-channel patch selection now drives the real multitimbral program map
- channel selection is exposed in VST mode
- the editor size exported to the host matches the actual V2 view size

## Standalone synth status

The x64 standalone keeps the original editor and adds:

- WinMM MIDI input support for current keyboards/controllers
- V2M record/export path
- RONAN-enabled builds where available

## V2M player

The x64 V2M player is a small native Windows app built around the original `V2MPlayer` core.

Features:

- drag and drop a `.v2m` file onto the window
- play, pause, stop
- clickable progress bar seek
- real-time visualization fed from the actual rendered audio
- automatic conversion of older V2M revisions on load

The player is intentionally minimal and Windows-native. It is not a DAW plugin.

## RONAN note

RONAN is enabled in these builds, but the public source drop does not contain the original English-to-phoneme helper that used to sit behind the speech workflow.

What that means:

- phoneme/speech support is still present in the engine path
- plain-English text-to-phoneme convenience conversion is stubbed
- the current stub lives in `e2p_stub.cpp`

## Build requirements

Toolchain:

- Visual Studio 2019 or newer with MSVC C++
- CMake 3.25 or newer
- Windows 10 SDK

Optional external dependencies:

- WTL
  - required for `V2_BUILD_STANDALONE=ON`
  - required for `V2_BUILD_VST3=ON`
- Steinberg VST3 SDK
  - required for `V2_BUILD_VST3=ON`

The CMake file was adjusted so the V2M player can build without the VST3 SDK and without WTL.

## CMake options

- `V2_ENABLE_RONAN`
- `V2_BUILD_STANDALONE`
- `V2_BUILD_RECORD_SMOKE`
- `V2_BUILD_VST3`
- `V2_BUILD_V2M_PLAYER`
- `V2_WTL_DIR`
- `V2_VST3_SDK_DIR`

## Example configure commands

Build only the V2M player:

```powershell
cmake -S . -B build -DV2_BUILD_V2M_PLAYER=ON -DV2_BUILD_STANDALONE=OFF -DV2_BUILD_VST3=OFF -DV2_BUILD_RECORD_SMOKE=OFF
cmake --build build --config Release --target v2m_player_x64
```

Build the standalone synth and VST3 plugin:

```powershell
cmake -S . -B build `
  -DV2_WTL_DIR="C:/path/to/WTL" `
  -DV2_VST3_SDK_DIR="C:/path/to/vst3sdk"
cmake --build build --config Release --target v2standalone_x64 farbrausch_v2_vst3
```

## Output names

- `Farbrausch V2 x64.exe`
- `Farbrausch V2.vst3`
- `Farbrausch V2M Player x64.exe`
- `v2record_smoke.exe`

## Current limitations

- the VST3 editor is still a fixed-size Windows editor, not a scalable cross-platform UI
- the plugin and editor path are Windows-only
- RONAN English-to-phoneme conversion remains stubbed in the public source release

## Source provenance

This port is based on the public Farbrausch source release and keeps the original license/material in this tree. The new work here is focused on compatibility, wrappers, build tooling, and Windows x64 usability.
