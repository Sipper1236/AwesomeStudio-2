// BatchProcessor.h
// Batch processing system for Awesome Studio DAW

#pragma once

#include <JuceHeader.h>
#include <vector>
#include <string>
#include <functional>

// Batch job structure
struct BatchJob
{
    juce::File inputFile;
    juce::File outputFile;
    juce::String outputFormat; // "WAV", "MP3", "FLAC", etc.
    int sampleRate;
    int bitDepth;
    bool processFx;
    bool normalize;
    float normalizeTargetDb;
    bool isCompleted;
    bool hasError;
    juce::String errorMessage;
};

// Batch processor class
class BatchProcessor
{
public:
    BatchProcessor();
    ~BatchProcessor();

    // Add a file to batch processing queue
    void addJob(const juce::File& inputFile, const juce::File& outputFile,
                const juce::String& format = "WAV", int sampleRate = 44100, int bitDepth = 24);

    // Add multiple files from directory
    int addDirectory(const juce::File& inputDir, const juce::File& outputDir,
                     const juce::String& extension = "*.wav", const juce::String& format = "WAV");

    // Configure global settings
    void setNormalize(bool enable, float targetDb = -1.0f);
    void setSampleRateConversion(int targetSampleRate);
    void setBitDepth(int targetBitDepth);

    // Process all queued jobs
    void processAllAsync(std::function<void(int current, int total)> progressCallback = nullptr,
                         std::function<void()> completionCallback = nullptr);

    // Process single job synchronously
    bool processJob(BatchJob& job);

    // Clear queue
    void clearQueue();

    // Get queue status
    int getTotalJobs() const { return (int)jobs.size(); }
    int getCompletedJobs() const;
    int getFailedJobs() const;
    const std::vector<BatchJob>& getJobs() const { return jobs; }

    // Check if currently processing
    bool isProcessing() const { return processing; }

    // Cancel current batch processing
    void cancel();

private:
    std::vector<BatchJob> jobs;
    bool globalNormalize;
    float globalNormalizeTargetDb;
    int globalTargetSampleRate;
    int globalTargetBitDepth;
    bool processing;
    bool cancelRequested;

    // Internal audio conversion helper
    bool convertAudioFile(const juce::File& input, const juce::File& output,
                          const juce::String& format, int targetSampleRate, int targetBitDepth,
                          bool normalize, float normalizeDb);
};
