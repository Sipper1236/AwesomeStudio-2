# Project plan

## Vision

Build a pattern-first digital audio workstation with a fast MIDI workflow and a reliable audio engine. FL Studio, Reaper, and Ardour are product references; parity is a long-term goal, not a claim about the current build.

## Shipped foundation

The current Qt 6 executable provides a multi-track Playlist with movable MIDI and imported or recorded WAV audio clips, standalone or during-recording live audio input monitoring, Linux ALSA MIDI input monitoring and recording with per-note channel preservation, per-track Synth ADSR envelopes, three-band EQ and volume/pan keyframe automation, Delay/Reverb/Compressor/Low-pass Filter inserts on tracks, buses, and Master, mixer submix buses with nested/group routing and multiple track and bus auxiliary sends, per-bus controls and peak meters, per-clip piano rolls, waveform display, audio edge trimming and selectable linear, smooth, and equal-power fades, a 16-step drum sequencer with four assignable sample pads, editable MIDI clip lengths and common time signatures, MIDI import/export, project save/open, and WAV export. Audio media is loaded into memory and referenced by path. See [FEATURE_PARITY_TRACKER.md](FEATURE_PARITY_TRACKER.md) for exact boundaries.

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
- [ ] Arpeggiator velocity patterns
- [x] Built-in synth preview and arrangement playback
- [x] Project save/open and stereo WAV export
- [x] Playlist with multi-track MIDI clips and basic mixer controls
- [x] 16-step sequencer and synthesized drum rack
- [x] Editable clip length and common time signatures
- [x] Standard MIDI File import/export (formats 0/1, PPQN)
- [x] Count-in and metronome
- [x] WAV audio import, Playlist playback, waveform display, and WAV render integration
- [x] Basic audio region edge trimming and selectable linear, smooth, and equal-power fades
- [x] One WAV sample assignment per drum rack pad with synthesized fallback
- [x] Per-track Synth ADSR envelope controls in live playback and render
- [x] Undoable per-track volume and pan keyframes with linear interpolation in live playback and WAV render
- [x] Basic mono/stereo WAV input recording to a new audio track, aligned after count-in when playback is available
- [x] Live MIDI monitoring and basic note recording from ALSA input on Linux
- [x] Learnable MIDI CC mapping for master/selected-track level and pan, plus tempo
- [ ] Native MIDI input backends for other platforms and broader controller support

### 2. Arrangement and mixing

- [x] Basic left/right edge trimming for WAV audio regions
- [x] Undoable equal-power crossfades between overlapping audio regions
- [x] Named arrangement markers with ruler seeking and undoable editing
- [ ] Custom fade shapes, comping, and portable media packaging
- [x] Mixer submix buses with track assignment, level/pan, mute/solo, parent routing, and metering
- [x] Multiple post-track auxiliary sends per track to mixer buses
- [x] Multiple post-fader auxiliary sends from buses to other mixer buses, with cycle prevention
- [x] Persistent Delay, Reverb, and Compressor inserts on tracks, buses, and Master in realtime playback and render
- [x] Resonant low-pass filter insert with cutoff and Q controls in realtime playback and render
- [ ] More configurable EQ in the playback graph
- [x] Graphical per-track volume/pan automation lanes with undoable point editing
- [x] Touch recording for volume/pan automation from Mixer controls during playback
- [x] Latch recording for volume/pan automation until playback stops or Write is disarmed
- [x] Realtime per-track, bus, and master peak meters in the Mixer
- [x] Persistent per-track three-band EQ in realtime playback and WAV/stem rendering
- [x] Export each arrangement track as an isolated WAV stem
- [ ] Automation trim mode and plugin parameter automation

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
