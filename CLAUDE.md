# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What ASampler is

A VST3 sampler plugin for hip-hop / trap producers, built on JUCE 8. The **primary user is a blind producer using a screen reader (NVDA/JAWS) in REAPER on Windows.** Accessibility is the structural constraint that shapes every architectural decision — not a polish item at the end.

The MPC-style workflow is the model: `kNumPads` (16) pads, choke groups for hi-hats, and a broad per-pad control surface. What's implemented per pad today (all as host parameters — see the `param_ids` block and `makePadGroup` in `PluginProcessor.*`):

- **Playback:** volume / pan / pitch / ADSR, sample start-end trim, reverse, one-shot (ignore note-off), loop (`LoopMode` off / forward / ping-pong with loop start-end).
- **Pitch envelope:** per-pad decay envelope on pitch (amount in semitones + time) — starts offset and slides back to target in ratio-space. Classic 808 / kick "punch". Feeds `pitchRatio` alongside glide/bend.
- **Filter:** per-pad `FilterMode` (off / LP / HP / BP) with cutoff + resonance, plus a dedicated **filter envelope** (`Filter Env Amount` in octaves, with its own ADSR) — the per-hit sweep that LFO/velocity/aftertouch modulation could not express.
- **Character (lo-fi):** post-filter `SaturationMode` (Tube / Tape / Hard Fold) + amount, bit-depth reduction (16 = off), and sample-rate reduction (downsample-hold factor, 1 = off). Backed by `engine/Saturation.h`; chain order is filter → saturation → downsample → bit-quantize.
- **LFO:** per-pad LFO — rate, depth, `LfoShape` (Sine / Triangle / Square / Saw), `LfoDest` (off / pitch / cutoff / volume / pan). Backed by `engine/Lfo.h`; one eval per sample routed to the selected destination.
- **Slice cleanup:** edge fade-in / fade-out (ms), forward-loop crossfade (ms), and a zero-crossing snap bool that nudges start/end (and loop) indices onto the nearest crossing at trigger time — click-free windows without a manual edit action.
- **Tempo sync:** per-pad `Tempo Sync` bool + `Original BPM`, plus `Stretch Mode` (Repitch / Stretch). **Repitch** (default) scales the playback ratio by (host BPM / original BPM) so pitch moves with tempo — the classic MPC sound, and free. **Stretch** time-stretches instead, so a 90 BPM riff fits an 80 BPM beat at its original pitch.
- **Formant:** per-pad `Formant` (semitones) shifts the spectral envelope without touching pitch — the control resampling physically cannot provide, since there playback rate *is* pitch. `Formant Correct` additionally holds formants **fixed as the pad is played across the keyboard**, so a bass or vocal sample stops chipmunking as it goes up. See the offline-render section for how.
- **Gain staging:** per-layer gain trim (`Layer N Gain`), plus a Normalise action that sets that gain so the layer peaks at -1 dBFS — non-destructive, automatable and undoable, unlike rewriting the audio.
- **Velocity modulation:** beyond the amplitude `VelocityCurve`, per-hit velocity can also drive filter cutoff, pitch, and sample start (Vel→Cutoff / Vel→Pitch / Vel→Start), applied at trigger time. Vel→Pitch is added *after* the glide snapshot so it doesn't pollute the note-to-note glide delta.
- **Voicing:** choke group, `maxVoices` per-pad cap (1 = mono), glide/portamento (`GlideMode` off / legato / always / legato-rate / always-rate + glide time). The rate modes read Glide Time as **ms per semitone**, so one setting suits both a semitone step and an octave jump; `Vel to Glide` shortens the slide with velocity, giving per-note control from the piano roll. Glide interpolates linearly in **semitones**, not frequency ratio, so slides move at a constant musical rate.
- **Stereo:** per-pad `Stereo Width` (M/S, 0 = mono … 2 = double) and `Mono Below` (collapse everything under N Hz to centre) — the standard way to keep 808s and bass phase-coherent.
- **Note dispatch:** `Mode` (Fixed / Keyboard). Fixed is the strict MPC default (fires on the one assigned `Note`); Keyboard re-purposes `Note` as a root and responds across `[Low Note, High Note]`, pitching by (incomingNote − root) — ReaSamplOmatic-style, so an 808 pad can stretch across the keyboard while neighbouring pads stay on fixed drum notes in the same kit.
- **Velocity layers / multisampling:** up to `kNumLayers` (4) samples per pad. `LayerMode` selects how a trigger picks among loaded layers — Velocity (band by velocity), RoundRobin, Random, or **KeyZones**. In KeyZones each layer has its own `Low/High/Root Note`, so one pad holds a real multisampled instrument instead of a single sample stretched (and chipmunked) across the keyboard; a note outside every zone falls back to the nearest-root loaded layer rather than going silent. `VelocityCurve` shapes incoming velocity.
- **Expression:** mute / solo (DAW-style gate), channel aftertouch routing (`AftertouchDest` off / volume / filter cutoff + amount), note-repeat (`NoteRepeatRate` quarter…32nd-triplet, tempo-synced to host BPM) with per-pad swing (delays odd off-beats) and humanize (timing + velocity jitter).

Global features: master gain, pitch-bend range, a global **Key + Scale** (any pad with `Scale Lock` on snaps incoming notes to it in Keyboard mode, so a melodic chop can't land off-key), **auto-chop** (slice the sample on one pad across the first N pads — `ChopMode` equal / transient-log-energy / transient-spectral-flux, backed by `engine/Transient.*`), **MIDI learn** (arm a param, move a CC, and it binds; 128 CC → parameter slots), and a **vocal generator** (type text → Windows SAPI5 text-to-speech renders it to a WAV → loads onto the `Vocal Target Pad`, then optionally auto-chops across pads or spreads across the keyboard in Keyboard mode — a built-in, no-recording vocal-chop source, backed by `source/VocalSynth.*`). The plugin exposes a **Main stereo output bus plus one output bus per pad**, so pads can be routed to separate REAPER tracks.

Workflow features added on top of that: **undo/redo** (the APVTS owns a `juce::UndoManager`, so a chop across 16 pads is one undo step), **kits** (`.asamplerkit` files carrying the full parameter state plus sample paths, and four built-in factory kits — Trap / Boom Bap / Lo-Fi / 808 Set — that configure envelopes, choke groups, note layout and character but leave samples alone), **pad copy/paste** and Clear All, **slice-to-MIDI export** (writes a `.mid` with one note per slice rather than relying on VST3 drag-out, which isn't accessible), **pad audition** from the editor or the QWERTY keys `Z X C V / A S D F / Q W E R / 1 2 3 4`, **snap-to-transients** (nudges a pad's start/end onto the nearest detected onsets — zero-cross snap removes clicks, this fixes a boundary auto-chop placed slightly off the hit), and **missing-sample recovery** (a moved project keeps its sample references, reports them as missing, and can relink them by name from a chosen folder).

## Accessibility-first architecture

The single rule that drives everything:

> **Every controllable thing must be a standard `juce::AudioProcessorParameter` exposed to the host.**

REAPER's FX parameter list reads host parameters directly via screen reader without any custom GUI work — this is the accessibility floor. Internal-only state that should be user-tweakable but isn't a parameter is an accessibility bug, not a stylistic choice. Build a feature as a parameter first, then optionally add a custom UI control on top.

That rule scales badly on its own: the ~82 per-pad controls x 16 pads are ~1,300 host parameters, and arrowing through hundreds of entries to reach "Pad 9 Filter Cutoff" is its own accessibility failure. The **Focus proxy group** (`source/FocusProxyParameter.h`) is the counterweight — a `Focused Pad` parameter plus one mirrored `Focus / ...` parameter per per-pad control, forwarding to whichever pad is selected. The user navigates ~82 parameters instead of ~1,300. **Any new per-pad parameter must be added to `padParamSpecs()` in `PluginProcessor.cpp` as well as `makePadGroup`** — a test asserts the two stay in step, because a parameter missing from the Focus group is reachable only by scrolling the full per-pad list.

Consequences worth knowing before editing:

- The editor (`source/PluginEditor.*`) is custom but built from `juce::TextButton` and `juce::Label` — both expose Windows UIA accessibility roles automatically. Any new custom component must set `setTitle()` / `setDescription()` and, if it's not a built-in JUCE widget, override `createAccessibilityHandler()`. Prefer JUCE's built-in `Slider` / `Button` / `ComboBox` over custom-drawn knobs.
- After any user-visible state change, post a UIA announcement via `getAccessibilityHandler()->postAnnouncement(...)` so the screen reader speaks even when focus didn't move. The editor already does this for sample-load results; do the same for any new async user action.
- Sample selection uses `juce::FileChooser::launchAsync` (native OS dialog, fully accessible). Never roll a custom file browser.
- **Announcements are deduplicated and prioritised** (`ASamplerEditor::announce`). An identical repeat is dropped — bursty actions fire the change listener many times for one logical event, and a queue of identical speech is something the user has to sit through before they can act. Failures use `AnnouncementPriority::high` so they interrupt; routine confirmations use `medium` so they don't talk over the user.
- **Destructive actions say what they will replace, and are undoable.** Chop button labels name the pads they overwrite; Clear All confirms via `NativeMessageBox` first. Losing 16 pads is recoverable for a sighted user who can see it happen — it is not otherwise.
- Live parameter changes must reach sounding voices (`VoiceLiveParams` / `Voice::setLiveParams`). A knob that produces no audible change on a held note reads as a broken plugin when you cannot see the value move.
- Build targets are **VST3** on every platform plus **AU** on macOS (JUCE drops AU elsewhere) — load the plugin in REAPER to test the GUI with a screen reader. A Standalone format was previously built as a no-DAW accessibility harness; if that workflow is needed again, add `Standalone` back to the `FORMATS` list in `CMakeLists.txt` (and re-add the artefact path to the "Build outputs" section below).

## Build & test

### Common commands

- **Dev build (RelWithDebInfo) + tests:** `build.bat`
- **Release build + tests:** `build_release.bat`
- **Reconfigure from scratch:** delete the `build\` directory, then re-run a build script
- **Build only (skip ctest):** `cmake --build build --config RelWithDebInfo`
- **Build one target** (the fast loop — `PluginProcessor.cpp` is ~4k lines, a full build is slow): `cmake --build build --config RelWithDebInfo --target ASamplerTests` (or `asampler_core`, `ASampler`)
- **Run a single test by name regex:** `ctest --test-dir build -C RelWithDebInfo -R <regex> --output-on-failure`
- **List discovered tests:** `ctest --test-dir build -C RelWithDebInfo -N`
- **Run by Catch2 tag** (faster than ctest, which re-launches the exe per test case): `build\tests\RelWithDebInfo\ASamplerTests.exe "[formantcorrect]"`, and `--list-tests` / `--list-tags` to see what exists

`catch_discover_tests` registers each `TEST_CASE` under its full sentence name, so `ctest -R` matches the prose, e.g. `-R "A chop across pads can be undone"`.

### Build outputs

- VST3: `build\ASampler_artefacts\<Config>\VST3\ASampler.vst3\` (load this folder in REAPER)
- AU (macOS only): `build/ASampler_artefacts/<Config>/AU/ASampler.component`
- Test binary: `build\tests\<Config>\ASamplerTests.exe`

### SDK locations

- **JUCE 8.0.15** at `C:\_tools\JUCE` (added via `add_subdirectory` in `CMakeLists.txt`)
- **Catch2 3.5.4** at `C:\_tools\Catch2-3.5.4` (same)

Override with `-DASAMPLER_JUCE_DIR=...` / `-DASAMPLER_CATCH2_DIR=...` if they move. Both are `CACHE` variables, so an existing `build\` dir keeps the path it was first configured with — the build scripts detect a cached path that no longer exists and reconfigure from scratch.

### Test suite conventions

~265 Catch2 v3 `TEST_CASE`s across 14 files, all tagged. The tags are the practical index into the suite — `[plugin]` (processor-level, the bulk), `[voice]`, `[vocoder]`, `[state]` (save/restore round-trips), `[stretch]` / `[formantcorrect]` / `[formantpitch]`, `[focus]` and `[accessibility]` (the Focus-proxy parity checks), plus one per feature area (`[chop]`, `[glide]`, `[keyzones]`, `[midilearn]`, `[noterepeat]`, `[undo]`, …). Tag a new test with the feature area and, if it is processor-level, `[plugin]` as well.

Conventions the existing tests share, worth copying:

- Processor-level tests construct a real `ASamplerProcessor` on the stack and drive it through its public API — there is no mock layer.
- Tests set parameters through a local `setParam` helper using `setValueNotifyingHost`. That is deliberate and does *not* contradict the production rule below (`setRangedParam`): tests are asserting DSP/dispatch behaviour, not undo history. Tests that assert undo behaviour go through the processor's own action methods instead.
- Audio fixtures are generated, not checked in: a `writeSineWav`-style helper writes a short WAV into the system temp dir and the test loads it back.
- Anything that depends on the machine's environment degrades rather than fails — the vocal tests accept `SynthFailed` on a box with no installed SAPI voice.

## Project layout

```
CMakeLists.txt              — top-level: JUCE/Catch2 wiring, plugin target, core lib
source/
  PluginProcessor.h/.cpp    — JUCE AudioProcessor, parameter layout, MIDI dispatch, sample loading,
                              undo, kits, copy/paste, slice-MIDI export, audition
  FocusProxyParameter.h     — host parameter that forwards to the focused pad (the Focus group)
  PluginEditor.h/.cpp       — accessible editor: per-pad Load/Reset/Chop TextButtons + vocal-generator row + status label
  VocalSynth.h/.cpp         — Windows SAPI5 text-to-speech → WAV (on the ASampler plugin target, not asampler_core)
  engine/                   — DSP / sampler engine (built as static lib asampler_core)
    AtomicSharedPtr.h       — portable atomic shared_ptr (std::atomic on MSVC, free-function fallback on libc++)
    Sample.h                — AudioBuffer + source sample rate
    Voice.h/.cpp            — one playing voice: pitch (linear interp), pan, ADSR, filter, loop, glide; holds shared_ptr to its source
    VoicePool.h/.cpp        — fixed-size pool, per-pad trigger/release/choke/gate/render (incl. per-pad-bus routing)
    DefaultSample.h         — generates a built-in A4 sine pluck used until the user loads a real sample
    VelocityCurve.h         — velocity-shaping curves (header-only)
    FilterMode.h            — per-pad filter type enum + labels (header-only)
    LoopMode.h              — loop mode enum + labels (header-only)
    Saturation.h            — saturation modes + bit-depth reduction (header-only)
    Lfo.h                   — LFO shapes/destinations + waveform eval (header-only)
    Transient.h/.cpp        — onset detection (log-energy + spectral-flux) for transient-mode auto-chop
    PhaseVocoder.h/.cpp     — offline time-stretch + formant shift (STFT with phase locking,
                              transient-aware phase reset, cepstral envelope warping)
tests/
  CMakeLists.txt            — test exe wiring (catch_discover_tests); links asampler_core + ASampler_SharedCode
  test_*.cpp                — Catch2 v3 tests (voice, voice_live, voice_pool, velocity_curve,
                              saturation, lfo, transient, plugin_processor, vocal, production,
                              melodic, phasevocoder, formant_correct, formant_pitch)
build.bat / build_release.bat
.github/workflows/build.yml — CI: macOS (universal VST3 + AU) and Windows builds, tests, zipped artifacts
```

`test_plugin_processor.cpp` (and `test_vocal.cpp`) construct the real `ASamplerProcessor`, so the test target links `ASampler_SharedCode` (the JUCE-generated static lib carrying the `JucePlugin_*` defines) in addition to `asampler_core` — see `tests/CMakeLists.txt` for the artefacts-dir link path that makes that resolve. `VocalSynth.cpp` self-links `sapi.lib` / `ole32.lib` via `#pragma comment(lib, …)`, so pulling it out of `ASampler_SharedCode` needs no extra link config in `tests/CMakeLists.txt`. The vocal tests tolerate a machine with no installed speech voice (they accept `SynthFailed` and only assert the loaded-sample post-conditions when synthesis actually succeeds).

### MIDI → audio flow

`processBlock` first runs MIDI learn (`handleMidiLearn`) over the input, then `buildCombinedMidi` merges the host MIDI with any synthetic note-repeat triggers for held pads, then it splits the block at each event so notes start sample-accurately. Per event: note-on scans all pads — a Fixed pad matches when the incoming note equals its `Note`; a Keyboard pad matches when the note is within `[Low Note, High Note]` and pitches by (note − root). Every match triggers a voice. Note-off releases via `releasePad` (Fixed: release all voices on the pad) or `releasePadNote` (Keyboard: release only voices triggered by that incoming note). Choke groups hard-stop other pads in the same group before triggering. `handlePitchBend` and `handleAftertouch` push modulation onto active voices; aftertouch routing is per-pad (`AftertouchDest`).

Output routing: voices render through the per-pad-bus overload of `VoicePool::renderAdding`, one destination buffer per pad, falling back to Main for disabled pad buses. Mute/solo is resolved per block by `resolvePadGate` and pushed to voices as a 0/1 gate (voices keep advancing when gated — DAW-style mute).

Parameter atomics (`std::atomic<float>*`) are cached at construction in `padParams[]` (`PadParamPtrs`) to avoid per-block string-keyed lookups in the audio thread. If you add a new per-pad parameter, also extend `PadParamPtrs` and `cacheParamPointers()` — that pairing is the single source of truth for what the audio thread reads. Global params have their own cached pointers (`masterGainParam`, `chopCountParam`, etc.).

The default sample is regenerated in `prepareToPlay` at the host rate so `Voice`'s rate-compensation path doesn't have to interpolate it. Pads that still hold the default sample get re-pointed at the new-rate buffer; pads with user-loaded samples keep their own.

### Sample loading & lifetime

Each pad holds `kNumLayers` (4) velocity layers; each layer stores its sample in an `AtomicSharedPtr<const Sample>` (`engine/AtomicSharedPtr.h`). That is C++20's `std::atomic<std::shared_ptr>` where the standard library provides it (MSVC), and a `std::atomic_load` / `std::atomic_store` wrapper where it doesn't (Apple's libc++). **Don't declare `std::atomic<std::shared_ptr<...>>` directly** — it compiles on Windows and breaks the macOS build. The message thread `store()`s a new sample; the audio thread picks a layer (per the pad's `LayerMode`) and `load()`s a snapshot in the note-on path, passing it to `VoicePool::triggerPad`, which moves it into the `Voice`. The voice retains its own strong reference for the rest of playback — swapping the pad's sample while a voice is mid-render is safe.

The editor loads into whichever layer the **Edit Layer** host parameter selects (`loadSampleForPad`), or an explicit index (`loadSampleForPadLayer`). Auto-chop (`chopAcrossPads`) distributes one pad's sample across the first N pads, setting each destination's start/end window.

This is *not* perfectly lock-free on either platform (both implementations may take a brief internal lock on load/store), but with sample swaps happening only on explicit user action it's well within real-time budget. If audio-thread contention ever shows up under stress, the upgrade path is a wait-free SPSC handoff queue rather than a different shared_ptr.

Pad sample file paths are persisted alongside the parameter state under a `<samples>` child ValueTree in `getStateInformation`. `setStateInformation` re-reads the audio files on whichever thread the host calls it on; a path that no longer resolves is **kept and flagged `missing`** rather than dropped, so the reference survives a re-save and `relinkMissingSamples()` can repair it by filename from a folder the user picks. Dropping it would quietly lose the user's kit.

Editor-initiated loads go through `loadSampleForPadLayerAsync`, which reads on a single-threaded `juce::ThreadPool` and applies the result back on the message thread. A synchronous read would freeze the host, which is far worse without sight — there is nothing to look at that says "still working" — so both the start and the outcome are announced. The pool is drained in the destructor because its jobs capture `this`.

### Time-stretch and formant (offline rendering)

A sampler's playback rate *is* its pitch control, so resampling can never separate pitch from formants or from duration. Breaking that lock needs spectral analysis and resynthesis (`source/engine/PhaseVocoder.*`), which is far too heavy to run per voice per block.

So it runs **offline**: when a pad's `Stretch Mode` / `Tempo Sync` / `Formant` settings imply non-neutral processing, the pad renders a processed copy of its sample on the loader thread, and voices play that copy at ratio 1.0. Consequences:

- A `juce::Timer` on the processor polls `desiredStretchFor(pad)` and kicks off renders when the target changes. It's a timer rather than a parameter listener because the target also depends on **host tempo**, which no parameter callback reports.
- `PadLayer::processed` holds the render (null = play the raw sample). `renderGeneration` makes a superseded render discard its result instead of overwriting a newer one.
- **Anything that swaps a layer's audio must clear `processed` and reset `renderedTimeRatio`/`renderedFormant`** — load, reset, kit load, paste, clear-all, state restore. A stale render keeps playing the *previous* sample, which is close to impossible to diagnose by ear.
- `renderStretchNow()` forces the work synchronously; tests use it, and the editor's "Render stretch" button gives the user a way to not wait.
- Repitch mode (the default) renders nothing, so the classic behaviour stays free.

**Per-note formant correction** (`Formant Correct`, Keyboard mode only) needs one more step, because a single render cannot follow a pitch that changes per note. The trick: the correction for a note *k* semitones from the root is always the same thing — a formant shift of *−k* — and a keyboard only has a few dozen distinct values of *k*. So the layer pre-renders a `FormantTable`, one variant per semitone across its playable range, indexed by `offset + kMaxFormantOffset` for an O(1) array lookup at note-on. Notes outside the table fall back to the uncorrected sample rather than going silent.

Two bounds worth knowing: the range is clamped to ±24 semitones, and a job whose (sample seconds × variant count) exceeds a budget is **refused with an explanation** instead of quietly allocating hundreds of megabytes. With `LayerMode::KeyZones` each layer spans only its own zone, so the two features compose cheaply rather than multiplying.

Correction covers the voice's **whole** pitch offset, not just the note interval. By the time the sample is chosen, `cfg.pitchSemitones` already carries the note interval, the `Pitch` parameter, Repitch-mode tempo sync and Vel→Pitch, so rounding it to the nearest semitone gives one index that corrects all of them at once. Rounding is what makes the continuous contributors tractable — pre-rendering every 0.01-semitone `Pitch` value is impossible, and half a semitone of residual formant error is inaudible.

`desiredFormantTableFor` therefore has to span every contributor when sizing the table: note range (zero for a Fixed pad), the `Pitch` parameter, the tempo-sync semitones, and zero-to-full `Vel→Pitch`. Rebuilds are **debounced by two timer ticks**, because sweeping the Pitch knob walks that range a semitone at a time and would otherwise queue a full render per step.

Quality-wise the vocoder does the three things that separate a usable stretcher from a naive one: **phase locking** (rotate each spectral peak's region rigidly, instead of letting bins drift apart into "phasiness"), **transient-aware phase reset** (take analysis phase verbatim at detected onsets, so attacks don't smear), and **cepstral envelope warping** for formants.

Two CMake targets matter:

- **`asampler_core`** — static library, pure engine code, no JUCE plugin entry points. Links `juce_audio_basics` / `juce_audio_formats` / `juce_dsp`. The test binary links this directly so tests don't drag in the VST3 wrapper.
- **`ASampler`** — `juce_add_plugin` target producing VST3. Links `asampler_core` + `juce::juce_audio_utils`.

Engine logic goes in `source/engine/` and is added to `asampler_core` sources in the top-level `CMakeLists.txt`. Anything that needs `JucePlugin_*` macros or `createPluginFilter()` lives in `source/PluginProcessor.*` and is on the `ASampler` plugin target instead.

## Parameter conventions

- IDs live in `namespace asampler::param_ids` in `PluginProcessor.h`: global params (`masterGain`, `chopCount`, `editLayer`, …) are `inline constexpr` strings; per-pad IDs are helper functions (`padVolume(int)`, `padPitch(int)`, …) that build `"pad_<n>_<field>"`. One central list keeps `ParameterID` versions consistent and diffs screen-reader-friendly.
- Parameters are owned by `juce::AudioProcessorValueTreeState state` on the processor. Use `state.getRawParameterValue(id)->load()` in `processBlock` (or the cached `padParams[]` pointers on the audio thread).
- Per-pad params are added under an `AudioProcessorParameterGroup` named `"Pad <n>"` (`makePadGroup`), so REAPER shows them grouped (e.g. "Pad 1 / Volume") — the child param's own name stays short ("Volume", "Pitch").
- Every parameter needs a human-readable `withStringFromValueFunction` (e.g. `"-6.00 dB"`, not `-6.0`). Screen readers read the formatted string — bare floats are useless to the user.
- **Write parameters through `setRangedParam`, not `setValueNotifyingHost`.** APVTS records undo history from its *ValueTree*, and it only copies parameter values into that tree on an internal timer. `setRangedParam` writes the tree directly (via `getParameterAsValue`), which is both synchronous and undoable; writing the parameter object means a batch action like auto-chop lands outside the transaction opened for it and cannot be undone as one step. `materialiseParameterValueProperties()` (called once at construction) exists for the same reason: APVTS creates each parameter node without a `value` property, and the first write to a missing property is deliberately *not* undoable.
- **Choice/enum params are append-only.** `AudioParameterChoice` (and the `ChopMode` / `LayerMode` / `GlideMode` / `PadMode` / `AftertouchDest` / `NoteRepeatRate` / `LoopMode` / `FilterMode` / `SaturationMode` / `LfoShape` / `LfoDest` enums) serialise by integer index — reordering or inserting a value silently corrupts saved project state. Only append new values at the end.

### Adding a per-pad parameter

Four places must move together, and the compiler catches none of them:

1. **`param_ids`** (`PluginProcessor.h`) — add the `pad<Field>(int)` ID helper.
2. **`makePadGroup`** (`PluginProcessor.cpp`) — create the parameter, with a range, a default that preserves existing kits' behaviour, and a `withStringFromValueFunction`.
3. **`padParamSpecs()`** — **append** an entry at the end. This one list drives the Focus proxy group, pad copy/paste (which keys by spec index) and the parity test; appending rather than inserting also keeps the Focus list order stable for a user navigating it by memory.
4. **`PadParamPtrs` + `cacheParamPointers()`** — add the cached `std::atomic<float>*` if the audio thread reads it. That pairing is the single source of truth for what `processBlock` may touch; never do a string-keyed lookup per block.

Then, if the value should audibly change a *held* note, route it through `VoiceLiveParams` / `Voice::setLiveParams` — a control that only takes effect on the next trigger reads as broken when you cannot see the value move. And if it swaps or invalidates a layer's audio, clear `processed` / `renderedTimeRatio` / `renderedFormant` (see the offline-render section).

## Platform

Development happens on Windows (the `.bat` scripts), but the plugin also builds for **macOS**. The developer has no Mac, so macOS is built only by **GitHub Actions** (`.github/workflows/build.yml`): on every push it clones JUCE and Catch2 at pinned versions, builds a universal (arm64 + x86_64, macOS 11+) VST3 and AU, runs the full test suite, and uploads zipped bundles. It runs the same three steps (`cmake configure → cmake --build → ctest`) rather than translating the batch files. **Bumping JUCE or Catch2 means updating `JUCE_VERSION` / `CATCH2_VERSION` in the workflow as well as the local SDK.**

Portability rules the Mac build depends on — MSVC accepts all of these, so a Windows-only build won't catch them:

- No `std::atomic<std::shared_ptr>` — use `AtomicSharedPtr` (see Sample loading).
- File paths go through `juce::File::getSpecialLocation` / `getChildFile`, never literals like `"C:/..."`, which are relative on macOS.
- Platform APIs sit behind `#if JUCE_WINDOWS` with a stub `else` that fails gracefully, like `VocalSynth.cpp`.

macOS bundles are only **ad-hoc signed**, not Developer ID signed or notarized. A downloaded copy is quarantined by Gatekeeper until the user runs `xattr -dr com.apple.quarantine <bundle>`. Proper signing needs a paid Apple Developer account and secrets added to the workflow.

The project is a **git repository** hosted on GitHub, since that is what drives the macOS build. `build/` and `.claude/settings.local.json` are ignored.

The vocal generator (`source/VocalSynth.*`) is a hard Windows dependency — it uses the SAPI5 COM API (`<sapi.h>`, `sapi.lib`, `ole32.lib`). It's guarded by `#if JUCE_WINDOWS` with a stub `else` branch that always fails, so the file stays compilable elsewhere; a real cross-platform port would swap in a platform TTS engine behind the same `synthesizeSpeechToWav` signature. Generated vocals are written to `%APPDATA%\ASampler\vocals\*.wav` (durable so pad sample paths round-trip through project save/load).
