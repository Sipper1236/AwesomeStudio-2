# Awesome Studio

Awesome Studio is an early-stage digital audio workstation. The current executable has a Playlist with MIDI and WAV audio tracks, editable piano rolls, a built-in synth, a drum sequencer, basic track mixing, project save/load, and WAV render.

This is not feature parity with FL Studio, Reaper, or Ardour yet. The project is being built in vertical slices; [FEATURE_PARITY_TRACKER.md](FEATURE_PARITY_TRACKER.md) lists shipped behavior and outstanding work.

## Current workflow

- Add instrument tracks and patterns from the Browser or toolbar. Click an empty Playlist row to add a clip; drag clips in time or between tracks, and double-click a clip to open its piano roll.
- Draw, move, resize, select, and delete notes across the full 128-note MIDI range in a scrollable piano roll. Shift-click to select notes for group movement.
- Set the grid to quarter, eighth, sixteenth, or thirty-second notes, edit velocity, and undo or redo note edits.
- Play and loop the full arrangement at 20–300 BPM.
- Enable an accented metronome and set a 1, 2, or 4 bar count-in. Count-in is included in WAV renders.
- Set a common time signature and edit MIDI clip lengths in quarter-note beats.
- Choose sine, triangle, saw, or square per instrument track.
- Add a drum rack with synthesized kick, snare, closed-hat, and open-hat voices, then program 16 steps per bar. Step edits and piano-roll notes share undo and redo.
- Import mono or stereo WAV audio as a Playlist track. Supported sources are PCM 8/16/24/32-bit or IEEE float32 WAV at 8–192 kHz; regions display a waveform and play in the arrangement and WAV export.
- Trim a loaded audio region from either edge by dragging its handle in the Playlist; the left edge updates the source offset and arrangement position together.
- Set linear fade-in and fade-out durations in beats for the selected audio region.
- Adjust track level, pan, mute, solo, and master level.
- Import Standard MIDI Files (formats 0 and 1 with PPQN timing), and export arrangements as multitrack MIDI or stereo 16-bit WAV.
- Save and reopen `.aws` projects.
- Use the editor without an audio device; connect an output device to audition notes.

Version 7 project files are JSON written atomically. Versions 1–6 migrate with compatible synth, meter, metronome, and count-in defaults. Audio clips store the source WAV path (currently absolute); missing or unsupported sources reopen as offline regions. Sources are loaded into memory, limited to 128 MiB per WAV and 40 million decoded samples, and project files do not package media. MIDI import reads note events and the first tempo and time-signature events in format 0 or 1 PPQN files, and maps channel 10 tracks to the drum rack. MIDI export writes a format 1 file with a conductor track and places drum racks on channel 10. WAV export renders synths, the synthesized drum rack, imported audio with region fades, metronome, and configured count-in at 44.1 kHz.

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

There is no audio recording, fade curves/crossfades, streaming playback, mixer buses/sends/effects, third-party plugin hosting, or automation yet. The drum rack uses synthesized sounds rather than samples. MIDI format 2 and SMPTE-timed files are not supported. Older JUCE-oriented source files remain in `src/core`, but the build does not include them and they are not shipped capabilities.

The project plan names GPL-3.0, but the repository currently has no `LICENSE` file. Add the official license text before distributing releases.
