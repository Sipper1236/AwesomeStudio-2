// Automation.h
// Automation editing system for Awesome Studio DAW

#pragma once

#include <JuceHeader.h>
#include <vector>
#include <map>
#include <string>

// Automation parameter ID
using ParamID = int;

// Automation curve point for smooth interpolation
struct AutomationCurvePoint
{
    int64_t time;
    float value;
    float tangentIn;      // Incoming tangent for smooth curve
    float tangentOut;     // Outgoing tangent for smooth curve
};

// Automation point structure
struct AutomationPoint
{
    int64_t time;          // Time in samples
    float value;           // Parameter value (0.0 to 1.0)
    int64_t lastUpdate;    // Last update timestamp
    bool useCurve;         // Whether to use Bézier curve interpolation
};

// Automation curve point for smooth interpolation
struct AutomationCurvePoint
{
    int64_t time;
    float value;
    float tangentIn;      // Incoming tangent for smooth curve
    float tangentOut;     // Outgoing tangent for smooth curve
};

// Automation envelope data
class AutomationEnvelope
{
public:
    // Create new envelope
    AutomationEnvelope(ParamID paramId, const juce::String& paramName);

    // Add a point to the envelope
    void addPoint(int64_t time, float value);

    // Remove a point
    void removePoint(int64_t time);

    // Get value at specific time (with interpolation)
    float getValueAtTime(int64_t time) const;

    // Get all points
    const std::vector<AutomationPoint>& getPoints() const;

    // Get envelope name
    juce::String getParameterName() const;

    // Get parameter ID
    ParamID getParameterID() const;

    // Update a point
    void updatePoint(int64_t time, float value);

    // Clear all points
    void clear();

    // Get envelope duration
    int64_t getDuration() const;

private:
    ParamID paramId;
    juce::String paramName;
    std::vector<AutomationPoint> points;
    juce::AudioSamplePosition startTime;
};

// Automation data manager
class AutomationManager
{
public:
    // Create new automation envelope
    AutomationEnvelope* createEnvelope(ParamID paramId, const juce::String& paramName);

    // Get existing envelope
    AutomationEnvelope* getEnvelope(ParamID paramId) const;

    // Remove envelope
    void removeEnvelope(ParamID paramId);

    // Get all envelopes
    const std::map<ParamID, AutomationEnvelope*>& getAllEnvelopes() const;

    // Clear all automation
    void clearAll();

    // Get envelope for parameter
    int64_t getEnvelopeDuration(ParamID paramId) const;

    // Render automation to audio buffer
    void renderAutomation(juce::AudioBuffer<float>& buffer, int64_t startSample, int64_t numSamples);

    // Serialize automation data
    juce::MemoryBlock serialize() const;

    // Deserialize automation data
    void deserialize(const juce::MemoryBlock& data);

    // Save to file
    bool saveToFile(const juce::File& file) const;

    // Load from file
    bool loadFromFile(const juce::File& file);

    // Get envelope point count
    int getEnvelopePointCount(ParamID paramId) const;

    // Find point closest to time
    int64_t findPointNearTime(ParamID paramId, int64_t time) const;

private:
    std::map<ParamID, AutomationEnvelope*> envelopes;
};

// Automation parameter component for UI
class AutomationParameter : public juce::Component,
                            public juce::DragAndDropTarget,
                            public juce::Button::ButtonListener
{
public:
    AutomationParameter(ParamID paramId, const juce::String& paramName,
                        juce::AudioProcessorValueTreeState& paramState);

    ~AutomationParameter() override;

    // Component painting
    void paint(juce::Graphics& g) override;
    void resized() override;

    // Drag and drop
    void dragEnter(const juce::DragAndDropTarget::Pair& pair) override;
    void dragExit(const juce::DragAndDropTarget::Pair& pair) override;
    void dragMove(const juce::DragAndDropTarget::Pair& pair) override;
    void dragUp(const juce::DragAndDropTarget::Pair& pair) override;
    void dragDrop(const juce::DragAndDropTarget::Pair& pair) override;

    // Button listener
    void buttonClicked(juce::Button* button) override;

    // Get preferred size
    juce::Size getPreferredSize() const override;

private:
    ParamID paramId;
    juce::String paramName;
    juce::AudioProcessorValueTreeState& paramState;

    // UI components
    juce::Label paramLabel;
    juce::Slider valueSlider;
    juce::Button automationButton; // Toggle automation write
    juce::TooltipToolTip tooltip;

    // Automation data
    std::unique_ptr<AutomationEnvelope> envelope;

    // Drag source info
    bool isDragging;
    juce::Point<int> dragStartPos;
};