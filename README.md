# Awesome Studio

Awesome Studio is a pattern-based MIDI workstation in early development. The current executable has a Playlist with multiple instrument tracks, movable MIDI clips, editable piano rolls, a built-in synth, basic track mixing, project save/load, and WAV render.

This is not feature parity with FL Studio, Reaper, or Ardour yet. The project is being built in vertical slices; [FEATURE_PARITY_TRACKER.md](FEATURE_PARITY_TRACKER.md) lists shipped behavior and outstanding work.

## Current workflow

- Add instrument tracks and patterns from the Browser or toolbar. Click an empty Playlist row to add a clip; drag clips in time or between tracks, and double-click a clip to open its piano roll.
- Draw, move, resize, select, and delete notes across the full 128-note MIDI range in a scrollable piano roll. Shift-click to select notes for group movement.
- Set the grid to quarter, eighth, sixteenth, or thirty-second notes, edit velocity, and undo or redo note edits.
- Play and loop the full arrangement at 20–300 BPM.
- Set a common time signature and edit MIDI clip lengths in quarter-note beats.
- Choose sine, triangle, saw, or square per instrument track.
- Adjust track level, pan, mute, solo, and master level.
- Import Standard MIDI Files (formats 0 and 1 with PPQN timing), and export arrangements as multitrack MIDI or stereo 16-bit WAV.
- Save and reopen `.aws` projects.
- Use the editor without an audio device; connect an output device to audition notes.

Version 3 project files are JSON written atomically. Version 1 projects migrate to one instrument track and one MIDI clip; version 2 projects open in 4/4. MIDI import reads note events and the first tempo and time-signature events in format 0 or 1 PPQN files. MIDI export writes a format 1 file with a conductor track. WAV export renders the same basic synth used for playback at 44.1 kHz.

## Build

Requirements:

- CMake 3.16 or newer
- A C++17 compiler
- Qt 6.2 or newer with Widgets and Multimedia

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/AwesomeStudio
```

On Windows, launch `build/AwesomeStudio.exe`. Qt's Multimedia backend uses the active platform audio service.

## Known gaps

There are no audio clips or live recording, step sequencer or drum instrument, mixer buses/sends/effects, third-party plugin hosting, or automation lanes yet. MIDI format 2 and SMPTE-timed files are not supported. Older JUCE-oriented source files remain in `src/core`, but the build does not include them and they are not shipped capabilities.

The project plan names GPL-3.0, but the repository currently has no `LICENSE` file. Add the official license text before distributing releases.
