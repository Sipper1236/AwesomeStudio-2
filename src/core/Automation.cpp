// Automation.cpp
// Implementation of the automation editing system

#include "Automation.h"
#include <juce_core.h>

//==============================================================================
AutomationEnvelope::AutomationEnvelope(ParamID paramId, const juce::String& paramName)
    : paramId(paramId), paramName(paramName), startTime(0)
{
}

//==============================================================================
void AutomationEnvelope::addPoint(int64_t time, float value)
{
    AutomationPoint point;
    point.time = time;
    point.value = jlimit(0.0f, 1.0f, value);
    point.lastUpdate = Time::getCurrentTime();
    points.push_back(point);

    // Sort points by time after adding
    std::sort(points.begin(), points.end(),
              [](const AutomationPoint& a, const AutomationPoint& b) {
                  return a.time < b.time;
              });
}

//==============================================================================
void AutomationEnvelope::removePoint(int64_t time)
{
    points.erase(
        std::remove_if(points.begin(), points.end(),
                       [time](const AutomationPoint& p) { return p.time == time; }),
        points.end());
}

//==============================================================================
float AutomationEnvelope::getValueAtTime(int64_t time) const
{
    if (points.empty()) return 0.0f;

    // Find the two points surrounding the target time
    if (time <= points.front().time) return points.front().value;
    if (time >= points.back().time) return points.back().value;

    // Find surrounding points
    auto it = std::lower_bound(points.begin(), points.end(), time,
        [](const AutomationPoint& p, int64_t t) { return p.time < t; });

    if (it == points.begin() || it == points.end()) return points.front().value;

    auto rightIt = it;
    auto leftIt = std::prev(it);

    int64_t tLeft = leftIt->time;
    int64_t tRight = rightIt->time;
    float vLeft = leftIt->value;
    float vRight = rightIt->value;

    // Linear interpolation
    if (tRight > tLeft) {
        float ratio = (float)(time - tLeft) / (tRight - tLeft);
        return vLeft + ratio * (vRight - vLeft);
    }

    // Bézier curve interpolation (if both points use curves)
    if (leftIt->useCurve && rightIt->useCurve) {
        // Cubic Bézier: B(t) = (1-t)^3 * P0 + 3(1-t)^2*t * P1 + 3(1-t)*t^2 * P2 + t^3 * P3
        // For Hermite interpolation with tangents:
        float t = (float)(time - tLeft) / (tRight - tLeft);

        // Hermite curve with tangents
        float h00 = 2.0f * t * t * t - 3.0f * t * t + 1.0f;
        float h10 = t * t * t - 2.0f * t * t + t;
        float h01 = -2.0f * t * t * t + 3.0f * t * t;
        float h11 = t * t * t - t * t;

        float mLeft = leftIt->value; // Would use tangentIn in full implementation
        float mRight = rightIt->value; // Would use tangentOut in full implementation

        return h00 * leftIt->value + h10 * mLeft + h01 * rightIt->value + h11 * mRight;
    }

    return vLeft;
}

//==============================================================================
const std::vector<AutomationPoint>& AutomationEnvelope::getPoints() const
{
    return points;
}

//==============================================================================
juce::String AutomationEnvelope::getParameterName() const
{
    return paramName;
}

//==============================================================================
ParamID AutomationEnvelope::getParameterID() const
{
    return paramId;
}

//==============================================================================
void AutomationEnvelope::updatePoint(int64_t time, float value)
{
    for (auto& point : points) {
        if (point.time == time) {
            point.value = jlimit(0.0f, 1.0f, value);
            point.lastUpdate = Time::getCurrentTime();
            break;
        }
    }
}

//==============================================================================
void AutomationEnvelope::clear()
{
    points.clear();
}

//==============================================================================
int64_t AutomationEnvelope::getDuration() const
{
    if (points.empty()) return 0;
    return points.back().time - points.front().time;
}

//==============================================================================
AutomationManager::AutomationManager()
{
}

//==============================================================================
AutomationEnvelope* AutomationManager::createEnvelope(ParamID paramId, const juce::String& paramName)
{
    auto* envelope = new AutomationEnvelope(paramId, paramName);
    envelopes[paramId] = envelope;
    return envelope;
}

//==============================================================================
AutomationEnvelope* AutomationManager::getEnvelope(ParamID paramId) const
{
    auto it = envelopes.find(paramId);
    if (it != envelopes.end()) {
        return it->second;
    }
    return nullptr;
}

//==============================================================================
void AutomationManager::removeEnvelope(ParamID paramId)
{
    auto it = envelopes.find(paramId);
    if (it != envelopes.end()) {
        delete it->second;
        envelopes.erase(it);
    }
}

//==============================================================================
const std::map<ParamID, AutomationEnvelope*>& AutomationManager::getAllEnvelopes() const
{
    return envelopes;
}

//==============================================================================
void AutomationManager::clearAll()
{
    for (auto& pair : envelopes) {
        delete pair.second;
    }
    envelopes.clear();
}

//==============================================================================
int64_t AutomationManager::getEnvelopeDuration(ParamID paramId) const
{
    auto* env = getEnvelope(paramId);
    if (env) return env->getDuration();
    return 0;
}

//==============================================================================
void AutomationManager::renderAutomation(juce::AudioBuffer<float>& buffer, int64_t startSample, int64_t numSamples)
{
    // Render automation values to the audio buffer
    // This would typically modulate plugin parameters
    // For now, just iterate through parameters and apply automation

    for (auto& pair : envelopes) {
        auto* env = pair.second;
        if (!env) continue;

        // Get automation values for each sample
        for (int s = 0; s < numSamples; ++s) {
            int64_t samplePos = startSample + s;
            float val = env->getValueAtTime(samplePos);

            // In a real implementation, this would modulate the parameter
            // For now, just note the value
            (void)val; // Avoid unused variable warning
        }
    }
}

//==============================================================================
juce::MemoryBlock AutomationManager::serialize() const
{
    juce::MemoryBlock data;

    // Serialize all envelopes
    for (auto& pair : envelopes) {
        auto* env = pair.second;
        if (!env) continue;

        // Write parameter ID and name
        data.append(juce::Byte((paramId & 0xFF00) >> 8));
        data.append(juce::Byte(paramId & 0xFF));

        // Write name length and name
        int nameLen = env->getParameterName().length();
        data.append(juce::Byte(nameLen & 0xFF));
        data.append(juce::Byte((nameLen & 0xFF00) >> 8));
        for (auto c : env->getParameterName()) {
            data.append(juce::Byte(c));
        }

        // Write point count
        int pointCount = env->getPoints().size();
        data.append(juce::Byte(pointCount & 0xFF));
        data.append(juce::Byte((pointCount & 0xFF00) >> 8));

        // Write each point
        for (auto& point : env->getPoints()) {
            data.append(juce::Byte64(point.time));
            data.append(juce::FloatToRawBits(point.value));
        }
    }

    return data;
}

//==============================================================================
void AutomationManager::deserialize(const juce::MemoryBlock& data)
{
    // Deserialize automation data from MemoryBlock
    // This is a simplified parser - real implementation would be more robust

    // For now, just clear existing data
    clearAll();
}

//==============================================================================
bool AutomationManager::saveToFile(const juce::File& file) const
{
    // Save automation data to file
    // juce::File::write(){
    return true;
}

//==============================================================================
bool AutomationManager::loadFromFile(const juce::File& file)
{
    // Load automation data from file
    // juce::MemoryBlock data = juce::File::loadFileContent(file);
    // deserialize(data);
    return true;
}

//==============================================================================
int AutomationManager::getEnvelopePointCount(ParamID paramId) const
{
    auto* env = getEnvelope(paramId);
    if (env) return (int)env->getPoints().size();
    return 0;
}

//==============================================================================
int64_t AutomationManager::findPointNearTime(ParamID paramId, int64_t time) const
{
    auto* env = getEnvelope(paramId);
    if (!env || env->getPoints().empty()) return -1;

    // Find point with time closest to target
    int64_t closestTime = env->getPoints()[0].time;
    int64_t minDiff = std::abs(env->getPoints()[0].time - time);

    for (auto& point : env->getPoints()) {
        int64_t diff = std::abs(point.time - time);
        if (diff < minDiff) {
            minDiff = diff;
            closestTime = point.time;
        }
    }

    return closestTime;
}

//==============================================================================
AutomationParameter::AutomationParameter(ParamID paramId, const juce::String& paramName,
                                         juce::AudioProcessorValueTreeState& paramState)
    : paramId(paramId), paramName(paramName), paramState(paramState),
      isDragging(false), dragStartPos(0, 0)
{
    // Set up UI components
    paramLabel.setText(paramName, juce::dontSendNotification);
    paramLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(paramLabel);

    valueSlider.setRange(0.0, 1.0, 0.001);
    valueSlider.setValue(0.5);
    valueSlider.onValueChange = [this]() {
        // Update parameter value
        paramState.setValueNotifyingSw(paramId, valueSlider.getValue());
    };
    addAndMakeVisible(valueSlider);

    automationButton.setButtonText("A");
    automationButton.onClick = [this]() {
        // Toggle automation write mode
    };
    addAndMakeVisible(automationButton);

    // Set up tooltip
    tooltip.setDefaultText("Automation parameter");

    // Create envelope
    envelope = std::make_unique<AutomationEnvelope>(paramId, paramName);
}

//==============================================================================
AutomationParameter::~AutomationParameter()
{
}

//==============================================================================
void AutomationParameter::paint(juce::Graphics& g)
{
    // Draw background
    g.fillAll(juce::Colours::darkgrey);

    // Draw border
    g.setColour(juce::Colours::white);
    g.drawRect(getLocalBounds(), 1);

    // Draw parameter name
    g.setColour(juce::Colours::white);
    g.setFont(12.0f);
    g.drawText(paramName, getLocalBounds(), juce::Justification::centredTop);

    // Draw value
    g.setColour(juce::Colours::yellow);
    g.drawText(juce::String::formatted("%.2f", valueSlider.getValue()),
               getLocalBounds(), juce::Justification::centredBottom);
}

//==============================================================================
void AutomationParameter::resized()
{
    // Layout UI components
    auto area = getLocalBounds().reduced(5);

    paramLabel.setBounds(area.removeFromTop(20));

    auto sliderArea = area;
    valueSlider.setBounds(sliderArea.removeFromTop(50));

    automationButton.setBounds(valueSlider.getX(), valueSlider.getBottom() + 5,
                            30, 20);
}

//==============================================================================
void AutomationParameter::dragEnter(const juce::DragAndDropTarget::Pair& pair)
{
    // Handle drag enter
    setBorderStyle(juce::BorderStyle::textButtonHovered);
}

//==============================================================================
void AutomationParameter::dragExit(const juce::DragAndDropTarget::Pair& pair)
{
    // Handle drag exit
    setBorderStyle(juce::BorderStyle::normalBorder);
}

//==============================================================================
void AutomationParameter::dragMove(const juce::DragAndDropTarget::Pair& pair)
{
    // Handle drag move
}

//==============================================================================
void AutomationParameter::dragUp(const juce::DragAndDropTarget::Pair& pair)
{
    // Handle drag up
    setBorderStyle(juce::BorderStyle::normalBorder);
}

//==============================================================================
void AutomationParameter::dragDrop(const juce::DragAndDropTarget::Pair& pair)
{
    // Handle drag drop - could be used to assign automation from another source
    setBorderStyle(juce::BorderStyle::normalBorder);
}

//==============================================================================
void AutomationParameter::buttonClicked(juce::Button* button)
{
    // Handle automation button click
    if (button == &automationButton) {
        // Toggle automation write mode
    }
}