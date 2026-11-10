# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What ASampler is

A VST3 sampler plugin for hip-hop / trap producers, built on JUCE 8. The **primary user is a blind producer using a screen reader (NVDA/JAWS) in REAPER on Windows.** Accessibility is the structural constraint that shapes every architectural decision — not a polish item at the end.

The MPC-style workflow is the model: `kNumPads` (32) pads in two banks of `kPadsPerBank` (16), choke groups for hi-hats, and a broad per-pad control surface. What's implemented per pad today (all as host parameters — see the `param_ids` block and `makePadGroup` in `PluginProcessor.*`):

- **Playback:** volume / pan / pitch / ADSR, sample start-end trim, reverse, one-shot (ignore note-off), loop (`LoopMode` off / forward / ping-pong with loop start-end).
- **Pitch envelope:** per-pad decay envelope on pitch (amount in semitones + time) — starts offset and slides back to target in ratio-space. Classic 808 / kick "punch". Feeds `pitchRatio` alongside glide/bend.
- **Filter:** per-pad `FilterMode` (off / LP / HP / BP) with cutoff + resonance, plus a dedicated **filter envelope** (`Filter Env Amount` in octaves, with its own ADSR) — the per-hit sweep that LFO/velocity modulation could not express.
- **Character (lo-fi):** post-filter `SaturationMode` (Tube / Tape / Hard Fold) + amount, bit-depth reduction (16 = off), and sample-rate reduction (downsample-hold factor, 1 = off). Backed by `engine/Saturation.h`; chain order is filter → saturation → downsample → bit-quantize.
- **LFOs:** two per pad — rate, depth, `LfoShape` (Sine / Triangle / Square / Saw), `LfoDest` (off / pitch / cutoff / volume / pan), and `LfoSync` (Off, or a note length from 4 bars to 1/32 incl. triplets/dotted; `lfoRateFor` turns it into Hz from host BPM, overriding Rate). Backed by `engine/Lfo.h`. The voice sums every source per destination, so two LFOs on one destination add.
- **Mod wheel:** CC 1 is handled by the processor (`currentModWheel`, pushed via `VoicePool::setModWheel`, smoothed over 20 ms in the voice). Per pad, `Mod Wheel Destination` (`ModDest`: off / pitch / cutoff / volume / pan / LFO 1 depth / LFO 2 depth) and a signed `Mod Wheel Amount`. The modulation block is skipped entirely when no source is routed (`Voice::updateModActive`).
- **Slice cleanup:** edge fade-in / fade-out (ms), forward-loop crossfade (ms), and a zero-crossing snap bool that nudges start/end (and loop) indices onto the nearest crossing at trigger time — click-free windows without a manual edit action.
- **Speed / Keep Length:** `Speed` (±100%, 0.5×–2×) multiplies the stretch time ratio at any Stretch Mode, so it renders offline like stretch. `Keep Length` (Keyboard pads) reuses the per-semitone `FormantTable`: each variant's time ratio is scaled by 2^(k/12) so every note lasts as long as the root, with the formant shift only if Formant Correct is also on (`FormantTableKey` carries both flags).
- **Tempo sync:** per-pad `Tempo Sync` bool + `Original BPM`, plus `Stretch Mode` (Repitch / Stretch). **Repitch** (default) scales the playback ratio by (host BPM / original BPM) so pitch moves with tempo — the classic MPC sound, and free. **Stretch** time-stretches instead, so a 90 BPM riff fits an 80 BPM beat at its original pitch.
- **Formant:** per-pad `Formant` (semitones) shifts the spectral envelope without touching pitch — the control resampling physically cannot provide, since there playback rate *is* pitch. `Formant Correct` additionally holds formants **fixed as the pad is played across the keyboard**, so a bass or vocal sample stops chipmunking as it goes up. See the offline-render section for how.
- **Gain staging:** per-layer gain trim (`Layer N Gain`), plus a Normalise action that sets that gain so the layer peaks at -1 dBFS — non-destructive, automatable and undoable, unlike rewriting the audio.
- **Velocity modulation:** beyond the amplitude `VelocityCurve`, per-hit velocity can also drive filter cutoff, pitch, and sample start (Vel→Cutoff / Vel→Pitch / Vel→Start), applied at trigger time. Vel→Pitch is added *after* the glide snapshot so it doesn't pollute the note-to-note glide delta.
- **Voicing:** choke group, `maxVoices` per-pad cap (1 = mono), glide/portamento (`GlideMode` off / legato / always / legato-rate / always-rate + glide time). The rate modes read Glide Time as **ms per semitone**, so one setting suits both a semitone step and an octave jump; `Vel to Glide` shortens the slide with velocity, giving per-note control from the piano roll. Glide interpolates linearly in **semitones**, not frequency ratio, so slides move at a constant musical rate.
- **Stereo:** per-pad `Stereo Width` (M/S, 0 = mono … 2 = double) and `Mono Below` (collapse everything under N Hz to centre) — the standard way to keep 808s and bass phase-coherent.
- **Note dispatch:** `Mode` (Fixed / Keyboard). Fixed is the strict MPC default (fires on the one assigned `Note`); Keyboard re-purposes `Note` as a root and responds across `[Low Note, High Note]`, pitching by (incomingNote − root) — ReaSamplOmatic-style, so an 808 pad can stretch across the keyboard while neighbouring pads stay on fixed drum notes in the same kit.
- **Velocity layers / multisampling:** up to `kNumLayers` (8) samples per pad. `LayerMode` selects how a trigger picks among loaded layers — Velocity, RoundRobin, Random, or **KeyZones**. In Velocity mode every loaded layer whose `Layer N Vel Low/High` range holds the hit plays (**stacking**; the voice cap becomes Max Voices × layers sounding), falling back to the nearest range; layers 1–4 default to the old four bands so existing kits sound the same, 5–8 to 0–127. Each layer also has `Trigger` (Press / **Release** — release layers are skipped at note-on and fired from `triggerReleaseLayers` after the note-off, at the stored note-on velocity, never looping) and `Transpose` / `Fine Tune`. `selectLayersForTrigger` picks, `planLayer` builds one layer's voice config. In KeyZones each layer has its own `Low/High/Root Note`, so one pad holds a real multisampled instrument instead of a single sample stretched (and chipmunked) across the keyboard; a note outside every zone falls back to the nearest-root loaded layer rather than going silent. `VelocityCurve` shapes incoming velocity.
- **Expression:** mute / solo (DAW-style gate), note-repeat (`NoteRepeatRate` quarter…32nd-triplet, tempo-synced to host BPM) with per-pad swing (delays odd off-beats) and humanize (timing + velocity jitter).

Global features: master gain, pitch-bend range, a global **Key + Scale** (any pad with `Scale Lock` on snaps incoming notes to it in Keyboard mode, so a melodic chop can't land off-key), **auto-chop** (slice the sample on one pad across the first N pads — `ChopMode` equal / transient-log-energy / transient-spectral-flux, backed by `engine/Transient.*`), **MIDI learn** (arm a param, move a CC, and it binds; 128 CC → parameter slots), and **Audition On Edit** (when a pad's Start/End settles after an edit, the pad plays from the new point — boundaries placed by ear; never while the transport runs). The plugin exposes a **Main stereo output bus plus one output bus per pad**, so pads can be routed to separate REAPER tracks.

Workflow features added on top of that: **undo/redo** (the APVTS owns a `juce::UndoManager`, so a chop across 32 pads is one undo step), **kits** (`.asamplerkit` files carrying the full parameter state plus sample paths, and four built-in factory kits — Trap / Boom Bap / Lo-Fi / 808 Set — that configure envelopes, choke groups, note layout and character but leave samples alone), **pad copy/paste** and Clear All, **slice-to-MIDI export** (writes a `.mid` with one note per slice rather than relying on VST3 drag-out, which isn't accessible), **pad audition** from the editor or the QWERTY keys `Z X C V / A S D F / Q W E R / 1 2 3 4`, **snap-to-transients** (nudges a pad's start/end onto the nearest detected onsets — zero-cross snap removes clicks, this fixes a boundary auto-chop placed slightly off the hit), **missing-sample recovery** (a moved project keeps its sample references, reports them as missing, and can relink them by name from a chosen folder), **sample browsing** (`browseSample`: next / previous audio file in the pad's folder, natural-sorted, keeping the pad's settings and announcing "name, N of M"), **Describe** (`describePad`, backed by `engine/SampleAnalysis.*`: length, peak/RMS, hit count, tempo, key or pitch — information a sighted user reads off a waveform), **bounce** (`bouncePad`: renders a pad through a `Voice` with all its settings to a 24-bit WAV under `%APPDATA%/ASampler/bounces` and loads it onto the next empty pad), and **factory kits as programs** (program 0 is "Current", programs 1… are the factory kits, so a host preset menu or program change selects them).

Added after the Sampler-Access comparison (all processor API first, editor on top, all tested in `tests/test_workflow.cpp` and `test_editor.cpp`):

- **Sample preview / try-before-you-keep** (`previewSample`, `keepPreview`, `cancelPreview`): a candidate is stored in `previewOverride[pad]` (an `AtomicSharedPtr`) and replaces the edit layer at trigger time, raw (no stretch render); `padHoldsLoadedSample` counts it and `referenceSample` / `getPadReferenceSeconds` measure slice points against it. `setLayerAudio` clears it, so any real load ends a preview. The pad is auditioned only while the transport is stopped.
- **Raw preview player** (`playPreview`): one buffer mixed onto Main after master gain, used for **slice scrubbing** (`previewSlicePoint`: 80 ms forward from Start, or the 150 ms before End reversed). Outgoing buffers are parked in `previewRetired` so the audio thread never drops the last reference.
- **Multi-load** (`loadSamples`: one per pad / round-robin layers / velocity layers, one loader job, one undo step), **kits with samples or as templates** (`KitSaveMode`; copies go to "<kit> samples" with a `relative` path that `resolveKitSamplePath` falls back to; a template has `template="1"` and leaves samples and names alone), **trim silence** (`trimSilence`, -30 dB onset / -60 dB tail; `Trim Silence On Load` global), **pad names** (`padNames`, saved in state/kits/copy-paste, cleared by Clear All), **apply to pads** (`applyControlToPads` / `applySectionToPads`, keyed by `padParamSpecs()` index), **note learn** (`armNoteLearn`: the audio thread swallows the next note-on and its note-off, the timer applies it in `processPendingNoteLearn`).
- **Live notes / MIDI out:** editor pad keys call `queueLiveNote` (a SPSC `AbstractFifo`); `processBlock` merges them into the input MIDI (after the note-learn filter) at the `Computer Key Velocity`, and **the block's MIDI output is exactly those notes** — host MIDI is never echoed. `producesMidi()` is true and `NEEDS_MIDI_OUTPUT` is set.
- **REAPER** (`source/ReaperIntegration.*`): `getVST3ClientExtensions()` returns an extension whose `setIHostApplication` queries `IReaperHostApplication` and fills a `ReaperApi` table of function pointers (null in other hosts; tests install fakes). `toggleReaperRecord` calls `CSurf_OnRecord`; `grabReaperItem` runs action **40257** (glue, ignoring time selection, including fades — verified in REAPER 7.82; 40606 would expand to a time selection), copies the glued file to `%APPDATA%/ASampler/grabbed`, and calls `Undo_DoUndo2` only if `GetProjectStateChangeCount` moved.

Removed on purpose before 1.0 (don't reintroduce without a reason): the SAPI5 vocal generator and channel-aftertouch routing. Kept on purpose despite being niche: MIDI learn, note repeat, layers, Formant Correct.

## Accessibility-first architecture

The single rule that drives everything:

> **Every controllable thing must be a standard `juce::AudioProcessorParameter` exposed to the host.**

REAPER's FX parameter list reads host parameters directly via screen reader without any custom GUI work — this is the accessibility floor. Internal-only state that should be user-tweakable but isn't a parameter is an accessibility bug, not a stylistic choice. Build a feature as a parameter first, then optionally add a custom UI control on top.

That rule scales badly on its own: the 146 per-pad controls x 32 pads are ~4,700 host parameters (4,830 with the Focus group and globals), and arrowing through hundreds of entries to reach "Pad 9 Filter Cutoff" is its own accessibility failure. The **Focus proxy group** (`source/FocusProxyParameter.h`) is the counterweight — a `Focused Pad` parameter plus one mirrored `Focus ...` parameter per per-pad control, forwarding to whichever pad is selected. The user navigates ~160 parameters instead of ~4,800.

How the list is organised (all of it driven by `padParamSpecs()`):

- **Name prefixes.** Per-pad params are named "P9 Filter Cutoff", proxies "Focus Filter Cutoff" — the name alone says which pad, because many hosts flatten groups.
- **Nested groups.** Each pad group (and the Focus group) is split into sections — Main, Trigger, Slice, Envelope, Filter, Character, Pitch and Glide, Modulation, Repeat, Time and Formant, Stereo, Layers — from the spec's `section` field (`pad_N_<section>` / `focus_<section>` group IDs).
- **Stable proxy IDs.** `FocusProxyParameter` is an `AudioProcessorParameterWithID` with ID `focus_<suffix>` (`focusProxyIdFor(spec)`), so its VST3 parameter hash is stable instead of index-derived; look proxies up by ID, never by position. Its text comes from the *focused* pad's formatter (slice points read as time in that pad's sample).
- **Focus moves are not undo steps.** `setFocusedPad` writes the tree value without an UndoManager before setting the parameter (APVTS's flush only records a change when tree and parameter differ). Walking the pad grid must not fill the undo history.
- **Bool params are `SnappedBoolParameter`**, which reports 0/1 from `getValue()`. Plain `AudioParameterBool` keeps a raw 0.235 a host wrote while APVTS reads it as 0, so a state restore skips the write and the host list disagrees with the project (pluginval strictness 10 catches this). **Any new per-pad parameter must be added to `padParamSpecs()` in `PluginProcessor.cpp` as well as `makePadGroup`** — a test asserts the two stay in step, because a parameter missing from the Focus group is reachable only by scrolling the full per-pad list.

Consequences worth knowing before editing:

- The editor (`source/PluginEditor.*`) is keyboard-first: a heading, a 4x4 pad grid showing one bank (Pad 1 bottom-left, one Tab stop, arrows/Home/End move and select, Up from the top row crosses into bank B, Page Up/Down switch bank from anywhere and announce it, Space/Enter play; all 32 `PadButton`s exist and only the current bank's are visible), pad-action buttons whose labels name the selected pad, kit actions, and a status line. Shortcuts: L load (multi-select → pads-or-layers menu), N/P browse, Shift+N/P try a sample, T/O edit Start/End by ear, K note learn, I describe, B bounce, F2 rename, Ctrl+S/O kits, Ctrl+V paste files (`ClipboardFiles.*`, CF_HDROP), Ctrl+digits jump to a pad on Ctrl release (`modifierKeysChanged`), Ctrl+R/G REAPER, Ctrl+Z/Y undo/redo, F1 help, and the QWERTY pad keys (current bank — `padForKey(key, bank)`), which send live note-on while held and note-off from `keyStateChanged`; Shift + pad key speaks the pad. **Edit modes** (`ASamplerEditor::Mode`: EditStart / EditEnd / TrySample) own arrows / Enter / Escape / Space while active — `handleModeKey` runs first in both `keyPressed` and `PadButton::onKeyFirst`. Files can be dropped on a pad (`FileDragAndDropTarget`). It is built from `juce::TextButton` and `juce::Label` — both expose Windows UIA accessibility roles automatically. Any new custom component must set `setTitle()` / `setDescription()` and, if it's not a built-in JUCE widget, override `createAccessibilityHandler()`. Prefer JUCE's built-in `Slider` / `Button` / `ComboBox` over custom-drawn knobs.
- After any user-visible state change, announce it so the screen reader speaks even when focus didn't move — set the processor status (`setStatus`); the editor's change listener routes it through `ASamplerEditor::announce`. Announcements go through **`ScreenReaderAnnouncer`** (`source/ScreenReaderAnnouncer.*`), which raises a **UIA notification event** (`UiaRaiseNotificationEvent`, loaded dynamically) from a hidden child HWND so NVDA/JAWS/Narrator speak it in the user's own voice and settings. JUCE's `postAnnouncement` uses a separate SAPI voice on Windows, which talks over the screen reader; it remains the fallback (and the macOS path, where it maps to VoiceOver).
- Sample selection uses `juce::FileChooser::launchAsync` (native OS dialog, fully accessible). Never roll a custom file browser.
- **Announcements are deduplicated and prioritised** (`ASamplerEditor::announce`). An identical repeat is dropped — bursty actions fire the change listener many times for one logical event, and a queue of identical speech is something the user has to sit through before they can act. Failures use `AnnouncementPriority::high` so they interrupt; routine confirmations use `medium` so they don't talk over the user.
- **Destructive actions say what they will replace, and are undoable.** Chop button labels name the pads they overwrite; Clear All confirms via `NativeMessageBox` first. Losing 32 pads is recoverable for a sighted user who can see it happen — it is not otherwise.
- Live parameter changes must reach sounding voices (`VoiceLiveParams` / `Voice::setLiveParams`). A knob that produces no audible change on a held note reads as a broken plugin when you cannot see the value move.
- Note names use **middle C = C3** (MIDI 36 = C1, the default Pad 1 note), matching REAPER and MPC convention; tests assert names, so keep it consistent (`describePadTrigger`, `SampleAnalysis::describePitch`).
- Slice points (Start/End/Loop Start/Loop End) are normalised 0..1 internally but **read and parse as time** in the pad's sample (`formatSlicePoint` / `parseSlicePoint`: "1.250 s", "480 ms", "50%"). The timer calls `updateHostDisplay(parameterInfoChanged)` when a pad's length changes so hosts refresh the text.
- Build targets are **VST3** on every platform plus **AU** on macOS (JUCE drops AU elsewhere) — load the plugin in REAPER to test the GUI with a screen reader. A Standalone format was previously built as a no-DAW accessibility harness; if that workflow is needed again, add `Standalone` back to the `FORMATS` list in `CMakeLists.txt` (and re-add the artefact path to the "Build outputs" section below).

## Build & test

### Common commands

- **Dev build (RelWithDebInfo) + tests:** `build.bat`
- **Release build + tests:** `build_release.bat`
- **Reconfigure from scratch:** delete the `build\` directory, then re-run a build script
- **Build only (skip ctest):** `cmake --build build --config RelWithDebInfo`
- **Rebuild the VST3 bundle:** `--target ASampler_VST3`. Building `ASamplerTests` (or `ASampler`) refreshes the shared code but not the `.vst3` a host loads.
- **Build one target** (the fast loop — `PluginProcessor.cpp` is ~4.5k lines, a full build is slow): `cmake --build build --config RelWithDebInfo --target ASamplerTests` (or `asampler_core`, `ASampler`). `ASamplerTests` depends on the `ASampler` target (`add_dependencies` in `tests/CMakeLists.txt`) because it links the generated `ASampler_SharedCode.lib`; without that, a test-only build silently links a stale processor.
- **Validate like a host:** [pluginval](https://github.com/Tracktion/pluginval) `--strictness-level 10 --validate-in-process --output-dir <dir> --validate build\ASampler_artefacts\RelWithDebInfo\VST3\ASampler.vst3`. On Windows it is a GUI-subsystem exe that prints nothing — read the log in `--output-dir` and the exit code. CI runs it on both platforms.
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

~350 Catch2 v3 `TEST_CASE`s across 19 files, all tagged. The tags are the practical index into the suite — `[plugin]` (processor-level, the bulk), `[voice]`, `[vocoder]`, `[state]` (save/restore round-trips) and `[robustness]` (damaged/foreign state), `[stretch]` / `[formantcorrect]` / `[formantpitch]`, `[focus]` and `[accessibility]` (the Focus-proxy parity checks), `[editor]` (a real editor driven by key presses), `[announce]` (UIA notifications end to end), `[analysis]`, plus one per feature area (`[chop]`, `[glide]`, `[keyzones]`, `[layers]`, `[workflow]`, `[preview]`, `[scrub]`, `[reaper]`, `[midilearn]`, `[noterepeat]`, `[undo]`, `[browse]`, `[describe]`, `[bounce]`, `[programs]`, `[groups]`, `[labels]`, `[slicetime]`, `[editaudition]`, …). `[.benchmark]` is hidden (CPU timing for all `kNumVoices` at root and +12), as is `[.stretchbench]` (phase-vocoder timing) — run it explicitly. Tag a new test with the feature area and, if it is processor-level, `[plugin]` as well.

Conventions the existing tests share, worth copying:

- Processor-level tests construct a real `ASamplerProcessor` on the stack and drive it through its public API — there is no mock layer.
- Tests set parameters through a local `setParam` helper using `setValueNotifyingHost`. That is deliberate and does *not* contradict the production rule below (`setRangedParam`): tests are asserting DSP/dispatch behaviour, not undo history. Tests that assert undo behaviour go through the processor's own action methods instead.
- Audio fixtures are generated, not checked in: a `writeSineWav`-style helper writes a short WAV into the system temp dir and the test loads it back.
- Anything that depends on the machine's environment degrades rather than fails — the UIA announcer test `SKIP`s off Windows or without a desktop. `catch_discover_tests` sets `SKIP_RETURN_CODE 4` so ctest reports those as skipped, not failed.

## Project layout

```
CMakeLists.txt              — top-level: JUCE/Catch2 wiring, plugin target, core lib
source/
  PluginProcessor.h/.cpp    — JUCE AudioProcessor, parameter layout, MIDI dispatch, sample loading,
                              undo, kits/programs, copy/paste, slice-MIDI export, audition,
                              browse, describe, bounce
  FocusProxyParameter.h     — host parameter that forwards to the focused pad (the Focus group)
  PluginEditor.h/.cpp       — keyboard-first editor: pad grid, pad/kit action buttons, status line
  ScreenReaderAnnouncer.h/.cpp — speaks status through UIA notifications (Windows), JUCE announcement elsewhere
  ReaperIntegration.h/.cpp  — ReaperApi function table, filled from IReaperHostApplication via VST3ClientExtensions
  ClipboardFiles.h/.cpp     — files copied in Explorer (CF_HDROP), or file paths as clipboard text
  engine/                   — DSP / sampler engine (built as static lib asampler_core)
    AtomicSharedPtr.h       — portable atomic shared_ptr (std::atomic on MSVC, free-function fallback on libc++)
    Sample.h                — AudioBuffer + source sample rate
    Voice.h/.cpp            — one playing voice: pitch (Catmull-Rom cubic; band-limited polyphase windowed-sinc above +2 st — see below), pan, ADSR, filter, loop, glide; holds shared_ptr to its source
    VoicePool.h/.cpp        — fixed-size pool, per-pad trigger/release/choke/gate/render (incl. per-pad-bus routing)
    DefaultSample.h         — generates a built-in A4 sine pluck used until the user loads a real sample
    VelocityCurve.h         — velocity-shaping curves (header-only)
    FilterMode.h            — per-pad filter type enum + labels (header-only)
    LoopMode.h              — loop mode enum + labels (header-only)
    Saturation.h            — saturation modes + bit-depth reduction (header-only)
    Lfo.h                   — LFO shapes/destinations + waveform eval (header-only)
    Transient.h/.cpp        — onset detection (log-energy + spectral-flux) for transient-mode auto-chop
    SampleAnalysis.h/.cpp   — Describe: peak/RMS, hit count, tempo (autocorrelation), key (chroma +
                              Krumhansl, with a tonalness gate so noise reports no key), pitch (YIN)
    PhaseVocoder.h/.cpp     — offline time-stretch + formant shift (STFT with phase locking,
                              transient-aware phase reset, cepstral envelope warping, stereo linking)
tests/
  CMakeLists.txt            — test exe wiring (catch_discover_tests); links asampler_core + ASampler_SharedCode
  test_*.cpp                — Catch2 v3 tests (voice, voice_live, voice_pool, velocity_curve,
                              saturation, lfo, transient, plugin_processor, production, melodic,
                              phasevocoder, formant_correct, formant_pitch, release, analysis,
                              announcer, editor, layers, workflow)
docs/manual.html            — complete user manual (screen-reader-first; installed with the plugin), incl. a
                              parameter reference — regenerate the facts with `ASamplerTests.exe "[.paramdump]"`
                              (writes a TSV of every global/Focus/Pad 1 parameter's range and default)
scripts/ASampler - Create pad tracks.lua — REAPER ReaScript: one routed track per pad (see Output routing)
CHANGELOG.md
installer/ASampler.iss      — Inno Setup script for the Windows installer (built in CI)
README.md
build.bat / build_release.bat
.github/workflows/build.yml — CI: Windows and macOS builds, tests, pluginval, installers (.exe / .pkg)
                              and zipped bundles; a v* tag publishes a GitHub release
```

The processor-level tests construct the real `ASamplerProcessor` (and `test_editor.cpp` a real editor), so the test target links `ASampler_SharedCode` (the JUCE-generated static lib carrying the `JucePlugin_*` defines) in addition to `asampler_core` — see `tests/CMakeLists.txt` for the artefacts-dir link path that makes that resolve. `JucePlugin_*` macros aren't visible to test sources; use `ASamplerProcessor::getVersionString()` and similar accessors.

### MIDI → audio flow

`processBlock` first filters a note-learn capture out of the input, merges queued live notes (computer keys) into it, then runs MIDI learn (`handleMidiLearn`), then `buildCombinedMidi` merges the host MIDI with any synthetic note-repeat triggers, then queued auditions (editor / Audition On Edit) are added — audition notes are held for the window length, max 4 s — then it splits the block at each event so notes start sample-accurately. Per event: note-on scans all pads — a Fixed pad matches when the incoming note equals its `Note`; a Keyboard pad matches when the note is within `[Low Note, High Note]`, and pitches by (note − root). Where they overlap, **one note sounds one thing**: a Fixed pad holding a loaded sample (`padHoldsLoadedSample`, a pointer compare against `defaultSample`, audio-thread safe) keeps its note and the Keyboard pad skips it; a Fixed pad still on the built-in tone steps aside (`keyboardPadCovers`). Without that, a Keyboard pad at its default full range doubled every drum — and with 32 pads the default notes (36–67) reach well into the keyboard. Every match triggers a voice. All three note scans (note-on, note-off, note-repeat hold) go through `padRespondsToNote`, so they can't disagree. Note-off releases via `releasePad` (Fixed: release all voices on the pad) or `releasePadNote` (Keyboard: release only voices triggered by that incoming note). Choke groups hard-stop other pads in the same group before triggering. The per-trigger decisions (layer, sample snapshot, pitch, velocity mods, formant variant) live in `planTrigger`, which `bouncePad` reuses so a bounce sounds like a live hit. `handlePitchBend` pushes bend onto active voices. The pool holds `kNumVoices` = `kNumPads * 8` = 256 voices, so every pad can sit at the per-pad cap at once. Per the `[.benchmark]` test at 44.1 kHz: all 256 at root ~32% of one core; at +12 (sinc path) ~89%.

**Interpolation.** At speed ≤ `kSincMinSpeed` (1.12, ~+2 st) the voice reads with Catmull-Rom cubic. Above it, it reads through a band-limited windowed-sinc kernel (Kaiser β 6.5, 16 zero crossings, cutoff 0.88/speed, speed capped at 4) so pitched-up content is filtered instead of aliasing (the `[interpolation]` tests require aliases < −40 dB). Kernels are precomputed as static polyphase banks (128 phases, linear blend between phases, banks spaced 2% in speed, each phase normalised to unity gain), built once in `Voice::prepare`. `besselI0` is a local series because libc++ lacks `std::cyl_bessel_i`.

Output routing: voices render through the per-pad-bus overload of `VoicePool::renderAdding`, one destination buffer per pad, falling back to Main for disabled pad buses. Pad buses are declared disabled by default; REAPER reports all 66 output pins but only activates a pad bus once its pins are connected, so an ordinary stereo track hears everything on Main. The REAPER script widens the track to 2 + 2N channels, maps output pins 1:1 (`TrackFX_SetPinMappings`), and adds a post-FX/pre-fader send from channel pair 2p to a new "ASampler Pad p" track; it skips pads that already have a send, so re-running is safe. This was verified in a real REAPER instance by rendering stems (Pad 1/2 audible only on their own tracks, untouched pads still on Main). Mute/solo is resolved per block by `resolvePadGate` and pushed to voices as a 0/1 gate (voices keep advancing when gated — DAW-style mute).

Parameter atomics (`std::atomic<float>*`) are cached at construction in `padParams[]` (`PadParamPtrs`) to avoid per-block string-keyed lookups in the audio thread. If you add a new per-pad parameter, also extend `PadParamPtrs` and `cacheParamPointers()` — that pairing is the single source of truth for what the audio thread reads. Global params have their own cached pointers (`masterGainParam`, `chopCountParam`, etc.).

The default sample is regenerated in `prepareToPlay` at the host rate so `Voice`'s rate-compensation path doesn't have to interpolate it. Pads that still hold the default sample get re-pointed at the new-rate buffer; pads with user-loaded samples keep their own.

### Sample loading & lifetime

Each pad holds `kNumLayers` (8) layers; each layer stores its sample in an `AtomicSharedPtr<const Sample>` (`engine/AtomicSharedPtr.h`). That is C++20's `std::atomic<std::shared_ptr>` where the standard library provides it (MSVC), and a `std::atomic_load` / `std::atomic_store` wrapper where it doesn't (Apple's libc++). **Don't declare `std::atomic<std::shared_ptr<...>>` directly** — it compiles on Windows and breaks the macOS build. The message thread `store()`s a new sample; the audio thread picks a layer (per the pad's `LayerMode`) and `load()`s a snapshot in the note-on path, passing it to `VoicePool::triggerPad`, which moves it into the `Voice`. The voice retains its own strong reference for the rest of playback — swapping the pad's sample while a voice is mid-render is safe.

The editor loads into whichever layer the **Edit Layer** host parameter selects (`loadSampleForPad`), or an explicit index (`loadSampleForPadLayer`). Auto-chop (`chopAcrossPads`) distributes one pad's sample across the first N pads, setting each destination's start/end window.

This is *not* perfectly lock-free on either platform (both implementations may take a brief internal lock on load/store), but with sample swaps happening only on explicit user action it's well within real-time budget. If audio-thread contention ever shows up under stress, the upgrade path is a wait-free SPSC handoff queue rather than a different shared_ptr.

Pad sample file paths are persisted alongside the parameter state under a `<samples>` child ValueTree in `getStateInformation`. `setStateInformation` re-reads the audio files on whichever thread the host calls it on; a path that no longer resolves is **kept and flagged `missing`** rather than dropped, so the reference survives a re-save and `relinkMissingSamples()` can repair it by filename from a folder the user picks. Dropping it would quietly lose the user's kit.

Editor-initiated loads (`loadSampleForPadLayerAsync`, and browsing) go through `startAsyncLoad`, which reads on a single-threaded `juce::ThreadPool` and applies the result back on the message thread via `callAsync`. **Every `callAsync` from a pool job must capture `std::weak_ptr<bool> weakAlive = alive` and bail if it expired** — the destructor drains the pool, but a completion already queued on the message thread still runs after the processor is gone (pluginval caught this as a crash in render completions). A synchronous read would freeze the host, which is far worse without sight — there is nothing to look at that says "still working" — so both the start and the outcome are announced. The pool is drained in the destructor because its jobs capture `this`.

### Time-stretch and formant (offline rendering)

A sampler's playback rate *is* its pitch control, so resampling can never separate pitch from formants or from duration. Breaking that lock needs spectral analysis and resynthesis (`source/engine/PhaseVocoder.*`), which is far too heavy to run per voice per block.

So it runs **offline**: when a pad's `Stretch Mode` / `Tempo Sync` / `Formant` settings imply non-neutral processing, the pad renders a processed copy of its sample on `renderPool` (`renderThreadCount()` = clamp(cores − 1, 1, 4) threads, separate from the loader pool), and voices play that copy at ratio 1.0. Consequences:

- A `juce::Timer` on the processor polls `desiredStretchFor(pad)` and kicks off renders when the target changes. It's a timer rather than a parameter listener because the target also depends on **host tempo**, which no parameter callback reports.
- `PadLayer::processed` holds the render (null = play the raw sample). `renderGeneration` / `tableGeneration` (atomics) make a superseded render discard its result instead of overwriting a newer one — and, through `PhaseVocoder::processOffline`'s `shouldAbort` callback (polled every 32 frames), stop mid-render rather than finish work nobody will hear. The timer runs every 100 ms.
- **Anything that swaps a layer's audio must clear `processed` and reset `renderedTimeRatio`/`renderedFormant`** — load, reset, kit load, paste, chop, clear-all, state restore. All of these go through `setLayerAudio(pad, layer, sample, path, missing)`, which also drops the formant/keep-length table and any preview; use it rather than storing into a layer directly. A stale render keeps playing the *previous* sample, which is close to impossible to diagnose by ear.
- `renderStretchNow()` forces the work synchronously; tests use it, and the editor's "Render stretch" button gives the user a way to not wait.
- `setStateInformation` rejects a root of the wrong type (status "Could not read the saved ASampler state…"), drops non-finite PARAM values, re-materialises `value` properties and clears undo history — a damaged chunk must never reach the audio thread as NaN.
- Repitch mode (the default) renders nothing, so the classic behaviour stays free.

**Per-note formant correction** (`Formant Correct`, Keyboard mode only) needs one more step, because a single render cannot follow a pitch that changes per note. The trick: the correction for a note *k* semitones from the root is always the same thing — a formant shift of *−k* — and a keyboard only has a few dozen distinct values of *k*. So the layer pre-renders a `FormantTable`, one variant per semitone across its playable range, indexed by `offset + kMaxFormantOffset` for an O(1) array lookup at note-on. Notes outside the table fall back to the uncorrected sample rather than going silent.

Two bounds worth knowing: the range is clamped to ±24 semitones, and a job whose (sample seconds × variant count) exceeds a budget is **refused with an explanation** instead of quietly allocating hundreds of megabytes. With `LayerMode::KeyZones` each layer spans only its own zone, so the two features compose cheaply rather than multiplying.

Correction covers the voice's **whole** pitch offset, not just the note interval. By the time the sample is chosen, `cfg.pitchSemitones` already carries the note interval, the `Pitch` parameter, Repitch-mode tempo sync and Vel→Pitch, so rounding it to the nearest semitone gives one index that corrects all of them at once. Rounding is what makes the continuous contributors tractable — pre-rendering every 0.01-semitone `Pitch` value is impossible, and half a semitone of residual formant error is inaudible.

`desiredFormantTableFor` therefore has to span every contributor when sizing the table: note range (zero for a Fixed pad), the `Pitch` parameter, the tempo-sync semitones, and zero-to-full `Vel→Pitch`. Variants render in parallel on `std::thread`s. Rebuilds are **debounced by three timer ticks** (300 ms), because sweeping the Pitch knob walks that range a semitone at a time and would otherwise queue a full render per step.

Quality-wise the vocoder does the things that separate a usable stretcher from a naive one: **phase locking** (rotate each spectral peak's region rigidly, instead of letting bins drift apart into "phasiness"), **transient-aware phase reset** (take analysis phase verbatim at detected onsets, so attacks don't smear), **cepstral envelope warping** for formants, and **stereo linking**. For speed it packs two channels into one complex FFT (analysis and resynthesis), computes two channels' envelopes in one pass (`EnvelopeWork`), and synthesises as analysis bin × a shared rotor, so there is no per-channel trig (10 s stereo: stretch ~120 ms, formant ~180 ms). All channels share one phase *rotation* per bin (synthesis phase = the channel's own analysis phase + rotation; rotation advances by trueFreq × (hopS − hopA)), so inter-channel phase comes out exactly as it went in. True frequency is estimated from the mid signal, or the side when louder; peaks for locking are picked on the summed channel magnitudes. Processing channels independently collapsed the correlation of two instruments panned apart from 0.68 to 0.22 — the `[stereo]` tests guard this. The analysis window is ~46 ms at any rate (`fftOrderFor`: 2048 at 44.1/48 kHz, 4096 at 88.2/96, 8192 above).

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
- **Choice/enum params are append-only.** `AudioParameterChoice` (and the `ChopMode` / `LayerMode` / `GlideMode` / `PadMode` / `NoteRepeatRate` / `LoopMode` / `FilterMode` / `SaturationMode` / `LfoShape` / `LfoDest` enums) serialise by integer index — reordering or inserting a value silently corrupts saved project state. Only append new values at the end.

### Adding a per-pad parameter

Four places must move together, and the compiler catches none of them:

1. **`param_ids`** (`PluginProcessor.h`) — add the `pad<Field>(int)` ID helper.
2. **`makePadGroup`** (`PluginProcessor.cpp`) — create the parameter, with a range, a default that preserves existing kits' behaviour, and a `withStringFromValueFunction`.
3. **`padParamSpecs()`** — **append** an entry at the end. This one list drives the Focus proxy group, pad copy/paste (which keys by spec index) and the parity test; appending rather than inserting also keeps the Focus list order stable for a user navigating it by memory.
4. **`PadParamPtrs` + `cacheParamPointers()`** — add the cached `std::atomic<float>*` if the audio thread reads it. That pairing is the single source of truth for what `processBlock` may touch; never do a string-keyed lookup per block.

Then, if the value should audibly change a *held* note, route it through `VoiceLiveParams` / `Voice::setLiveParams` — a control that only takes effect on the next trigger reads as broken when you cannot see the value move. And if it swaps or invalidates a layer's audio, clear `processed` / `renderedTimeRatio` / `renderedFormant` (see the offline-render section).

## Platform

ASampler is released for **Windows and macOS**, but the developer has no Mac: no Mac build has ever been tried in a host or with VoiceOver. **Customer-facing material (README, `docs/manual.html`, CHANGELOG, release notes) must say so wherever it describes the Mac version** — never present it as tested. macOS is built only by **GitHub Actions** (`.github/workflows/build.yml`): on every push it clones JUCE and Catch2 at pinned versions, builds a universal (arm64 + x86_64, macOS 11+) VST3 and AU, runs the full test suite and pluginval (`--skip-gui-tests`), and uploads a `.pkg` plus zipped bundles. Pushing a `v*` tag runs the `release` job, which checks the tag against the `project()` version and publishes both platforms' files with that version's CHANGELOG section as notes. It runs the same three steps (`cmake configure → cmake --build → ctest`) rather than translating the batch files. **Bumping JUCE or Catch2 means updating `JUCE_VERSION` / `CATCH2_VERSION` in the workflow as well as the local SDK.**

Portability rules the Mac build depends on — MSVC accepts all of these, so a Windows-only build won't catch them:

- No `std::atomic<std::shared_ptr>` — use `AtomicSharedPtr` (see Sample loading).
- File paths go through `juce::File::getSpecialLocation` / `getChildFile`, never literals like `"C:/..."`, which are relative on macOS.
- Platform APIs sit behind `#if JUCE_WINDOWS` with an `else` that degrades gracefully, like `ScreenReaderAnnouncer.cpp`.

macOS bundles are only **ad-hoc signed**, not Developer ID signed or notarized. A downloaded copy is quarantined by Gatekeeper until the user runs `xattr -dr com.apple.quarantine <bundle>` (the README and manual give the command). Proper signing needs a paid Apple Developer account and secrets added to the workflow.

The project is open source (GPL-3.0-or-later, `LICENSE`; JUCE under its AGPLv3 option) at **github.com/hermis501/asampler**; GitHub Actions is what builds the Mac version. `build/` and `.claude/settings.local.json` are ignored.

`ScreenReaderAnnouncer` is the one Windows-specific piece: the UIA path sits behind `#if JUCE_WINDOWS` and the other platforms use JUCE's announcement. Bounces are written to `%APPDATA%/ASampler/bounces` (via `getSpecialLocation`, so the Mac equivalent works) and are durable so pad sample paths round-trip through project save/load.

Release packaging lives in CI: the Windows job builds an Inno Setup installer (`installer/ASampler.iss`, VST3 into `{commoncf64}\VST3`, plus the manual, changelog and the REAPER script into `{app}\REAPER`. When REAPER has been run on the account, a `reaperaction` task (checked by default) also copies the script into `{userappdata}\REAPER\Scripts\ASampler` and appends to `reaper-kb.ini` the exact `SCR 4 0 RS8fbce… "Custom: …" "ASampler/…"` line REAPER itself writes for that path, unless a line for the script already exists (a hand-loaded one may carry a shortcut). Uninstall removes every `SCR` line naming the script plus the `KEY` lines bound to their IDs. The ini is edited as raw bytes so other entries survive unchanged, and only while REAPER is closed (`FindWindowByClassName('REAPERwnd')`, Retry/Cancel), because REAPER rewrites it from memory. Verified against REAPER 7.82: an externally written line loads as the action, with or without an existing ini) and the macOS job a `pkgbuild`/`productbuild` package (VST3 + AU into `/Library/Audio/Plug-Ins`, bundles marked non-relocatable). The version comes from the `project(ASampler VERSION …)` line — bump it there.
