// MeteringView.h
// Advanced metering view for Awesome Studio DAW

#pragma once

#include <JuceHeader.h>
#include <vector>

// Metering channel structure
struct MeteringChannel
{
    float peakLevel;      // Current peak level (0.0 to 1.0)
    float rmsLevel;       // Current RMS level (0.0 to 1.0)
    float truePeak;       // Current true peak level (0.0 to 1.0)
    float lufsLevel;      // Current LUFS level (LUFS value)
    int channelIndex;    // Channel index
    juce::Colour colour; // Channel color
};

// Advanced metering view component
class MeteringView : public juce::Component,
                       public juce::Timer
{
public:
    MeteringView();
    ~MeteringView() override;

    // Component methods
    void paint(juce::Graphics& g) override;
    void resized() override;

    // Timer callback for updates
    void timerCallback() override;

    // Update metering data for a channel
    void updateChannel(int channelIndex, float peak, float rms, float truePeak, float lufs);

    // Add a new channel
    void addChannel(int channelIndex, const juce::Colour& colour);

    // Remove a channel
    void removeChannel(int channelIndex);

    // Set LUFS range
    void setLufsRange(float minLufs, float maxLufs);

    // Get preferred size
    juce::Size getPreferredSize() const override;

private:
    // Drawing methods
    void drawPeakMeter(juce::Graphics& g, int x, int y, int width, int height, float level);
    void drawRmsMeter(juce::Graphics& g, int x, int y, int width, int height, float level);
    void drawTruePeakMeter(juce::Graphics& g, int x, int y, int width, int height, float level);
    void drawLufsMeter(juce::Graphics& g, int x, int y, int width, int height, float level);

    // Metering data
    std::vector<MeteringChannel> channels;

    // LUFS range
    float minLufs;
    float maxLufs;

    // Colors
    juce::Colour backgroundColour;
    juce::Colour peakColour;
    juce::Colour rmsColour;
    juce::Colour truePeakColour;
    juce::Colour lufsColour;
    juce::Colour textColour;

    // Fonts
    juce::Font labelFont;
    juce::Font valueFont;
};
