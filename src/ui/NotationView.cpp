// NotationView.cpp
// MIDI notation view implementation for Awesome Studio DAW

#include "NotationView.h"
#include <JuceHeader.h>
#include <cmath>

//==============================================================================
NotationView::NotationView()
    : timeSigNumerator(4), timeSigDenominator(4), keySignature(0), keyMode(0),
      transportPosition(0), isEditing(false), editingNoteIndex(-1)
{
    // Initialize fonts
    noteFont = juce::Font("Bravura Text", 16.0f, juce::Font::bold);
    lyricFont = juce::Font("Times New Roman", 12.0f);
    chordFont = juce::Font("Times New Roman", 14.0f, juce::Font::bold);

    // Initialize colors
    noteColour = juce::Colours::black;
    lyricColour = juce::Colours::darkgrey;
    chordColour = juce::Colours::blue;
    staffColour = juce::Colours::black;
    playheadColour = juce::Colours::red;

    // Set up mouse and key listeners
    setMouseClickGrabsKeyboardFocus(true);
}

//==============================================================================
NotationView::~NotationView()
{
}

//==============================================================================
void NotationView::paint(juce::Graphics& g)
{
    // Fill background
    g.fillAll(juce::Colours::white);

    // Draw staff lines
    drawStaffLines(g, 0, 0, getWidth());

    // Draw notes
    drawNotes(g, 0, 0, getWidth());

    // Draw chord symbols
    drawChordSymbols(g, 0, 0, getWidth());

    // Draw lyrics
    drawLyrics(g, 0, 0, getWidth());

    // Draw playhead
    drawPlayhead(g, 0, 0, getWidth());
}

//==============================================================================
void NotationView::resized()
{
    // Component resized - no child components to layout explicitly
}

//==============================================================================
void NotationView::drawStaffLines(juce::Graphics& g, int x, int y, int width)
{
    // Draw standard 5-line staff
    g.setColour(staffColour);

    // Staff line spacing (10 pixels between lines)
    const int lineSpacing = 10;
    const int staffHeight = 4 * lineSpacing; // 5 lines total

    // Calculate center position
    int centerY = y + getHeight() / 2;

    // Draw 5 staff lines
    for (int i = 0; i < 5; ++i) {
        int lineY = centerY - staffHeight / 2 + i * lineSpacing;
        g.drawLine(x, lineY, x + width, lineY, 1.0f);
    }
}

//==============================================================================
void NotationView::drawNotes(juce::Graphics& g, int x, int y, int width)
{
    // Draw MIDI notes as sheet music notation
    g.setColour(noteColour);
    g.setFont(noteFont);

    // Calculate note positions based on time
    for (const auto& note : notes) {
        // Calculate x position based on time
        float noteX = x + (note.startTime / 1000.0f) * width;

        // Calculate y position based on pitch
        // MIDI note 60 (middle C) is at center of staff
        int noteY = y + getHeight() / 2 - (note.noteNumber - 60) * 5;

        // Draw note head
        g.drawEllipse(noteX - 5, noteY - 5, 10, 10, 1.0f);

        // Draw stem if needed (for notes above/below staff)
        if (note.noteNumber > 71 || note.noteNumber < 48) { // Above/below staff
            // Draw stem
            int stemY = note.noteNumber > 71 ? noteY - 20 : noteY + 20;
            g.drawLine(noteX, noteY, noteX, stemY, 1.0f);

            // Draw flag if needed (for 8th notes and shorter)
            if (note.endTime - note.startTime < 1000) { // 8th note or shorter
                g.drawLine(noteX, stemY, noteX + 10, stemY - 10, 1.0f);
            }
        }

        // Draw note name
        juce::String noteName = juce::MidiMessage::getMidiNoteName(note.noteNumber, true, true, 3);
        g.drawText(noteName, noteX + 10, noteY - 10, 30, 20, juce::Justification::centred);
    }
}

//==============================================================================
void NotationView::drawChordSymbols(juce::Graphics& g, int x, int y, int width)
{
    // Draw chord symbols
    g.setColour(chordColour);
    g.setFont(chordFont);

    for (const auto& chord : chordSymbols) {
        // Calculate x position based on time
        float chordX = x + (chord.time / 1000.0f) * width;

        // Draw chord symbol above staff
        g.drawText(chord.symbol, chordX, y + 20, 50, 20, juce::Justification::centred);
    }
}

//==============================================================================
void NotationView::drawLyrics(juce::Graphics& g, int x, int y, int width)
{
    // Draw lyrics under notes
    g.setColour(lyricColour);
    g.setFont(lyricFont);

    for (const auto& note : notes) {
        if (!note.lyric.isEmpty()) {
            // Calculate x position based on time
            float lyricX = x + (note.startTime / 1000.0f) * width;

            // Draw lyric below staff
            g.drawText(note.lyric, lyricX, y + getHeight() - 30, 50, 20, juce::Justification::centred);
        }
    }
}

//==============================================================================
void NotationView::drawPlayhead(juce::Graphics& g, int x, int y, int width)
{
    // Draw playhead at current transport position
    g.setColour(playheadColour);

    // Calculate x position based on time
    float playheadX = x + (transportPosition / 1000.0f) * width;

    // Draw vertical line
    g.drawLine(playheadX, y, playheadX, y + getHeight(), 2.0f);
}

//==============================================================================
void NotationView::addNote(int noteNumber, int velocity, int64_t startTime, int64_t endTime, const juce::String& lyric)
{
    NotationNote note;
    note.noteNumber = juce::jlimit(0, 127, noteNumber);
    note.velocity = juce::jlimit(0, 127, velocity);
    note.startTime = startTime;
    note.endTime = endTime;
    note.lyric = lyric;

    notes.push_back(note);
    repaint();
}

//==============================================================================
void NotationView::addChordSymbol(int64_t time, const juce::String& symbol)
{
    ChordSymbol chord;
    chord.time = time;
    chord.symbol = symbol;

    chordSymbols.push_back(chord);
    repaint();
}

//==============================================================================
void NotationView::setTimeSignature(int numerator, int denominator)
{
    timeSigNumerator = numerator;
    timeSigDenominator = denominator;
    repaint();
}

//==============================================================================
void NotationView::setKeySignature(int key, int mode)
{
    keySignature = key;
    keyMode = mode;
    repaint();
}

//==============================================================================
void NotationView::setTransportPosition(int64_t position)
{
    transportPosition = position;
    repaint();
}

//==============================================================================
juce::Size NotationView::getPreferredSize() const
{
    // Return preferred size for the notation view
    return juce::Size(800, 300); // Default size
}

//==============================================================================
void NotationView::mouseDown(const juce::MouseEvent& event)
{
    // Handle mouse down event
    // Could be used for note selection or editing
}

//==============================================================================
void NotationView::mouseDrag(const juce::MouseEvent& event)
{
    // Handle mouse drag event
    // Could be used for note dragging
}

//==============================================================================
void NotationView::mouseUp(const juce::MouseEvent& event)
{
    // Handle mouse up event
}

//==============================================================================
void NotationView::mouseDoubleClick(const juce::MouseEvent& event)
{
    // Handle mouse double click event
    // Could be used for note editing
}

//==============================================================================
void NotationView::keyPressed(const juce::KeyPress& key)
{
    // Handle key press event
    // Could be used for note editing via keyboard
}

//==============================================================================
void NotationView::keyReleased(const juce::KeyPress& key)
{
    // Handle key release event
}