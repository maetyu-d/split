# Example Scripts

Load these `.split` files in the IDE using `Load`.

Included:
- `squarepusher_jungle.split`
- `venetian_7_8_drill.split`
- `autechre_polymeter.split`
- `aphex_mutation.split`
- `tt_metro_breaks.split`
- `tt_pattern_registers.split`
- `tt_logic_math_prob.split`
- `tt_sections_songform.split`

Lane layout used by all examples:
- Track 1-2: rhythmic/percussion
- Track 3: bass
- Track 4-5: melodic

Song-form helpers in the language:
- `from <cycle>` and `to <cycle>` can be added to event lines
- Example: `note 0 3 [60 - 67 -] 0.7 0.25 lead from 1 to 8`
- `barLoop <startBar> <endBar>` loops global playback between bars (inclusive), e.g. `barLoop 3 16`
- `tempo <step> <bpm> ...` can change tempo during playback (same condition tail as other events)
