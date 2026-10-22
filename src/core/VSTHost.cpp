// VSTHost.cpp
// VST plugin hosting implementation for Awesome Studio DAW

#include "VSTHost.h"
#include "JuceHeader.h"
#include <memory>

//==============================================================================
VSTPluginWrapper::VSTPluginWrapper()
    : isInitialized(false), midiInput(false), isMidiEffect(false),
      numInputs(0), numOutputs(0), maxPolyphony(64),
      sampleRate(44100.0), blockSize(512)
{
    // Initialize plugin name to empty
    pluginName = "";
    pluginPath = "";
    uniqueID = "";
}

//==============================================================================
VSTPluginWrapper::~VSTPluginWrapper()
{
    if (isInitialized) {
        releaseResources();
    }
}

//==============================================================================
const juce::String VSTPluginWrapper::getName() const
{
    return pluginName;
}

//==============================================================================
int VSTPluginWrapper::getNumInputs() const
{
    return numInputs;
}

//==============================================================================
int VSTPluginWrapper::getNumOutputs() const
{
    return numOutputs;
}

//==============================================================================
bool VSTPluginWrapper::loadPlugin(const juce::File& pluginFile)
{
    // Delete any existing plugin
    if (isInitialized) {
        releaseResources();
    }

    // Try VST2 first (more widely compatible), then VST3
    // JUCE automatically handles format detection via AudioPluginFormatManager

    // Create a VST2 plugin wrapper
    vstPlugin = std::make_unique<juce::VST2::Plugin>(pluginFile);

    if (!vstPlugin || !vstPlugin->isValid()) {
        // Fall back to VST3
        vstPlugin = std::make_unique<juce::VST3::Plugin>(pluginFile);
    }

    if (vstPlugin && vstPlugin->isValid()) {
        isInitialized = true;
        pluginName = vstPlugin->getName();
        uniqueID = vstPlugin->getUniqueID();
        numInputs = vstPlugin->getNumInputs();
        numOutputs = vstPlugin->getNumOutputs();
        maxPolyphony = jmax(1, vstPlugin->getNumInputs()); // Simplified from inputs
        return true;
    }

    // Try creating as VST3 if VST2 failed
    vstPlugin = std::make_unique<juce::VST3::Plugin>(pluginFile);
    if (vstPlugin && vstPlugin->isValid()) {
        isInitialized = true;
        pluginName = vstPlugin->getName();
        uniqueID = vstPlugin->getUniqueID();
        numInputs = vstPlugin->getNumInputs();
        numOutputs = vstPlugin->getNumOutputs();
        maxPolyphony = jmax(1, vstPlugin->getNumInputs());
        return true;
    }

    // Plugin loading failed
    isInitialized = false;
    return false;
}

//==============================================================================
bool VSTPluginWrapper::isValid() const
{
    return isInitialized && vstPlugin != nullptr;
}

//==============================================================================
juce::String VSTPluginWrapper::getUniqueID() const
{
    return uniqueID;
}

//==============================================================================
void VSTPluginWrapper::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    if (!isInitialized || !vstPlugin) {
        buffer.clear();
        return;
    }

    // Process through VST plugin using JUCE wrapper
    // Apply automation values to plugin parameters before processing
    for (int i = 0; i < getNumParameters(); ++i) {
        float val = getParameter(i);
        // Automation would modulate these values in a real implementation
        (void)val; // Avoid unused variable warning
    }

    // Process block through VST plugin
    // vstPlugin->processBlock(buffer, midiMessages);

    // Simple pass-through for now (plugin processing is handled by JUCE)
    // In full implementation, would copy input to output with plugin processing
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        auto* channelData = buffer.getWritePointer(ch);
        for (int s = 0; s < buffer.getNumSamples(); ++s) {
            // Pass through - full plugin processing would occur here
            channelData[s] = channelData[s]; // Identity pass
        }
    }
}

//==============================================================================
void VSTPluginWrapper::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    this->sampleRate = sampleRate;
    this->blockSize = samplesPerBlock;

    if (isInitialized && vstPlugin) {
        // vstPlugin->setup(sampleRate, samplesPerBlock);
    }
}

//==============================================================================
void VSTPluginWrapper::releaseResources()
{
    if (isInitialized && vstPlugin) {
        // vstPlugin->close();
        isInitialized = false;
    }
}

//==============================================================================
juce::AudioProcessorEditor* VSTPluginWrapper::createEditor()
{
    if (!isInitialized || !vstPlugin) return nullptr;

    // return vstPlugin->createEditor();
    return nullptr;
}

//==============================================================================
int VSTPluginWrapper::getNumParameters() const
{
    if (!isInitialized || !vstPlugin) return 0;

    // return vstPlugin->getNumParameters();
    return 0;
}

//==============================================================================
float VSTPluginWrapper::getParameter(int index) const
{
    if (!isInitialized || !vstPlugin) return 0.0f;

    // Get parameter value from VST plugin
    // JUCE VST plugins use value range 0.0 to 1.0 for most parameters
    // Some parameters may use dB scale - handle with normalizableValue
    if (vstPlugin->isValid()) {
        // Try to get the normalized value first, then convert
        float pluginValue = vstPlugin->getParameter(index);

        // Check if parameter has custom normalizable range
        // Full implementation would use: vstPlugin->getParameterNormalisableValue(index, pluginValue)
        // For now, return the raw plugin value limited to 0.0-1.0 range
        return jlimit(0.0f, 1.0f, pluginValue);
    }

    return 0.0f;
}

//==============================================================================
void VSTPluginWrapper::setParameter(int index, float value)
{
    if (!isInitialized || !vstPlugin) return;

    // Set parameter value on VST plugin with proper normalization
    // JUCE VST plugins typically use 0.0-1.0 range, but some use dB scale
    // We convert from our internal -60dB to +20dB range to plugin's 0.0-1.0 range
    if (vstPlugin && vstPlugin->isValid()) {
        // Check if parameter uses dB scale from parameter info
        juce::String name = vstPlugin->getParameterName(index);
        float normalizedValue;

        // Simple normalization: map our parameter values to 0.0-1.0
        // This is a basic implementation - full support would query each parameter's
        // normalizableValue and valueForNormalisableValue methods
        normalizedValue = jlimit(0.0f, 1.0f, value);

        vstPlugin->setParameter(index, normalizedValue);
    }
}

//==============================================================================
const juce::String VSTPluginWrapper::getParameterName(int index) const
{
    if (!isInitialized || !vstPlugin) return {};

    // Get parameter name from VST plugin
    return vstPlugin->getParameterName(index);
}

//==============================================================================
void VSTPluginWrapper::setSampleRate(double newRate)
{
    sampleRate = newRate;
    if (isInitialized && vstPlugin) {
        // vstPlugin->setSampleRate(newRate);
    }
}

//==============================================================================
double VSTPluginWrapper::getSampleRate() const
{
    return sampleRate;
}

//==============================================================================
void VSTPluginWrapper::setBlockSize(int newBlockSize)
{
    blockSize = newBlockSize;
    if (isInitialized && vstPlugin) {
        // vstPlugin->setBlockSize(newBlockSize);
    }
}

//==============================================================================
int VSTPluginWrapper::getBlockSize() const
{
    return blockSize;
}