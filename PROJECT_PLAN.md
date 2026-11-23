# Project plan

## Vision

Build a pattern-first digital audio workstation with a fast MIDI workflow and a reliable audio engine. FL Studio, Reaper, and Ardour are product references; parity is a long-term goal, not a claim about the current build.

## Shipped foundation

The current Qt 6 executable provides a multi-track Playlist with movable MIDI and imported or recorded WAV audio clips, standalone or during-recording live audio input monitoring to Master or through an armed Audio track's mixer path, with per-device manual placement compensation, Linux ALSA MIDI input monitoring and recording with per-note channel preservation, per-track Synth ADSR envelopes, seven factory synth presets plus reusable user patches saved in app data, and selectable resonant filters with bipolar cutoff-envelope modulation, three-band EQ with adjustable crossovers, seven parametric bands with a draggable response graph, and volume/pan keyframe automation, Delay/Reverb/Compressor/Low-pass Filter inserts on tracks, buses, and Master, mixer submix buses with nested/group routing and multiple track and bus auxiliary sends, per-bus controls and peak meters, per-clip piano rolls, waveform display, audio edge trimming with selectable linear, smooth, equal-power, custom-power, and drawn fade curves, a 16-step drum sequencer with four assignable sample pads, five velocity layers per pad, per-pad waveform start editing, and independent per-layer tuning, loop, and ADSR overrides, editable MIDI clip lengths and common time signatures, MIDI import/export, project save/open, WAV export, and optional FLAC export. Audio media is loaded into memory and referenced by path. See [FEATURE_PARITY_TRACKER.md](FEATURE_PARITY_TRACKER.md) for exact boundaries.

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
- [x] Live MIDI monitoring and basic note recording from ALSA input on Linux
- [x] Learnable MIDI CC and NRPN mappings for master level, selected-track level/pan/mute/solo, synth-filter cutoff/resonance, synth amplitude-envelope ADSR, filter-envelope amount/ADSR/enable, oscillator 2 mix, and tempo
- [ ] Native MIDI input backends for other platforms and broader controller support

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
- [x] Touch recording for volume/pan automation from Mixer controls during playback
- [x] Latch recording for volume/pan automation until playback stops or Write is disarmed
- [x] Realtime per-track, bus, and master peak meters in the Mixer
- [x] Persistent per-track three-band EQ in realtime playback and WAV/stem rendering
- [x] Export each arrangement track as an isolated WAV stem
- [x] Automation trim mode and LV2/CLAP parameter automation for playback and render

### 3. Plugin and production workflow

- [ ] VST3 and CLAP discovery, loading, state, and editor lifecycle
- [ ] Plugin parameter automation and preset handling
- [ ] MIDI device management and native input backends for other platforms
- [ ] Autosave, backups, project recovery, and migrations
- [ ] Multi-format export, stems, batch rendering, and progress reporting
- [ ] Cross-platform packaging and a focused automated test suite

## Build requirements

- CMake 3.16+
- C++17 compiler
- Qt 6.2+ Widgets and Multimedia

Configure with `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`, then build with `cmake --build build --parallel`.

## Ownership and release note

The project plan currently identifies GPL-3.0, but the repository does not contain a `LICENSE` file. Include the official license text before publishing a release. The current build intentionally excludes older JUCE-oriented drafts until they are integrated and can be validated.
