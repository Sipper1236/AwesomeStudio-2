// BatchProcessor.cpp
// Batch processing system implementation for Awesome Studio DAW

#include "BatchProcessor.h"
#include <thread>

BatchProcessor::BatchProcessor()
    : globalNormalize(false), globalNormalizeTargetDb(-1.0f),
      globalTargetSampleRate(44100), globalTargetBitDepth(24),
      processing(false), cancelRequested(false)
{
}

BatchProcessor::~BatchProcessor()
{
    cancel();
}

void BatchProcessor::addJob(const juce::File& inputFile, const juce::File& outputFile,
                            const juce::String& format, int sampleRate, int bitDepth)
{
    BatchJob job;
    job.inputFile = inputFile;
    job.outputFile = outputFile;
    job.outputFormat = format;
    job.sampleRate = sampleRate > 0 ? sampleRate : globalTargetSampleRate;
    job.bitDepth = bitDepth > 0 ? bitDepth : globalTargetBitDepth;
    job.processFx = true;
    job.normalize = globalNormalize;
    job.normalizeTargetDb = globalNormalizeTargetDb;
    job.isCompleted = false;
    job.hasError = false;

    jobs.push_back(job);
}

int BatchProcessor::addDirectory(const juce::File& inputDir, const juce::File& outputDir,
                                 const juce::String& extension, const juce::String& format)
{
    if (!inputDir.isDirectory()) return 0;
    if (!outputDir.exists()) outputDir.createDirectory();

    juce::Array<juce::File> files;
    inputDir.findChildFiles(files, juce::File::findFiles, false, extension);

    int count = 0;
    for (const auto& file : files) {
        juce::String outExt = "." + format.toLowerCase();
        juce::File outFile = outputDir.getChildFile(file.getFileNameWithoutExtension() + outExt);
        addJob(file, outFile, format);
        count++;
    }

    return count;
}

void BatchProcessor::setNormalize(bool enable, float targetDb)
{
    globalNormalize = enable;
    globalNormalizeTargetDb = targetDb;
}

void BatchProcessor::setSampleRateConversion(int targetSampleRate)
{
    globalTargetSampleRate = targetSampleRate;
}

void BatchProcessor::setBitDepth(int targetBitDepth)
{
    globalTargetBitDepth = targetBitDepth;
}

void BatchProcessor::clearQueue()
{
    if (!processing) {
        jobs.clear();
    }
}

int BatchProcessor::getCompletedJobs() const
{
    int count = 0;
    for (const auto& job : jobs) {
        if (job.isCompleted && !job.hasError) count++;
    }
    return count;
}

int BatchProcessor::getFailedJobs() const
{
    int count = 0;
    for (const auto& job : jobs) {
        if (job.hasError) count++;
    }
    return count;
}

void BatchProcessor::cancel()
{
    if (processing) {
        cancelRequested = true;
    }
}

bool BatchProcessor::processJob(BatchJob& job)
{
    if (!job.inputFile.existsAsFile()) {
        job.hasError = true;
        job.errorMessage = "Input file does not exist: " + job.inputFile.getFullPathName();
        return false;
    }

    return convertAudioFile(job.inputFile, job.outputFile, job.outputFormat,
                            job.sampleRate, job.bitDepth,
                            job.normalize, job.normalizeTargetDb);
}

bool BatchProcessor::convertAudioFile(const juce::File& input, const juce::File& output,
                                       const juce::String& format, int targetSampleRate, int targetBitDepth,
                                       bool normalize, float normalizeDb)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(input));
    if (!reader) return false;

    juce::AudioBuffer<float> buffer((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read(&buffer, 0, (int)reader->lengthInSamples, 0, true, true);

    // Normalization if requested
    if (normalize) {
        float maxMagnitude = buffer.getMagnitude(0, buffer.getNumSamples());
        if (maxMagnitude > 0.0001f) {
            float targetGain = juce::Decibels::decibelsToGain(normalizeDb);
            float scale = targetGain / maxMagnitude;
            buffer.applyGain(scale);
        }
    }

    // Determine output format
    juce::AudioFormat* outFormat = nullptr;
    if (format.equalsIgnoreCase("WAV")) {
        outFormat = formatManager.findFormatForFileExtension("wav");
    } else if (format.equalsIgnoreCase("FLAC")) {
        outFormat = formatManager.findFormatForFileExtension("flac");
    } else if (format.equalsIgnoreCase("AIFF")) {
        outFormat = formatManager.findFormatForFileExtension("aiff");
    } else {
        outFormat = formatManager.findFormatForFileExtension("wav"); // default
    }

    if (!outFormat) return false;

    if (output.existsAsFile()) output.deleteFile();

    std::unique_ptr<juce::FileOutputStream> outStream(output.createOutputStream());
    if (!outStream) return false;

    std::unique_ptr<juce::AudioFormatWriter> writer(
        outFormat->createWriterFor(outStream.get(), (double)targetSampleRate,
                                   buffer.getNumChannels(), targetBitDepth, {}, 0));

    if (!writer) return false;

    outStream.release(); // Writer took ownership
    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());

    return true;
}

void BatchProcessor::processAllAsync(std::function<void(int current, int total)> progressCallback,
                                     std::function<void()> completionCallback)
{
    if (processing) return;

    processing = true;
    cancelRequested = false;

    std::thread([this, progressCallback, completionCallback]() {
        int total = (int)jobs.size();
        for (int i = 0; i < total; ++i) {
            if (cancelRequested) break;

            if (progressCallback) {
                juce::MessageManager::callAsync([progressCallback, i, total]() {
                    progressCallback(i, total);
                });
            }

            bool success = processJob(jobs[i]);
            jobs[i].isCompleted = true;
            jobs[i].hasError = !success;
        }

        processing = false;

        if (completionCallback) {
            juce::MessageManager::callAsync([completionCallback]() {
                completionCallback();
            });
        }
    }).detach();
}
