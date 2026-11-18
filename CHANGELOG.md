# Changelog

## Unreleased

- **No more clicks when a sound is cut.** A choked pad (the open hi-hat cut by
  the closed one) and a mono pad's previous note now fade out over 3
  milliseconds instead of stopping dead, so 808s retrigger cleanly.
- **Find a setting** (Ctrl+F): type part of a setting's name, in any section,
  and Enter goes straight to it. Up and Down choose between matches.
- **REAPER automation from the window:** Ctrl+Shift+A on a setting turns its
  automation envelope on, shown and armed for writing, or off again keeping
  its points. "Turn off all ASampler automation in REAPER" in the pad menu
  turns off every active one. Needs REAPER 7 or later.
- **Grab several REAPER items at once:** Ctrl+G with several items selected
  puts each on its own pad, in timeline order, from the selected pad on. Each
  is rendered on its own, so touching slices are not merged, and the result
  says which loaded pads were replaced.

## 1.1.0

- **Pad settings in the plugin window.** Every setting of the selected pad is
  now in ASampler's own window too, one section at a time, as knobs, lists and
  checkboxes. Until now a pad's filter or envelope could only be reached
  through the host's parameter list, which REAPER has and most other DAWs
  (Studio One, Live, Logic) don't. It works with a mouse and with a screen
  reader: arrows change a knob by 1% (Shift 10%), Home and End go to the ends,
  Enter types a value, and double-click resets it.
- **Sample browser.** Ctrl+B swaps the pad settings for a list of the samples
  in a folder. Moving onto a file plays it; Enter loads it onto the selected
  pad or opens a folder, Backspace goes up, and a letter jumps to the next
  name starting with it. Files can be dragged onto a pad, or out into the DAW.
- **Pad menu.** Shift+F10, the Applications key, a right-click on a pad or the
  More actions button open a menu with every action for the selected pad. The
  less common actions moved there from the button rows, so Tab gets through
  the window faster; their shortcuts are unchanged.
- **For sighted users:** a waveform of the selected pad with draggable Start,
  End and loop markers, a playhead, zoom, and other pads' slices of the same
  file marked; pads coloured by what they hold (loaded, built-in tone,
  missing), lighting up when they play; an output meter; graphs for the
  envelope, filter and LFOs; tooltips on the buttons. The window can be
  resized or set from 75% to 200%, and remembers its size. None of these take
  a Tab stop or add anything for a screen reader to read.
- **Save a pad as audio.** Drag a pad out of the window, or use "Save Pad as
  audio file" in the pad menu, to get a WAV of it rendered with all its
  settings, as a bounce would be.
- **Starter Drums** factory kit (program 5): eight drum sounds made by
  ASampler itself (kick, hats, snare, clap, rim, tom, 808) on pads 1 to 8, so
  there is something to play before you have samples. The other factory kits
  are unchanged and keep their program numbers.
- **Tune** (U, or the pad menu): detects the sample's pitch and sets the pad's
  Pitch so it plays in tune, on the nearest note, or on its root key in
  Keyboard mode.
- **Hold** and **Filter Env Hold**: keep the amp or filter envelope at full
  level for a while after the attack, before it decays. At 0, the default,
  nothing changes.
- **Exit Loop On Release**: letting go of a looping note leaves the loop and
  plays the sample's own tail.
- **REAPER undo:** a sample or pad name change (load, browse, chop,
  rename) also adds a step to REAPER's undo history, so REAPER's Undo can take
  it back.
- **REAPER scripts can load samples:** the new "ASampler - Load file onto pad"
  script loads a file onto a pad without opening the window, and the manual
  documents the simple protocol for writing your own.
- **Fixed:** Undo after Reset or Clear all pads now brings the samples back
  too. Before, it only restored the settings, though the buttons said it
  would restore the pads.

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
