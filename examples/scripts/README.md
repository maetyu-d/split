# Example Scripts

Load these `.split` files in the IDE using `Load`.

Included:
- `squarepusher_jungle.split`
- `venetian_7_8_drill.split`
- `autechre_polymeter.split`
- `aphex_mutation.split`
- `amen_pressure_172.split`
- `reese_halftime_160.split`
- `rimstep_oddcycle_174.split`
- `acid_break_architecture_178.split`
- `tt_metro_breaks.split`
- `tt_pattern_registers.split`
- `tt_logic_math_prob.split`
- `tt_sections_songform.split`

Lane layout used by all examples:
- Track 1 (lane 0): drum plugin (MIDI map below)
- Track 2: rhythmic/percussion
- Track 3: melodic
- Track 4 (lane 3): FM bass (mono legato + velocity accents)
- Track 5: melodic

Lane 0 drum MIDI map used in examples:
- 36 = kick
- 38 = snare
- 42 = closed hat
- 46 = open hat
- 49 = crash
- 51 = ride
- 39 = clap
- 37 = rim/side

FM bass behavior used in examples on lane 3:
- Overlapping notes (durations longer than step spacing) to trigger mono legato glide.
- Velocity accents (higher velocities on selected steps) to drive accent response.

Song-form helpers in the language:
- `from <cycle>` and `to <cycle>` can be added to event lines
- Example: `note 0 3 [60 - 67 -] 0.7 0.25 lead from 1 to 8`
- `barLoop <startBar> <endBar>` loops global playback between bars (inclusive), e.g. `barLoop 3 16`
- `tempo <step> <bpm> ...` can change tempo during playback (same condition tail as other events)
