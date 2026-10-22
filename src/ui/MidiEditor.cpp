// MidiEditor.cpp
// MIDI piano roll editor implementation for Awesome Studio DAW

#include "MidiEditor.h"
#include <JuceHeader.h>
#include <cmath>
#include <algorithm>

//==============================================================================
MidiEditor::MidiEditor()
    : isEditing(false), editingNoteIndex(-1), editingStartTime(0),
      transportPosition(0.0), playbackRate(1.0),
      horizontalZoom(1.0), verticalZoom(1.0),
      visibleStartTime(0), visibleEndTime(120), // 2 minutes visible
      isDraggingNote(false), isDraggingBottomEdge(false),
      isDraggingLeftEdge(false), isEditingVelocity(false)
{
    // Initialize note colors for different pitch ranges
    for (int i = 0; i < 128; i++) {
        // Color based on octave - higher octaves = brighter colors
        int octave = i / 12;
        int noteInOctave = i % 12;

        // Create a hue based on the note
        float hue = (noteInOctave * 30.0f) % 360.0f;
        float saturation = 0.7f + (octave * 0.1f);
        float value = 0.9f - (octave * 0.05f);

        // Clamp value
        value = jlimit(0.1f, 1.0f, value);

        noteColours[i] = juce::Colour::fromHSV(hue, saturation, value);
    }

    // Set timer for updates (every 50ms)
    startTimer(50);
}

//==============================================================================
MidiEditor::~MidiEditor()
{
    stopTimer();
}

//==============================================================================
void MidiEditor::paint(juce::Graphics& g)
{
    // Background
    g.fillAll(juce::Colours::black);

    // Draw grid
    g.setColour(juce::Colours::darkgrey);

    // Calculate visible area
    int width = getWidth();
    int height = getHeight();

    // Horizontal grid lines (note rows)
    for (int i = 0; i <= 128; i += 12) { // Every octave
        int y = jmap(i, 0, 128, height, 0); // Inverted Y axis
        g.drawLine(0, y, width, y, 1.0f);
    }

    // Vertical grid lines (time divisions)
    int beatsPerView = 4; // Show 4 beats
    int secondsPerBeat = 2.0; // 2 seconds per beat at current tempo
    int pixelsPerBeat = jmap(beatsPerView, 1, 16, 50, width);

    for (int i = 0; i < (width / pixelsPerBeat) + 2; i++) {
        int x = jmap(i, 0, width / pixelsPerBeat + 2, 0, width);
        if (x > 0 && x < width) {
            g.drawLine(x, 0, x, height, 0.5f);
        }
    }

    // Draw notes
    for (const auto& note : notes) {
        // Check if note is in visible range
        if (note.endTime < visibleStartTime * getSampleRate() ||
            note.startTime > visibleEndTime * getSampleRate()) {
            continue;
        }

        // Calculate position
        int noteY = jmap(note.noteNumber, 0, 128, height, 0);
        int64_t noteStart = note.startTime;
        int64_t noteEnd = note.endTime;

        // Clip to visible range
        int64_t startClip = jmax(noteStart, visibleStartTime * getSampleRate());
        int64_t endClip = jmin(noteEnd, visibleEndTime * getSampleRate());

        if (startClip >= endClip) continue;

        int xStart = jmap(startClip, visibleStartTime * getSampleRate(), visibleEndTime * getSampleRate(), 0, width);
        int xEnd = jmap(endClip, visibleStartTime * getSampleRate(), visibleEndTime * getSampleRate(), 0, width);

        // Skip if note is completely outside visible area
        if (xEnd <= 0 || xStart >= width) continue;

        // Ensure valid coordinates
        xStart = jmax(0, xStart);
        xEnd = jmin(width, xEnd);
        noteY = jmax(0, jmin(height, noteY));

        // Draw note rectangle
        g.setColour(note.colour);
        g.fillRect(xStart, noteY + 2, xEnd - xStart, 12); // 12px height note

        // Draw note border
        g.setColour(juce::Colours::white);
        g.drawRect(xStart, noteY + 2, xStart + xStart - xStart, 12, 1.0f);

        // Draw selected border
        if (note.isSelected) {
            g.setColour(juce::Colours::yellow);
            g.drawRect(xStart - 2, noteY, xEnd - xStart + 4, 16, 2.0f);
        }

        // Draw note name/number
        g.setColour(juce::Colours::white);
        g.setFont(9.0f);
        juce::String noteName = juce::MidiMessage::getMidiNoteName(note.noteNumber);
        g.drawText(noteName, xStart, noteY + 14, xEnd - xStart, 10,
                   juce::Justification::centredTop);
    }

    // Draw selected notes highlight
    for (const auto& note : selectedNotes) {
        if (note.endTime < visibleStartTime * getSampleRate() ||
            note.startTime > visibleEndTime * getSampleRate()) {
            continue;
        }

        int noteY = jmap(note.noteNumber, 0, 128, height, 0);
        int64_t noteStart = note.startTime;
        int64_t noteEnd = note.endTime;

        int64_t startClip = jmax(noteStart, visibleStartTime * getSampleRate());
        int64_t endClip = jmin(noteEnd, visibleEndTime * getSampleRate());

        if (startClip >= endClip) continue;

        int xStart = jmap(startClip, visibleStartTime * getSampleRate(), visibleEndTime * getSampleRate(), 0, width);
        int xEnd = jmap(endClip, visibleStartTime * getSampleRate(), visibleEndTime * getSampleRate(), 0, width);

        xStart = jmax(0, xStart);
        xEnd = jmin(width, xEnd);
        noteY = jmax(0, jmin(height, noteY));

        g.setColour(juce::Colours::yellow.withAlpha(0.5f));
        g.fillRect(xStart - 2, noteY, xEnd - xStart + 4, 16);
    }

    // Draw playhead (current position)
    double playheadX = jmap(transportPosition, visibleStartTime * getSampleRate(),
                            visibleEndTime * getSampleRate(), 0, width);
    g.setColour(juce::Colours::yellow);
    g.drawLine(playheadX, 0, playheadX, getHeight(), 2.0f);

    // Draw rulers
    g.setColour(juce::Colours::darkgrey);
    g.drawLine(0, 0, 0, getHeight(), 1.0f);
    g.drawLine(0, getHeight() - 12, getWidth(), getHeight() - 12, 1.0f);
}

//==============================================================================
void MidiEditor::resized()
{
    // Component resized - no child components to layout explicitly
}

//==============================================================================
void MidiEditor::mouseDown(const juce::MouseEvent& event)
{
    isDraggingNote = false;
    isDraggingBottomEdge = false;
    isDraggingLeftEdge = false;
    isEditingVelocity = false;

    // Find note under mouse
    int mouseY = event.y;
    int mouseX = event.x;

    // Convert Y to note number
    int noteNumber = jmap(mouseY, getHeight(), 0, 0, 127);
    noteNumber = jclamp(noteNumber, 0, 127);

    // Find note at this position
    for (int i = 0; i < (int)notes.size(); i++) {
        const auto& note = notes[i];

        // Check if mouse is within note bounds
        int noteY = jmap(note.noteNumber, 0, 128, getHeight(), 0);
        int noteHeight = 12;

        if (mouseY >= noteY && mouseY <= noteY + noteHeight) {
            // Check if within note time range
            int64_t noteStart = note.startTime;
            int64_t noteEnd = note.endTime;
            int64_t startClip = jmax(noteStart, visibleStartTime * getSampleRate());
            int64_t endClip = jmin(noteEnd, visibleEndTime * getSampleRate());

            int noteXStart = jmap(startClip, visibleStartTime * getSampleRate(), visibleEndTime * getSampleRate(), 0, getWidth());
            int xEnd = jmap(endClip, visibleStartTime * getSampleRate(), visibleEndTime * getSampleRate(), 0, getWidth());
            int noteXEnd = xStart + (endClip - startClip) * getWidth() / ((visibleEndTime - visibleStartTime) * getSampleRate());

            if (mouseX >= xStart && mouseX <= noteXEnd) {
                // Found note under mouse
                isDraggingNote = true;
                editingNoteIndex = i;
                editingStartTime = note.startTime;
                dragStartPos = event.eventPosition;

                // Select this note
                selectedNotes.clear();
                selectedNotes.push_back(note);

                // Begin dragging based on where clicked
                int clickYRelativeToNote = mouseY - noteY;
                if (clickYRelativeToNote < 4) { // Near top edge - drag up/down
                    isDraggingBottomEdge = false;
                    isDraggingLeftEdge = false;
                }
                else if (clickYRelativeToNote > noteHeight - 4) { // Near bottom edge
                    isDraggingBottomEdge = true;
                }
                else if (mouseX < getWidth() / 2) { // Left half - velocity edit
                    isEditingVelocity = true;
                }

                break;
            }
    }

    // Start a new note if clicking in empty space (with modifier)
    if (!isDraggingNote && !isEditingVelocity && !ModifierKeys::getCurrentModifiers().isCommandDown()) {
        // Add new note at mouse position
        int64_t mouseTime = visibleStartTime * getSampleRate() +
            jmap(mouseX, 0, getWidth(), visibleStartTime * getSampleRate(),
                 visibleEndTime * getSampleRate());

        addNote(noteNumber, 100, mouseTime, mouseTime + getSampleRate() / 4); // 1/4 note default
        selectedNotes.clear();
        selectedNotes.push_back(notes.back());
        isDraggingNote = true;
        editingNoteIndex = (int)notes.size() - 1;
        editingStartTime = notes.back().startTime;
    }
}

//==============================================================================
void MidiEditor::mouseUp(const juce::MouseEvent& event)
{
    isDraggingNote = false;
    isDraggingBottomEdge = false;
    isDraggingLeftEdge = false;
    isEditingVelocity = false;

    // If dragging a note, handle the drag completion
    if (editingNoteIndex >= 0 && editingNoteIndex < (int)notes.size()) {
        // Snap to grid
        // ... would implement grid snapping here
    }

    // Deselect if clicking outside notes
    if (!isDraggingNote && !ModifierKeys::getCurrentModifiers().isShiftDown()) {
        deselectAllNotes();
    }
}

//==============================================================================
void MidiEditor::mouseMove(const juce::MouseEvent& event)
{
    if (isDraggingNote && editingNoteIndex >= 0 && editingNoteIndex < (int)notes.size()) {
        // Drag note horizontally (change time)
        if (!isEditingVelocity && !isDraggingBottomEdge && !isDraggingLeftEdge) {
            int64_t newTime = editNoteTime(editingNoteIndex, event.eventPosition.x);
            if (newTime != notes[editingNoteIndex].startTime) {
                notes[editingNoteIndex].startTime = newTime;
                // Also adjust end time to maintain note duration
                notes[editingNoteIndex].endTime = notes[editingNoteIndex].startTime +
                    (notes[editingNoteIndex].endTime - notes[editingNoteIndex].startTime);
            }
        }
        // Drag bottom edge (change duration)
        else if (isDraggingBottomEdge) {
            int64_t newEndTime = editNoteTime(editingNoteIndex, event.eventPosition.x, true);
            if (newEndTime != notes[editingNoteIndex].endTime) {
                notes[editingNoteIndex].endTime = newEndTime;
            }
        }
        // Drag left edge (change start time)
        else if (isDraggingLeftEdge) {
            int64_t newStartTime = editNoteTime(editingNoteIndex, event.eventPosition.x, false, true);
            if (newStartTime != notes[editingNoteIndex].startTime) {
                notes[editingNoteIndex].startTime = newStartTime;
                // Adjust end time to maintain duration or velocity
                notes[editingNoteIndex].endTime = notes[editingNoteIndex].startTime +
                    (notes[editingNoteIndex].endTime - notes[editingNoteIndex].startTime);
            }
        }

        // Update velocity if in velocity edit mode
        if (isEditingVelocity) {
            // ... would update velocity based on Y position
        }

        // Repaint
        repaint();
        return;
    }

    // If editing velocity
    if (isEditingVelocity) {
        // Update velocity based on Y position
        int mouseY = event.y;
        int noteY = 0; // Would need to know which note
        // ... would update velocity
        repaint();
    }
}

//==============================================================================
void MidiEditor::mouseDoubleClick(const juce::MouseEvent& event)
{
    // Double-click on empty space - might open note properties
    // or start a new note at that position
    if (!isDraggingNote) {
        int mouseY = event.y;
        int mouseX = event.x;

        // Convert Y to note number
        int noteNumber = jmap(mouseY, getHeight(), 0, 0, 127);
        noteNumber = jclamp(noteNumber, 0, 127);

        // Add new note at mouse position
        int64_t mouseTime = visibleStartTime * getSampleRate() +
            jmap(mouseX, 0, getWidth(), visibleStartTime * getSampleRate(),
                 visibleEndTime * getSampleRate());

        addNote(noteNumber, 100, mouseTime, mouseTime + getSampleRate() / 4);
        selectedNotes.clear();
        selectedNotes.push_back(notes.back());
    }
}

//==============================================================================
void MidiEditor::keyPressed(const juce::KeyPress& key)
{
    // Handle keyboard commands for the piano roll
    if (key == juce::KeyPress('delete', juce::ModifierKeys::deleteKey, 0)) {
        // Delete selected notes
        deselectAllNotes();
        // Remove selected notes from list
        for (auto it = notes.rbegin(); it != notes.rend(); ) {
            if (std::find(selectedNotes.begin(), selectedNotes.end(), *it) != selectedNotes.end()) {
                it = notes.erase(std::next(it).base());
            }
            else {
                ++it;
            }
        }
        selectedNotes.clear();
        repaint();
    }
    else if (key == juce::KeyPress('c', juce::ModifierKeys::controlKey, 0)) {
        // Copy selected notes
        // ... would implement copy
    }
    else if (key == juce::KeyPress('x', juce::ModifierKeys::controlKey, 0)) {
        // Cut selected notes
        // ... would implement cut
    }
    else if (key == juce::KeyPress('v', juce::ModifierKeys::controlKey, 0)) {
        // Paste notes
        // ... would implement paste
    }
    else if (key == juce::KeyPress('z', juce::ModifierKeys::controlKey, 0)) {
        // Undo
        // ... would implement undo
    }
    else if (key == juce::KeyPress('y', juce::ModifierKeys::controlKey, 0)) {
        // Redo
        // ... would implement redo
    }
    else {
        // Handle arrow keys for note movement
        if (key == juce::KeyPress::arrowUpKey) {
            // Move selected notes up one semitone
            for (auto& note : selectedNotes) {
                note.noteNumber = jclamp(note.noteNumber - 1, 0, 127);
            }
            repaint();
        }
        else if (key == juce::KeyPress::arrowDownKey) {
            // Move selected notes down one semitone
            for (auto& note : selectedNotes) {
                note.noteNumber = jclamp(note.noteNumber + 1, 0, 127);
            }
            repaint();
        }
        else if (key == juce::KeyPress::arrowLeftKey) {
            // Move selected notes left (earlier)
            for (auto& note : selectedNotes) {
                note.startTime = jmax(0, note.startTime - getSampleRate() / 4); // 16th note
            }
            repaint();
        }
        else if (key == juce::KeyPress::arrowRightKey) {
            // Move selected notes right (later)
            for (auto& note : selectedNotes) {
                note.startTime = note.startTime + getSampleRate() / 4; // 16th note
            }
            repaint();
        }
    }
}

//==============================================================================
void MidiEditor::keyReleased(const juce::KeyPress& key)
{
    // Handle key release if needed
}

//==============================================================================
void MidiEditor::timerCallback()
{
    // Update transport position based on playback state
    // This would be connected to the actual transport
    // For now, just repaint to show playhead position
    repaint();
}

//==============================================================================
void MidiEditor::addNote(int noteNumber, int velocity, int64_t startTime, int64_t endTime)
{
    MidiNote note;
    note.noteNumber = jclamp(noteNumber, 0, 127);
    note.velocity = jclamp(velocity, 0, 127);
    note.startTime = jclamp(startTime, 0, 999999999);
    note.endTime = jclamp(endTime, 0, 999999999);
    note.isPlaying = false;
    note.isSelected = false;
    note.colour = noteColours[note.noteNumber];

    notes.push_back(note);
}

//==============================================================================
void MidiEditor::removeNote(int noteNumber, int64_t startTime)
{
    for (auto it = notes.begin(); it != notes.end(); ++it) {
        if (it->noteNumber == noteNumber && it->startTime == startTime) {
            notes.erase(it);
            return;
        }
    }
}

//==============================================================================
void MidiEditor::selectNote(int noteNumber, int64_t startTime)
{
    // Find and select note
    for (auto& note : notes) {
        if (note.noteNumber == noteNumber && note.startTime == startTime) {
            note.isSelected = true;
            selectedNotes.push_back(note);
            return;
        }
    }
}

//==============================================================================
void MidiEditor::deselectAllNotes()
{
    for (auto& note : notes) {
        note.isSelected = false;
    }
    selectedNotes.clear();
}

//==============================================================================
const std::vector<MidiNote>& MidiEditor::getSelectedNotes() const
{
    return selectedNotes;
}

//==============================================================================
const std::vector<MidiNote>& MidiEditor::getAllNotes() const
{
    return notes;
}

//==============================================================================
void MidiEditor::setTransportInfo(double position, double playbackRate)
{
    this->transportPosition = position;
    this->playbackRate = playbackRate;
    repaint(); // Update playhead position
}

//==============================================================================
double MidiEditor::getTransportPosition() const
{
    return transportPosition;
}

//==============================================================================
int64_t MidiEditor::editNoteTime(int noteIndex, int mouseX, bool isEnd /*= false*/,
                                  bool isStart /*= false*/)
{
    // Convert mouse X position to time
    int64_t mouseTime = visibleStartTime * getSampleRate() +
        jmap(mouseX, 0, getWidth(), visibleStartTime * getSampleRate(),
             visibleEndTime * getSampleRate());

    if (isEnd) {
        // Clip to valid range
        int64_t minTime = notes[noteIndex].startTime;
        int64_t maxTime = visibleEndTime * getSampleRate();
        return jmax(minTime, jmin(mouseTime, maxTime));
    }
    else if (isStart) {
        int64_t minTime = visibleStartTime * getSampleRate();
        int64_t maxTime = jmin(notes[noteIndex].endTime - 1, visibleEndTime * getSampleRate());
        return jmax(minTime, jmin(mouseTime, maxTime));
    }
    else {
        // Just return the time
        return mouseTime;
    }
}

//==============================================================================
// Helper: Map value from one range to another
int jmap(int value, int inMin, int inMax, int outMin, int outMax)
{
    return outMin + (outMax - outMin) * (value - inMin) / (inMax - inMin);
}

int jmap(double value, double inMin, double inMax, double outMin, double outMax)
{
    return outMin + (outMax - outMin) * (value - inMin) / (inMax - inMin);
}

int jclamp(int val, int minVal, int maxVal)
{
    return jmin(jmax(val, minVal), maxVal);
}