#include "WavExporter.h"

#include "SynthRenderer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <vector>

#if defined(AWESOME_STUDIO_HAS_FLAC)
#include <FLAC/stream_encoder.h>
#endif
#if defined(AWESOME_STUDIO_HAS_SNDFILE)
#include <sndfile.h>
#endif
#if defined(AWESOME_STUDIO_HAS_LAME)
#include <lame/lame.h>
#endif

namespace
{
constexpr quint16 channelCount = 2;

struct SampleFormatInfo
{
    quint16 formatTag = 1;
    quint16 bitsPerSample = 16;
    quint16 bytesPerSample = 2;
};

#if defined(AWESOME_STUDIO_HAS_FLAC)
struct FlacEncoderGuard
{
    ~FlacEncoderGuard()
    {
        if (encoder == nullptr)
            return;
        if (initialized)
            FLAC__stream_encoder_finish(encoder);
        FLAC__stream_encoder_delete(encoder);
    }

    bool finish()
    {
        if (encoder == nullptr || !initialized)
            return false;
        const bool success = FLAC__stream_encoder_finish(encoder);
        initialized = false;
        return success;
    }

    FLAC__StreamEncoder* encoder = nullptr;
    bool initialized = false;
};
#endif

#if defined(AWESOME_STUDIO_HAS_SNDFILE)
struct SndfileEncoderGuard
{
    ~SndfileEncoderGuard()
    {
        if (encoder != nullptr)
            sf_close(encoder);
    }

    bool finish()
    {
        if (encoder == nullptr)
            return false;
        const int status = sf_close(encoder);
        encoder = nullptr;
        return status == 0;
    }

    SNDFILE* encoder = nullptr;
};
#endif

#if defined(AWESOME_STUDIO_HAS_LAME)
struct LameEncoderGuard
{
    ~LameEncoderGuard()
    {
        if (encoder != nullptr)
            lame_close(encoder);
    }

    bool finish()
    {
        if (encoder == nullptr)
            return false;
        const int status = lame_close(encoder);
        encoder = nullptr;
        return status == 0;
    }

    lame_global_flags* encoder = nullptr;
};
#endif

bool sampleFormatInfo(WavSampleFormat format, SampleFormatInfo& info)
{
    switch (format)
    {
    case WavSampleFormat::Pcm16:
        info = {1, 16, 2};
        return true;
    case WavSampleFormat::Pcm24:
        info = {1, 24, 3};
        return true;
    case WavSampleFormat::Float32:
        info = {3, 32, 4};
        return true;
    }
    return false;
}

void setError(QString* error, const QString& message)
{
    if (error != nullptr)
        *error = message;
}

bool installEncodedFile(const QString& encodedPath, const QString& destinationPath, QString* error)
{
    QFile encodedFile(encodedPath);
    if (!encodedFile.open(QIODevice::ReadOnly))
    {
        setError(error, encodedFile.errorString());
        return false;
    }
    QSaveFile outputFile(destinationPath);
    if (!outputFile.open(QIODevice::WriteOnly))
    {
        setError(error, outputFile.errorString());
        return false;
    }
    while (!encodedFile.atEnd())
    {
        const QByteArray chunk = encodedFile.read(1024 * 1024);
        if (chunk.isEmpty())
        {
            if (encodedFile.error() != QFileDevice::NoError)
            {
                setError(error, encodedFile.errorString());
                return false;
            }
            break;
        }
        if (outputFile.write(chunk) != chunk.size())
        {
            setError(error, outputFile.errorString());
            return false;
        }
    }
    if (!outputFile.commit())
    {
        setError(error, outputFile.errorString());
        return false;
    }
    return true;
}

void write16(char* destination, quint16 value)
{
    destination[0] = static_cast<char>(value & 0xff);
    destination[1] = static_cast<char>((value >> 8) & 0xff);
}

void write32(char* destination, quint32 value)
{
    destination[0] = static_cast<char>(value & 0xff);
    destination[1] = static_cast<char>((value >> 8) & 0xff);
    destination[2] = static_cast<char>((value >> 16) & 0xff);
    destination[3] = static_cast<char>((value >> 24) & 0xff);
}

void writeIntegerSample(char* destination, qint64 value, int byteCount)
{
    const quint64 bits = static_cast<quint64>(value);
    for (int byte = 0; byte < byteCount; ++byte)
        destination[byte] = static_cast<char>((bits >> (byte * 8)) & 0xff);
}

void writeFloatSample(char* destination, float value)
{
    quint32 bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "WAV float samples require 32-bit floats");
    std::memcpy(&bits, &value, sizeof(bits));
    write32(destination, bits);
}

float outputSample(float sample, float masterGain, bool softClip)
{
    const float gained = sample * masterGain;
    if (!std::isfinite(gained))
        return 0.0f;
    return softClip ? std::clamp(std::tanh(gained), -1.0f, 1.0f) : gained;
}

QString safeStemName(const QString& source)
{
    QString result;
    for (const QChar character : source.trimmed())
    {
        if (character.isLetterOrNumber() || character == QLatin1Char('-') || character == QLatin1Char('_'))
        {
            result.append(character);
        }
        else if (!result.isEmpty() && !result.endsWith(QLatin1Char('_')))
        {
            result.append(QLatin1Char('_'));
        }
    }
    while (result.endsWith(QLatin1Char('_')))
        result.chop(1);
    return result.isEmpty() ? QStringLiteral("Track") : result;
}
}

bool WavExporter::exportProject(const ProjectData& project, const QString& path, QString* error,
                                double startBeat, double endBeat, const WavExportOptions& options,
                                AudioExportContainer container, const ProgressCallback& progress)
{
    ProjectData safeProject = project;
    safeProject.normalize();
    const bool customRange = endBeat >= 0.0;
    if (customRange && (!std::isfinite(startBeat) || !std::isfinite(endBeat) || startBeat < 0.0 ||
                        endBeat <= startBeat || endBeat > safeProject.songLengthBeats()))
    {
        setError(error, QStringLiteral("Choose a beat range inside the arrangement with an end after its start."));
        return false;
    }
    if (customRange)
        safeProject.countInBars = 0;

    switch (options.sampleRate)
    {
    case 44100:
    case 48000:
    case 88200:
    case 96000:
    case 192000:
        break;
    default:
        setError(error, QStringLiteral("Choose a supported audio sample rate."));
        return false;
    }
    const quint32 sampleRate = static_cast<quint32>(options.sampleRate);

    SampleFormatInfo formatInfo;
    if (!sampleFormatInfo(options.sampleFormat, formatInfo))
    {
        setError(error, QStringLiteral("Choose a supported audio sample format."));
        return false;
    }
    if (container != AudioExportContainer::Wav && container != AudioExportContainer::Flac &&
        container != AudioExportContainer::OggVorbis && container != AudioExportContainer::Mp3)
    {
        setError(error, QStringLiteral("Choose a supported audio container."));
        return false;
    }
    const bool wavContainer = container == AudioExportContainer::Wav;
    const bool flacContainer = container == AudioExportContainer::Flac;
    const bool oggContainer = container == AudioExportContainer::OggVorbis;
    const bool mp3Container = container == AudioExportContainer::Mp3;
    if (mp3Container && sampleRate != 44100 && sampleRate != 48000)
    {
        setError(error, QStringLiteral("MP3 export supports 44.1 kHz and 48 kHz input."));
        return false;
    }
    if (mp3Container && options.mp3BitrateKbps != 128 && options.mp3BitrateKbps != 192 &&
        options.mp3BitrateKbps != 256 && options.mp3BitrateKbps != 320)
    {
        setError(error, QStringLiteral("Choose a supported MP3 bitrate."));
        return false;
    }
    if (flacContainer && options.sampleFormat == WavSampleFormat::Float32)
    {
        setError(error, QStringLiteral("FLAC export supports 16-bit or 24-bit integer PCM."));
        return false;
    }
#if !defined(AWESOME_STUDIO_HAS_FLAC)
    if (flacContainer)
    {
        setError(error, QStringLiteral("FLAC export is unavailable because this build has no libFLAC support."));
        return false;
    }
#endif
#if !defined(AWESOME_STUDIO_HAS_SNDFILE)
    if (oggContainer)
    {
        setError(error, QStringLiteral("Ogg Vorbis export is unavailable because this build has no libsndfile support."));
        return false;
    }
#endif
#if !defined(AWESOME_STUDIO_HAS_LAME)
    if (mp3Container)
    {
        setError(error, QStringLiteral("MP3 export is unavailable because this build has no LAME support."));
        return false;
    }
#endif
    const bool integerPcm = formatInfo.formatTag == 1;
    if (options.dither && !integerPcm && !oggContainer && !mp3Container)
    {
        setError(error, QStringLiteral("Dither is available for integer PCM output only."));
        return false;
    }
    const quint32 bytesPerSampleFrame = channelCount * formatInfo.bytesPerSample;

    auto snapshot = std::make_shared<const ProjectData>(safeProject);
    SynthRenderer renderer;
    renderer.setProject(snapshot, sampleRate);

    qint64 countInFrames = 0;
    qint64 rangeStartFrame = 0;
    qint64 frameCount = 0;
    if (customRange)
    {
        rangeStartFrame = safeProject.frameAtBeat(startBeat, sampleRate);
        const qint64 rangeEndFrame = safeProject.frameAtBeat(endBeat, sampleRate);
        frameCount = rangeEndFrame - rangeStartFrame;
        if (frameCount <= 0)
        {
            setError(error, QStringLiteral("The selected beat range is shorter than one audio frame."));
            return false;
        }
    }
    else
    {
        const qint64 beatUnitFrames = static_cast<qint64>(std::llround(
            sampleRate * 60.0 / safeProject.tempoAtBeat(0.0) * 4.0 / safeProject.beatUnit));
        countInFrames = static_cast<qint64>(safeProject.countInBars) *
                        safeProject.beatsPerBar * beatUnitFrames;
        const double durationSeconds = safeProject.secondsAtBeat(safeProject.songLengthBeats()) +
                                       renderer.effectTailSeconds() + 0.1;
        frameCount = countInFrames + static_cast<qint64>(std::ceil(durationSeconds * sampleRate));
    }
    const quint64 dataSize = static_cast<quint64>(frameCount) * bytesPerSampleFrame;
    if (wavContainer && dataSize > std::numeric_limits<quint32>::max() - 36)
    {
        setError(error, QStringLiteral("This pattern is too long to store in a standard WAV file."));
        return false;
    }

    QSaveFile wavFile(path);
    QTemporaryFile flacTemporaryFile(
        QDir(QFileInfo(path).absolutePath()).filePath(QStringLiteral(".awesome-studio-flac-XXXXXX")));
    QTemporaryFile oggTemporaryFile(
        QDir(QFileInfo(path).absolutePath()).filePath(QStringLiteral(".awesome-studio-ogg-XXXXXX")));
    QTemporaryFile mp3TemporaryFile(
        QDir(QFileInfo(path).absolutePath()).filePath(QStringLiteral(".awesome-studio-mp3-XXXXXX")));
#if defined(AWESOME_STUDIO_HAS_FLAC)
    FlacEncoderGuard flacEncoder;
#endif
#if defined(AWESOME_STUDIO_HAS_SNDFILE)
    SndfileEncoderGuard oggEncoder;
#endif
#if defined(AWESOME_STUDIO_HAS_LAME)
    LameEncoderGuard mp3Encoder;
#endif
    if (wavContainer)
    {
        if (!wavFile.open(QIODevice::WriteOnly))
        {
            setError(error, wavFile.errorString());
            return false;
        }

        char header[44]{};
        std::memcpy(header, "RIFF", 4);
        write32(header + 4, static_cast<quint32>(dataSize + 36));
        std::memcpy(header + 8, "WAVEfmt ", 8);
        write32(header + 16, 16);
        write16(header + 20, formatInfo.formatTag);
        write16(header + 22, channelCount);
        write32(header + 24, sampleRate);
        write32(header + 28, sampleRate * bytesPerSampleFrame);
        write16(header + 32, bytesPerSampleFrame);
        write16(header + 34, formatInfo.bitsPerSample);
        std::memcpy(header + 36, "data", 4);
        write32(header + 40, static_cast<quint32>(dataSize));
        if (wavFile.write(header, sizeof(header)) != sizeof(header))
        {
            setError(error, wavFile.errorString());
            return false;
        }
    }
#if defined(AWESOME_STUDIO_HAS_FLAC)
    else if (flacContainer)
    {
        if (!flacTemporaryFile.open())
        {
            setError(error, flacTemporaryFile.errorString());
            return false;
        }
        const QString temporaryPath = flacTemporaryFile.fileName();
        flacTemporaryFile.close();

        flacEncoder.encoder = FLAC__stream_encoder_new();
        if (flacEncoder.encoder == nullptr ||
            !FLAC__stream_encoder_set_channels(flacEncoder.encoder, channelCount) ||
            !FLAC__stream_encoder_set_bits_per_sample(flacEncoder.encoder, formatInfo.bitsPerSample) ||
            !FLAC__stream_encoder_set_sample_rate(flacEncoder.encoder, sampleRate) ||
            !FLAC__stream_encoder_set_compression_level(flacEncoder.encoder, 5))
        {
            setError(error, QStringLiteral("Could not configure the FLAC encoder."));
            return false;
        }
        const QByteArray encodedPath = QFile::encodeName(temporaryPath);
        const FLAC__StreamEncoderInitStatus initStatus = FLAC__stream_encoder_init_file(
            flacEncoder.encoder, encodedPath.constData(), nullptr, nullptr);
        if (initStatus != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
        {
            setError(error, QStringLiteral("Could not start FLAC encoding (status %1).")
                                 .arg(static_cast<int>(initStatus)));
            return false;
        }
        flacEncoder.initialized = true;
    }
#endif
#if defined(AWESOME_STUDIO_HAS_SNDFILE)
    if (oggContainer)
    {
        if (!oggTemporaryFile.open())
        {
            setError(error, oggTemporaryFile.errorString());
            return false;
        }
        const QString temporaryPath = oggTemporaryFile.fileName();
        oggTemporaryFile.close();
        SF_INFO info{};
        info.samplerate = static_cast<int>(sampleRate);
        info.channels = channelCount;
        info.format = SF_FORMAT_OGG | SF_FORMAT_VORBIS;
        if (!sf_format_check(&info))
        {
            setError(error, QStringLiteral("This libsndfile build does not support Ogg Vorbis encoding."));
            return false;
        }
        const QByteArray encodedPath = QFile::encodeName(temporaryPath);
        oggEncoder.encoder = sf_open(encodedPath.constData(), SFM_WRITE, &info);
        if (oggEncoder.encoder == nullptr)
        {
            setError(error, QString::fromLocal8Bit(sf_strerror(nullptr)));
            return false;
        }
        float quality = std::clamp(options.vorbisQuality, 0.0f, 1.0f);
        if (sf_command(oggEncoder.encoder, SFC_SET_VBR_ENCODING_QUALITY,
                       &quality, sizeof(quality)) != SF_TRUE)
        {
            setError(error, QStringLiteral("Could not set Ogg Vorbis encoding quality."));
            return false;
        }
    }
#endif
#if defined(AWESOME_STUDIO_HAS_LAME)
    if (mp3Container)
    {
        if (!mp3TemporaryFile.open())
        {
            setError(error, mp3TemporaryFile.errorString());
            return false;
        }
        mp3Encoder.encoder = lame_init();
        if (mp3Encoder.encoder == nullptr ||
            lame_set_num_channels(mp3Encoder.encoder, channelCount) < 0 ||
            lame_set_in_samplerate(mp3Encoder.encoder, static_cast<int>(sampleRate)) < 0 ||
            lame_set_out_samplerate(mp3Encoder.encoder, static_cast<int>(sampleRate)) < 0 ||
            lame_set_brate(mp3Encoder.encoder, options.mp3BitrateKbps) < 0 ||
            lame_set_quality(mp3Encoder.encoder, 2) < 0 ||
            lame_set_VBR(mp3Encoder.encoder, vbr_off) < 0 ||
            lame_init_params(mp3Encoder.encoder) < 0)
        {
            setError(error, QStringLiteral("Could not configure the MP3 encoder."));
            return false;
        }
    }
#endif

    constexpr int framesPerChunk = 2048;
    QByteArray audio(framesPerChunk * bytesPerSampleFrame, Qt::Uninitialized);
    std::vector<float> oggAudio(static_cast<size_t>(framesPerChunk) * channelCount);
    std::vector<unsigned char> mp3Buffer(static_cast<size_t>(framesPerChunk) * 5 / 4 + 7200);
#if defined(AWESOME_STUDIO_HAS_FLAC)
    std::vector<FLAC__int32> flacAudio(static_cast<size_t>(framesPerChunk) * channelCount);
#endif
    std::mt19937 ditherGenerator(0x41575331u);
    std::uniform_real_distribution<double> unitNoise(0.0, 1.0);
    qint64 writtenFrames = 0;
    while (writtenFrames < frameCount)
    {
        const int frames = static_cast<int>(std::min<qint64>(framesPerChunk, frameCount - writtenFrames));
        for (int index = 0; index < frames; ++index)
        {
            renderer.beginMixerFrame();
            const qint64 projectFrame = customRange
                ? rangeStartFrame + writtenFrames + index
                : writtenFrames + index - countInFrames;
            StereoSample rendered = renderer.renderSample(projectFrame);
            const StereoSample mixerOutput = renderer.finishMixerFrame();
            rendered.left += mixerOutput.left;
            rendered.right += mixerOutput.right;
            rendered = renderer.processMasterEffects(rendered);
            char* frame = wavContainer ? audio.data() + index * bytesPerSampleFrame : nullptr;
            const float output[2] = {
                outputSample(rendered.left, safeProject.masterGain,
                             oggContainer || mp3Container ||
                                 options.sampleFormat != WavSampleFormat::Float32),
                outputSample(rendered.right, safeProject.masterGain,
                             oggContainer || mp3Container ||
                                 options.sampleFormat != WavSampleFormat::Float32)
            };
            for (int channel = 0; channel < 2; ++channel)
            {
                char* destination = !wavContainer
                    ? nullptr : frame + channel * formatInfo.bytesPerSample;
                if (oggContainer || mp3Container)
                {
                    oggAudio[static_cast<size_t>(index) * channelCount + channel] = output[channel];
                    continue;
                }
                if (options.sampleFormat == WavSampleFormat::Float32)
                {
                    writeFloatSample(destination, output[channel]);
                    continue;
                }

                const int shift = formatInfo.bitsPerSample - 1;
                const qint64 maximum = (qint64{1} << shift) - 1;
                const qint64 minimum = -(qint64{1} << shift);
                double scaled = static_cast<double>(output[channel]) * maximum;
                if (options.dither)
                {
                    const double firstNoise = unitNoise(ditherGenerator);
                    const double secondNoise = unitNoise(ditherGenerator);
                    scaled += firstNoise - secondNoise;
                }
                const qint64 quantized = static_cast<qint64>(std::lrint(scaled));
                const qint64 bounded = std::clamp(quantized, minimum, maximum);
#if defined(AWESOME_STUDIO_HAS_FLAC)
                if (flacContainer)
                    flacAudio[static_cast<size_t>(index) * channelCount + channel] =
                        static_cast<FLAC__int32>(bounded);
                else
#endif
                    writeIntegerSample(destination, bounded, formatInfo.bytesPerSample);
            }
        }
        renderer.serviceLv2LatencyChanges();

        const qint64 bytes = static_cast<qint64>(frames) * bytesPerSampleFrame;
        if (flacContainer)
        {
#if defined(AWESOME_STUDIO_HAS_FLAC)
            if (!FLAC__stream_encoder_process_interleaved(flacEncoder.encoder,
                    flacAudio.data(), static_cast<unsigned>(frames)))
            {
                setError(error, QStringLiteral("FLAC encoding failed while rendering audio."));
                return false;
            }
#endif
        }
        else if (oggContainer)
        {
#if defined(AWESOME_STUDIO_HAS_SNDFILE)
            if (sf_writef_float(oggEncoder.encoder, oggAudio.data(), frames) != frames)
            {
                setError(error, QString::fromLocal8Bit(sf_strerror(oggEncoder.encoder)));
                return false;
            }
#endif
        }
        else if (mp3Container)
        {
#if defined(AWESOME_STUDIO_HAS_LAME)
            const int encodedBytes = lame_encode_buffer_interleaved_ieee_float(
                mp3Encoder.encoder, oggAudio.data(), frames, mp3Buffer.data(),
                static_cast<int>(mp3Buffer.size()));
            if (encodedBytes < 0)
            {
                setError(error, QStringLiteral("MP3 encoding failed while rendering audio."));
                return false;
            }
            if (mp3TemporaryFile.write(reinterpret_cast<const char*>(mp3Buffer.data()),
                                       encodedBytes) != encodedBytes)
            {
                setError(error, mp3TemporaryFile.errorString());
                return false;
            }
#endif
        }
        else if (wavFile.write(audio.constData(), bytes) != bytes)
        {
            setError(error, wavFile.errorString());
            return false;
        }
        writtenFrames += frames;
        if (progress && !progress(writtenFrames, frameCount))
        {
            setError(error, QStringLiteral("Audio export was canceled."));
            return false;
        }
    }

    if (flacContainer)
    {
#if defined(AWESOME_STUDIO_HAS_FLAC)
        if (!flacEncoder.finish())
        {
            setError(error, QStringLiteral("Could not finalize the FLAC file."));
            return false;
        }
        return installEncodedFile(flacTemporaryFile.fileName(), path, error);
#else
        return false;
#endif
    }
    if (oggContainer)
    {
#if defined(AWESOME_STUDIO_HAS_SNDFILE)
        if (!oggEncoder.finish())
        {
            setError(error, QStringLiteral("Could not finalize the Ogg Vorbis file."));
            return false;
        }
        return installEncodedFile(oggTemporaryFile.fileName(), path, error);
#else
        return false;
#endif
    }
    if (mp3Container)
    {
#if defined(AWESOME_STUDIO_HAS_LAME)
        const int encodedBytes = lame_encode_flush(mp3Encoder.encoder, mp3Buffer.data(),
                                                    static_cast<int>(mp3Buffer.size()));
        if (encodedBytes < 0 ||
            mp3TemporaryFile.write(reinterpret_cast<const char*>(mp3Buffer.data()),
                                   encodedBytes) != encodedBytes ||
            !mp3TemporaryFile.flush())
        {
            setError(error, encodedBytes < 0 ? QStringLiteral("Could not finalize the MP3 stream.")
                                             : mp3TemporaryFile.errorString());
            return false;
        }
        mp3TemporaryFile.close();
        if (!mp3Encoder.finish())
        {
            setError(error, QStringLiteral("Could not close the MP3 encoder."));
            return false;
        }
        return installEncodedFile(mp3TemporaryFile.fileName(), path, error);
#else
        return false;
#endif
    }
    if (!wavFile.commit())
    {
        setError(error, wavFile.errorString());
        return false;
    }
    return true;
}

bool WavExporter::exportStems(const ProjectData& project, const QString& directoryPath, QString* error,
                              double startBeat, double endBeat, const WavExportOptions& options)
{
    ProjectData source = project;
    source.normalize();
    if (source.tracks.isEmpty())
    {
        setError(error, QStringLiteral("The project has no tracks to export."));
        return false;
    }

    const QDir directory(directoryPath);
    if (!directory.exists())
    {
        setError(error, QStringLiteral("The selected stem output folder does not exist."));
        return false;
    }

    QVector<QString> paths;
    paths.reserve(source.tracks.size());
    for (int index = 0; index < source.tracks.size(); ++index)
    {
        const InstrumentTrack& track = source.tracks[index];
        const QString fileName = QStringLiteral("%1_%2_%3.wav")
            .arg(index + 1, 3, 10, QLatin1Char('0'))
            .arg(track.id)
            .arg(safeStemName(track.name));
        const QString path = directory.filePath(fileName);
        if (QFileInfo::exists(path))
        {
            setError(error, QStringLiteral("Stem file already exists: %1. Choose an empty folder.").arg(fileName));
            return false;
        }
        paths.push_back(path);
    }

    QVector<QString> createdPaths;
    createdPaths.reserve(paths.size());
    for (int trackIndex = 0; trackIndex < source.tracks.size(); ++trackIndex)
    {
        ProjectData stemProject = source;
        for (int index = 0; index < stemProject.tracks.size(); ++index)
        {
            stemProject.tracks[index].muted = index != trackIndex;
            stemProject.tracks[index].solo = false;
        }
        for (MixerBus& bus : stemProject.buses)
        {
            bus.muted = false;
            bus.solo = false;
        }

        QString renderError;
        if (!exportProject(stemProject, paths[trackIndex], &renderError, startBeat, endBeat, options))
        {
            for (const QString& createdPath : createdPaths)
                QFile::remove(createdPath);
            setError(error, QStringLiteral("Could not export %1: %2")
                                  .arg(QFileInfo(paths[trackIndex]).fileName(), renderError));
            return false;
        }
        createdPaths.push_back(paths[trackIndex]);
    }
    return true;
}
