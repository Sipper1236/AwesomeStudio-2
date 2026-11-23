# Awesome Studio

Awesome Studio is an early-stage digital audio workstation. The current executable has a Playlist with MIDI and WAV audio tracks, editable piano rolls, a built-in synth, a drum sequencer, basic track mixing, project save/load, and WAV render.

This is not feature parity with FL Studio, Reaper, or Ardour yet. The project is being built in vertical slices; [FEATURE_PARITY_TRACKER.md](FEATURE_PARITY_TRACKER.md) lists shipped behavior and outstanding work.

## Current workflow

- Add instrument tracks and patterns from the Browser or toolbar. Click an empty Playlist row to add a clip; drag clips in time or between tracks, and double-click a clip to open its piano roll.
- Draw, move, resize, select, and delete notes across the full 128-note MIDI range in a scrollable piano roll. Shift-click to select notes for group movement.
- Double-click a note or choose **Edit → Note properties…** to enter exact pitch, start, length, velocity, and MIDI channel values.
- Quantize selected note starts to the current piano-roll grid; when nothing is selected, quantize the whole clip. Quantize edits can be undone.
- Choose a root key and scale to highlight in-scale pitches; enable **Scale snap** to constrain newly drawn notes and keyboard auditions. Existing notes are left as they are.
- Select one or more piano-roll root notes, then use **Edit → Build chord from selection** to add major, minor, diminished, augmented, suspended, or seventh chords. Added notes copy the root notes' timing, velocity, and MIDI channel and can be undone.
- Toggle **Ghost notes** in the piano-roll toolbar to see faint, non-editable notes from other MIDI clips on the same track while editing the selected clip.
- Select chord notes with matching start and length, then use **Edit → Arpeggiate selected chord…** to choose up, down, or up/down direction, octave range, gate length, and source, fade, or repeating accent velocity patterns. The grid-timed conversion is one undoable edit.
- Use **Edit → Strum selected chord upward** to stagger selected chord tones slightly by pitch while preserving their lengths and velocities.
- Set straight, dotted, or triplet quarter through thirty-second note grids, edit velocity, and undo or redo note edits.
- Play and loop the full arrangement at 20–300 BPM.
- Enable an accented metronome and set a 1, 2, or 4 bar count-in. Count-in is included in WAV renders.
- Select an audio input and record a WAV take; with audio output connected, the take starts after count-in, then appears as a new Playlist track. Recording also works without output, starting immediately.
- Turn on **Monitor audio** in the Recording toolbar to hear the selected input through the master output, either on its own or during recording. Monitoring does not feed back into the recorded WAV; starting a take stops standalone monitoring.
- On Linux builds with ALSA development files, select a MIDI input and record note on/off and pitch-bend events into a bar 1 instrument pattern; count-in and looping follow the transport, and the take can be undone.
- Under **MIDI → Enable controller mappings**, listen for CC controls without recording notes. Use **MIDI → Learn controller for** to map a hardware control to master level, selected-track level or pan, or tempo. Mappings are channel-specific and saved in user settings.
- Turn on **Monitor MIDI** to play incoming notes on the selected synth or drum rack without recording. Monitoring supports overlapping notes, sample-loaded drum pads, and channel pitch bend for synth voices. Set the project-wide range from **MIDI → Set pitch-bend range…** (0.01–48 semitones); the setting is undoable, saved in the project, and written as MIDI RPN pitch-bend sensitivity on export. MIDI files with RPN sensitivity are imported with that range. Pitch bends are stored in clips and affect arrangement playback. Edit them in the piano roll's **Pitch Bend** lane below Velocity: click to add a point, drag to shape it, and right-click or press Delete to remove it. Monitoring can stay on while MIDI recording is armed.
- Set a common time signature and edit MIDI clip lengths in quarter-note beats.
- Choose sine, triangle, saw, or square per instrument track.
- Shape each built-in Synth track with adjustable attack, decay, sustain, and release; the envelope is saved in the project and used for playback and WAV export.
- Add a drum rack with synthesized kick, snare, closed-hat, and open-hat voices, then program 16 steps per bar. Step edits and piano-roll notes share undo and redo.
- Assign a mono or stereo WAV sample to each of the drum rack's four pads; clearing a pad restores its synthesized sound.
- Import mono or stereo WAV audio as a Playlist track. Supported sources are PCM 8/16/24/32-bit or IEEE float32 WAV at 8–192 kHz; regions display a waveform and play in the arrangement and WAV export.
- Trim a loaded audio region from either edge by dragging its handle in the Playlist; the left edge updates the source offset and arrangement position together.
- Set fade-in and fade-out durations in beats and choose Linear, Smooth, or Equal power curves for the selected audio region.
- To crossfade overlapping regions on the same audio track, select the earlier region and choose **Create crossfade with overlapping region** in the Audio Region panel. The incoming region must extend beyond the outgoing region; the paired equal-power fades render in playback and WAV exports and share undo/redo.
- Adjust track level, pan, mute, solo, and master level; watch per-track and master peak meters during playback and live MIDI monitoring.
- Add submixes with **Mixer → Add mixer bus**, route tracks from their Mixer **Output** selectors, and adjust each bus's level, pan, mute, solo, output, and peak meter. Set a bus output to another bus to build nested/group routes, or add bus auxiliary sends for parallel routing. Cyclic routes and sends are rejected. Removing a bus reroutes its outputs to its former parent.
- Add multiple **Aux sends** per track to feed mixer buses in parallel with the main output. Sends are post-track level, pan, EQ, and inserts, and respect destination bus mute and solo.
- Add multiple **Aux sends** per bus to feed other buses after the source bus level, pan, and inserts. Bus sends and parent routes share cycle prevention and follow solo/mute state through the routing graph.
- Add and reorder Delay, Reverb, Compressor, or Low-pass Filter inserts on tracks, buses, or Master. Their order and settings are saved with the project and run during realtime playback, WAV export, and stems.
- Enable Automation lanes in the Playlist toolbar to show track volume or pan. Click a lane to add a point, drag points to reshape it, and right-click or press Delete to remove a point. Edits share undo/redo with the keyframe list; playback and WAV export interpolate linearly.
- Arm Write automation, choose Volume or Pan and Touch or Latch, then move the matching Mixer control during playback. Touch commits when you release the control; Latch keeps writing its last value until playback stops or Write is disarmed. Each pass is one undoable edit and is audible while recording.
- Import Standard MIDI Files (formats 0 and 1 with PPQN timing), and export arrangements as multitrack MIDI or stereo 16-bit WAV.
- Use **File → Export track stems…** to render every track to a separate stereo WAV in a folder. Each stem includes that track's EQ and effects, routed bus and Master effects, and level/pan automation plus the master level; mute and solo state do not omit tracks.
- Select any track and use **Track Equalizer** in the Browser to adjust low (below 120 Hz), mid (120 Hz–6 kHz), and high (above 6 kHz) bands from −18 to +18 dB. The EQ runs in realtime arrangement playback, full WAV exports, and track stems.
- Right-click the Playlist ruler to add a named arrangement marker; right-click a marker to rename or remove it. Left-click the ruler or a marker to seek and start playback. Marker edits can be undone and are saved in the project.
- Save and reopen `.aws` projects.
- Use the editor without an audio device; connect an output device to audition notes.

Version 22 project files are JSON written atomically and store up to 1,000 named arrangement markers, per-track three-band EQ, mixer buses with parent routes and auxiliary sends, multiple track auxiliary sends, audio fade curves, the project-wide MIDI pitch-bend range, per-clip pitch-bend events, and up to eight Delay, Reverb, Compressor, or Low-pass Filter inserts per mixer channel (48 inserts per project). Version 21 files retain their existing data and default to a two-semitone pitch-bend range; version 20 files retain existing project data and load with empty pitch-bend event lists; version 19 files retain their Delay, Reverb, and Compressor chains with a default low-pass configuration; version 18 files migrate with empty effect chains; version 17 projects retain their multiple track sends and nested bus routes; version 16 projects migrate their single track send and route buses to Master; version 15 projects default fades to linear; version 14 projects migrate with sends off; version 13 projects migrate with no buses and all tracks routed to Master; version 12 projects also gain flat (0 dB) EQ settings; version 11 projects gain an empty marker list. Versions 1–10 migrate with compatible synth-envelope, meter, metronome, count-in, automation, and default MIDI-channel values. MIDI notes retain their zero-based channel through ALSA recording, project save/load, and Standard MIDI File import/export; notes created in the app use the instrument track's export channel. Pitch-bend events are stored with each MIDI clip and use the configured range during live monitoring and arrangement playback. Track volume and pan automation is stored as beat/value keyframes. Audio clips and drum pads store source WAV paths (currently absolute); missing or unsupported sources reopen offline. Sources are loaded into memory, limited to 128 MiB per WAV, 40 million decoded samples per source, and 512 MiB of decoded audio per project. Project files do not package media. MIDI import reads note, channel pitch-bend, and RPN pitch-bend sensitivity events plus the first tempo and time-signature events in format 0 or 1 PPQN files, and maps channel 10 tracks to the drum rack. MIDI export writes a format 1 file with a conductor track, pitch-bend sensitivity RPN events, and unassigned drum-rack notes on channel 10. WAV export renders synths with their ADSR envelopes, track EQ/automation/inserts, routed bus and Master inserts, synthesized or sampled drums, imported audio with region fades and track processing, metronome, and configured count-in at 44.1 kHz; insert tails are included.

## Build

Requirements:

- CMake 3.16 or newer
- A C++17 compiler
- Qt 6.2 or newer with Widgets and Multimedia
- On Linux, libasound development files enable MIDI input recording; this dependency is optional.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/AwesomeStudio
```

On Windows, launch `build/AwesomeStudio.exe`. Qt's Multimedia backend uses the active platform audio service.

## Known gaps

Audio recording currently writes a separate WAV file per take, stores its path in the project, and loads the take into memory after recording. Live audio input monitoring routes directly to Master and works independently or during a take; there is no latency compensation or track-armed input monitoring. Live MIDI monitoring and recording use ALSA sequencer input on Linux builds; other platforms need native MIDI backends. Learnable MIDI CC mappings currently cover master level, selected-track level and pan, and tempo. The track EQ is a basic three-band equalizer, and the built-in Delay, Reverb, Compressor, and Low-pass Filter are compact first-party inserts; there is no punch recording, custom audio fade shapes, streaming playback, or third-party plugin hosting. Track volume and pan automation has graphical linear lanes, undoable keyframe editing, and touch/latch recording from Mixer controls; trim mode and plugin parameter automation remain future work. Drum pads support one sample each without velocity layers, tuning, or per-pad envelopes. MIDI format 2 and SMPTE-timed files are not supported. Older JUCE-oriented source files remain in `src/core`, but the build does not include them and they are not shipped capabilities.

The project plan names GPL-3.0, but the repository currently has no `LICENSE` file. Add the official license text before distributing releases.
