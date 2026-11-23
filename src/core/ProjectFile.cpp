#include "ProjectFile.h"

#include "AudioFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr int currentProjectVersion = 49;
constexpr auto projectFormat = "awesome-studio-project";

struct MediaLoadContext
{
    QHash<QString, std::shared_ptr<const AudioAsset>> assets;
    QSet<QString> unavailableAssets;
    quint64 decodedBytes = 0;
};

void setError(QString* error, const QString& message)
{
    if (error != nullptr)
        *error = message;
}

void loadAudioReference(QString& path, const QString& mediaBaseDir, MediaLoadContext& media,
                        std::shared_ptr<const AudioAsset>& asset)
{
    if (path.isEmpty())
        return;
    if (QDir::isRelativePath(path))
        path = QDir(mediaBaseDir).absoluteFilePath(path);

    const QFileInfo fileInfo(path);
    if (!fileInfo.exists() || !fileInfo.isFile())
        return;
    QString cacheKey = fileInfo.canonicalFilePath();
    if (cacheKey.isEmpty())
        cacheKey = fileInfo.absoluteFilePath();
    const auto cached = media.assets.constFind(cacheKey);
    if (cached != media.assets.cend())
    {
        asset = *cached;
        return;
    }
    if (media.unavailableAssets.contains(cacheKey))
        return;

    std::shared_ptr<const AudioAsset> decoded;
    QString audioError;
    if (!AudioFile::loadWav(path, decoded, &audioError))
    {
        media.unavailableAssets.insert(cacheKey);
        return;
    }
    const quint64 bytes = static_cast<quint64>(decoded->samples.size() + decoded->waveformPeaks.size()) *
                          sizeof(float);
    if (bytes > AudioFile::maximumProjectDecodedBytes - media.decodedBytes)
    {
        media.unavailableAssets.insert(cacheKey);
        return;
    }
    media.decodedBytes += bytes;
    media.assets.insert(cacheKey, decoded);
    asset = std::move(decoded);
}

QJsonArray saveNotes(const QVector<MidiNote>& notes)
{
    QJsonArray result;
    for (const MidiNote& note : notes)
    {
        result.push_back(QJsonObject{
            {QStringLiteral("id"), static_cast<qint64>(note.id)},
            {QStringLiteral("pitch"), note.pitch},
            {QStringLiteral("velocity"), note.velocity},
            {QStringLiteral("probabilityPercent"), note.probabilityPercent},
            {QStringLiteral("ratchetCount"), note.ratchetCount},
            {QStringLiteral("startBeat"), note.startBeat},
            {QStringLiteral("lengthBeats"), note.lengthBeats},
            {QStringLiteral("channel"), note.channel}
        });
    }
    return result;
}

QJsonArray savePitchBends(const QVector<MidiPitchBend>& pitchBends)
{
    QJsonArray result;
    for (const MidiPitchBend& bend : pitchBends)
    {
        result.push_back(QJsonObject{
            {QStringLiteral("channel"), bend.channel},
            {QStringLiteral("value"), bend.value},
            {QStringLiteral("beat"), bend.beat}
        });
    }
    return result;
}

QJsonArray saveAutomation(const QVector<AutomationPoint>& points)
{
    QJsonArray result;
    for (const AutomationPoint& point : points)
    {
        result.push_back(QJsonObject{
            {QStringLiteral("beat"), point.beat},
            {QStringLiteral("value"), point.value},
            {QStringLiteral("curve"), static_cast<int>(point.curveToNext)}
        });
    }
    return result;
}

QJsonArray saveTempoChanges(const QVector<TempoChange>& changes)
{
    QJsonArray result;
    for (const TempoChange& change : changes)
    {
        result.push_back(QJsonObject{
            {QStringLiteral("beat"), change.beat},
            {QStringLiteral("bpm"), change.bpm},
            {QStringLiteral("rampFromPrevious"), change.rampFromPrevious}
        });
    }
    return result;
}

QJsonArray saveEffects(const QVector<MixerEffect>& effects)
{
    QJsonArray result;
    for (const MixerEffect& effect : effects)
    {
        QJsonArray pluginParameters;
        for (float value : effect.pluginParameterValues)
            pluginParameters.push_back(value);
        QJsonArray pluginParameterAutomation;
        for (const QVector<AutomationPoint>& points : effect.pluginParameterAutomation)
            pluginParameterAutomation.push_back(saveAutomation(points));
        result.push_back(QJsonObject{
            {QStringLiteral("type"), static_cast<int>(effect.type)},
            {QStringLiteral("enabled"), effect.enabled},
            {QStringLiteral("wet"), effect.wet},
            {QStringLiteral("delayMs"), effect.delayMs},
            {QStringLiteral("feedback"), effect.feedback},
            {QStringLiteral("roomSize"), effect.roomSize},
            {QStringLiteral("damping"), effect.damping},
            {QStringLiteral("thresholdDb"), effect.thresholdDb},
            {QStringLiteral("ratio"), effect.ratio},
            {QStringLiteral("attackMs"), effect.attackMs},
            {QStringLiteral("releaseMs"), effect.releaseMs},
            {QStringLiteral("makeupDb"), effect.makeupDb},
            {QStringLiteral("cutoffHz"), effect.cutoffHz},
            {QStringLiteral("resonance"), effect.resonance},
            {QStringLiteral("pluginUri"), effect.pluginUri},
            {QStringLiteral("pluginName"), effect.pluginName},
            {QStringLiteral("pluginParameterValues"), pluginParameters},
            {QStringLiteral("pluginParameterAutomation"), pluginParameterAutomation},
            {QStringLiteral("pluginState"), effect.pluginState}
        });
    }
    return result;
}

bool loadAutomation(const QJsonValue& value, QVector<AutomationPoint>& points,
                    const QString& parameterName, int version, QString* error);

bool loadEffects(const QJsonValue& value, QVector<MixerEffect>& effects,
                 const QString& ownerName, int version, QString* error)
{
    if (!value.isArray() || value.toArray().size() > 8)
    {
        setError(error, QStringLiteral("The %1 has invalid or excessive effect data.").arg(ownerName));
        return false;
    }
    for (const QJsonValue& effectValue : value.toArray())
    {
        if (!effectValue.isObject())
        {
            setError(error, QStringLiteral("The %1 contains an invalid effect.").arg(ownerName));
            return false;
        }
        const QJsonObject object = effectValue.toObject();
        const int type = object.value(QStringLiteral("type")).toInt(-1);
        if (type < 0 || type > (version >= 39 ? 5 : version >= 34 ? 4 : 3))
        {
            setError(error, QStringLiteral("The %1 contains an unsupported effect type.").arg(ownerName));
            return false;
        }
        MixerEffect effect = defaultMixerEffect(static_cast<MixerEffectType>(type));
        effect.enabled = object.value(QStringLiteral("enabled")).toBool(true);
        const double wet = object.value(QStringLiteral("wet")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        const double delayMs = object.value(QStringLiteral("delayMs")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        const double feedback = object.value(QStringLiteral("feedback")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        const double roomSize = object.value(QStringLiteral("roomSize")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        const double damping = object.value(QStringLiteral("damping")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        const double thresholdDb = object.value(QStringLiteral("thresholdDb")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        const double ratio = object.value(QStringLiteral("ratio")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        const double attackMs = object.value(QStringLiteral("attackMs")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        const double releaseMs = object.value(QStringLiteral("releaseMs")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        const double makeupDb = object.value(QStringLiteral("makeupDb")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        const double cutoffHz = object.value(QStringLiteral("cutoffHz")).toDouble(20000.0);
        const double resonance = object.value(QStringLiteral("resonance")).toDouble(0.707);
        if (!std::isfinite(wet) || !std::isfinite(delayMs) || !std::isfinite(feedback) ||
            !std::isfinite(roomSize) || !std::isfinite(damping) || !std::isfinite(thresholdDb) ||
            !std::isfinite(ratio) || !std::isfinite(attackMs) || !std::isfinite(releaseMs) ||
            !std::isfinite(makeupDb) || !std::isfinite(cutoffHz) || !std::isfinite(resonance))
        {
            setError(error, QStringLiteral("The %1 contains invalid effect settings.").arg(ownerName));
            return false;
        }
        effect.wet = static_cast<float>(wet);
        effect.delayMs = static_cast<float>(delayMs);
        effect.feedback = static_cast<float>(feedback);
        effect.roomSize = static_cast<float>(roomSize);
        effect.damping = static_cast<float>(damping);
        effect.thresholdDb = static_cast<float>(thresholdDb);
        effect.ratio = static_cast<float>(ratio);
        effect.attackMs = static_cast<float>(attackMs);
        effect.releaseMs = static_cast<float>(releaseMs);
        effect.makeupDb = static_cast<float>(makeupDb);
        effect.cutoffHz = static_cast<float>(cutoffHz);
        effect.resonance = static_cast<float>(resonance);
        if (version >= 34)
        {
            effect.pluginUri = object.value(QStringLiteral("pluginUri")).toString();
            effect.pluginName = object.value(QStringLiteral("pluginName")).toString();
            const QJsonValue parameterValues = object.value(QStringLiteral("pluginParameterValues"));
            if (!parameterValues.isArray() || parameterValues.toArray().size() > 256)
            {
                setError(error, QStringLiteral("The %1 contains invalid plugin parameter data.")
                                    .arg(ownerName));
                return false;
            }
            for (const QJsonValue& parameterValue : parameterValues.toArray())
            {
                const double number = parameterValue.toDouble(
                    std::numeric_limits<double>::quiet_NaN());
                if (!std::isfinite(number) || std::abs(number) > 1.0e9)
                {
                    setError(error, QStringLiteral("The %1 contains invalid plugin parameter data.")
                                        .arg(ownerName));
                    return false;
                }
                effect.pluginParameterValues.push_back(static_cast<float>(number));
            }
            if (isPluginEffect(effect.type) && effect.pluginUri.isEmpty())
            {
                setError(error, QStringLiteral("The %1 contains a plugin effect without a URI.")
                                    .arg(ownerName));
                return false;
            }
        }
        if (version >= 35)
        {
            const QJsonValue automationValue =
                object.value(QStringLiteral("pluginParameterAutomation"));
            if (!automationValue.isArray() || automationValue.toArray().size() > 256)
            {
                setError(error, QStringLiteral("The %1 contains invalid plugin automation data.")
                                    .arg(ownerName));
                return false;
            }
            for (const QJsonValue& curveValue : automationValue.toArray())
            {
                QVector<AutomationPoint> points;
                if (!loadAutomation(curveValue, points, QStringLiteral("plugin parameter"),
                                    version, error))
                    return false;
                effect.pluginParameterAutomation.push_back(std::move(points));
            }
        }
        if (version >= 38)
        {
            const QJsonValue stateValue = object.value(QStringLiteral("pluginState"));
            if (!stateValue.isUndefined() && !stateValue.isString())
            {
                setError(error, QStringLiteral("The %1 contains invalid plugin state data.")
                                    .arg(ownerName));
                return false;
            }
            effect.pluginState = stateValue.toString();
            if (effect.pluginState.toUtf8().size() > maximumPluginStateBytes)
            {
                setError(error, QStringLiteral("The %1 contains plugin state larger than 1 MiB.")
                                    .arg(ownerName));
                return false;
            }
        }
        effects.push_back(effect);
    }
    return true;
}

bool loadAutomation(const QJsonValue& value, QVector<AutomationPoint>& points,
                    const QString& parameterName, int version, QString* error)
{
    if (value.isUndefined())
        return true;
    if (!value.isArray() || value.toArray().size() > 10000)
    {
        setError(error, QStringLiteral("The track has invalid or excessive %1 automation data.")
                           .arg(parameterName));
        return false;
    }
    for (const QJsonValue& item : value.toArray())
    {
        if (!item.isObject())
        {
            setError(error, QStringLiteral("The track contains an invalid %1 automation point.")
                               .arg(parameterName));
            return false;
        }
        const QJsonObject object = item.toObject();
        AutomationPoint point;
        point.beat = object.value(QStringLiteral("beat")).toDouble(-1.0);
        point.value = static_cast<float>(object.value(QStringLiteral("value")).toDouble(
            std::numeric_limits<double>::quiet_NaN()));
        if (version >= 36)
        {
            const int curve = object.value(QStringLiteral("curve")).toInt(0);
            if (curve < static_cast<int>(AutomationPoint::Curve::Linear) ||
                curve > static_cast<int>(AutomationPoint::Curve::Hold))
            {
                setError(error, QStringLiteral("The track contains an unsupported %1 automation curve.")
                                   .arg(parameterName));
                return false;
            }
            point.curveToNext = static_cast<AutomationPoint::Curve>(curve);
        }
        if (!std::isfinite(point.beat) || !std::isfinite(point.value) || point.beat < 0.0)
        {
            setError(error, QStringLiteral("The track contains an out-of-range %1 automation point.")
                               .arg(parameterName));
            return false;
        }
        points.push_back(point);
    }
    return true;
}

bool loadNotes(const QJsonValue& value, QVector<MidiNote>& notes, int version, QString* error)
{
    if (!value.isArray())
    {
        setError(error, QStringLiteral("The project is missing MIDI note data."));
        return false;
    }
    const QJsonArray array = value.toArray();
    if (array.size() > 100000)
    {
        setError(error, QStringLiteral("This project contains too many MIDI notes to open safely."));
        return false;
    }

    for (const QJsonValue& item : array)
    {
        if (!item.isObject())
        {
            setError(error, QStringLiteral("The project contains an invalid MIDI note."));
            return false;
        }

        const QJsonObject object = item.toObject();
        const qint64 storedId = object.value(QStringLiteral("id")).toInteger();
        MidiNote note;
        if (storedId < 0)
        {
            setError(error, QStringLiteral("The project contains an invalid MIDI note ID."));
            return false;
        }
        note.id = static_cast<quint64>(storedId);
        note.pitch = object.value(QStringLiteral("pitch")).toInt(-1);
        note.velocity = object.value(QStringLiteral("velocity")).toInt(-1);
        if (version >= 28)
            note.probabilityPercent = object.value(QStringLiteral("probabilityPercent")).toInt(-1);
        if (version >= 29)
            note.ratchetCount = object.value(QStringLiteral("ratchetCount")).toInt(-1);
        note.startBeat = object.value(QStringLiteral("startBeat")).toDouble(-1.0);
        note.lengthBeats = object.value(QStringLiteral("lengthBeats")).toDouble(-1.0);
        note.channel = object.value(QStringLiteral("channel")).toInt(-1);
        if (note.pitch < 0 || note.pitch > 127 || note.velocity < 1 || note.velocity > 127 ||
            note.probabilityPercent < 0 || note.probabilityPercent > 100 ||
            note.ratchetCount < 1 || note.ratchetCount > 8 ||
            note.startBeat < 0.0 || note.lengthBeats <= 0.0 || note.channel < -1 || note.channel > 15)
        {
            setError(error, QStringLiteral("The project contains an out-of-range MIDI note."));
            return false;
        }
        notes.push_back(note);
    }
    return true;
}

bool loadPitchBends(const QJsonValue& value, QVector<MidiPitchBend>& pitchBends, QString* error)
{
    if (!value.isArray())
    {
        setError(error, QStringLiteral("The project is missing MIDI pitch-bend data."));
        return false;
    }
    const QJsonArray array = value.toArray();
    if (array.size() > 100000)
    {
        setError(error, QStringLiteral("This project contains too many MIDI pitch-bend events to open safely."));
        return false;
    }

    for (const QJsonValue& item : array)
    {
        if (!item.isObject())
        {
            setError(error, QStringLiteral("The project contains an invalid MIDI pitch-bend event."));
            return false;
        }
        const QJsonObject object = item.toObject();
        MidiPitchBend bend;
        bend.channel = object.value(QStringLiteral("channel")).toInt(-1);
        bend.value = object.value(QStringLiteral("value")).toInt(-1);
        bend.beat = object.value(QStringLiteral("beat")).toDouble(-1.0);
        if (bend.channel < 0 || bend.channel > 15 || bend.value < 0 || bend.value > 16383 ||
            !std::isfinite(bend.beat) || bend.beat < 0.0 || bend.beat > 16384.0)
        {
            setError(error, QStringLiteral("The project contains an out-of-range MIDI pitch-bend event."));
            return false;
        }
        pitchBends.push_back(bend);
    }
    return true;
}

QJsonObject saveClip(const MidiClip& clip)
{
    return QJsonObject{
        {QStringLiteral("id"), static_cast<qint64>(clip.id)},
        {QStringLiteral("name"), clip.name},
        {QStringLiteral("startBeat"), clip.startBeat},
        {QStringLiteral("lengthBeats"), clip.lengthBeats},
        {QStringLiteral("stepsPerBar"), clip.stepsPerBar},
        {QStringLiteral("notes"), saveNotes(clip.notes)},
        {QStringLiteral("pitchBends"), savePitchBends(clip.pitchBends)},
        {QStringLiteral("audioSourcePath"), clip.audioSourcePath},
        {QStringLiteral("audioStartFrame"), static_cast<qint64>(clip.audioStartFrame)},
        {QStringLiteral("audioFadeInBeats"), clip.audioFadeInBeats},
        {QStringLiteral("audioFadeOutBeats"), clip.audioFadeOutBeats},
        {QStringLiteral("audioFadeInCurve"), static_cast<int>(clip.audioFadeInCurve)},
        {QStringLiteral("audioFadeOutCurve"), static_cast<int>(clip.audioFadeOutCurve)},
        {QStringLiteral("audioFadeInShape"), clip.audioFadeInShape},
        {QStringLiteral("audioFadeOutShape"), clip.audioFadeOutShape}
    };
}

QJsonObject saveTrack(const InstrumentTrack& track)
{
    QJsonArray clips;
    for (const MidiClip& clip : track.clips)
        clips.push_back(saveClip(clip));

    return QJsonObject{
        {QStringLiteral("id"), static_cast<qint64>(track.id)},
        {QStringLiteral("name"), track.name},
        {QStringLiteral("instrumentType"), static_cast<int>(track.instrumentType)},
        {QStringLiteral("clapPluginId"), track.clapPluginId},
        {QStringLiteral("clapPluginName"), track.clapPluginName},
        {QStringLiteral("clapPluginState"), track.clapPluginState},
        {QStringLiteral("clapParameterValues"), [&track]
         {
             QJsonArray values;
             for (float value : track.clapParameterValues)
                 values.push_back(value);
             return values;
         }()},
        {QStringLiteral("clapParameterAutomation"), [&track]
         {
             QJsonArray automation;
             for (const QVector<AutomationPoint>& points : track.clapParameterAutomation)
                 automation.push_back(saveAutomation(points));
             return automation;
         }()},
        {QStringLiteral("waveform"), static_cast<int>(track.waveform)},
        {QStringLiteral("secondOscillatorEnabled"), track.secondOscillatorEnabled},
        {QStringLiteral("secondOscillatorWaveform"), static_cast<int>(track.secondOscillatorWaveform)},
        {QStringLiteral("secondOscillatorDetuneCents"), track.secondOscillatorDetuneCents},
        {QStringLiteral("secondOscillatorMix"), track.secondOscillatorMix},
        {QStringLiteral("synthFilterEnabled"), track.synthFilterEnabled},
        {QStringLiteral("synthFilterType"), static_cast<int>(track.synthFilterType)},
        {QStringLiteral("synthFilterCutoffHz"), track.synthFilterCutoffHz},
        {QStringLiteral("synthFilterResonance"), track.synthFilterResonance},
        {QStringLiteral("synthFilterEnvelopeEnabled"), track.synthFilterEnvelopeEnabled},
        {QStringLiteral("synthFilterEnvelopeAmountSemitones"),
         track.synthFilterEnvelopeAmountSemitones},
        {QStringLiteral("synthFilterEnvelopeAttackSeconds"),
         track.synthFilterEnvelopeAttackSeconds},
        {QStringLiteral("synthFilterEnvelopeDecaySeconds"),
         track.synthFilterEnvelopeDecaySeconds},
        {QStringLiteral("synthFilterEnvelopeSustainLevel"),
         track.synthFilterEnvelopeSustainLevel},
        {QStringLiteral("synthFilterEnvelopeReleaseSeconds"),
         track.synthFilterEnvelopeReleaseSeconds},
        {QStringLiteral("attackSeconds"), track.attackSeconds},
        {QStringLiteral("decaySeconds"), track.decaySeconds},
        {QStringLiteral("sustainLevel"), track.sustainLevel},
        {QStringLiteral("releaseSeconds"), track.releaseSeconds},
        {QStringLiteral("gain"), track.gain},
        {QStringLiteral("pan"), track.pan},
        {QStringLiteral("eqLowDb"), track.eqLowDb},
        {QStringLiteral("eqMidDb"), track.eqMidDb},
        {QStringLiteral("eqHighDb"), track.eqHighDb},
        {QStringLiteral("eqLowCrossoverHz"), track.eqLowCrossoverHz},
        {QStringLiteral("eqHighCrossoverHz"), track.eqHighCrossoverHz},
        {QStringLiteral("eqParametricBands"), [&track]
         {
             QJsonArray bands;
             for (const ParametricEqBand& band : track.eqParametricBands)
                 bands.push_back(QJsonObject{
                     {QStringLiteral("frequencyHz"), band.frequencyHz},
                     {QStringLiteral("gainDb"), band.gainDb},
                     {QStringLiteral("q"), band.q}
                 });
             return bands;
         }()},
        {QStringLiteral("outputBusId"), static_cast<qint64>(track.outputBusId)},
        {QStringLiteral("effects"), saveEffects(track.effects)},
        {QStringLiteral("sends"), [&track]
         {
             QJsonArray sends;
             for (const MixerSend& send : track.sends)
                 sends.push_back(QJsonObject{
                     {QStringLiteral("busId"), static_cast<qint64>(send.busId)},
                     {QStringLiteral("gain"), send.gain}
                 });
             return sends;
         }()},
        {QStringLiteral("volumeAutomation"), saveAutomation(track.volumeAutomation)},
        {QStringLiteral("panAutomation"), saveAutomation(track.panAutomation)},
        {QStringLiteral("muted"), track.muted},
        {QStringLiteral("solo"), track.solo},
        {QStringLiteral("drumSamples"), [&track]
         {
             QJsonArray samples;
             for (const DrumPadSample& sample : track.drumSamples)
                 samples.push_back(QJsonObject{
                     {QStringLiteral("path"), sample.path},
                     {QStringLiteral("veryLowVelocityPath"), sample.veryLowVelocityPath},
                     {QStringLiteral("veryLowVelocityThreshold"), sample.veryLowVelocityThreshold},
                     {QStringLiteral("lowVelocityPath"), sample.lowVelocityPath},
                     {QStringLiteral("lowVelocityThreshold"), sample.lowVelocityThreshold},
                     {QStringLiteral("highVelocityPath"), sample.highVelocityPath},
                     {QStringLiteral("highVelocityThreshold"), sample.highVelocityThreshold},
                     {QStringLiteral("veryHighVelocityPath"), sample.veryHighVelocityPath},
                     {QStringLiteral("veryHighVelocityThreshold"), sample.veryHighVelocityThreshold},
                     {QStringLiteral("tuningSemitones"), sample.tuningSemitones},
                     {QStringLiteral("startPositionPercent"), sample.startPositionPercent},
                     {QStringLiteral("loopEnabled"), sample.loopEnabled},
                     {QStringLiteral("attackSeconds"), sample.attackSeconds},
                     {QStringLiteral("decaySeconds"), sample.decaySeconds},
                     {QStringLiteral("sustainLevel"), sample.sustainLevel},
                     {QStringLiteral("releaseSeconds"), sample.releaseSeconds},
                     {QStringLiteral("velocityLayerSettings"), [&sample]
                      {
                          QJsonArray layers;
                          for (size_t layer = 0; layer < sample.velocityLayerSettings.size(); ++layer)
                          {
                              const DrumLayerSettings& settings = sample.velocityLayerSettings[layer];
                              layers.push_back(QJsonObject{
                                  {QStringLiteral("custom"), sample.velocityLayerSettingsCustom[layer]},
                                  {QStringLiteral("tuningSemitones"), settings.tuningSemitones},
                                  {QStringLiteral("startPositionPercent"), settings.startPositionPercent},
                                  {QStringLiteral("loopEnabled"), settings.loopEnabled},
                                  {QStringLiteral("attackSeconds"), settings.attackSeconds},
                                  {QStringLiteral("decaySeconds"), settings.decaySeconds},
                                  {QStringLiteral("sustainLevel"), settings.sustainLevel},
                                  {QStringLiteral("releaseSeconds"), settings.releaseSeconds}
                              });
                          }
                          return layers;
                      }()}
                 });
             return samples;
         }()},
        {QStringLiteral("clips"), clips}
    };
}

bool readId(const QJsonObject& object, const QString& key, quint64& id, QString* error)
{
    const qint64 value = object.value(key).toInteger();
    if (value < 0)
    {
        setError(error, QStringLiteral("The project contains an invalid %1 ID.").arg(key));
        return false;
    }
    id = static_cast<quint64>(value);
    return true;
}

bool loadClip(const QJsonValue& value, MidiClip& clip, const QString& mediaBaseDir,
              MediaLoadContext& media, int version, QString* error)
{
    if (!value.isObject())
    {
        setError(error, QStringLiteral("The project contains an invalid MIDI clip."));
        return false;
    }
    const QJsonObject object = value.toObject();
    if (!readId(object, QStringLiteral("id"), clip.id, error))
        return false;
    clip.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Pattern"));
    clip.startBeat = object.value(QStringLiteral("startBeat")).toDouble(-1.0);
    clip.lengthBeats = object.value(QStringLiteral("lengthBeats")).toDouble(-1.0);
    if (version >= 26)
        clip.stepsPerBar = object.value(QStringLiteral("stepsPerBar")).toInt(16);
    if (clip.startBeat < 0.0 || clip.lengthBeats <= 0.0)
    {
        setError(error, QStringLiteral("The project contains an out-of-range MIDI clip."));
        return false;
    }
    clip.audioSourcePath = object.value(QStringLiteral("audioSourcePath")).toString();
    const qint64 storedAudioStart = object.value(QStringLiteral("audioStartFrame")).toInteger(0);
    if (storedAudioStart < 0)
    {
        setError(error, QStringLiteral("The project contains an invalid audio trim position."));
        return false;
    }
    clip.audioStartFrame = storedAudioStart;
    clip.audioFadeInBeats = object.value(QStringLiteral("audioFadeInBeats")).toDouble(0.0);
    clip.audioFadeOutBeats = object.value(QStringLiteral("audioFadeOutBeats")).toDouble(0.0);
    if (version >= 16)
    {
        const int fadeInCurve = object.value(QStringLiteral("audioFadeInCurve")).toInt(-1);
        const int fadeOutCurve = object.value(QStringLiteral("audioFadeOutCurve")).toInt(-1);
        const int maximumFadeCurve = version >= 33 ? 3 : 2;
        if (fadeInCurve < 0 || fadeInCurve > maximumFadeCurve ||
            fadeOutCurve < 0 || fadeOutCurve > maximumFadeCurve)
        {
            setError(error, QStringLiteral("The audio region contains an invalid fade curve."));
            return false;
        }
        clip.audioFadeInCurve = static_cast<AudioFadeCurve>(fadeInCurve);
        clip.audioFadeOutCurve = static_cast<AudioFadeCurve>(fadeOutCurve);
    }
    if (version >= 33)
    {
        clip.audioFadeInShape = object.value(QStringLiteral("audioFadeInShape")).toDouble(-1.0);
        clip.audioFadeOutShape = object.value(QStringLiteral("audioFadeOutShape")).toDouble(-1.0);
        if (!std::isfinite(clip.audioFadeInShape) || clip.audioFadeInShape < 0.25 ||
            clip.audioFadeInShape > 4.0 || !std::isfinite(clip.audioFadeOutShape) ||
            clip.audioFadeOutShape < 0.25 || clip.audioFadeOutShape > 4.0)
        {
            setError(error, QStringLiteral("The audio region contains an invalid custom fade shape."));
            return false;
        }
    }
    loadAudioReference(clip.audioSourcePath, mediaBaseDir, media, clip.audioSource);
    if (!loadNotes(object.value(QStringLiteral("notes")), clip.notes, version, error))
        return false;
    return version < 21 ||
           loadPitchBends(object.value(QStringLiteral("pitchBends")), clip.pitchBends, error);
}

bool loadTrack(const QJsonValue& value, InstrumentTrack& track, const QString& mediaBaseDir,
               MediaLoadContext& media, int version, QString* error)
{
    if (!value.isObject())
    {
        setError(error, QStringLiteral("The project contains an invalid instrument track."));
        return false;
    }
    const QJsonObject object = value.toObject();
    if (!readId(object, QStringLiteral("id"), track.id, error))
        return false;
    track.name = object.value(QStringLiteral("name")).toString(QStringLiteral("Synth"));
    const int instrumentType = object.value(QStringLiteral("instrumentType")).toInt(0);
    if (instrumentType < 0 || instrumentType > (version >= 40 ? 3 : 2))
    {
        setError(error, QStringLiteral("The project contains an unsupported instrument type."));
        return false;
    }
    track.instrumentType = static_cast<InstrumentType>(instrumentType);
    if (version >= 40)
    {
        const QJsonValue pluginId = object.value(QStringLiteral("clapPluginId"));
        const QJsonValue pluginName = object.value(QStringLiteral("clapPluginName"));
        const QJsonValue pluginState = object.value(QStringLiteral("clapPluginState"));
        const QJsonValue parameterValues = object.value(QStringLiteral("clapParameterValues"));
        if ((!pluginId.isString() && !pluginId.isUndefined()) ||
            (!pluginName.isString() && !pluginName.isUndefined()) ||
            (!pluginState.isString() && !pluginState.isUndefined()) ||
            !parameterValues.isArray() || parameterValues.toArray().size() > 256)
        {
            setError(error, QStringLiteral("The track contains invalid CLAP instrument settings."));
            return false;
        }
        track.clapPluginId = pluginId.toString();
        track.clapPluginName = pluginName.toString();
        track.clapPluginState = pluginState.toString();
        if (track.clapPluginId.size() > 1024 || track.clapPluginName.size() > 512 ||
            track.clapPluginState.toUtf8().size() > maximumPluginStateBytes)
        {
            setError(error, QStringLiteral("The track contains oversized CLAP instrument settings."));
            return false;
        }
        for (const QJsonValue& value : parameterValues.toArray())
        {
            const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(number) || number < -1.0e9 || number > 1.0e9)
            {
                setError(error, QStringLiteral("The track contains an invalid CLAP parameter value."));
                return false;
            }
            track.clapParameterValues.push_back(static_cast<float>(number));
        }
        if (version >= 41)
        {
            const QJsonValue parameterAutomation =
                object.value(QStringLiteral("clapParameterAutomation"));
            if (!parameterAutomation.isUndefined() &&
                (!parameterAutomation.isArray() || parameterAutomation.toArray().size() > 256))
            {
                setError(error, QStringLiteral("The track contains invalid CLAP instrument automation."));
                return false;
            }
            for (const QJsonValue& curve : parameterAutomation.toArray())
            {
                QVector<AutomationPoint> points;
                if (!loadAutomation(curve, points, QStringLiteral("CLAP instrument parameter"),
                                    version, error))
                    return false;
                track.clapParameterAutomation.push_back(std::move(points));
            }
        }
    }
    track.waveform = static_cast<SynthWaveform>(object.value(QStringLiteral("waveform")).toInt(0));
    if (version >= 23)
    {
        track.secondOscillatorEnabled = object.value(QStringLiteral("secondOscillatorEnabled")).toBool(false);
        track.secondOscillatorWaveform = static_cast<SynthWaveform>(
            object.value(QStringLiteral("secondOscillatorWaveform")).toInt(
                static_cast<int>(track.secondOscillatorWaveform)));
        track.secondOscillatorDetuneCents = static_cast<float>(
            object.value(QStringLiteral("secondOscillatorDetuneCents")).toDouble(0.0));
        track.secondOscillatorMix = static_cast<float>(
            object.value(QStringLiteral("secondOscillatorMix")).toDouble(0.5));
    }
    if (version >= 24)
    {
        track.synthFilterEnabled = object.value(QStringLiteral("synthFilterEnabled")).toBool(false);
        if (version >= 42)
            track.synthFilterType = static_cast<SynthFilterType>(
                object.value(QStringLiteral("synthFilterType")).toInt(0));
        if (version >= 43)
        {
            track.synthFilterEnvelopeEnabled =
                object.value(QStringLiteral("synthFilterEnvelopeEnabled")).toBool(false);
            track.synthFilterEnvelopeAmountSemitones = static_cast<float>(
                object.value(QStringLiteral("synthFilterEnvelopeAmountSemitones")).toDouble(24.0));
            track.synthFilterEnvelopeAttackSeconds = static_cast<float>(
                object.value(QStringLiteral("synthFilterEnvelopeAttackSeconds")).toDouble(0.008));
            track.synthFilterEnvelopeDecaySeconds = static_cast<float>(
                object.value(QStringLiteral("synthFilterEnvelopeDecaySeconds")).toDouble(0.2));
            track.synthFilterEnvelopeSustainLevel = static_cast<float>(
                object.value(QStringLiteral("synthFilterEnvelopeSustainLevel")).toDouble(0.0));
            track.synthFilterEnvelopeReleaseSeconds = static_cast<float>(
                object.value(QStringLiteral("synthFilterEnvelopeReleaseSeconds")).toDouble(0.2));
        }
        track.synthFilterCutoffHz = static_cast<float>(
            object.value(QStringLiteral("synthFilterCutoffHz")).toDouble(20000.0));
        track.synthFilterResonance = static_cast<float>(
            object.value(QStringLiteral("synthFilterResonance")).toDouble(0.707));
    }
    track.attackSeconds = static_cast<float>(
        object.value(QStringLiteral("attackSeconds")).toDouble(track.attackSeconds));
    track.decaySeconds = static_cast<float>(
        object.value(QStringLiteral("decaySeconds")).toDouble(track.decaySeconds));
    track.sustainLevel = static_cast<float>(
        object.value(QStringLiteral("sustainLevel")).toDouble(track.sustainLevel));
    track.releaseSeconds = static_cast<float>(
        object.value(QStringLiteral("releaseSeconds")).toDouble(track.releaseSeconds));
    track.gain = static_cast<float>(object.value(QStringLiteral("gain")).toDouble(0.8));
    track.pan = static_cast<float>(object.value(QStringLiteral("pan")).toDouble(0.0));
    if (version >= 13)
    {
        track.eqLowDb = static_cast<float>(object.value(QStringLiteral("eqLowDb")).toDouble(0.0));
        track.eqMidDb = static_cast<float>(object.value(QStringLiteral("eqMidDb")).toDouble(0.0));
        track.eqHighDb = static_cast<float>(object.value(QStringLiteral("eqHighDb")).toDouble(0.0));
        track.eqLowCrossoverHz = static_cast<float>(
            object.value(QStringLiteral("eqLowCrossoverHz")).toDouble(120.0));
        track.eqHighCrossoverHz = static_cast<float>(
            object.value(QStringLiteral("eqHighCrossoverHz")).toDouble(6000.0));
    }
    if (version >= 47)
    {
        const QJsonValue bandsValue = object.value(QStringLiteral("eqParametricBands"));
        if (bandsValue.isArray())
        {
            const QJsonArray bands = bandsValue.toArray();
            for (qsizetype index = 0;
                 index < bands.size() && index < static_cast<qsizetype>(track.eqParametricBands.size());
                 ++index)
            {
                if (!bands[index].isObject())
                    continue;
                const QJsonObject bandObject = bands[index].toObject();
                ParametricEqBand& band = track.eqParametricBands[static_cast<size_t>(index)];
                band.frequencyHz = static_cast<float>(
                    bandObject.value(QStringLiteral("frequencyHz")).toDouble(band.frequencyHz));
                band.gainDb = static_cast<float>(
                    bandObject.value(QStringLiteral("gainDb")).toDouble(band.gainDb));
                band.q = static_cast<float>(
                    bandObject.value(QStringLiteral("q")).toDouble(band.q));
            }
        }
    }
    if (version >= 14 && !readId(object, QStringLiteral("outputBusId"), track.outputBusId, error))
        return false;
    if (version >= 17)
    {
        const QJsonValue sendsValue = object.value(QStringLiteral("sends"));
        if (!sendsValue.isArray() || sendsValue.toArray().size() > 64)
        {
            setError(error, QStringLiteral("The track has invalid or excessive mixer sends."));
            return false;
        }
        for (const QJsonValue& sendValue : sendsValue.toArray())
        {
            if (!sendValue.isObject())
            {
                setError(error, QStringLiteral("The track contains an invalid mixer send."));
                return false;
            }
            const QJsonObject sendObject = sendValue.toObject();
            MixerSend send;
            if (!readId(sendObject, QStringLiteral("busId"), send.busId, error))
                return false;
            const double gain = sendObject.value(QStringLiteral("gain")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
            if (send.busId == 0 || !std::isfinite(gain))
            {
                setError(error, QStringLiteral("The track contains an invalid mixer send setting."));
                return false;
            }
            send.gain = static_cast<float>(gain);
            track.sends.push_back(send);
        }
    }
    else if (version >= 16)
    {
        quint64 sendBusId = 0;
        if (!readId(object, QStringLiteral("sendBusId"), sendBusId, error))
            return false;
        const double sendGain = object.value(QStringLiteral("sendGain")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(sendGain))
        {
            setError(error, QStringLiteral("The track contains an invalid mixer send level."));
            return false;
        }
        if (sendBusId != 0)
            track.sends.push_back({sendBusId, static_cast<float>(sendGain)});
    }
    if (version >= 19 &&
        !loadEffects(object.value(QStringLiteral("effects")), track.effects,
                     QStringLiteral("track"), version, error))
        return false;
    if (!loadAutomation(object.value(QStringLiteral("volumeAutomation")), track.volumeAutomation,
                        QStringLiteral("volume"), version, error) ||
        !loadAutomation(object.value(QStringLiteral("panAutomation")), track.panAutomation,
                        QStringLiteral("pan"), version, error))
        return false;
    track.muted = object.value(QStringLiteral("muted")).toBool(false);
    track.solo = object.value(QStringLiteral("solo")).toBool(false);
    const QJsonValue drumSampleValue = object.value(QStringLiteral("drumSamples"));
    if (drumSampleValue.isArray())
    {
        const QJsonArray drumSamples = drumSampleValue.toArray();
        if (drumSamples.size() > static_cast<qsizetype>(track.drumSamples.size()))
        {
            setError(error, QStringLiteral("The drum rack contains too many sample assignments."));
            return false;
        }
        for (qsizetype index = 0; index < drumSamples.size(); ++index)
        {
            DrumPadSample& sample = track.drumSamples[static_cast<size_t>(index)];
            if (version >= 25)
            {
                if (!drumSamples[index].isObject())
                {
                    setError(error, QStringLiteral("The drum rack contains an invalid sample assignment."));
                    return false;
                }
                const QJsonObject sampleObject = drumSamples[index].toObject();
                sample.path = sampleObject.value(QStringLiteral("path")).toString();
                const double tuningSemitones = sampleObject.value(QStringLiteral("tuningSemitones")).toDouble(
                    std::numeric_limits<double>::quiet_NaN());
                const double startPositionPercent = sampleObject.value(
                    QStringLiteral("startPositionPercent")).toDouble(std::numeric_limits<double>::quiet_NaN());
                if (!std::isfinite(tuningSemitones) || !std::isfinite(startPositionPercent))
                {
                    setError(error, QStringLiteral("The drum rack contains invalid sample controls."));
                    return false;
                }
                sample.tuningSemitones = static_cast<float>(tuningSemitones);
                sample.startPositionPercent = static_cast<float>(startPositionPercent);
                if (version >= 27)
                    sample.loopEnabled = sampleObject.value(QStringLiteral("loopEnabled")).toBool(false);
                if (version >= 30)
                {
                    const QJsonValue highVelocityPath = sampleObject.value(QStringLiteral("highVelocityPath"));
                    sample.highVelocityThreshold = sampleObject.value(
                        QStringLiteral("highVelocityThreshold")).toInt(-1);
                    if ((!highVelocityPath.isString() && !highVelocityPath.isNull()) ||
                        sample.highVelocityThreshold < 1 || sample.highVelocityThreshold > 127)
                    {
                        setError(error, QStringLiteral("The drum rack contains an invalid velocity layer."));
                        return false;
                    }
                    sample.highVelocityPath = highVelocityPath.toString();
                }
                if (version >= 31)
                {
                    const double attackSeconds = sampleObject.value(
                        QStringLiteral("attackSeconds")).toDouble(std::numeric_limits<double>::quiet_NaN());
                    const double decaySeconds = sampleObject.value(
                        QStringLiteral("decaySeconds")).toDouble(std::numeric_limits<double>::quiet_NaN());
                    const double sustainLevel = sampleObject.value(
                        QStringLiteral("sustainLevel")).toDouble(std::numeric_limits<double>::quiet_NaN());
                    const double releaseSeconds = sampleObject.value(
                        QStringLiteral("releaseSeconds")).toDouble(std::numeric_limits<double>::quiet_NaN());
                    if (!std::isfinite(attackSeconds) || !std::isfinite(decaySeconds) ||
                        !std::isfinite(sustainLevel) || !std::isfinite(releaseSeconds))
                    {
                        setError(error, QStringLiteral("The drum rack contains invalid pad envelope settings."));
                        return false;
                    }
                    sample.attackSeconds = static_cast<float>(attackSeconds);
                    sample.decaySeconds = static_cast<float>(decaySeconds);
                    sample.sustainLevel = static_cast<float>(sustainLevel);
                    sample.releaseSeconds = static_cast<float>(releaseSeconds);
                }
                if (version >= 32)
                {
                    const QJsonValue lowVelocityPath = sampleObject.value(
                        QStringLiteral("lowVelocityPath"));
                    sample.lowVelocityThreshold = sampleObject.value(
                        QStringLiteral("lowVelocityThreshold")).toInt(-1);
                    if ((!lowVelocityPath.isString() && !lowVelocityPath.isNull()) ||
                        sample.lowVelocityThreshold < 1 || sample.lowVelocityThreshold > 126 ||
                        (!lowVelocityPath.toString().isEmpty() && !sample.highVelocityPath.isEmpty() &&
                         sample.lowVelocityThreshold >= sample.highVelocityThreshold))
                    {
                        setError(error, QStringLiteral("The drum rack contains an invalid low-velocity layer."));
                        return false;
                    }
                    sample.lowVelocityPath = lowVelocityPath.toString();
                }
                if (version >= 44)
                {
                    const QJsonValue veryLowVelocityPath = sampleObject.value(
                        QStringLiteral("veryLowVelocityPath"));
                    const QJsonValue veryHighVelocityPath = sampleObject.value(
                        QStringLiteral("veryHighVelocityPath"));
                    sample.veryLowVelocityThreshold = sampleObject.value(
                        QStringLiteral("veryLowVelocityThreshold")).toInt(-1);
                    sample.veryHighVelocityThreshold = sampleObject.value(
                        QStringLiteral("veryHighVelocityThreshold")).toInt(-1);
                    if ((!veryLowVelocityPath.isString() && !veryLowVelocityPath.isNull()) ||
                        (!veryHighVelocityPath.isString() && !veryHighVelocityPath.isNull()) ||
                        sample.veryLowVelocityThreshold < 1 ||
                        sample.veryHighVelocityThreshold > 127 ||
                        sample.veryLowVelocityThreshold > sample.lowVelocityThreshold ||
                        sample.lowVelocityThreshold >= sample.highVelocityThreshold ||
                        sample.highVelocityThreshold > sample.veryHighVelocityThreshold)
                    {
                        setError(error, QStringLiteral("The drum rack contains invalid outer velocity layers."));
                        return false;
                    }
                    sample.veryLowVelocityPath = veryLowVelocityPath.toString();
                    sample.veryHighVelocityPath = veryHighVelocityPath.toString();
                }
                if (version >= 45)
                {
                    const QJsonValue layerSettingsValue = sampleObject.value(
                        QStringLiteral("velocityLayerSettings"));
                    if (!layerSettingsValue.isArray() ||
                        layerSettingsValue.toArray().size() !=
                            static_cast<qsizetype>(sample.velocityLayerSettings.size()))
                    {
                        setError(error, QStringLiteral("The drum rack contains invalid per-layer controls."));
                        return false;
                    }
                    const QJsonArray layerSettings = layerSettingsValue.toArray();
                    for (qsizetype layer = 0; layer < layerSettings.size(); ++layer)
                    {
                        if (!layerSettings[layer].isObject())
                        {
                            setError(error, QStringLiteral("The drum rack contains invalid per-layer controls."));
                            return false;
                        }
                        const QJsonObject layerObject = layerSettings[layer].toObject();
                        const QJsonValue customValue = layerObject.value(QStringLiteral("custom"));
                        const QJsonValue loopValue = layerObject.value(QStringLiteral("loopEnabled"));
                        DrumLayerSettings& settings = sample.velocityLayerSettings[
                            static_cast<size_t>(layer)];
                        const double tuning = layerObject.value(QStringLiteral("tuningSemitones"))
                            .toDouble(std::numeric_limits<double>::quiet_NaN());
                        const double start = layerObject.value(QStringLiteral("startPositionPercent"))
                            .toDouble(std::numeric_limits<double>::quiet_NaN());
                        const double attack = layerObject.value(QStringLiteral("attackSeconds"))
                            .toDouble(std::numeric_limits<double>::quiet_NaN());
                        const double decay = layerObject.value(QStringLiteral("decaySeconds"))
                            .toDouble(std::numeric_limits<double>::quiet_NaN());
                        const double sustain = layerObject.value(QStringLiteral("sustainLevel"))
                            .toDouble(std::numeric_limits<double>::quiet_NaN());
                        const double release = layerObject.value(QStringLiteral("releaseSeconds"))
                            .toDouble(std::numeric_limits<double>::quiet_NaN());
                        if (!customValue.isBool() || !loopValue.isBool() ||
                            !std::isfinite(tuning) || !std::isfinite(start) ||
                            !std::isfinite(attack) || !std::isfinite(decay) ||
                            !std::isfinite(sustain) || !std::isfinite(release))
                        {
                            setError(error, QStringLiteral("The drum rack contains invalid per-layer controls."));
                            return false;
                        }
                        sample.velocityLayerSettingsCustom[static_cast<size_t>(layer)] =
                            customValue.toBool();
                        settings.tuningSemitones = static_cast<float>(tuning);
                        settings.startPositionPercent = static_cast<float>(start);
                        settings.loopEnabled = loopValue.toBool();
                        settings.attackSeconds = static_cast<float>(attack);
                        settings.decaySeconds = static_cast<float>(decay);
                        settings.sustainLevel = static_cast<float>(sustain);
                        settings.releaseSeconds = static_cast<float>(release);
                    }
                }
            }
            else
            {
                if (!drumSamples[index].isString())
                {
                    setError(error, QStringLiteral("The drum rack contains an invalid sample path."));
                    return false;
                }
                sample.path = drumSamples[index].toString();
            }
            loadAudioReference(sample.path, mediaBaseDir, media, sample.source);
            loadAudioReference(sample.veryLowVelocityPath, mediaBaseDir, media,
                               sample.veryLowVelocitySource);
            loadAudioReference(sample.lowVelocityPath, mediaBaseDir, media, sample.lowVelocitySource);
            loadAudioReference(sample.highVelocityPath, mediaBaseDir, media, sample.highVelocitySource);
            loadAudioReference(sample.veryHighVelocityPath, mediaBaseDir, media,
                               sample.veryHighVelocitySource);
        }
    }
    else if (!drumSampleValue.isUndefined() && !drumSampleValue.isNull())
    {
        setError(error, QStringLiteral("The drum rack has invalid sample assignments."));
        return false;
    }
    const QJsonValue clipValue = object.value(QStringLiteral("clips"));
    if (!clipValue.isArray() || clipValue.toArray().size() > 10000)
    {
        setError(error, QStringLiteral("The track has invalid or excessive clip data."));
        return false;
    }
    for (const QJsonValue& clipJson : clipValue.toArray())
    {
        MidiClip clip;
        if (!loadClip(clipJson, clip, mediaBaseDir, media, version, error))
            return false;
        track.clips.push_back(std::move(clip));
    }
    return true;
}
}

bool ProjectFile::save(const QString& path, const ProjectData& project, QString* error)
{
    ProjectData safeProject = project;
    safeProject.normalize();

    const auto statesAreWithinLimit = [](const QVector<MixerEffect>& effects)
    {
        return std::all_of(effects.cbegin(), effects.cend(), [](const MixerEffect& effect)
        {
            return effect.pluginState.toUtf8().size() <= maximumPluginStateBytes;
        });
    };
    if (!statesAreWithinLimit(safeProject.masterEffects) ||
        !std::all_of(safeProject.tracks.cbegin(), safeProject.tracks.cend(),
                     [&statesAreWithinLimit](const InstrumentTrack& track)
                     { return statesAreWithinLimit(track.effects) &&
                         track.clapPluginState.toUtf8().size() <= maximumPluginStateBytes; }) ||
        !std::all_of(safeProject.buses.cbegin(), safeProject.buses.cend(),
                     [&statesAreWithinLimit](const MixerBus& bus)
                     { return statesAreWithinLimit(bus.effects); }))
    {
        setError(error, QStringLiteral("A plugin state is larger than 1 MiB; the project was not saved."));
        return false;
    }

    QJsonArray tracks;
    for (const InstrumentTrack& track : safeProject.tracks)
        tracks.push_back(saveTrack(track));
    QJsonArray markers;
    for (const TimelineMarker& marker : safeProject.markers)
        markers.push_back(QJsonObject{
            {QStringLiteral("beat"), marker.beat},
            {QStringLiteral("name"), marker.name}
        });
    QJsonArray buses;
    for (const MixerBus& bus : safeProject.buses)
        buses.push_back(QJsonObject{
            {QStringLiteral("id"), static_cast<qint64>(bus.id)},
            {QStringLiteral("name"), bus.name},
            {QStringLiteral("outputBusId"), static_cast<qint64>(bus.outputBusId)},
            {QStringLiteral("sends"), [&bus]
             {
                 QJsonArray sends;
                 for (const MixerSend& send : bus.sends)
                     sends.push_back(QJsonObject{
                         {QStringLiteral("busId"), static_cast<qint64>(send.busId)},
                         {QStringLiteral("gain"), send.gain}
                     });
                 return sends;
             }()},
            {QStringLiteral("effects"), saveEffects(bus.effects)},
            {QStringLiteral("gain"), bus.gain},
            {QStringLiteral("pan"), bus.pan},
            {QStringLiteral("muted"), bus.muted},
            {QStringLiteral("solo"), bus.solo}
        });

    const QJsonObject root{
        {QStringLiteral("format"), QString::fromLatin1(projectFormat)},
        {QStringLiteral("version"), currentProjectVersion},
        {QStringLiteral("title"), safeProject.title},
        {QStringLiteral("tempoBpm"), safeProject.tempoBpm},
        {QStringLiteral("beatsPerBar"), safeProject.beatsPerBar},
        {QStringLiteral("beatUnit"), safeProject.beatUnit},
        {QStringLiteral("bars"), safeProject.bars},
        {QStringLiteral("loopEnabled"), safeProject.loopEnabled},
        {QStringLiteral("metronomeEnabled"), safeProject.metronomeEnabled},
        {QStringLiteral("countInBars"), safeProject.countInBars},
        {QStringLiteral("masterGain"), safeProject.masterGain},
        {QStringLiteral("pitchBendRangeSemitones"), safeProject.pitchBendRangeSemitones},
        {QStringLiteral("masterEffects"), saveEffects(safeProject.masterEffects)},
        {QStringLiteral("markers"), markers},
        {QStringLiteral("tempoChanges"), saveTempoChanges(safeProject.tempoChanges)},
        {QStringLiteral("buses"), buses},
        {QStringLiteral("tracks"), tracks}
    };

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        setError(error, file.errorString());
        return false;
    }

    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size())
    {
        setError(error, file.errorString());
        return false;
    }
    if (!file.commit())
    {
        setError(error, file.errorString());
        return false;
    }
    return true;
}

bool ProjectFile::load(const QString& path, ProjectData& project, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        setError(error, file.errorString());
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        setError(error, parseError.errorString());
        return false;
    }

    const QString mediaBaseDir = QFileInfo(path).absoluteDir().absolutePath();
    MediaLoadContext media;
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() != QString::fromLatin1(projectFormat))
    {
        setError(error, QStringLiteral("This file is not an Awesome Studio project."));
        return false;
    }
    const int version = root.value(QStringLiteral("version")).toInt(-1);
    if (version < 1 || version > currentProjectVersion)
    {
        setError(error, QStringLiteral("This project uses an unsupported file version."));
        return false;
    }

    ProjectData loaded;
    loaded.tracks.clear();
    loaded.title = root.value(QStringLiteral("title")).toString(QStringLiteral("Untitled"));
    loaded.tempoBpm = root.value(QStringLiteral("tempoBpm")).toDouble(120.0);
    loaded.beatsPerBar = root.value(QStringLiteral("beatsPerBar")).toInt(4);
    loaded.beatUnit = version >= 3 ? root.value(QStringLiteral("beatUnit")).toInt(4) : 4;
    loaded.bars = root.value(QStringLiteral("bars")).toInt(4);
    loaded.loopEnabled = root.value(QStringLiteral("loopEnabled")).toBool(true);
    loaded.metronomeEnabled = version >= 5 ? root.value(QStringLiteral("metronomeEnabled")).toBool(false) : false;
    loaded.countInBars = version >= 5 ? root.value(QStringLiteral("countInBars")).toInt(0) : 0;
    loaded.masterGain = static_cast<float>(root.value(QStringLiteral("masterGain")).toDouble(0.8));
    if (version >= 37)
    {
        const QJsonValue tempoValue = root.value(QStringLiteral("tempoChanges"));
        if (!tempoValue.isArray() || tempoValue.toArray().size() > 1000)
        {
            setError(error, QStringLiteral("The project has invalid or excessive tempo changes."));
            return false;
        }
        for (const QJsonValue& changeValue : tempoValue.toArray())
        {
            if (!changeValue.isObject())
            {
                setError(error, QStringLiteral("The project contains an invalid tempo change."));
                return false;
            }
            const QJsonObject object = changeValue.toObject();
            const double beat = object.value(QStringLiteral("beat")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
            const double bpm = object.value(QStringLiteral("bpm")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(beat) || beat < 0.0 || beat > 16384.0 ||
                !std::isfinite(bpm) || bpm < 20.0 || bpm > 300.0)
            {
                setError(error, QStringLiteral("The project contains an invalid tempo change."));
                return false;
            }
            loaded.tempoChanges.push_back({beat, bpm,
                version >= 48 && object.value(QStringLiteral("rampFromPrevious")).toBool(false)});
        }
    }
    if (version >= 22)
    {
        const double pitchBendRange = root.value(QStringLiteral("pitchBendRangeSemitones")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(pitchBendRange) || pitchBendRange < 0.01 || pitchBendRange > 48.0)
        {
            setError(error, QStringLiteral("The project has an invalid MIDI pitch-bend range."));
            return false;
        }
        loaded.pitchBendRangeSemitones = pitchBendRange;
    }
    if (version >= 19 &&
        !loadEffects(root.value(QStringLiteral("masterEffects")), loaded.masterEffects,
                     QStringLiteral("Master channel"), version, error))
        return false;
    if (version >= 12)
    {
        const QJsonValue markerValue = root.value(QStringLiteral("markers"));
        if (!markerValue.isArray() || markerValue.toArray().size() > 1000)
        {
            setError(error, QStringLiteral("The project has invalid or excessive timeline markers."));
            return false;
        }
        for (const QJsonValue& markerJson : markerValue.toArray())
        {
            if (!markerJson.isObject())
            {
                setError(error, QStringLiteral("The project contains an invalid timeline marker."));
                return false;
            }
            const QJsonObject markerObject = markerJson.toObject();
            TimelineMarker marker;
            marker.beat = markerObject.value(QStringLiteral("beat")).toDouble(-1.0);
            marker.name = markerObject.value(QStringLiteral("name")).toString();
            if (!std::isfinite(marker.beat) || marker.beat < 0.0 || marker.beat > 16384.0 ||
                marker.name.trimmed().isEmpty())
            {
                setError(error, QStringLiteral("The project contains an out-of-range timeline marker."));
                return false;
            }
            loaded.markers.push_back(std::move(marker));
        }
    }
    if (version >= 14)
    {
        const QJsonValue busValue = root.value(QStringLiteral("buses"));
        if (!busValue.isArray() || busValue.toArray().size() > 64)
        {
            setError(error, QStringLiteral("The project has invalid or excessive mixer bus data."));
            return false;
        }
        for (const QJsonValue& busJson : busValue.toArray())
        {
            if (!busJson.isObject())
            {
                setError(error, QStringLiteral("The project contains an invalid mixer bus."));
                return false;
            }
            const QJsonObject busObject = busJson.toObject();
            MixerBus bus;
            if (!readId(busObject, QStringLiteral("id"), bus.id, error))
                return false;
            if (version >= 17 && !readId(busObject, QStringLiteral("outputBusId"),
                                         bus.outputBusId, error))
                return false;
            if (version >= 18)
            {
                const QJsonValue sendsValue = busObject.value(QStringLiteral("sends"));
                if (!sendsValue.isArray() || sendsValue.toArray().size() > 64)
                {
                    setError(error, QStringLiteral("The mixer bus has invalid or excessive auxiliary sends."));
                    return false;
                }
                for (const QJsonValue& sendValue : sendsValue.toArray())
                {
                    if (!sendValue.isObject())
                    {
                        setError(error, QStringLiteral("The mixer bus contains an invalid auxiliary send."));
                        return false;
                    }
                    const QJsonObject sendObject = sendValue.toObject();
                    MixerSend send;
                    if (!readId(sendObject, QStringLiteral("busId"), send.busId, error))
                        return false;
                    const double sendGain = sendObject.value(QStringLiteral("gain")).toDouble(
                        std::numeric_limits<double>::quiet_NaN());
                    if (send.busId == 0 || !std::isfinite(sendGain))
                    {
                        setError(error, QStringLiteral("The mixer bus contains an invalid send setting."));
                        return false;
                    }
                    send.gain = static_cast<float>(sendGain);
                    bus.sends.push_back(send);
                }
            }
            if (version >= 19 &&
                !loadEffects(busObject.value(QStringLiteral("effects")), bus.effects,
                             QStringLiteral("mixer bus"), version, error))
                return false;
            bus.name = busObject.value(QStringLiteral("name")).toString().trimmed();
            const double gain = busObject.value(QStringLiteral("gain")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
            const double pan = busObject.value(QStringLiteral("pan")).toDouble(
                std::numeric_limits<double>::quiet_NaN());
            if (bus.id == 0 || bus.name.isEmpty() || !std::isfinite(gain) || !std::isfinite(pan))
            {
                setError(error, QStringLiteral("The project contains an invalid mixer bus setting."));
                return false;
            }
            bus.gain = static_cast<float>(gain);
            bus.pan = static_cast<float>(pan);
            bus.muted = busObject.value(QStringLiteral("muted")).toBool(false);
            bus.solo = busObject.value(QStringLiteral("solo")).toBool(false);
            loaded.buses.push_back(std::move(bus));
        }
    }

    if (version == 1)
    {
        InstrumentTrack track;
        track.id = 1;
        track.name = QStringLiteral("Synth 1");
        track.waveform = static_cast<SynthWaveform>(root.value(QStringLiteral("waveform")).toInt(0));
        track.gain = 1.0f;
        MidiClip clip;
        clip.id = 1;
        clip.name = QStringLiteral("Pattern 1");
        clip.lengthBeats = loaded.patternLengthBeats();
        if (!loadNotes(root.value(QStringLiteral("notes")), clip.notes, version, error))
            return false;
        track.clips.push_back(std::move(clip));
        loaded.tracks.push_back(std::move(track));
    }
    else
    {
        const QJsonValue trackValue = root.value(QStringLiteral("tracks"));
        if (!trackValue.isArray() || trackValue.toArray().size() > 256)
        {
            setError(error, QStringLiteral("The project has invalid or excessive track data."));
            return false;
        }
        for (const QJsonValue& trackJson : trackValue.toArray())
        {
            InstrumentTrack track;
            if (!loadTrack(trackJson, track, mediaBaseDir, media, version, error))
                return false;
            loaded.tracks.push_back(std::move(track));
        }
    }

    loaded.normalize();
    project = std::move(loaded);
    return true;
}
