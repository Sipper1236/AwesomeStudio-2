// MidiEditor.h
// MIDI piano roll editor for Awesome Studio DAW

#pragma once

#include <JuceHeader.h>
#include <vector>
#include <map>
#include <string>

struct MidiNote
{
    int noteNumber;      // 0-127
    int velocity;        // 0-127
    int64_t startTime;   // In samples
    int64_t endTime;     // In samples
    bool isPlaying;      // Currently playing/active
    bool isSelected;     // User selection
    juce::Colour colour; // Note color for visualization
};

class MidiEditor : public juce::Component,
                   public juce::MouseListener,
                   public juce::KeyListener,
                   public juce::Timer
{
public:
    MidiEditor();
    ~MidiEditor() override;

    // Component painting and sizing
    void paint(juce::Graphics& g) override;
    void resized() override;

    // Mouse events
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;

    // Key events
    void keyPressed(const juce::KeyPress& key) override;
    void keyReleased(const juce::KeyPress& key) override;
    bool keyFocusChanged(bool) override { return false; }

    // Timer for updates
    void timerCallback() override;

    // Note operations
    void addNote(int noteNumber, int velocity, int64_t startTime, int64_t endTime);
    void removeNote(int noteNumber, int64_t startTime);
    void selectNote(int noteNumber, int64_t startTime);
    void deselectAllNotes();

    // Get selected notes
    const std::vector<MidiNote>& getSelectedNotes() const;

    // Get all notes
    const std::vector<MidiNote>& getAllNotes() const;

    // Set the MIDI editor's host transport info
    void setTransportInfo(double position, double playbackRate);

    // Get the transport position
    double getTransportPosition() const;

private:
    std::vector<MidiNote> notes;
    std::vector<MidiNote> selectedNotes;

    // Editing state
    bool isEditing;
    int editingNoteIndex;
    int64_t editingStartTime;
    juce::Rectangle<int> editingRegion;

    // UI state
    int64_t transportPosition;
    double playbackRate;

    // Zoom/scroll
    double horizontalZoom;
    double verticalZoom;
    int64_t visibleStartTime;
    int64_t visibleEndTime;

    // Mouse drag state
    bool isDraggingNote;
    bool isDraggingBottomEdge;
    bool isDraggingLeftEdge;
    juce::Point<int> dragStartPos;

    // Velocity edit mode
    bool isEditingVelocity;

    // Note color palette
    std::map<int, juce::Colour> noteColours;
};