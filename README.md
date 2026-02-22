# Split Audio Programming (JUCE)

Two desktop apps that work together over OSC:

- `Pattern IDE` (language + live transport + timeline view)
- `Plugin Host` (AU/VST3 lane host + mixer + recording)

## What It Does

### Pattern IDE
- Split-style pattern language (`trigger`, `note`, `mod`, `break`, `drill`, mutations, sections)
- Teletype-style mode (`language teletype`) with scripts, `RUN`, math/logic, pattern memory, metro/clock
- OSC monitor + log view toggle
- Transport display (bar/beat/step)
- Deterministic timeline panel (hideable, draggable splitters)
- Script save/load

### Plugin Host
- 10 track lanes + master lane
- AU/VST3 plugin scanning and loading (instruments and effects)
- Per-lane hidden/expandable FX chain + master FX chain
- Lane gain/pan/mute/solo + master gain
- Plugin editor windows for instruments/effects
- OSC lane activity indicators
- Stereo master level meter with 0 dB / -3 dB guides
- Save/load host config (`.sconfig`)
- Recording controls:
  - `Record` (stereo WAV)
  - `Rec Settings` (file, bit depth, sample rate)
- `Audio Settings` button for device/buffer setup

## Build

From project root:

```bash
cmake -S . -B build
cmake --build build -j8
```

## Run Both Apps

```bash
open "/Users/md/Downloads/Split audio programming/build/PluginHost_artefacts/Plugin Host.app"
open "/Users/md/Downloads/Split audio programming/build/PatternIDE_artefacts/Pattern IDE.app"
```

Recommended order:
1. Open `Plugin Host` first, set audio device/output, load plugins.
2. Open `Pattern IDE`, load a script, press `Run`.

## Examples

Scripts are in:

- `examples/scripts/`

Includes split-style and TT-style examples:

- `squarepusher_jungle.split`
- `venetian_7_8_drill.split`
- `autechre_polymeter.split`
- `aphex_mutation.split`
- `tt_metro_breaks.split`
- `tt_pattern_registers.split`
- `tt_logic_math_prob.split`
- `tt_sections_songform.split`

## Release Zip (macOS)

Combined app zip:

- `release/Split-macOS-builds.zip`

Contains:
- `Plugin Host.app`
- `Pattern IDE.app`

## OSC Protocol

See:

- `docs/osc_protocol.md`
