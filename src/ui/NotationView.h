// NotationView.h
// MIDI notation view for Awesome Studio DAW

#pragma once

#include <JuceHeader.h>
#include <vector>
#include <map>
#include <string>

// Note structure for notation view
struct NotationNote
{
    int noteNumber;      // MIDI note number (0-127)
    int64_t startTime;    // Start time in samples
    int64_t endTime;      // End time in samples
    int velocity;       // Note velocity (0-127)
    juce::String lyric;    // Lyric text (if any)
};

// Chord symbol structure
struct ChordSymbol
{
    int64_t time;           // Time in samples
    juce::String symbol;    // Chord symbol (e.g. "Cmaj7")
};

// Notation view component
class NotationView : public juce::Component,
                      public juce::MouseListener,
                      public juce::KeyListener
{
public:
    NotationView();
    ~NotationView() override;

    // Component methods
    void paint(juce::Graphics& g) override;
    void resized() override;

    // Mouse events
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;

    // Key events
    void keyPressed(const juce::KeyPress& key) override;
    void keyReleased(const juce::KeyPress& key) override;

    // Add MIDI note to notation
    void addNote(int noteNumber, int velocity, int64_t startTime, int64_t endTime, const juce::String& lyric = {});

    // Add chord symbol
    void addChordSymbol(int64_t time, const juce::String& symbol);

    // Set time signature
    void setTimeSignature(int numerator, int denominator);

    // Set key signature
    void setKeySignature(int key, int mode);

    // Set transport position
    void setTransportPosition(int64_t position);

    // Get preferred size
    juce::Size getPreferredSize() const override;

private:
    // Drawing methods
    void drawStaffLines(juce::Graphics& g, int x, int y, int width);
    void drawNotes(juce::Graphics& g, int x, int y, int width);
    void drawChordSymbols(juce::Graphics& g, int x, int y, int width);
    void drawLyrics(juce::Graphics& g, int x, int y, int width);
    void drawPlayhead(juce::Graphics& g, int x, int y, int width);

    // Note data
    std::vector<NotationNote> notes;
    std::vector<ChordSymbol> chordSymbols;

    // Time signature
    int timeSigNumerator;
    int timeSigDenominator;

    // Key signature
    int keySignature;
    int keyMode; // 0=major, 1=minor

    // Transport position
    int64_t transportPosition;

    // Editing state
    bool isEditing;
    int editingNoteIndex;
    juce::Point<int> dragStartPos;

    // Fonts
    juce::Font noteFont;
    juce::Font lyricFont;
    juce::Font chordFont;

    // Colors
    juce::Colour noteColour;
    juce::Colour lyricColour;
    juce::Colour chordColour;
    juce::Colour staffColour;
    juce::Colour playheadColour;
};