// MeteringView.cpp
// Advanced metering implementation for Awesome Studio DAW

#include "MeteringView.h"
#include <JuceHeader.h>
#include <cmath>

//==============================================================================
MeteringView::MeteringView()
    : minLufs(-48.0f), maxLufs(0.0f), channels(), backgroundColour(juce::Colours::lightgrey),
      peakColour(juce::Colours::red), rmsColour(juce::Colours::yellow),
      truePeakColour(juce::Colours::white), lufsColour(juce::Colours::darkblue),
      textColour(juce::Colours::black)
{
    // Set up timer for metering updates
    startTimer(10); // Update every 10ms

    // Initialize fonts
    labelFont = juce::Font("Arial", 10.0f);
    valueFont = juce::Font("Arial", 12.0f);
}

//==============================================================================
MeteringView::~MeteringView()
{
    stopTimer();
}

//==============================================================================
void MeteringView::paint(juce::Graphics& g)
{
    // Fill background
    g.fillAll(backgroundColour);

    // Draw channel meters
    for (const auto& channel : channels) {
        // Calculate position
        int x = 10 + (channel.channelIndex * 100); // 100px per channel
        int y = 10;
        int width = 90; // Width of each meter
        int height = 100; // Height of each meter

        // Draw channel label
        g.setColour(textColour);
        g.setFont(labelFont);
        g.drawText("Channel", x, y + height + 10, width, 20, juce::Justification::left);
        g.drawText("#" + juce::String(channel.channelIndex + 1), x + 10, y + height + 10, width - 20, 20, juce::Justification::left);

        // Draw peak meter
        drawPeakMeter(g, x, y, width, height, channel.peakLevel);

        // Draw RMS meter
        drawRmsMeter(g, x, y + height + 20, width, height, channel.rmsLevel);

        // Draw true peak meter
        drawTruePeakMeter(g, x, y + height + 40, width, height, channel.truePeak);

        // Draw LUFS meter
        drawLufsMeter(g, x, y + height + 60, width, height, channel.lufsLevel);
    }
}

//==============================================================================
void MeteringView::resized()
{
    // Component resized - no child components to layout explicitly
}

//==============================================================================
void MeteringView::timerCallback()
{
    // Repaint to update metering
    repaint();
}

//==============================================================================
void MeteringView::drawPeakMeter(juce::Graphics& g, int x, int y, int width, int height, float level)
{
    // Draw peak meter background
    g.setColour(juce::Colours::lightgrey);
    g.fillRect(x, y, width, height);

    // Draw peak meter needle
    g.setColour(peakColour);
    g.drawLine(x + width / 2, y, x + width / 2, y + height, 2.0f);

    // Draw peak level indicator
    float needleHeight = height * level;
    g.setColour(peakColour.withAlpha(0.5f));
    g.fillRect(x + width / 2 - 2, y + height - needleHeight, 4, needleHeight);

    // Draw peak level text
    g.setColour(textColour);
    g.setFont(valueFont);
    g.drawText("P" + juce::String::floatToString(level, 2), x + 10, y + height + 10, width - 20, 20, juce::Justification::left);
}

//==============================================================================
void MeteringView::drawRmsMeter(juce::Graphics& g, int x, int y, int width, int height, float level)
{
    // Draw RMS meter background
    g.setColour(juce::Colours::lightgrey);
    g.fillRect(x, y, width, height);

    // Draw RMS meter needle
    g.setColour(rmsColour);
    g.drawLine(x + width / 2, y, x + width / 2, y + height, 2.0f);

    // Draw RMS level indicator
    float needleHeight = height * level;
    g.setColour(rmsColour.withAlpha(0.5f));
    g.fillRect(x + width / 2 - 2, y + height - needleHeight, 4, needleHeight);

    // Draw RMS level text
    g.setColour(textColour);
    g.setFont(valueFont);
    g.drawText("RMS" + juce::String::floatToString(level, 2), x + 10, y + height + 10, width - 20, 20, juce::Justification::left);
}

//==============================================================================
void MeteringView::drawTruePeakMeter(juce::Graphics& g, int x, int y, int width, int height, float level)
{
    // Draw true peak meter background
    g.setColour(juce::Colours::lightgrey);
    g.fillRect(x, y, width, height);

    // Draw true peak meter needle
    g.setColour(truePeakColour);
    g.drawLine(x + width / 2, y, x + width / 2, y + height, 2.0f);

    // Draw true peak level indicator
    float needleHeight = height * level;
    g.setColour(truePeakColour.withAlpha(0.5f));
    g.fillRect(x + width / 2 - 2, y + height - needleHeight, 4, needleHeight);

    // Draw true peak level text
    g.setColour(textColour);
    g.setFont(valueFont);
    g.drawText("TP" + juce::String::floatToString(level, 2), x + 10, y + height + 10, width - 20, 20, juce::Justification::left);
}

//==============================================================================
void MeteringView::drawLufsMeter(juce::Graphics& g, int x, int y, int width, int height, float level)
{
    // Draw LUFS meter background
    g.setColour(juce::Colours::lightgrey);
    g.fillRect(x, y, width, height);

    // Draw LUFS meter needle
    g.setColour(lufsColour);
    g.drawLine(x + width / 2, y, x + width / 2, y + height, 2.0f);

    // Draw LUFS level indicator
    float needleHeight = height * (level - minLufs) / (maxLufs - minLufs);
    g.setColour(lufsColour.withAlpha(0.5f));
    g.fillRect(x + width / 2 - 2, y + height - needleHeight, 4, needleHeight);

    // Draw LUFS level text
    g.setColour(textColour);
    g.setFont(valueFont);
    g.drawText("LUFS: " + juce::String::floatToString(level, 1), x + 10, y + height + 10, width - 20, 20, juce::Justification::left);
}

//==============================================================================
void MeteringView::updateChannel(int channelIndex, float peak, float rms, float truePeak, float lufs)
{
    if (channelIndex >= 0 && channelIndex < (int)channels.size()) {
        channels[channelIndex].peakLevel = jlimit(0.0f, 1.0f, peak);
        channels[channelIndex].rmsLevel = jlimit(0.0f, 1.0f, rms);
        channels[channelIndex].truePeak = jlimit(0.0f, 1.0f, truePeak);
        channels[channelIndex].lufsLevel = lufs;

        repaint();
    }
}

//==============================================================================
void MeteringView::addChannel(int channelIndex, const juce::Colour& colour)
{
    if (channelIndex >= 0 && channelIndex < (int)channels.size()) {
        channels[channelIndex].colour = colour;
        repaint();
    }
    else {
        MeteringChannel channel;
        channel.channelIndex = channelIndex;
        channel.colour = colour;
        channels.push_back(channel);
        repaint();
    }
}

//==============================================================================
void MeteringView::removeChannel(int channelIndex)
{
    if (channelIndex >= 0 && channelIndex < (int)channels.size()) {
        channels.erase(channels.begin() + channelIndex);
        repaint();
    }
}

//==============================================================================
void MeteringView::setLufsRange(float minLufs, float maxLufs)
{
    this->minLufs = minLufs;
    this->maxLufs = maxLufs;
    repaint();
}

//==============================================================================
juce::Size MeteringView::getPreferredSize() const
{
    // Return preferred size for the metering view
    return juce::Size(1200, 200); // Default size
}