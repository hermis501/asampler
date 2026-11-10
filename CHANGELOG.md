# Changelog

## 1.0.0

First public release, for Windows and macOS. The Mac version comes from the
automatic build and hasn't been tried on a real Mac yet.

### Working with a screen reader

- Everything is a host parameter, so REAPER's FX parameter list reaches the
  whole instrument. Names carry the pad number ("P9 Filter Cutoff"), and each
  pad is split into twelve sections.
- The Focus group follows the selected pad, so about 160 parameters cover all
  32 pads instead of 4,800.
- Messages go through your screen reader (NVDA, JAWS and Narrator on Windows,
  VoiceOver on macOS) instead of a separate voice. Repeats are skipped and
  errors interrupt.
- Slice points read and can be typed as time: "1.250 s", "480 ms", "50%".
- The editor is a 4x4 pad grid driven by the keyboard. Arrows move, Space
  plays, Page Up and Page Down switch banks. Every button says which pad it
  acts on. F1 speaks the shortcuts.
- T and O set Start and End by ear with the arrow keys, playing a snippet at
  each step. Enter keeps the change, Escape puts it back.
- N and P load the next or previous file in the folder. Shift+N and Shift+P
  try it on the pad first, in the beat if it's playing, and Enter keeps it.
- I describes a sample: length, level, how many hits, tempo, and key or pitch.
- Shift with a pad key says what's on the pad without playing it. F2 names a
  pad. K learns the pad's note from your MIDI keyboard.

### Sampler

- 32 pads in two banks, 8 layers per pad, 256 voices.
- Layers by velocity (ranges can overlap and stack), round robin, random or
  key zones. Layers can play on release, and each has its own tuning and gain.
- Fixed pads for drums, Keyboard pads for 808s and melodies, with a global
  key and scale lock. One note never plays two pads by accident.
- Per pad: envelope, filter with its own envelope, saturation, bit and
  sample-rate reduction, two LFOs (free or tempo-synced), mod wheel routing,
  pitch envelope, glide, velocity to cutoff/pitch/start, stereo width, Mono
  Below, choke groups, mute and solo, note repeat with swing and humanize.
- Tempo sync by repitching or by time-stretching, plus Speed, Keep Length,
  formant shift and Formant Correct. Stretching runs in the background and
  keeps the stereo image intact.
- Pitching up is band-limited, so high notes don't alias.
- Auto-chop across 2 to 32 pads, evenly or on transients. Snap to transients
  or zero crossings, fades and loop crossfades.

### Workflow

- Load several files at once, one per pad or as layers. Drop files on a pad,
  or copy them in Explorer and paste with Ctrl+V. Trim Silence can run on
  every load.
- Kits (`.asamplerkit`) can save sample locations, carry copies of the
  samples, or be settings-only templates. Four factory kits (Trap, Boom Bap,
  Lo-Fi, 808 Set) are also the plugin's programs.
- Bounce a pad to a new sample, normalise, copy and paste pads, apply a
  setting to many pads, export slices as MIDI, MIDI learn.
- Full undo and redo. A chop across 32 pads is one step.
- Missing samples are kept in the project and can be relinked by name.
- Computer keys play the pads and send the notes to the MIDI output, so you
  can record them.
- Main stereo output plus 32 pad outputs. The "Create pad tracks" REAPER
  script makes a routed track per pad. The Windows installer adds it to
  REAPER's Actions list, and uninstalling removes it again.
- In REAPER, Ctrl+R toggles record and Ctrl+G grabs the selected item onto
  the pad.
- Reads WAV, AIFF, FLAC, Ogg Vorbis and MP3.

### Reliability

- Passes pluginval at its strictest level.
- Damaged or foreign project data is rejected instead of reaching the audio.
