# Project plan

## Vision

Build a pattern-first digital audio workstation with a fast MIDI workflow and a reliable audio engine. FL Studio, Reaper, and Ardour are product references; parity is a long-term goal, not a claim about the current build.

## Shipped foundation

The current Qt 6 executable provides a multi-track Playlist with movable MIDI and imported or recorded WAV audio clips, standalone or during-recording live audio input monitoring to Master or through an armed Audio track's mixer path, with per-device manual placement compensation, Linux ALSA, Windows WinMM, and macOS CoreMIDI backends for MIDI input monitoring and recording with per-note channel preservation, per-track Synth ADSR envelopes, seven factory synth presets plus reusable user patches saved in app data with import/export and removal, and selectable resonant filters with bipolar cutoff-envelope modulation, three-band EQ with adjustable crossovers, seven parametric bands with a draggable response graph, and volume/pan and synth filter cutoff/resonance keyframe automation, Delay/Reverb/Compressor/Low-pass Filter inserts on tracks, buses, and Master, mixer submix buses with nested/group routing and multiple track and bus auxiliary sends, per-bus controls and peak meters, per-clip piano rolls, waveform display, audio edge trimming with selectable linear, smooth, equal-power, custom-power, drawn, and draggable cubic Bezier fade curves, a 16-step drum sequencer with four assignable sample pads, five velocity layers per pad, per-pad waveform start editing, and independent per-layer tuning, loop, and ADSR overrides, editable MIDI clip lengths and common time signatures, MIDI import/export, project save/open, WAV export, and optional FLAC export. Audio media is loaded into memory and referenced by path. See [FEATURE_PARITY_TRACKER.md](FEATURE_PARITY_TRACKER.md) for exact boundaries.

## Roadmap

### 1. Pattern workflow

- [x] Buildable desktop application and event loop
- [x] Piano-roll note entry, movement, resizing, deletion, and velocity editing
- [x] Quantize selected or all piano-roll note starts to the current snap grid
- [x] Straight, dotted, and triplet piano-roll snap divisions
- [x] Highlight selectable key/scale pitches and optionally snap new piano-roll notes to the scale
- [x] Piano-roll chord insertion for common triads, suspended chords, and sevenths
- [x] Toggleable piano-roll ghost notes from other MIDI clips on the same track
- [x] Basic ascending arpeggiation for selected chord notes on the current grid
- [x] Exact single-note pitch, start, duration, velocity, and channel properties
- [x] Upward strumming for selected chord notes
- [x] Arpeggiator direction, octave-range, and gate controls
- [x] Arpeggiator velocity patterns
- [x] Built-in synth preview and arrangement playback
- [x] Project save/open and stereo WAV export
- [x] Optional FLAC full-arrangement and beat-range export at 16-bit or 24-bit PCM
- [x] Playlist with multi-track MIDI clips and basic mixer controls
- [x] 16-step sequencer and synthesized drum rack
- [x] Editable clip length and common time signatures
- [x] Standard MIDI File import (formats 0–2 with PPQN or SMPTE) and format 0/1 export; format 2 sequences are arranged consecutively with tempo maps
- [x] Count-in and metronome
- [x] Linear tempo ramps between Playlist tempo markers with exact beat/time conversion and WAV rendering
- [x] WAV audio import, Playlist playback, waveform display, and WAV render integration
- [x] Audio region edge trimming with linear, smooth, equal-power, custom-power, and freehand drawn fades
- [x] One WAV sample assignment per drum rack pad with synthesized fallback
- [x] Per-pad sample waveform display with draggable start-position editing
- [x] Per-track Synth ADSR envelope controls in live playback and render
- [x] Undoable per-track volume and pan keyframes with linear interpolation in live playback and WAV render
- [x] Mono/stereo WAV input recording with count-in, optional beat-range punch recording, per-input manual placement compensation, monitoring through Master or an armed Audio track, and direct recording to the armed track
- [x] Live MIDI monitoring and basic note recording from ALSA on Linux, WinMM on Windows, and CoreMIDI on macOS
- [x] Learnable MIDI CC and NRPN mappings for master level, selected-track level/pan/mute/solo, synth waveform and oscillator 2 enable/waveform/detune/mix, synth-filter enable/type/cutoff/resonance, synth amplitude-envelope ADSR, filter-envelope amount/ADSR/enable, and tempo
- [x] Native MIDI input backends for Linux ALSA, Windows WinMM, and macOS CoreMIDI
- [ ] Broader controller target coverage

### 2. Arrangement and mixing

- [x] Basic left/right edge trimming for WAV audio regions
- [x] Undoable equal-power crossfades between overlapping audio regions
- [x] Named arrangement markers with ruler seeking and undoable editing
- [x] Custom power and freehand drawn fades, portable media packaging, and WAV relinking
- [ ] Audio comping and format-2 time-signature changes
- [x] Mixer submix buses with track assignment, level/pan, mute/solo, parent routing, and metering
- [x] Multiple post-track auxiliary sends per track to mixer buses
- [x] Multiple post-fader auxiliary sends from buses to other mixer buses, with cycle prevention
- [x] Persistent Delay, Reverb, and Compressor inserts on tracks, buses, and Master in realtime playback and render
- [x] Resonant low-pass filter insert with cutoff and Q controls in realtime playback and render
- [x] Adjustable low/mid and mid/high crossover frequencies for the per-track EQ in playback and render
- [x] Seven per-track parametric EQ bands with editable frequency, gain, and Q plus a draggable response graph
- [x] Graphical per-track volume/pan automation lanes with undoable point editing
- [x] Built-in synth filter cutoff/resonance automation in the Playlist, project files, playback, and exports
- [x] Touch/Latch/Trim recording of synth filter cutoff/resonance moves during playback
- [x] Touch recording for volume/pan automation from Mixer controls during playback
- [x] Latch recording for volume/pan automation until playback stops or Write is disarmed
- [x] Realtime per-track, bus, and master peak meters in the Mixer
- [x] Persistent per-track three-band EQ in realtime playback and WAV/stem rendering
- [x] Export each arrangement track as an isolated WAV stem
- [x] Automation trim mode and LV2/CLAP parameter automation for playback and render

### 3. Plugin and production workflow

- [x] Linux LV2 effect hosting and CLAP effect/instrument discovery, loading, parameter controls, and saved plug-in state
- [x] LV2/CLAP/VST3 effect parameter automation plus reusable built-in synth patches
- [x] Compensate the fixed CLAP host buffering delay and activation-reported plug-in latency across Mixer routes
- [x] Cross-platform VST3 audio-effect hosting with generic controls, automation, saved state, and latency compensation
- [x] Cross-platform VST3 instrument tracks with generic controls, per-parameter Playlist automation, saved parameter and plug-in state, playback/export, and MIDI audition
- [x] Native VST3 editor windows on Windows, macOS, and Linux/X11, with generic controls retained on Wayland
- [x] VST3 MIDI pitch bend through plug-ins' advertised MIDI-mapping assignments
- [ ] VST3 note expression
- [ ] Dynamic plug-in latency changes and transport context/timing compensation
- [x] Native MIDI input backends for Linux ALSA, Windows WinMM, and macOS CoreMIDI
- [ ] Broader controller target coverage
- [x] Autosave, rotating backups, project recovery, and versioned migrations
- [x] Optional multi-format export, stems, cancellable batch rendering, and progress reporting
- [x] Block-based CLAP processing with a 64-frame host buffering delay
- [ ] Linux package formats, signed installers, and a focused automated test suite

## Build requirements

- CMake 3.28+
- C++17 compiler
- Qt 6.2+ Widgets and Multimedia

Configure with `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`, then build with `cmake --build build --parallel`.

## Ownership and release note

The repository includes the official GPL-3.0 text in `LICENSE`. The current build intentionally excludes older JUCE-oriented drafts until they are integrated and can be validated.
