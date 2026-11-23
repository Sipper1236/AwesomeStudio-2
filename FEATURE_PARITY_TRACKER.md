# Feature parity tracker

Status describes the executable built by the repository's CMake target. Source files that are not compiled into the app do not count as implemented features.

| Area | Current status | Gap to a full DAW |
|---|---|---|
| Build and desktop shell | Working Qt 6 app with menus, docks, and saved window layout | Packaging, crash recovery, accessibility review |
| Piano roll | Per-clip full MIDI pitch range, straight/dotted/triplet snap grids, draw/move/resize/delete, Shift multi-select, velocity lane, MIDI-note undo/redo, editable clip length, quantization, exact single-note pitch/start/length/velocity/channel properties, key/scale highlighting and optional scale snapping, common chord insertion, optional same-track ghost notes, configurable up/down/up-down chord arpeggiation, and upward chord strumming | Multi-note property editing, arpeggiator velocity patterns |
| Instruments | Multiple polyphonic synth tracks with per-track ADSR envelopes plus a four-pad drum rack with synthesized voices or one assigned WAV sample per pad | Filters, oscillator stacks, presets, velocity layers, pad tuning/envelopes, plugin instruments |
| Transport | Arrangement play/stop, ruler click-to-seek, named arrangement markers, 20–300 BPM, common editable time signatures, song loop, off/1–4 bar count-in, accented metronome, count-in-aware audio recording | Punch recording, tempo maps |
| Projects | Versioned v12 `.aws` JSON save/open with atomic writes; v1 projects migrate to a track and clip; v2–v9 files gain compatible meter/instrument/transport and empty automation defaults, v10 files gain default MIDI-channel values, and v11 files gain an empty marker list; audio sources reopen from saved paths and missing or over-budget media stay offline; shared source cache with 512 MiB project memory cap | Autosave, backups, portable media packaging, broader migration tooling |
| Audio export | Full arrangement stereo 16-bit WAV at 44.1 kHz plus isolated per-track stereo WAV stems; renders imported audio, sampled drums, and linear region fades | More formats, range selection, render queue, dithering options |
| Arrangement | Playlist with MIDI and WAV audio tracks, waveform regions, movable clips, left/right audio edge trimming, linear fade-in/out and undoable crossfades for overlapping regions, named ruler markers with undoable add/rename/remove and click-to-seek, editable MIDI clip lengths, and Standard MIDI File import/export (formats 0 and 1, PPQN timing) | Curved fade envelopes, comping, MIDI format 2/SMPTE import |
| Step sequencing | 16-step per-bar editor with kick, snare, closed-hat, and open-hat lanes; note edits share the piano-roll undo history; each pad can load a WAV or use its synthesized voice | Variable step divisions, pattern chaining, per-step probability/ratchets, velocity layers, sample start/loop controls |
| Mixer | Per-track level, pan, mute, solo, and master level; live per-track and master peak meters include arrangement playback and MIDI monitoring; track volume/pan automation is applied to playback and WAV export | Buses, sends, inserts, routing graph, groups, effects |
| Audio recording | Select an input device, record mono/stereo WAV takes, and add them as new Playlist audio tracks; uses count-in when output is available, works without output, and respects project and WAV size limits | Input monitoring, latency compensation, armed tracks, punch recording, take comping |
| MIDI input | ALSA input device enumeration, polyphonic live monitoring for synth and sampled drum-rack notes, note recording, and channel-specific learnable CC mappings for master level, selected-track level/pan, and tempo on Linux; note on/off, overlapping same-pitch notes, count-in, loop timing, pattern assignment, and undo/redo | Native MIDI backends on other platforms, pitch bend, NRPN, and broader controller targets |
| Plugins and effects | Not implemented in the executable | VST3/CLAP hosting, parameter automation, bundled effects |
| Automation | Per-track volume and pan keyframes with graphical Playlist lanes, click-to-add and drag-to-edit points, right-click/Delete removal, undo/redo, linear playback/render interpolation, and touch/latch recording from Mixer controls during playback | Trim mode, curved segments, plugin parameter automation |
| Collaboration and notation | Not implemented | Score view, collaboration, session interchange |

## Next slices

1. Add automation trim mode and plugin parameter automation with plugin hosting.
2. Add native MIDI input backends for other platforms and broader controller support; add audio monitoring, latency compensation, punch recording, and take comping.
3. Add curved audio fade envelopes, comping, and portable media packaging or relinking.
4. Add mixer buses, sends, meters, effects inserts, and a real-time routing graph.
5. Add recovery, platform packaging, and a test suite.

The older JUCE-oriented files under `src/core` are excluded from the current build and need separate integration work before they can change any status above.
