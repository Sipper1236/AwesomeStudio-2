# Feature parity tracker

Status describes the executable built by the repository's CMake target. Source files that are not compiled into the app do not count as implemented features.

| Area | Current status | Gap to a full DAW |
|---|---|---|
| Build and desktop shell | Working Qt 6 app with menus, docks, and saved window layout | Packaging, crash recovery, accessibility review |
| Piano roll | Per-clip full MIDI pitch range, snap grid, draw/move/resize/delete, Shift multi-select, velocity lane, MIDI-note undo/redo, editable clip length | Quantize, scale/chord tools, ghost notes |
| Instruments | Multiple polyphonic synth tracks plus a synthesized drum rack with kick, snare, closed/open hats | ADSR/filter controls, presets, sample-based drum rack, plugin instruments |
| Transport | Arrangement play/stop, 20–300 BPM, common editable time signatures, song loop, off/1–4 bar count-in, accented metronome | Punch recording, tempo maps, transport markers |
| Projects | Versioned v6 `.aws` JSON save/open with atomic writes; v1 projects migrate to a track and clip; v2–v5 files gain compatible meter/instrument/transport defaults; audio sources reopen from saved paths and missing files show as offline | Autosave, backups, portable media packaging, broader migration tooling |
| Audio export | Full arrangement, stereo 16-bit WAV at 44.1 kHz, including imported audio | More formats, stems, range selection, render queue, dithering options |
| Arrangement | Playlist with MIDI and WAV audio tracks, waveform regions, movable clips, left/right audio edge trimming, editable MIDI clip lengths, and Standard MIDI File import/export (formats 0 and 1, PPQN timing) | Audio fades, markers, comping, MIDI format 2/SMPTE import |
| Step sequencing | 16-step per-bar editor with kick, snare, closed-hat, and open-hat lanes; note edits share the piano-roll undo history | Variable step divisions, pattern chaining, per-step probability/ratchets, sample assignment |
| Mixer | Per-track level, pan, mute, solo, plus master level | Buses, sends, inserts, meters, routing graph, groups, effects |
| Audio recording | Not implemented | Input device selection, armed tracks, takes, latency compensation |
| MIDI input | Piano-roll preview only | Device enumeration, live input, recording, controller mapping |
| Plugins and effects | Not implemented in the executable | VST3/CLAP hosting, parameter automation, bundled effects |
| Automation | Not implemented in the executable | Lanes, curves, read/write modes, undo integration |
| Collaboration and notation | Not implemented | Score view, collaboration, session interchange |

## Next slices

1. Add live MIDI/audio recording with input monitoring.
2. Add audio clip fades, markers, and comping; add portable media packaging or relinking.
3. Add mixer buses, sends, meters, effects inserts, and a real-time routing graph.
4. Add third-party plugin hosting, presets, and parameter automation.
5. Add recovery, platform packaging, and a test suite.

The older JUCE-oriented files under `src/core` are excluded from the current build and need separate integration work before they can change any status above.
