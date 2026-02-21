# OSC Protocol (IDE -> Host)

Default host endpoint:
- Address: `127.0.0.1`
- Port: `9001`

Namespace:
- All message addresses are versioned under `/sap/v1/*`.

All event messages include a leading `lane` integer.

## 1) Simple trigger
Address: `/sap/v1/trigger`
Args:
1. `int lane`
2. `string eventId`
3. `float velocity` (0..1, optional; default 1.0)

Example:
- `/sap/v1/trigger 0 "kick" 1.0`

## 2) Pitched trigger
Address: `/sap/v1/note_on`
Args:
1. `int lane`
2. `int midiNote` (0..127)
3. `float velocity` (0..1)
4. `float durationSec` (optional; if omitted, explicit note_off is expected)
5. `string eventId` (optional)

Examples:
- `/sap/v1/note_on 1 60 0.8 0.25 "leadA"`
- `/sap/v1/note_on 1 67 0.7`

Companion note-off:
Address: `/sap/v1/note_off`
Args:
1. `int lane`
2. `int midiNote`
3. `float velocity` (optional)

## 3) Modulation
Address: `/sap/v1/mod`
Args:
1. `int lane`
2. `string target` (e.g. `"lane.gain"`, `"lane.pan"`, `"lane.mute"`, `"lane.solo"`, `"plugin[0].param[12]"`, `"master.gain"`)
3. `float value`
4. `float rampSec` (optional)

Example:
- `/sap/v1/mod 2 "plugin[0].param[12]" 0.45 0.10`

## Transport / utility
Address: `/sap/v1/transport`
Args:
1. `string command` (`"play"`, `"stop"`, `"recordStart"`, `"recordStop"`)

Address: `/sap/v1/tempo`
Args:
1. `float bpm`
