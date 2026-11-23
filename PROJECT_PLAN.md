# Project plan

## Vision

Build a pattern-first digital audio workstation with a fast MIDI workflow and a reliable audio engine. FL Studio, Reaper, and Ardour are product references; parity is a long-term goal, not a claim about the current build.

## Shipped foundation

The current Qt 6 executable provides a multi-track Playlist with movable MIDI clips, per-clip piano rolls, velocity editing, note undo/redo, per-track four-waveform synths, basic track mixing, tempo and arrangement-loop transport, versioned project save/open, and full-arrangement WAV export. See [FEATURE_PARITY_TRACKER.md](FEATURE_PARITY_TRACKER.md) for exact boundaries.

## Roadmap

### 1. Pattern workflow

- [x] Buildable desktop application and event loop
- [x] Piano-roll note entry, movement, resizing, deletion, and velocity editing
- [x] Built-in synth preview and arrangement playback
- [x] Project save/open and stereo WAV export
- [x] Playlist with multi-track MIDI clips and basic mixer controls
- [ ] Step sequencer and drum instrument
- [ ] Editable clip length, time signature, and count-in/metronome
- [ ] MIDI file import/export and live MIDI recording

### 2. Arrangement and mixing

- [ ] Audio-file clips, trimming, fades, markers, and comping
- [ ] Mixer buses, routing, sends, inserts, and peak meters
- [ ] Bundled EQ, delay, compression, and reverb in the playback graph
- [ ] Automation lanes with undoable edits

### 3. Plugin and production workflow

- [ ] VST3 and CLAP discovery, loading, state, and editor lifecycle
- [ ] Plugin parameter automation and preset handling
- [ ] MIDI device management and controller mapping
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
