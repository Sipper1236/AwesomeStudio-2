# Feature parity tracker

Status describes the executable built by the repository's CMake target. Source files that are not compiled into the app do not count as implemented features.

| Area | Current status | Gap to a full DAW |
|---|---|---|
| Build and desktop shell | Working Qt 6 app with menus, docks, and saved window layout | Packaging, crash recovery, accessibility review |
| Piano roll | Per-clip full MIDI pitch range, snap grid, draw/move/resize/delete, Shift multi-select, velocity lane, MIDI-note undo/redo | Quantize, scale/chord tools, ghost notes, MIDI import/export |
| Instruments | Multiple polyphonic tracks with per-track sine, triangle, saw, or square synth | ADSR/filter controls, presets, samplers, drum rack, plugin instruments |
| Transport | Arrangement play/stop, 20–300 BPM, song loop | Count-in, metronome, punch recording, time-signature editing |
| Projects | Versioned `.aws` JSON save/open with atomic writes; v1 projects migrate to a track and clip | Autosave, backups, media references, broader migration tooling |
| Audio export | Full arrangement, stereo 16-bit WAV at 44.1 kHz | More formats, stems, range selection, render queue, dithering options |
| Arrangement | Playlist with multiple instrument tracks and movable MIDI clips | Audio regions, markers, comping, step sequencer, pattern length editing |
| Mixer | Per-track level, pan, mute, solo, plus master level | Buses, sends, inserts, meters, routing graph, groups, effects |
| Audio recording | Not implemented | Input device selection, armed tracks, takes, latency compensation |
| MIDI input | Piano-roll preview only | Device enumeration, live input, recording, controller mapping |
| Plugins and effects | Not implemented in the executable | VST3/CLAP hosting, parameter automation, bundled effects |
| Automation | Not implemented in the executable | Lanes, curves, read/write modes, undo integration |
| Collaboration and notation | Not implemented | Score view, collaboration, session interchange |

## Next slices

1. Add step sequencing, editable clip length, time signature, and MIDI file import/export.
2. Add live MIDI input/recording and audio recording with input monitoring.
3. Add audio-file clips, clip fades, markers, and comping.
4. Add mixer buses, sends, meters, effects inserts, and a real-time routing graph.
5. Add third-party plugin hosting, presets, and parameter automation.
6. Add recovery, platform packaging, and a test suite.

The older JUCE-oriented files under `src/core` are excluded from the current build and need separate integration work before they can change any status above.
