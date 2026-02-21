# Split Audio Programming (JUCE)

This repository is a starter scaffold for a two-app audio language system:

1. `PatternIDE`:
- Language/IDE frontend (Tidal + SuperCollider style)
- Sends OSC only
- Emits three message families: trigger, note, modulation

2. `PluginHost`:
- Hosts AU/VST3 plugins
- Receives OSC and maps to plugin/MIDI/automation actions
- Handles mixing and WAV recording/export

## Protocol
See: `docs/osc_protocol.md`

## Build

```bash
cmake -S . -B build -DJUCE_DIR=/absolute/path/to/JUCE
cmake --build build
```

## Current status

Implemented:
- Two JUCE app targets in one CMake project
- OSC sender demo UI in `PatternIDE`
- OSC receiver engine stub in `PluginHost`
- Transport commands for recording start/stop
- WAV file writing scaffold

Not implemented yet:
- Real parser/runtime for pattern language
- Full plugin graph, plugin scanning/instantiation UI
- MIDI/event scheduler with sample-accurate timing
- Parameter mapping database and modulation ramp engine
- Full mixer strips (gain/pan/sends) and offline export path

## Suggested next implementation order

1. Define AST + scheduler in IDE and output timestamped OSC bundles.
2. Build host lane model (`Lane -> PluginChain -> MixerBus`).
3. Implement note/trigger routing to MIDI-capable plugin instances.
4. Implement modulation path resolver (`plugin[i].param[j]` and named paths).
5. Add record/export UI and offline bounce option.
