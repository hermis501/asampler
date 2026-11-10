# ASampler

A drum and chop sampler plugin (VST3) in the spirit of the MPC, made for
producers who work with a screen reader. Its first user is a blind hip-hop
producer on NVDA and REAPER, and most design decisions come from that.

The idea is simple: everything you can change is a normal host parameter, so
REAPER's FX parameter list gives you the whole instrument without any custom
screen. On top of that there is a small keyboard-driven editor for the things
a parameter can't do, like loading files, chopping, or setting a slice point
by ear. Whatever happens is spoken through your own screen reader, in your
own voice and speed.

## What it does

- 32 pads in two banks of 16. Each pad holds up to 8 samples as layers
  (velocity, round robin, random, or key zones for a proper multisampled
  instrument), and layers can also fire on note release.
- Per pad: volume, pan, tuning, envelope, filter with its own envelope,
  saturation and bit/sample-rate crush, two LFOs, mod wheel, glide, pitch
  envelope for 808s, stereo width, choke groups and note repeat with swing.
- Pads are either fixed to one note (drums) or spread across the keyboard
  (808s, chops, melodies), with an optional key and scale lock.
- Tempo sync by repitching like an MPC, or by real time-stretching. Formant
  shift, and Formant Correct so a vocal or bass doesn't chipmunk as you play
  it up the keyboard.
- Auto-chop a loop across pads, evenly or on transients. Snap to transients
  or zero crossings, fades, loop crossfades.
- Set Start and End by ear: T and O, then the arrow keys. Each step plays a
  short snippet around the point.
- Browse a folder with N and P, or try the next sample in the beat with
  Shift+N before you commit to it.
- Describe (I) tells you a sample's length, level, number of hits, tempo and
  key, the things you'd otherwise read off a waveform.
- Bounce a pad to a new sample, normalise, copy and paste pads, export slices
  as a MIDI file, save kits with or without their samples.
- Undo and redo for everything, including a chop across 32 pads.
- A main stereo output plus one per pad. The included REAPER script sets up
  a routed track for every pad in one go.
- In REAPER: Ctrl+R toggles record, Ctrl+G puts the selected item on a pad.

With 32 pads there are about 4,800 parameters, which nobody wants to arrow
through. The Focus group mirrors whichever pad is selected, so in practice
you work with about 160.

The manual, [docs/manual.html](docs/manual.html), covers all of it, with a
quick start, recipes, the full parameter list and troubleshooting. The
installer puts it in the Start menu. What changed between versions is in
[CHANGELOG.md](CHANGELOG.md).

## Download

Get the latest version from [Releases](../../releases).

- **Windows** (10 or later, 64-bit): run `ASampler-<version>-Windows-Setup.exe`.
  It installs the VST3 into `C:\Program Files\Common Files\VST3`. If you use
  REAPER, a checkbox (on by default) also adds the "Create pad tracks" script
  to the Actions list. The installer isn't code-signed yet, so SmartScreen
  may warn you: choose More info, then Run anyway.
- **macOS** (11 or later, Apple Silicon and Intel): run
  `ASampler-<version>-macOS.pkg`, which installs the VST3 and Audio Unit
  into `/Library/Audio/Plug-Ins`. It isn't signed by Apple, so the first
  time, Control-click the package and choose Open. If your DAW still won't
  load the plugin, run
  `sudo xattr -dr com.apple.quarantine /Library/Audio/Plug-Ins/VST3/ASampler.vst3 /Library/Audio/Plug-Ins/Components/ASampler.component`.

The Mac version is built and tested automatically on GitHub, but nobody has
tried it on a real Mac yet. If you do, please open an issue and say how it
went, especially with VoiceOver.

## Building from source

You need CMake 3.22 or newer, a C++20 compiler (Visual Studio 2022 or newer on
Windows, Xcode on macOS), [JUCE 8.0.15](https://github.com/juce-framework/JUCE)
and [Catch2 3.5.4](https://github.com/catchorg/Catch2).

```sh
cmake -S . -B build -DASAMPLER_JUCE_DIR=<path to JUCE> -DASAMPLER_CATCH2_DIR=<path to Catch2>
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The plugin ends up in `build/ASampler_artefacts/Release/VST3/ASampler.vst3`.
On Windows, `build.bat` and `build_release.bat` do the same with the SDKs in
`C:\_tools`. The Windows installer is made with Inno Setup 6 from
`installer/ASampler.iss`.

Every push is built and tested on Windows and macOS, and the plugin is checked
with [pluginval](https://github.com/Tracktion/pluginval). Pushing a `v*` tag
publishes a release with both installers.

[CLAUDE.md](CLAUDE.md) has the developer notes: how the code is laid out, why
things are done the way they are, and the accessibility rules a change has to
follow.

## License

ASampler is free software under the
[GNU General Public License v3.0 or later](LICENSE). It is built with
[JUCE](https://juce.com), used under JUCE's AGPLv3 licence option, which
GPLv3 is compatible with.

## Author

Hermis Kasperavičius, 2026. Bug reports and ideas are welcome in
[Issues](../../issues).
