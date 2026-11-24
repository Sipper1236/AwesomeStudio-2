#include "ArrangementEditor.h"

#include <QMouseEvent>
#include <QKeyEvent>
#include <QFontMetrics>
#include <QHash>
#include <QLineF>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>

namespace
{
constexpr int trackHeaderWidth = 142;
constexpr int rulerHeight = 28;
constexpr int trackHeight = 72;
constexpr int automationLaneHeight = 50;
constexpr int synthFilterCutoffAutomationTarget = 2;
constexpr int synthFilterResonanceAutomationTarget = 3;
constexpr int pluginAutomationTargetBase = 4;
constexpr int pluginParametersPerEffect = 256;
constexpr int maximumPluginEffectCount = 8;
constexpr int clapInstrumentAutomationTargetBase =
    pluginAutomationTargetBase + pluginParametersPerEffect * maximumPluginEffectCount;
constexpr int vst3InstrumentAutomationTargetBase = clapInstrumentAutomationTargetBase +
    pluginParametersPerEffect;

bool decodeClapInstrumentAutomationTarget(int target, qsizetype& parameterIndex)
{
    const int encoded = target - clapInstrumentAutomationTargetBase;
    if (encoded < 0 || encoded >= pluginParametersPerEffect)
        return false;
    parameterIndex = encoded;
    return true;
}

bool decodeVst3InstrumentAutomationTarget(int target, qsizetype& parameterIndex)
{
    const int encoded = target - vst3InstrumentAutomationTargetBase;
    if (encoded < 0 || encoded >= pluginParametersPerEffect)
        return false;
    parameterIndex = encoded;
    return true;
}
QColor trackColour(int index)
{
    static const QColor palette[] = {
        QColor(QStringLiteral("#47a7bd")), QColor(QStringLiteral("#9a7bd1")),
        QColor(QStringLiteral("#d68756")), QColor(QStringLiteral("#66a879")),
        QColor(QStringLiteral("#d26f91")), QColor(QStringLiteral("#b4a04d"))
    };
    return palette[index % static_cast<int>(std::size(palette))];
}

bool automationPointsEqual(const QVector<AutomationPoint>& left, const QVector<AutomationPoint>& right)
{
    if (left.size() != right.size())
        return false;
    for (qsizetype index = 0; index < left.size(); ++index)
    {
        if (std::abs(left[index].beat - right[index].beat) > 1.0e-9 ||
            std::abs(left[index].value - right[index].value) > 1.0e-6f ||
            left[index].curveToNext != right[index].curveToNext)
            return false;
    }
    return true;
}
}

class ArrangementEditor::Canvas final : public QWidget
{
public:
    explicit Canvas(ArrangementEditor* owner) : QWidget(owner), owner(owner)
    {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
    }

    void setProject(const ProjectData& value)
    {
        project = value;
        if (selectedAutomationPoint >= 0)
        {
            if (selectedAutomationBusId != 0)
            {
                const auto bus = std::find_if(project.buses.cbegin(), project.buses.cend(),
                    [this](const MixerBus& candidate) { return candidate.id == selectedAutomationBusId; });
                if (bus == project.buses.cend() || selectedAutomationPoint >= busAutomationPoints(*bus).size())
                {
                    selectedAutomationBusId = 0;
                    selectedAutomationPoint = -1;
                }
            }
            else
            {
            const InstrumentTrack* track = project.findTrack(selectedAutomationTrackId);
            if (track == nullptr || selectedAutomationPoint >= automationPoints(*track).size())
            {
                selectedAutomationTrackId = 0;
                selectedAutomationPoint = -1;
            }
            }
        }
        updateCanvasSize();
    }

    void setSelection(quint64 selectedTrack, quint64 selectedClip)
    {
        if (selectedTrackId != selectedTrack)
        {
            selectedAutomationTrackId = 0;
            selectedAutomationPoint = -1;
        }
        selectedTrackId = selectedTrack;
        selectedClipId = selectedClip;
        update();
    }

    void setPixelsPerBeat(int pixels)
    {
        pixelsPerBeat = std::clamp(pixels, 12, 64);
        updateCanvasSize();
    }

    void setPlayheadBeat(double beat)
    {
        playheadBeat = beat;
        update();
    }

    void setPunchRange(bool enabled, double punchInBeat, double punchOutBeat)
    {
        const bool finiteRange = std::isfinite(punchInBeat) && std::isfinite(punchOutBeat);
        if (!std::isfinite(punchInBeat))
            punchInBeat = 0.0;
        if (!std::isfinite(punchOutBeat))
            punchOutBeat = punchInBeat;
        punchInBeat = std::clamp(punchInBeat, 0.0, 16384.0);
        punchOutBeat = std::clamp(punchOutBeat, punchInBeat, 16384.0);
        punchRangeEnabled = enabled && finiteRange && punchOutBeat > punchInBeat;
        this->punchInBeat = punchInBeat;
        this->punchOutBeat = punchOutBeat;
        updateCanvasSize();
    }

    void setAutomationView(bool enabled, int parameter, float minimum, float maximum,
                           float defaultValue, const QString& pluginUri,
                           AutomationPoint::Curve defaultCurve, const QString& targetLabel)
    {
        automationViewEnabled = enabled;
        automationParameter = std::max(0, parameter);
        automationMinimum = std::isfinite(minimum) ? minimum : 0.0f;
        automationMaximum = std::isfinite(maximum) && maximum > automationMinimum
            ? maximum : automationMinimum + 1.0f;
        automationDefaultValue = std::isfinite(defaultValue)
            ? std::clamp(defaultValue, automationMinimum, automationMaximum) : automationMinimum;
        automationPluginUri = pluginUri;
        automationDefaultCurve = defaultCurve;
        automationTargetLabel = targetLabel;
        selectedAutomationTrackId = 0;
        selectedAutomationBusId = 0;
        selectedAutomationPoint = -1;
        updateCanvasSize();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor(QStringLiteral("#131820")));

        const int trackCount = std::max(1, static_cast<int>(project.tracks.size()) +
            ((automationViewEnabled && isBusAutomationTarget()) ? static_cast<int>(project.buses.size()) : 0));
        const int rowHeight = currentRowHeight();
        const double quarterNotesPerBar = project.quarterNotesPerBar();
        double visibleBeats = std::max(project.songLengthBeats() + quarterNotesPerBar * 4.0,
                                       quarterNotesPerBar * 16.0);
        for (const TimelineMarker& marker : project.markers)
            visibleBeats = std::max(visibleBeats, marker.beat + quarterNotesPerBar * 2.0);
        for (const TempoChange& change : project.tempoChanges)
            visibleBeats = std::max(visibleBeats, change.beat + quarterNotesPerBar * 2.0);
        const int gridWidth = static_cast<int>(std::ceil(visibleBeats * pixelsPerBeat));
        painter.fillRect(QRect(trackHeaderWidth, 0, gridWidth, rulerHeight), QColor(QStringLiteral("#202630")));
        painter.fillRect(QRect(0, 0, trackHeaderWidth, rulerHeight), QColor(QStringLiteral("#252d37")));

        for (int row = 0; row < trackCount; ++row)
        {
            const int y = rulerHeight + row * rowHeight;
            const bool selected = row < project.tracks.size() && project.tracks[row].id == selectedTrackId;
            painter.fillRect(QRect(trackHeaderWidth, y, gridWidth, rowHeight - 1),
                             row % 2 == 0 ? QColor(QStringLiteral("#191f27")) : QColor(QStringLiteral("#171d25")));
            painter.fillRect(QRect(0, y, trackHeaderWidth, rowHeight - 1),
                             selected ? QColor(QStringLiteral("#29343e")) : QColor(QStringLiteral("#222a33")));
            painter.setPen(QColor(QStringLiteral("#303944")));
            painter.drawLine(0, y + rowHeight - 1, width(), y + rowHeight - 1);

            if (row >= project.tracks.size())
                continue;
            const InstrumentTrack& track = project.tracks[row];
            painter.fillRect(QRect(0, y + 13, 3, trackHeight - 26), trackColour(row));
            painter.setPen(selected ? QColor(QStringLiteral("#f0f4f7")) : QColor(QStringLiteral("#c1cbd4")));
            painter.setFont(QFont(QStringLiteral("Sans Serif"), 9, selected ? QFont::DemiBold : QFont::Normal));
            painter.drawText(QRect(12, y + 8, trackHeaderWidth - 20, 22), Qt::AlignLeft | Qt::AlignVCenter, track.name);
            painter.setPen(QColor(QStringLiteral("#85919d")));
            painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
            painter.drawText(QRect(12, y + 31, trackHeaderWidth - 20, 18), Qt::AlignLeft | Qt::AlignVCenter,
                             track.muted ? tr("MUTED") :
                                 track.instrumentType == InstrumentType::DrumRack
                                     ? tr("DRUM · %1 clips").arg(track.clips.size())
                                     : track.instrumentType == InstrumentType::Audio
                                         ? tr("AUDIO · %1 regions").arg(track.clips.size())
                                         : tr("MIDI · %1 clips").arg(track.clips.size()));
            if (automationViewEnabled)
            {
                const int laneY = y + trackHeight;
                painter.fillRect(QRect(0, laneY, trackHeaderWidth, automationLaneHeight - 1),
                                 selected ? QColor(QStringLiteral("#252f39")) : QColor(QStringLiteral("#202730")));
                painter.setPen(QColor(QStringLiteral("#94a2ae")));
                painter.setFont(QFont(QStringLiteral("Sans Serif"), 8, QFont::DemiBold));
                const QString label = automationParameter == 0 ? tr("VOLUME · 0–100%") :
                    automationParameter == 1 ? tr("PAN · L–R") : automationTargetLabel;
                const QString shortLabel = painter.fontMetrics().elidedText(
                    label, Qt::ElideRight, trackHeaderWidth - 20);
                painter.drawText(QRect(12, laneY, trackHeaderWidth - 20, automationLaneHeight - 1),
                                 Qt::AlignLeft | Qt::AlignVCenter, shortLabel);
            }
        }

        const int totalBeats = static_cast<int>(std::ceil(visibleBeats));
        for (int beat = 0; beat <= totalBeats; ++beat)
        {
            const int x = trackHeaderWidth + beat * pixelsPerBeat;
            bool isBarLine = std::abs(static_cast<double>(beat)) < 0.02;
            for (const TimeSignatureChange& change : project.timeSignatureChanges)
            {
                if (change.beat > beat)
                    break;
                const double width = project.quarterNotesPerBarAtBeat(change.beat);
                isBarLine = isBarLine || std::abs(change.beat - beat) < 0.02;
                if (change.beat < beat && width > 0.0)
                {
                    const double count = std::floor((beat - change.beat) / width);
                    isBarLine = isBarLine || std::abs(change.beat + count * width - beat) < 0.02;
                }
            }
            if (project.timeSignatureChanges.isEmpty())
                isBarLine = std::abs(std::fmod(beat, quarterNotesPerBar)) < 0.02;
            else if (project.timeSignatureChanges.first().beat > beat)
            {
                isBarLine = std::abs(std::fmod(beat, quarterNotesPerBar)) < 0.02;
            }
            if (!isBarLine)
            {
                painter.setPen(QColor(QStringLiteral("#2b333d")));
                painter.drawLine(x, rulerHeight, x, rulerHeight + trackCount * rowHeight);
            }
        }
        int bar = 1;
        for (double beat = 0.0; beat <= visibleBeats;)
        {
            const int x = trackHeaderWidth + static_cast<int>(std::round(beat * pixelsPerBeat));
            painter.setPen(QColor(QStringLiteral("#596572")));
            painter.drawLine(x, rulerHeight, x, rulerHeight + trackCount * rowHeight);
            if (beat < visibleBeats)
            {
                painter.setPen(QColor(QStringLiteral("#e2e8ee")));
                painter.setFont(QFont(QStringLiteral("Sans Serif"), 8, QFont::DemiBold));
                painter.drawText(QRect(x + 5, 0,
                                       static_cast<int>(std::round(pixelsPerBeat * project.quarterNotesPerBarAtBeat(beat))) - 10,
                                       rulerHeight), Qt::AlignVCenter, QString::number(bar + 1));
            }
            double next = beat + project.quarterNotesPerBarAtBeat(beat);
            for (const TimeSignatureChange& change : project.timeSignatureChanges)
                if (change.beat > beat + 1.0e-6 && change.beat < next)
                {
                    next = change.beat;
                    break;
                }
            if (next <= beat + 1.0e-6)
                break;
            beat = next;
            ++bar;
        }

        if (punchRangeEnabled)
        {
            const int punchInX = trackHeaderWidth + static_cast<int>(std::round(
                punchInBeat * pixelsPerBeat));
            const int punchOutX = trackHeaderWidth + static_cast<int>(std::round(
                punchOutBeat * pixelsPerBeat));
            painter.fillRect(QRect(punchInX, rulerHeight, punchOutX - punchInX,
                                   trackCount * rowHeight), QColor(227, 135, 96, 24));
            painter.setPen(QPen(QColor(QStringLiteral("#e38760")), 1.5, Qt::DashLine));
            painter.drawLine(punchInX, 0, punchInX, rulerHeight + trackCount * rowHeight);
            painter.drawLine(punchOutX, 0, punchOutX, rulerHeight + trackCount * rowHeight);
        }

        if (automationViewEnabled)
        {
            for (int trackIndex = 0; trackIndex < project.tracks.size(); ++trackIndex)
            {
                const int rowY = rulerHeight + trackIndex * rowHeight;
                drawAutomationLane(painter, project.tracks[trackIndex], trackIndex, rowY, gridWidth);
            }
            if (isBusAutomationTarget())
            {
                for (int busIndex = 0; busIndex < project.buses.size(); ++busIndex)
                {
                    const MixerBus& bus = project.buses[busIndex];
                    const int row = static_cast<int>(project.tracks.size()) + busIndex;
                    const int rowY = rulerHeight + row * rowHeight;
                    const bool selected = bus.id == selectedAutomationBusId;
                    painter.fillRect(QRect(trackHeaderWidth, rowY, gridWidth, rowHeight - 1),
                                     row % 2 == 0 ? QColor(QStringLiteral("#191f27")) : QColor(QStringLiteral("#171d25")));
                    painter.fillRect(QRect(0, rowY, trackHeaderWidth, rowHeight - 1),
                                     selected ? QColor(QStringLiteral("#29343e")) : QColor(QStringLiteral("#222a33")));
                    painter.setPen(QColor(QStringLiteral("#303944")));
                    painter.drawLine(0, rowY + rowHeight - 1, width(), rowY + rowHeight - 1);
                    painter.fillRect(QRect(0, rowY + 13, 3, trackHeight - 26), QColor(QStringLiteral("#c18c57")));
                    painter.setPen(selected ? QColor(QStringLiteral("#f0f4f7")) : QColor(QStringLiteral("#c1cbd4")));
                    painter.setFont(QFont(QStringLiteral("Sans Serif"), 9, selected ? QFont::DemiBold : QFont::Normal));
                    painter.drawText(QRect(12, rowY + 8, trackHeaderWidth - 20, 22), Qt::AlignLeft | Qt::AlignVCenter, bus.name);
                    painter.setPen(QColor(QStringLiteral("#85919d")));
                    painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
                    painter.drawText(QRect(12, rowY + 31, trackHeaderWidth - 20, 18), Qt::AlignLeft | Qt::AlignVCenter,
                                     automationParameter == mixerBusGainAutomationTarget ? tr("MIXER BUS · LEVEL") :
                                     automationParameter == mixerBusPanAutomationTarget ? tr("MIXER BUS · PAN") :
                                     automationParameter == mixerBusMuteAutomationTarget ? tr("MIXER BUS · MUTE") :
                                     automationParameter == mixerBusSoloAutomationTarget ? tr("MIXER BUS · SOLO") :
                                     tr("MIXER BUS · SEND %1").arg(automationParameter - mixerSendLevelAutomationTargetBase + 1));
                    drawBusAutomationLane(painter, bus, rowY, gridWidth);
                }
            }
        }

        for (int trackIndex = 0; trackIndex < project.tracks.size(); ++trackIndex)
        {
            const InstrumentTrack& track = project.tracks[trackIndex];
            const int rowY = rulerHeight + trackIndex * rowHeight;
            QVector<const MidiClip*> orderedClips;
            orderedClips.reserve(track.clips.size());
            for (const MidiClip& clip : track.clips)
                orderedClips.push_back(&clip);
            std::stable_sort(orderedClips.begin(), orderedClips.end(), [](const MidiClip* left,
                                                                          const MidiClip* right)
            {
                const auto layer = [](const MidiClip& clip)
                {
                    if (clip.audioTakeGroupId == 0)
                        return 1;
                    return clip.audioTakeActive ? 2 : 0;
                };
                return layer(*left) < layer(*right);
            });
            for (const MidiClip* clipPointer : orderedClips)
            {
                const MidiClip& clip = *clipPointer;
                const QRectF bounds = clipBounds(clip, trackIndex);
                const bool selected = clip.id == selectedClipId && track.id == selectedTrackId;
                const bool inactiveTake = clip.audioTakeGroupId != 0 && !clip.audioTakeActive;
                QColor fill = trackColour(trackIndex);
                fill.setAlpha(inactiveTake ? 65 : selected ? 225 : 165);
                QPen outline(selected ? QColor(QStringLiteral("#d5fff7")) : fill.lighter(125), selected ? 2 : 1);
                if (inactiveTake)
                    outline.setStyle(Qt::DashLine);
                painter.setPen(outline);
                painter.setBrush(fill);
                painter.drawRoundedRect(bounds, 4.0, 4.0);

                if (selected && clip.isAudioRegion())
                {
                    painter.setPen(QPen(QColor(QStringLiteral("#d5fff7")), 2));
                    if (clip.audioSource != nullptr && clip.audioSource->frameCount() > clip.audioStartFrame)
                        painter.drawLine(QPointF(bounds.left() + 5, bounds.top() + 3),
                                         QPointF(bounds.left() + 5, bounds.bottom() - 3));
                    painter.drawLine(QPointF(bounds.right() - 5, bounds.top() + 3),
                                     QPointF(bounds.right() - 5, bounds.bottom() - 3));
                }

                painter.setPen(QColor(QStringLiteral("#102126")));
                painter.setFont(QFont(QStringLiteral("Sans Serif"), 8, QFont::DemiBold));
                QString clipLabel = clip.name;
                if (clip.audioTakeGroupId != 0)
                    clipLabel += clip.audioTakeActive ? tr(" · COMP") : tr(" · ALT");
                painter.drawText(bounds.adjusted(7, 3, -4, -3), Qt::AlignTop | Qt::AlignLeft,
                                 clip.isAudioRegion() ? clipLabel :
                                     QStringLiteral("%1\n%2 notes").arg(clipLabel).arg(clip.notes.size()));

                if (clip.isAudioRegion())
                {
                    if (clip.audioSource == nullptr || clip.audioSource->frameCount() <= clip.audioStartFrame)
                    {
                        painter.setPen(QColor(QStringLiteral("#3a2429")));
                        painter.drawText(bounds.adjusted(7, 25, -4, -3), Qt::AlignLeft | Qt::AlignVCenter,
                                         tr("OFFLINE AUDIO"));
                    }
                    else
                    {
                        const QRectF waveBounds = bounds.adjusted(6, 27, -6, -6);
                        const int waveLeft = static_cast<int>(waveBounds.left());
                        const int waveRight = static_cast<int>(waveBounds.right());
                        const qint64 sourceFrames = clip.audioSource->frameCount();
                        const double clipStartSeconds = project.secondsAtBeat(clip.startBeat);
                        const double beatsPerPixel = clip.lengthBeats / std::max(1.0, waveBounds.width());
                        const int centerY = static_cast<int>(waveBounds.center().y());
                        const int amplitude = std::max(2, static_cast<int>(waveBounds.height() * 0.48));
                        painter.setPen(QPen(QColor(QStringLiteral("#16323a")), 1));
                        for (int x = waveLeft; x <= waveRight; ++x)
                        {
                            const double firstBeat = clip.startBeat + (x - waveLeft) * beatsPerPixel;
                            const double nextBeat = clip.startBeat + (x - waveLeft + 1) * beatsPerPixel;
                            const qint64 firstFrame = clip.audioStartFrame + static_cast<qint64>(std::floor(
                                (project.secondsAtBeat(firstBeat) - clipStartSeconds) *
                                clip.audioSource->sampleRate));
                            const qint64 nextFrame = clip.audioStartFrame + static_cast<qint64>(std::ceil(
                                (project.secondsAtBeat(nextBeat) - clipStartSeconds) *
                                clip.audioSource->sampleRate));
                            const qint64 lastFrame = std::min(sourceFrames,
                                std::max(firstFrame + 1, nextFrame));
                            float peak = 0.0f;
                            if (firstFrame < lastFrame)
                            {
                                const qint64 firstPeak = std::max<qint64>(0, firstFrame) /
                                    AudioAsset::waveformBlockFrames;
                                const qint64 lastPeak = (lastFrame + AudioAsset::waveformBlockFrames - 1) /
                                    AudioAsset::waveformBlockFrames;
                                for (qint64 peakIndex = firstPeak;
                                     peakIndex < lastPeak && peakIndex < clip.audioSource->waveformPeaks.size();
                                     ++peakIndex)
                                {
                                    peak = std::max(peak, clip.audioSource->waveformPeaks[
                                        static_cast<qsizetype>(peakIndex)]);
                                }
                            }
                            const int height = static_cast<int>(std::round(std::min(1.0f, peak) * amplitude));
                            painter.drawLine(x, centerY - height, x, centerY + height);
                        }
                        if (clip.audioFadeInBeats > 0.0 || clip.audioFadeOutBeats > 0.0)
                        {
                            painter.setPen(QPen(QColor(QStringLiteral("#f0cf82")), 2));
                            if (clip.audioFadeInBeats > 0.0)
                            {
                                const double fadeWidth = std::min(waveBounds.width(),
                                    clip.audioFadeInBeats / clip.lengthBeats * waveBounds.width());
                                QPainterPath fadePath;
                                fadePath.moveTo(waveBounds.left(), waveBounds.bottom() - 1.0);
                                for (int step = 1; step <= 24; ++step)
                                {
                                    const double progress = static_cast<double>(step) / 24.0;
                                    const float gain = audioFadeEnvelope(clip.audioFadeInCurve, true, progress,
                                        clip.audioFadeInShape, &clip.audioFadeInCurvePoints);
                                    const double y = waveBounds.bottom() - 1.0 -
                                        gain * (waveBounds.height() - 2.0);
                                    fadePath.lineTo(waveBounds.left() + fadeWidth * progress, y);
                                }
                                painter.drawPath(fadePath);
                            }
                            if (clip.audioFadeOutBeats > 0.0)
                            {
                                const double fadeWidth = std::min(waveBounds.width(),
                                    clip.audioFadeOutBeats / clip.lengthBeats * waveBounds.width());
                                QPainterPath fadePath;
                                fadePath.moveTo(waveBounds.right() - fadeWidth, waveBounds.top() + 1);
                                for (int step = 1; step <= 24; ++step)
                                {
                                    const double progress = static_cast<double>(step) / 24.0;
                                    const float gain = audioFadeEnvelope(clip.audioFadeOutCurve, false, progress,
                                        clip.audioFadeOutShape, &clip.audioFadeOutCurvePoints);
                                    const double y = waveBounds.bottom() - 1.0 -
                                        gain * (waveBounds.height() - 2.0);
                                    fadePath.lineTo(waveBounds.right() - fadeWidth + fadeWidth * progress, y);
                                }
                                painter.drawPath(fadePath);
                            }
                        }
                        if (selected)
                            drawFadeBezierHandles(painter, clip, trackIndex);
                    }
                    continue;
                }

                painter.setPen(QPen(QColor(15, 26, 31, 90), 1));
                for (const MidiNote& note : clip.notes)
                {
                    const int noteX = trackHeaderWidth + static_cast<int>(std::round(
                        (clip.startBeat + note.startBeat) * pixelsPerBeat));
                    if (noteX >= bounds.left() && noteX <= bounds.right())
                    {
                        const int noteY = rowY + trackHeight - 11 - (note.pitch % 12) * 2;
                        painter.drawLine(noteX, noteY, noteX + std::max(2, static_cast<int>(note.lengthBeats * pixelsPerBeat)), noteY);
                    }
                }
            }
        }

        const QFont markerFont(QStringLiteral("Sans Serif"), 8, QFont::DemiBold);
        const QFontMetrics markerMetrics(markerFont);
        for (const TimelineMarker& marker : project.markers)
        {
            const int x = trackHeaderWidth + static_cast<int>(std::round(marker.beat * pixelsPerBeat));
            painter.setPen(QPen(QColor(QStringLiteral("#5bd5bd")), 2));
            painter.drawLine(x, rulerHeight, x, rulerHeight + trackCount * rowHeight);
            painter.setBrush(QColor(QStringLiteral("#5bd5bd")));
            painter.setPen(Qt::NoPen);
            painter.drawPolygon(QPolygon{QPoint(x - 4, 0), QPoint(x + 4, 0), QPoint(x, 7)});
            const int labelWidth = markerMetrics.horizontalAdvance(marker.name) + 12;
            const QRect labelRect(x + 5, 5, labelWidth, rulerHeight - 10);
            painter.setBrush(QColor(QStringLiteral("#28514c")));
            painter.setPen(QPen(QColor(QStringLiteral("#5bd5bd")), 1));
            painter.drawRoundedRect(labelRect, 3, 3);
            painter.setPen(QColor(QStringLiteral("#e0fff7")));
            painter.setFont(markerFont);
            painter.drawText(labelRect.adjusted(5, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, marker.name);
        }

        const QFont tempoFont(QStringLiteral("Sans Serif"), 8, QFont::DemiBold);
        const QFontMetrics tempoMetrics(tempoFont);
        for (const TempoChange& change : project.tempoChanges)
        {
            const int x = trackHeaderWidth + static_cast<int>(std::round(change.beat * pixelsPerBeat));
            const QString label = change.rampFromPrevious
                ? tr("Ramp to %1 BPM").arg(change.bpm, 0, 'f', 1)
                : tr("%1 BPM").arg(change.bpm, 0, 'f', 1);
            const int labelWidth = tempoMetrics.horizontalAdvance(label) + 10;
            const QRect labelRect(x + 5, 5, labelWidth, rulerHeight - 10);
            painter.setPen(QPen(QColor(QStringLiteral("#e4b966")), 1));
            painter.drawLine(x, rulerHeight, x, rulerHeight + trackCount * rowHeight);
            painter.setBrush(QColor(QStringLiteral("#514329")));
            painter.drawRoundedRect(labelRect, 3, 3);
            painter.setPen(QColor(QStringLiteral("#fff2d4")));
            painter.setFont(tempoFont);
            painter.drawText(labelRect.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
        }

        if (playheadBeat >= 0.0 && playheadBeat <= visibleBeats)
        {
            const int x = trackHeaderWidth + static_cast<int>(std::round(playheadBeat * pixelsPerBeat));
            painter.setPen(QPen(QColor(QStringLiteral("#f1a85c")), 2.0));
            painter.drawLine(x, rulerHeight, x, rulerHeight + trackCount * rowHeight);
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        setFocus(Qt::MouseFocusReason);
        const QPoint point = event->position().toPoint();
        if (point.y() >= 0 && point.y() < rulerHeight && point.x() >= trackHeaderWidth)
        {
            const double beat = std::clamp(
                (point.x() - trackHeaderWidth) / static_cast<double>(pixelsPerBeat), 0.0, 16384.0);
            const int markerIndex = markerAt(point);
            if (event->button() == Qt::LeftButton)
            {
                emit owner->markerSeekRequested(markerIndex >= 0 ? project.markers[markerIndex].beat : beat);
                return;
            }
            if (event->button() == Qt::RightButton)
            {
                QMenu menu(this);
                QAction* addAction = nullptr;
                QAction* renameAction = nullptr;
                QAction* removeAction = nullptr;
                double markerBeat = beat;
                if (markerIndex >= 0)
                {
                    markerBeat = project.markers[markerIndex].beat;
                    renameAction = menu.addAction(tr("Rename marker…"));
                    removeAction = menu.addAction(tr("Remove marker"));
                }
                else
                {
                    addAction = menu.addAction(tr("Add marker here…"));
                }
                menu.addSeparator();
                const int tempoIndex = tempoChangeAt(point);
                const double tempoBeat = tempoIndex >= 0 ? project.tempoChanges[tempoIndex].beat : beat;
                QAction* tempoAction = menu.addAction(tempoIndex >= 0
                    ? tr("Change tempo here…") : tr("Add tempo change here…"));
                QAction* rampAction = tempoIndex >= 0
                    ? menu.addAction(project.tempoChanges[tempoIndex].rampFromPrevious
                        ? tr("Use a step change") : tr("Ramp from previous tempo")) : nullptr;
                QAction* removeTempoAction = tempoIndex >= 0
                    ? menu.addAction(tr("Remove tempo change")) : nullptr;
                QAction* chosen = menu.exec(event->globalPosition().toPoint());
                if (chosen == nullptr)
                    return;
                if (chosen == addAction)
                    emit owner->markerAddRequested(beat);
                else if (chosen == renameAction)
                    emit owner->markerRenameRequested(markerBeat);
                else if (chosen == removeAction)
                    emit owner->markerRemoveRequested(markerBeat);
                else if (chosen == tempoAction)
                    emit owner->tempoChangeRequested(tempoBeat);
                else if (chosen == rampAction)
                    emit owner->tempoRampToggleRequested(tempoBeat);
                else if (chosen == removeTempoAction)
                    emit owner->tempoRemoveRequested(tempoBeat);
                return;
            }
        }
        const int row = (point.y() - rulerHeight) / currentRowHeight();
        if (automationViewEnabled && isBusAutomationTarget() &&
            row >= static_cast<int>(project.tracks.size()) && row < static_cast<int>(project.tracks.size()) + project.buses.size())
        {
            const int busIndex = row - static_cast<int>(project.tracks.size());
            MixerBus& bus = project.buses[busIndex];
            if (point.y() < rulerHeight + (row + 1) * currentRowHeight() - automationLaneHeight ||
                point.x() < trackHeaderWidth || !hasBusAutomationTarget(bus))
                return;
            selectedAutomationBusId = bus.id;
            selectedAutomationTrackId = 0;
            QVector<AutomationPoint>& curve = busAutomationPoints(bus);
            if (event->button() == Qt::RightButton)
            {
                const int hit = busAutomationPointAt(bus, row, point);
                if (hit >= 0)
                {
                    const QVector<AutomationPoint> before = curve;
                    curve.removeAt(hit);
                    emit owner->busAutomationChanged(bus.id, automationParameter, before, curve);
                    selectedAutomationPoint = -1;
                    update();
                }
                return;
            }
            if (event->button() != Qt::LeftButton)
                return;
            selectedAutomationPoint = busAutomationPointAt(bus, row, point);
            automationDragBusId = bus.id;
            automationDragTrackId = 0;
            automationDragBefore = curve;
            if (selectedAutomationPoint < 0)
            {
                if (curve.size() >= 10000)
                {
                    emit owner->automationPointLimitReached();
                    return;
                }
                const double beat = std::clamp((point.x() - trackHeaderWidth) / static_cast<double>(pixelsPerBeat), 0.0, 16384.0);
                const int laneY = rulerHeight + row * currentRowHeight() + trackHeight;
                const float value = automationValueAtY(laneY, point.y());
                auto insertAt = std::lower_bound(curve.begin(), curve.end(), beat,
                    [](const AutomationPoint& current, double targetBeat) { return current.beat < targetBeat; });
                if (insertAt != curve.end() && std::abs(insertAt->beat - beat) < 0.001)
                {
                    selectedAutomationPoint = static_cast<int>(insertAt - curve.begin());
                    insertAt->value = value;
                }
                else
                {
                    selectedAutomationPoint = static_cast<int>(insertAt - curve.begin());
                    curve.insert(insertAt, {beat, value, isDiscreteBusAutomationTarget()
                        ? AutomationPoint::Curve::Hold : automationDefaultCurve});
                }
            }
            draggingAutomationPoint = true;
            update();
            return;
        }
        if (row < 0 || row >= project.tracks.size())
            return;
        InstrumentTrack& track = project.tracks[row];
        const int rowY = rulerHeight + row * currentRowHeight();
        if (automationViewEnabled && point.y() >= rowY + trackHeight && point.x() >= trackHeaderWidth)
        {
            if (!hasAutomationTarget(track))
                return;
            automationNeedsTrackSelection = selectedTrackId != track.id;
            selectedTrackId = track.id;
            selectedAutomationTrackId = track.id;
            if (event->button() == Qt::RightButton)
            {
                const int hit = automationPointAt(track, row, point);
                if (hit >= 0)
                {
                    const quint64 trackId = track.id;
                    QVector<AutomationPoint> before = automationPoints(track);
                    QVector<AutomationPoint> after = before;
                    after.removeAt(hit);
                    if (automationNeedsTrackSelection)
                        emit owner->trackSelected(trackId);
                    emit owner->automationChanged(trackId, automationParameter, automationPluginUri,
                                                  before, after);
                    automationNeedsTrackSelection = false;
                    selectedAutomationPoint = -1;
                    update();
                }
                return;
            }
            if (event->button() != Qt::LeftButton)
                return;

            selectedAutomationPoint = automationPointAt(track, row, point);
            automationDragTrackId = track.id;
            automationDragBefore = automationPoints(track);
            if (selectedAutomationPoint < 0)
            {
                if (automationDragBefore.size() >= 10000)
                {
                    emit owner->automationPointLimitReached();
                    return;
                }
                QVector<AutomationPoint>& points = mutableAutomationPoints(track);
                const double beat = std::clamp(
                    (point.x() - trackHeaderWidth) / static_cast<double>(pixelsPerBeat), 0.0, 16384.0);
                const float value = automationValueAtY(rowY + trackHeight, point.y());
                auto insertAt = std::lower_bound(points.begin(), points.end(), beat,
                    [](const AutomationPoint& current, double targetBeat)
                    { return current.beat < targetBeat; });
                if (insertAt != points.end() && std::abs(insertAt->beat - beat) < 0.001)
                {
                    selectedAutomationPoint = static_cast<int>(insertAt - points.begin());
                    insertAt->value = value;
                }
                else if (insertAt != points.begin() && std::abs((insertAt - 1)->beat - beat) < 0.001)
                {
                    --insertAt;
                    selectedAutomationPoint = static_cast<int>(insertAt - points.begin());
                    insertAt->value = value;
                }
                else
                {
                    selectedAutomationPoint = static_cast<int>(insertAt - points.begin());
                    points.insert(insertAt, {beat, value, automationDefaultCurve});
                }
            }
            draggingAutomationPoint = true;
            update();
            return;
        }
        if (point.x() < trackHeaderWidth)
        {
            selectedTrackId = track.id;
            selectedClipId = 0;
            selectedAutomationTrackId = 0;
            selectedAutomationPoint = -1;
            emit owner->trackSelected(track.id);
            update();
            return;
        }

        const int hit = clipAt(point);
        if (event->button() == Qt::RightButton)
        {
            if (hit >= 0)
            {
                const MidiClip clip = track.clips[hit];
                if (clip.isAudioRegion())
                {
                    QMenu menu(this);
                    QAction* duplicateAction = menu.addAction(tr("Duplicate region"));
                    QAction* groupTakesAction = nullptr;
                    QAction* ungroupTakesAction = nullptr;
                    QAction* splitTakesAction = nullptr;
                    QMenu* useTakeMenu = nullptr;
                    QHash<QAction*, quint64> takeActions;
                    if (clip.audioTakeGroupId != 0)
                    {
                        ungroupTakesAction = menu.addAction(tr("Ungroup takes"));
                        splitTakesAction = menu.addAction(tr("Split all takes here"));
                        useTakeMenu = menu.addMenu(tr("Use take for this section"));
                        for (const MidiClip& candidate : track.clips)
                        {
                            if (!candidate.isAudioRegion() ||
                                candidate.audioTakeGroupId != clip.audioTakeGroupId ||
                                std::abs(candidate.startBeat - clip.startBeat) > 1.0e-6 ||
                                std::abs(candidate.lengthBeats - clip.lengthBeats) > 1.0e-6)
                                continue;
                            QAction* takeAction = useTakeMenu->addAction(
                                tr("%1 · take %2").arg(candidate.name).arg(candidate.id));
                            takeAction->setCheckable(true);
                            takeAction->setChecked(candidate.audioTakeActive);
                            takeActions.insert(takeAction, candidate.id);
                        }
                    }
                    else
                    {
                        int alignedRegionCount = 0;
                        for (const MidiClip& candidate : track.clips)
                        {
                            if (candidate.isAudioRegion() && candidate.audioTakeGroupId == 0 &&
                                std::abs(candidate.startBeat - clip.startBeat) <= 1.0e-6 &&
                                std::abs(candidate.lengthBeats - clip.lengthBeats) <= 1.0e-6)
                                ++alignedRegionCount;
                        }
                        if (alignedRegionCount >= 2)
                            groupTakesAction = menu.addAction(tr("Group aligned regions as takes"));
                    }
                    menu.addSeparator();
                    QAction* relinkAction = menu.addAction(tr("Relink WAV…"));
                    QAction* removeAction = menu.addAction(tr("Remove region"));
                    QAction* chosen = menu.exec(event->globalPosition().toPoint());
                    if (chosen == duplicateAction)
                        emit owner->clipDuplicateRequested(track.id, clip.id);
                    else if (groupTakesAction != nullptr && chosen == groupTakesAction)
                        emit owner->audioTakeGroupRequested(track.id, clip.id);
                    else if (ungroupTakesAction != nullptr && chosen == ungroupTakesAction)
                        emit owner->audioTakeUngroupRequested(track.id, clip.id);
                    else if (splitTakesAction != nullptr && chosen == splitTakesAction)
                    {
                        const double splitBeat = std::clamp(
                            (point.x() - trackHeaderWidth) / static_cast<double>(pixelsPerBeat),
                            clip.startBeat, clip.startBeat + clip.lengthBeats);
                        emit owner->audioTakeSplitRequested(track.id, clip.id, splitBeat);
                    }
                    else if (takeActions.contains(chosen))
                        emit owner->audioTakeSelected(track.id, takeActions.value(chosen));
                    else if (chosen == relinkAction)
                        emit owner->audioRegionRelinkRequested(track.id, clip.id);
                    else if (chosen == removeAction)
                        emit owner->clipRemoved(track.id, clip.id);
                }
                else
                {
                    QMenu menu(this);
                    QAction* duplicateAction = menu.addAction(tr("Duplicate pattern"));
                    QAction* patternChainAction = track.instrumentType == InstrumentType::DrumRack
                        ? menu.addAction(tr("Build pattern chain…")) : nullptr;
                    menu.addSeparator();
                    QAction* removeAction = menu.addAction(tr("Remove pattern"));
                    QAction* chosen = menu.exec(event->globalPosition().toPoint());
                    if (chosen == duplicateAction)
                        emit owner->clipDuplicateRequested(track.id, clip.id);
                    else if (patternChainAction != nullptr && chosen == patternChainAction)
                        emit owner->patternChainRequested(track.id, clip.id);
                    else if (chosen == removeAction)
                        emit owner->clipRemoved(track.id, clip.id);
                }
            }
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;

        selectedTrackId = track.id;
        if (hit >= 0)
        {
            selectedAutomationTrackId = 0;
            selectedAutomationPoint = -1;
            const MidiClip& clip = track.clips[hit];
            selectedClipId = clip.id;
            bool fadeIn = false;
            int handleIndex = -1;
            if (clip.isAudioRegion() && fadeBezierHandleAt(clip, row, point, fadeIn, handleIndex))
            {
                beginFadeBezierHandleDrag(track.id, clip, fadeIn, handleIndex);
                emit owner->clipSelected(track.id, clip.id);
                update();
                return;
            }
            if (event->modifiers().testFlag(Qt::ControlModifier) && clip.isAudioRegion() &&
                drawnFadeAt(clip, row, point, fadeIn))
            {
                beginFadeCurveDrawing(track.id, clip, fadeIn, point);
                emit owner->clipSelected(track.id, clip.id);
                update();
                return;
            }
            sourceTrackId = track.id;
            draggedClipId = clip.id;
            draggedTrackId = track.id;
            initialClipStart = clip.startBeat;
            initialClipLength = clip.lengthBeats;
            initialAudioStartFrame = clip.audioStartFrame;
            dragStartX = point.x();
            const QRectF bounds = clipBounds(clip, row);
            const bool nearLeftEdge = point.x() <= bounds.left() + 8;
            const bool nearRightEdge = point.x() >= bounds.right() - 8;
            resizingAudioRegion = clip.isAudioRegion() &&
                (nearRightEdge || (nearLeftEdge && clip.audioSource != nullptr &&
                                   clip.audioSource->frameCount() > clip.audioStartFrame));
            resizeLeftEdge = nearLeftEdge && !nearRightEdge;
            movingClip = !resizingAudioRegion;
            if (resizingAudioRegion)
                setCursor(Qt::SizeHorCursor);
            emit owner->clipSelected(track.id, clip.id);
            update();
            return;
        }

        selectedClipId = 0;
        const double beat = std::max(0.0, (point.x() - trackHeaderWidth) / static_cast<double>(pixelsPerBeat));
        const double barLength = project.quarterNotesPerBarAtBeat(beat);
        const double startBeat = std::floor(beat / barLength) * barLength;
        emit owner->clipAdded(track.id, startBeat);
        update();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        const QPoint point = event->position().toPoint();
        if (draggingFadeBezierHandle)
        {
            updateFadeBezierHandle(point);
            update();
            return;
        }
        if (drawingFadeCurve)
        {
            updateFadeCurveDrawing(point);
            update();
            return;
        }
        if (draggingAutomationPoint)
        {
            if (automationDragBusId != 0)
            {
                auto bus = std::find_if(project.buses.begin(), project.buses.end(),
                    [this](const MixerBus& candidate) { return candidate.id == automationDragBusId; });
                if (bus == project.buses.end()) return;
                auto& points = busAutomationPoints(*bus);
                if (selectedAutomationPoint < 0 || selectedAutomationPoint >= points.size()) return;
                const int index = static_cast<int>(bus - project.buses.begin());
                const int rowY = rulerHeight + (static_cast<int>(project.tracks.size()) + index) * currentRowHeight();
                double beat = std::clamp((point.x() - trackHeaderWidth) / static_cast<double>(pixelsPerBeat), 0.0, 16384.0);
                const double lo = selectedAutomationPoint > 0 ? points[selectedAutomationPoint - 1].beat + 0.001 : 0.0;
                const double hi = selectedAutomationPoint + 1 < points.size() ? points[selectedAutomationPoint + 1].beat - 0.001 : 16384.0;
                beat = lo <= hi ? std::clamp(beat, lo, hi) : points[selectedAutomationPoint].beat;
                points[selectedAutomationPoint].beat = beat;
                points[selectedAutomationPoint].value = automationValueAtY(rowY + trackHeight, point.y());
                update();
                return;
            }
            const int trackIndex = trackIndexForId(automationDragTrackId);
            InstrumentTrack* track = project.findTrack(automationDragTrackId);
            if (track == nullptr || trackIndex < 0)
                return;
            QVector<AutomationPoint>& points = mutableAutomationPoints(*track);
            if (selectedAutomationPoint < 0 || selectedAutomationPoint >= points.size())
                return;
            const int rowY = rulerHeight + trackIndex * currentRowHeight();
            double beat = std::clamp((point.x() - trackHeaderWidth) /
                                     static_cast<double>(pixelsPerBeat), 0.0, 16384.0);
            double minimumBeat = 0.0;
            double maximumBeat = 16384.0;
            if (selectedAutomationPoint > 0)
                minimumBeat = points[selectedAutomationPoint - 1].beat + 0.001;
            if (selectedAutomationPoint + 1 < points.size())
                maximumBeat = points[selectedAutomationPoint + 1].beat - 0.001;
            if (minimumBeat > maximumBeat)
                beat = points[selectedAutomationPoint].beat;
            else
                beat = std::clamp(beat, minimumBeat, maximumBeat);
            AutomationPoint& selected = points[selectedAutomationPoint];
            selected.beat = std::clamp(beat, 0.0, 16384.0);
            selected.value = automationValueAtY(rowY + trackHeight, point.y());
            update();
            return;
        }
        if (!movingClip && !resizingAudioRegion)
        {
            if (automationViewEnabled)
            {
                const int row = (point.y() - rulerHeight) / currentRowHeight();
                if (row >= 0 && row < project.tracks.size() && point.x() >= trackHeaderWidth &&
                    point.y() >= rulerHeight + row * currentRowHeight() + trackHeight)
                {
                    setCursor(automationPointAt(project.tracks[row], row, point) >= 0
                                  ? Qt::SizeAllCursor : Qt::CrossCursor);
                    return;
                }
            }
            const int hit = clipAt(point);
            if (hit >= 0)
            {
                const int row = (point.y() - rulerHeight) / currentRowHeight();
                const MidiClip& clip = project.tracks[row].clips[hit];
                bool fadeIn = false;
                int handleIndex = -1;
                if (clip.isAudioRegion() && fadeBezierHandleAt(clip, row, point, fadeIn, handleIndex))
                {
                    setCursor(Qt::SizeAllCursor);
                    return;
                }
                if (clip.isAudioRegion() && drawnFadeAt(clip, row, point, fadeIn))
                {
                    setCursor(Qt::CrossCursor);
                    return;
                }
                const QRectF bounds = clipBounds(clip, row);
                const bool nearLeftEdge = point.x() <= bounds.left() + 8;
                const bool nearRightEdge = point.x() >= bounds.right() - 8;
                if (clip.isAudioRegion() && (nearRightEdge ||
                    (nearLeftEdge && clip.audioSource != nullptr &&
                     clip.audioSource->frameCount() > clip.audioStartFrame)))
                {
                    setCursor(Qt::SizeHorCursor);
                    return;
                }
            }
            unsetCursor();
            return;
        }

        if (resizingAudioRegion)
        {
            const double rawDelta = (point.x() - dragStartX) / static_cast<double>(pixelsPerBeat);
            const double snap = std::max(0.25,
                project.quarterNotesPerBarAtBeat(initialClipStart) / 4.0);
            const double snappedDelta = std::round(rawDelta / snap) * snap;
            const MidiClip* originalClip = project.findClip(sourceTrackId, draggedClipId);
            if (originalClip == nullptr)
                return;

            double newStartBeat = initialClipStart;
            double newLengthBeats = initialClipLength;
            qint64 newAudioStartFrame = initialAudioStartFrame;
            if (resizeLeftEdge)
            {
                double minimumDelta = -initialClipStart;
                double maximumDelta = initialClipLength - 0.25;
                if (originalClip->audioSource != nullptr && originalClip->audioSource->sampleRate > 0)
                {
                    const double initialStartSeconds = project.secondsAtBeat(initialClipStart);
                    const double minimumStartBeat = project.beatAtSeconds(initialStartSeconds -
                        static_cast<double>(initialAudioStartFrame) /
                        originalClip->audioSource->sampleRate);
                    minimumDelta = std::max(minimumDelta,
                        minimumStartBeat - initialClipStart);
                    const qint64 maximumStartFrame = originalClip->audioSource->frameCount() - 1;
                    const double maximumStartBeat = project.beatAtSeconds(initialStartSeconds +
                        static_cast<double>(maximumStartFrame - initialAudioStartFrame) /
                        originalClip->audioSource->sampleRate);
                    maximumDelta = std::min(maximumDelta,
                        maximumStartBeat - initialClipStart);
                    maximumDelta = std::min(maximumDelta, 16384.0 - initialClipStart);
                    const double delta = std::clamp(snappedDelta, minimumDelta, maximumDelta);
                    newStartBeat = initialClipStart + delta;
                    newLengthBeats = initialClipLength - delta;
                    newAudioStartFrame = std::max<qint64>(0, initialAudioStartFrame +
                        static_cast<qint64>(std::llround((project.secondsAtBeat(newStartBeat) -
                            initialStartSeconds) * originalClip->audioSource->sampleRate)));
                }
            }
            else
            {
                double maximumLength = 16384.0 - initialClipStart;
                if (originalClip->audioSource != nullptr && originalClip->audioSource->sampleRate > 0)
                {
                    const double remainingSeconds = static_cast<double>(
                        originalClip->audioSource->frameCount() - initialAudioStartFrame) /
                        originalClip->audioSource->sampleRate;
                    maximumLength = std::min(maximumLength,
                        project.beatAtSeconds(project.secondsAtBeat(initialClipStart) + remainingSeconds) -
                        initialClipStart);
                }
                maximumLength = std::max(0.25, maximumLength);
                newLengthBeats = std::clamp(initialClipLength + snappedDelta, 0.25, maximumLength);
            }

            setLocalAudioTrim(sourceTrackId, draggedClipId, newStartBeat,
                              newLengthBeats, newAudioStartFrame);
            updateCanvasSize();
            return;
        }

        const double delta = (point.x() - dragStartX) / static_cast<double>(pixelsPerBeat);
            const double snap = std::max(0.25,
                project.quarterNotesPerBarAtBeat(initialClipStart) / 4.0);
        const double startBeat = std::max(0.0, std::round((initialClipStart + delta) / snap) * snap);
        const int row = std::clamp((point.y() - rulerHeight) / currentRowHeight(), 0,
                                   static_cast<int>(project.tracks.size()) - 1);
        const quint64 targetTrackId = project.tracks[row].id;
        setLocalClipPosition(draggedTrackId, targetTrackId, draggedClipId, startBeat);
        draggedTrackId = targetTrackId;
        selectedTrackId = targetTrackId;
        selectedClipId = draggedClipId;
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton)
            return;
        if (draggingFadeBezierHandle)
        {
            draggingFadeBezierHandle = false;
            unsetCursor();
            const MidiClip* clip = project.findClip(fadeBezierTrackId, fadeBezierClipId);
            const QVector<QPointF> after = clip == nullptr ? fadeBezierBefore :
                (fadeBezierIn ? clip->audioFadeInBezierHandles : clip->audioFadeOutBezierHandles);
            if (clip != nullptr && fadeBezierBefore != after)
                emit owner->audioFadeBezierHandlesChanged(fadeBezierTrackId, fadeBezierClipId,
                    fadeBezierIn, fadeBezierBefore, after);
            fadeBezierBefore.clear();
            update();
            return;
        }
        if (drawingFadeCurve)
        {
            drawingFadeCurve = false;
            unsetCursor();
            const MidiClip* clip = project.findClip(drawnFadeTrackId, drawnFadeClipId);
            const QVector<float> after = clip == nullptr ? drawnFadeBefore :
                (drawnFadeIn ? clip->audioFadeInCurvePoints : clip->audioFadeOutCurvePoints);
            if (clip != nullptr && drawnFadeBefore != after)
                emit owner->audioFadeCurveDrawn(drawnFadeTrackId, drawnFadeClipId, drawnFadeIn,
                                                drawnFadeBefore, after);
            drawnFadeBefore.clear();
            update();
            return;
        }
        if (draggingAutomationPoint)
        {
            draggingAutomationPoint = false;
            if (automationDragBusId != 0)
            {
                const quint64 busId = automationDragBusId;
                automationDragBusId = 0;
                const auto bus = std::find_if(project.buses.cbegin(), project.buses.cend(),
                    [busId](const MixerBus& candidate) { return candidate.id == busId; });
                const QVector<AutomationPoint> after = bus == project.buses.cend()
                    ? automationDragBefore : busAutomationPoints(*bus);
                if (!automationPointsEqual(automationDragBefore, after))
                    emit owner->busAutomationChanged(busId, automationParameter, automationDragBefore, after);
                automationDragBefore.clear();
                update();
                return;
            }
            const quint64 trackId = automationDragTrackId;
            const InstrumentTrack* track = project.findTrack(trackId);
            const QVector<AutomationPoint> after = track == nullptr
                ? automationDragBefore : automationPoints(*track);
            if (automationNeedsTrackSelection)
                emit owner->trackSelected(trackId);
            if (!automationPointsEqual(automationDragBefore, after))
                emit owner->automationChanged(trackId, automationParameter, automationPluginUri,
                                              automationDragBefore, after);
            automationNeedsTrackSelection = false;
            automationDragBefore.clear();
            update();
            return;
        }
        if (resizingAudioRegion)
        {
            resizingAudioRegion = false;
            unsetCursor();
            const MidiClip* clip = project.findClip(sourceTrackId, draggedClipId);
            if (clip != nullptr && (std::abs(clip->startBeat - initialClipStart) > 1.0e-9 ||
                                    std::abs(clip->lengthBeats - initialClipLength) > 1.0e-9 ||
                                    clip->audioStartFrame != initialAudioStartFrame))
                emit owner->audioRegionTrimmed(sourceTrackId, draggedClipId, clip->startBeat,
                                               clip->lengthBeats, clip->audioStartFrame);
            return;
        }
        if (!movingClip)
            return;
        movingClip = false;
        const MidiClip* clip = project.findClip(draggedTrackId, draggedClipId);
        if (clip != nullptr && (draggedTrackId != sourceTrackId ||
                                std::abs(clip->startBeat - initialClipStart) > 1.0e-9))
            emit owner->clipMoved(sourceTrackId, draggedTrackId, draggedClipId, clip->startBeat);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (automationViewEnabled)
            return;
        const QPoint point = event->position().toPoint();
        const int row = (point.y() - rulerHeight) / currentRowHeight();
        if (row < 0 || row >= project.tracks.size())
            return;
        const int hit = clipAt(point);
        if (hit >= 0)
        {
            const InstrumentTrack& track = project.tracks[row];
            emit owner->clipActivated(track.id, track.clips[hit].id);
        }
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
        {
            if (automationViewEnabled && selectedAutomationPoint >= 0)
            {
                if (selectedAutomationBusId != 0)
                {
                    auto bus = std::find_if(project.buses.begin(), project.buses.end(),
                        [this](const MixerBus& candidate) { return candidate.id == selectedAutomationBusId; });
                    if (bus != project.buses.end() && selectedAutomationPoint < busAutomationPoints(*bus).size())
                    {
                        const QVector<AutomationPoint> before = busAutomationPoints(*bus);
                        auto after = before;
                        after.removeAt(selectedAutomationPoint);
                        emit owner->busAutomationChanged(bus->id, automationParameter, before, after);
                        selectedAutomationPoint = -1;
                    }
                    update();
                    return;
                }
                const InstrumentTrack* track = project.findTrack(selectedAutomationTrackId);
                if (track != nullptr && selectedAutomationPoint < automationPoints(*track).size())
                {
                    const quint64 trackId = track->id;
                    const QVector<AutomationPoint> before = automationPoints(*track);
                    QVector<AutomationPoint> after = before;
                    after.removeAt(selectedAutomationPoint);
                    emit owner->automationChanged(trackId, automationParameter, automationPluginUri,
                                                  before, after);
                    selectedAutomationPoint = -1;
                    update();
                }
                return;
            }
            if (automationViewEnabled)
                return;
            if (selectedTrackId != 0 && selectedClipId != 0)
                emit owner->clipRemoved(selectedTrackId, selectedClipId);
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    int markerAt(const QPoint& position) const
    {
        const QFont font(QStringLiteral("Sans Serif"), 8, QFont::DemiBold);
        const QFontMetrics metrics(font);
        for (int index = project.markers.size() - 1; index >= 0; --index)
        {
            const TimelineMarker& marker = project.markers[index];
            const int x = trackHeaderWidth + static_cast<int>(std::round(marker.beat * pixelsPerBeat));
            const QRect bounds(x - 3, 0, metrics.horizontalAdvance(marker.name) + 16, rulerHeight);
            if (bounds.contains(position))
                return index;
        }
        return -1;
    }

    int tempoChangeAt(const QPoint& position) const
    {
        const QFont font(QStringLiteral("Sans Serif"), 8, QFont::DemiBold);
        const QFontMetrics metrics(font);
        for (int index = project.tempoChanges.size() - 1; index >= 0; --index)
        {
            const TempoChange& change = project.tempoChanges[index];
            const int x = trackHeaderWidth + static_cast<int>(std::round(change.beat * pixelsPerBeat));
            const QString label = tr("%1 BPM").arg(change.bpm, 0, 'f', 1);
            const QRect bounds(x + 5, 5, metrics.horizontalAdvance(label) + 10, rulerHeight - 10);
            if (bounds.contains(position))
                return index;
        }
        return -1;
    }

    int currentRowHeight() const
    {
        return trackHeight + (automationViewEnabled ? automationLaneHeight : 0);
    }

    bool isBusAutomationTarget() const
    {
        return automationParameter == mixerBusGainAutomationTarget ||
            automationParameter == mixerBusPanAutomationTarget ||
            automationParameter == mixerBusMuteAutomationTarget ||
            automationParameter == mixerBusSoloAutomationTarget ||
            isBusSendAutomationTarget();
    }

    bool isBusSendAutomationTarget() const
    {
        const int index = automationParameter - mixerSendLevelAutomationTargetBase;
        return index >= 0 && index < 64;
    }

    bool isDiscreteBusAutomationTarget() const
    {
        return automationParameter == mixerBusMuteAutomationTarget ||
            automationParameter == mixerBusSoloAutomationTarget;
    }

    QVector<AutomationPoint>& busAutomationPoints(MixerBus& bus) const
    {
        if (automationParameter == mixerBusPanAutomationTarget) return bus.panAutomation;
        if (automationParameter == mixerBusMuteAutomationTarget) return bus.muteAutomation;
        if (automationParameter == mixerBusSoloAutomationTarget) return bus.soloAutomation;
        if (isBusSendAutomationTarget())
        {
            const int index = automationParameter - mixerSendLevelAutomationTargetBase;
            if (index < bus.sends.size()) return bus.sends[index].automation;
            static QVector<AutomationPoint> invalid;
            return invalid;
        }
        return bus.gainAutomation;
    }

    const QVector<AutomationPoint>& busAutomationPoints(const MixerBus& bus) const
    {
        if (automationParameter == mixerBusPanAutomationTarget) return bus.panAutomation;
        if (automationParameter == mixerBusMuteAutomationTarget) return bus.muteAutomation;
        if (automationParameter == mixerBusSoloAutomationTarget) return bus.soloAutomation;
        if (isBusSendAutomationTarget())
        {
            const int index = automationParameter - mixerSendLevelAutomationTargetBase;
            if (index < bus.sends.size()) return bus.sends[index].automation;
            static const QVector<AutomationPoint> empty;
            return empty;
        }
        return bus.gainAutomation;
    }

    bool hasBusAutomationTarget(const MixerBus& bus) const
    {
        return !isBusSendAutomationTarget() ||
            automationParameter - mixerSendLevelAutomationTargetBase < bus.sends.size();
    }

    int busAutomationPointAt(const MixerBus& bus, int row, const QPoint& position) const
    {
        const int laneY = rulerHeight + row * currentRowHeight() + trackHeight;
        const auto& points = busAutomationPoints(bus);
        for (qsizetype index = 0; index < points.size(); ++index)
        {
            const QPoint center(trackHeaderWidth + static_cast<int>(std::round(points[index].beat * pixelsPerBeat)),
                                automationY(laneY, points[index].value));
            if ((center - position).manhattanLength() <= 12)
                return static_cast<int>(index);
        }
        return -1;
    }

    const QVector<AutomationPoint>& automationPoints(const InstrumentTrack& track) const
    {
        if (automationParameter == 0)
            return track.volumeAutomation;
        if (automationParameter == 1)
            return track.panAutomation;
        const int sendIndex = automationParameter - mixerSendLevelAutomationTargetBase;
        if (sendIndex >= 0 && sendIndex < track.sends.size())
            return track.sends[sendIndex].automation;
        if (automationParameter == trackEqLowGainAutomationTarget)
            return track.eqLowGainAutomation;
        if (automationParameter == trackEqMidGainAutomationTarget)
            return track.eqMidGainAutomation;
        if (automationParameter == trackEqHighGainAutomationTarget)
            return track.eqHighGainAutomation;
        if (automationParameter == trackEqLowCrossoverAutomationTarget)
            return track.eqLowCrossoverAutomation;
        if (automationParameter == trackEqHighCrossoverAutomationTarget)
            return track.eqHighCrossoverAutomation;
        const int parametricEncoded = automationParameter -
            trackEqParametricAutomationTargetBase;
        if (parametricEncoded >= 0 &&
            parametricEncoded < parametricEqBandCount * 3)
            return track.eqParametricAutomation[static_cast<size_t>(parametricEncoded / 3)]
                                                   [static_cast<size_t>(parametricEncoded % 3)];
        static const QVector<AutomationPoint> empty;
        if (automationParameter == synthFilterCutoffAutomationTarget ||
            automationParameter == synthFilterResonanceAutomationTarget)
        {
            if (track.instrumentType != InstrumentType::Synth)
                return empty;
            return automationParameter == synthFilterCutoffAutomationTarget
                ? track.synthFilterCutoffAutomation : track.synthFilterResonanceAutomation;
        }
        qsizetype instrumentParameterIndex = 0;
        if (decodeClapInstrumentAutomationTarget(automationParameter, instrumentParameterIndex))
        {
            if (track.instrumentType != InstrumentType::ClapInstrument ||
                automationPluginUri.isEmpty() || track.clapPluginId != automationPluginUri ||
                instrumentParameterIndex >= track.clapParameterAutomation.size())
                return empty;
            return track.clapParameterAutomation[instrumentParameterIndex];
        }
        if (decodeVst3InstrumentAutomationTarget(automationParameter, instrumentParameterIndex))
        {
            if (track.instrumentType != InstrumentType::Vst3Instrument ||
                automationPluginUri.isEmpty() || track.vst3PluginId != automationPluginUri ||
                instrumentParameterIndex >= track.vst3ParameterAutomation.size())
                return empty;
            return track.vst3ParameterAutomation[instrumentParameterIndex];
        }
        const int encoded = automationParameter - pluginAutomationTargetBase;
        if (encoded < 0 || encoded >= pluginParametersPerEffect * maximumPluginEffectCount ||
            automationPluginUri.isEmpty())
            return empty;
        const qsizetype effectIndex = encoded / pluginParametersPerEffect;
        const qsizetype parameterIndex = encoded % pluginParametersPerEffect;
        if (effectIndex >= track.effects.size())
            return empty;
        const MixerEffect& effect = track.effects[effectIndex];
        if (!isPluginEffect(effect.type) || effect.pluginUri != automationPluginUri ||
            parameterIndex >= effect.pluginParameterAutomation.size())
            return empty;
        return effect.pluginParameterAutomation[parameterIndex];
    }

    float baseAutomationValue(const InstrumentTrack& track) const
    {
        if (automationParameter == 0)
            return track.gain;
        if (automationParameter == 1)
            return track.pan;
        const int sendIndex = automationParameter - mixerSendLevelAutomationTargetBase;
        if (sendIndex >= 0 && sendIndex < track.sends.size())
            return track.sends[sendIndex].gain;
        if (automationParameter == trackEqLowGainAutomationTarget)
            return track.eqLowDb;
        if (automationParameter == trackEqMidGainAutomationTarget)
            return track.eqMidDb;
        if (automationParameter == trackEqHighGainAutomationTarget)
            return track.eqHighDb;
        if (automationParameter == trackEqLowCrossoverAutomationTarget)
            return track.eqLowCrossoverHz;
        if (automationParameter == trackEqHighCrossoverAutomationTarget)
            return track.eqHighCrossoverHz;
        const int parametricEncoded = automationParameter -
            trackEqParametricAutomationTargetBase;
        if (parametricEncoded >= 0 &&
            parametricEncoded < parametricEqBandCount * 3)
        {
            const ParametricEqBand& band = track.eqParametricBands[
                static_cast<size_t>(parametricEncoded / 3)];
            switch (parametricEncoded % 3)
            {
                case 0: return band.frequencyHz;
                case 1: return band.gainDb;
                default: return band.q;
            }
        }
        if (automationParameter == synthFilterCutoffAutomationTarget ||
            automationParameter == synthFilterResonanceAutomationTarget)
        {
            if (track.instrumentType != InstrumentType::Synth)
                return automationDefaultValue;
            return automationParameter == synthFilterCutoffAutomationTarget
                ? track.synthFilterCutoffHz : track.synthFilterResonance;
        }
        qsizetype instrumentParameterIndex = 0;
        if (decodeClapInstrumentAutomationTarget(automationParameter, instrumentParameterIndex))
        {
            if (track.instrumentType != InstrumentType::ClapInstrument ||
                automationPluginUri.isEmpty() || track.clapPluginId != automationPluginUri)
                return automationDefaultValue;
            return instrumentParameterIndex < track.clapParameterValues.size()
                ? track.clapParameterValues[instrumentParameterIndex] : automationDefaultValue;
        }
        if (decodeVst3InstrumentAutomationTarget(automationParameter, instrumentParameterIndex))
        {
            if (track.instrumentType != InstrumentType::Vst3Instrument ||
                automationPluginUri.isEmpty() || track.vst3PluginId != automationPluginUri)
                return automationDefaultValue;
            return instrumentParameterIndex < track.vst3ParameterValues.size()
                ? track.vst3ParameterValues[instrumentParameterIndex] : automationDefaultValue;
        }
        const int encoded = automationParameter - pluginAutomationTargetBase;
        if (encoded < 0 || encoded >= pluginParametersPerEffect * maximumPluginEffectCount ||
            automationPluginUri.isEmpty())
            return automationDefaultValue;
        const qsizetype effectIndex = encoded / pluginParametersPerEffect;
        const qsizetype parameterIndex = encoded % pluginParametersPerEffect;
        if (effectIndex >= track.effects.size())
            return automationDefaultValue;
        const MixerEffect& effect = track.effects[effectIndex];
        if (!isPluginEffect(effect.type) || effect.pluginUri != automationPluginUri)
            return automationDefaultValue;
        return parameterIndex < effect.pluginParameterValues.size()
            ? effect.pluginParameterValues[parameterIndex] : automationDefaultValue;
    }

    QVector<AutomationPoint>& mutableAutomationPoints(InstrumentTrack& track)
    {
        if (automationParameter == 0)
            return track.volumeAutomation;
        if (automationParameter == 1)
            return track.panAutomation;
        const int sendIndex = automationParameter - mixerSendLevelAutomationTargetBase;
        if (sendIndex >= 0 && sendIndex < track.sends.size())
            return track.sends[sendIndex].automation;
        if (automationParameter == trackEqLowGainAutomationTarget)
            return track.eqLowGainAutomation;
        if (automationParameter == trackEqMidGainAutomationTarget)
            return track.eqMidGainAutomation;
        if (automationParameter == trackEqHighGainAutomationTarget)
            return track.eqHighGainAutomation;
        if (automationParameter == trackEqLowCrossoverAutomationTarget)
            return track.eqLowCrossoverAutomation;
        if (automationParameter == trackEqHighCrossoverAutomationTarget)
            return track.eqHighCrossoverAutomation;
        const int parametricEncoded = automationParameter -
            trackEqParametricAutomationTargetBase;
        if (parametricEncoded >= 0 &&
            parametricEncoded < parametricEqBandCount * 3)
            return track.eqParametricAutomation[static_cast<size_t>(parametricEncoded / 3)]
                                                   [static_cast<size_t>(parametricEncoded % 3)];
        static QVector<AutomationPoint> invalid;
        if (automationParameter == synthFilterCutoffAutomationTarget ||
            automationParameter == synthFilterResonanceAutomationTarget)
        {
            if (track.instrumentType != InstrumentType::Synth)
                return invalid;
            return automationParameter == synthFilterCutoffAutomationTarget
                ? track.synthFilterCutoffAutomation : track.synthFilterResonanceAutomation;
        }
        qsizetype instrumentParameterIndex = 0;
        if (decodeClapInstrumentAutomationTarget(automationParameter, instrumentParameterIndex))
        {
            if (track.instrumentType != InstrumentType::ClapInstrument ||
                automationPluginUri.isEmpty() || track.clapPluginId != automationPluginUri)
                return invalid;
            while (track.clapParameterAutomation.size() <= instrumentParameterIndex)
                track.clapParameterAutomation.push_back({});
            return track.clapParameterAutomation[instrumentParameterIndex];
        }
        if (decodeVst3InstrumentAutomationTarget(automationParameter, instrumentParameterIndex))
        {
            if (track.instrumentType != InstrumentType::Vst3Instrument ||
                automationPluginUri.isEmpty() || track.vst3PluginId != automationPluginUri)
                return invalid;
            while (track.vst3ParameterAutomation.size() <= instrumentParameterIndex)
                track.vst3ParameterAutomation.push_back({});
            return track.vst3ParameterAutomation[instrumentParameterIndex];
        }
        const int encoded = automationParameter - pluginAutomationTargetBase;
        if (encoded < 0 || encoded >= pluginParametersPerEffect * maximumPluginEffectCount ||
            automationPluginUri.isEmpty())
            return invalid;
        const qsizetype effectIndex = encoded / pluginParametersPerEffect;
        const qsizetype parameterIndex = encoded % pluginParametersPerEffect;
        if (effectIndex >= track.effects.size())
            return invalid;
        MixerEffect& effect = track.effects[effectIndex];
        if (!isPluginEffect(effect.type) || effect.pluginUri != automationPluginUri)
            return invalid;
        while (effect.pluginParameterAutomation.size() <= parameterIndex)
            effect.pluginParameterAutomation.push_back({});
        return effect.pluginParameterAutomation[parameterIndex];
    }

    bool hasAutomationTarget(const InstrumentTrack& track) const
    {
        if (automationParameter == 0 || automationParameter == 1)
            return true;
        const int sendIndex = automationParameter - mixerSendLevelAutomationTargetBase;
        if (sendIndex >= 0 && sendIndex < 64)
            return sendIndex < track.sends.size();
        if (automationParameter == trackEqLowGainAutomationTarget ||
            automationParameter == trackEqMidGainAutomationTarget ||
            automationParameter == trackEqHighGainAutomationTarget ||
            automationParameter == trackEqLowCrossoverAutomationTarget ||
            automationParameter == trackEqHighCrossoverAutomationTarget ||
            (automationParameter >= trackEqParametricAutomationTargetBase &&
             automationParameter < trackEqParametricAutomationTargetBase +
                 parametricEqBandCount * 3))
            return true;
        if (automationParameter == synthFilterCutoffAutomationTarget ||
            automationParameter == synthFilterResonanceAutomationTarget)
            return track.instrumentType == InstrumentType::Synth;
        qsizetype instrumentParameterIndex = 0;
        if (decodeClapInstrumentAutomationTarget(automationParameter, instrumentParameterIndex))
            return track.instrumentType == InstrumentType::ClapInstrument &&
                !automationPluginUri.isEmpty() && track.clapPluginId == automationPluginUri;
        if (decodeVst3InstrumentAutomationTarget(automationParameter, instrumentParameterIndex))
            return track.instrumentType == InstrumentType::Vst3Instrument &&
                !automationPluginUri.isEmpty() && track.vst3PluginId == automationPluginUri;
        const int encoded = automationParameter - pluginAutomationTargetBase;
        if (encoded < 0 || encoded >= pluginParametersPerEffect * maximumPluginEffectCount ||
            automationPluginUri.isEmpty())
            return false;
        const qsizetype effectIndex = encoded / pluginParametersPerEffect;
        if (effectIndex >= track.effects.size())
            return false;
        const MixerEffect& effect = track.effects[effectIndex];
        return isPluginEffect(effect.type) && effect.pluginUri == automationPluginUri;
    }

    int trackIndexForId(quint64 trackId) const
    {
        for (qsizetype index = 0; index < project.tracks.size(); ++index)
            if (project.tracks[index].id == trackId)
                return static_cast<int>(index);
        return -1;
    }

    int automationPointAt(const InstrumentTrack& track, int trackIndex, const QPoint& position) const
    {
        const int laneY = rulerHeight + trackIndex * currentRowHeight() + trackHeight;
        const QVector<AutomationPoint>& points = automationPoints(track);
        for (qsizetype index = 0; index < points.size(); ++index)
        {
            const QPoint center(trackHeaderWidth + static_cast<int>(std::round(
                                    points[index].beat * pixelsPerBeat)),
                                automationY(laneY, points[index].value));
            if ((center - position).manhattanLength() <= 12)
                return static_cast<int>(index);
        }
        return -1;
    }

    double normalizedAutomationValue(float value) const
    {
        return std::clamp((static_cast<double>(value) - automationMinimum) /
                          (automationMaximum - automationMinimum), 0.0, 1.0);
    }

    int automationY(int laneY, float value) const
    {
        const double normalized = normalizedAutomationValue(value);
        const int inset = 7;
        return laneY + automationLaneHeight - inset - static_cast<int>(std::round(
            normalized * (automationLaneHeight - inset * 2)));
    }

    float automationValueAtY(int laneY, int y) const
    {
        const double normalized = std::clamp(
            static_cast<double>(laneY + automationLaneHeight - 7 - y) /
                static_cast<double>(automationLaneHeight - 14), 0.0, 1.0);
        return static_cast<float>(automationMinimum + normalized *
                                  (automationMaximum - automationMinimum));
    }

    void drawAutomationLane(QPainter& painter, const InstrumentTrack& track, int trackIndex,
                            int rowY, int gridWidth) const
    {
        const int laneY = rowY + trackHeight;
        const QRect laneRect(trackHeaderWidth, laneY, gridWidth, automationLaneHeight - 1);
        painter.fillRect(laneRect, QColor(QStringLiteral("#141a21")));
        const int top = laneY + 7;
        const int bottom = laneY + automationLaneHeight - 7;
        painter.setPen(QPen(QColor(QStringLiteral("#333d48")), 1));
        painter.drawLine(trackHeaderWidth, top, trackHeaderWidth + gridWidth, top);
        painter.drawLine(trackHeaderWidth, bottom, trackHeaderWidth + gridWidth, bottom);
        if (!hasAutomationTarget(track))
        {
            painter.setPen(QColor(QStringLiteral("#78838f")));
            painter.drawText(laneRect, Qt::AlignCenter, tr("No matching automation target on this track"));
            return;
        }
        if (automationMinimum < 0.0f && automationMaximum > 0.0f)
        {
            const int zeroY = automationY(laneY, 0.0f);
            painter.setPen(QPen(QColor(QStringLiteral("#29323c")), 1, Qt::DashLine));
            painter.drawLine(trackHeaderWidth, zeroY, trackHeaderWidth + gridWidth, zeroY);
        }

        const QVector<AutomationPoint>& points = automationPoints(track);
        const int right = trackHeaderWidth + gridWidth;
        QPainterPath curve;
        const float baseValue = baseAutomationValue(track);
        if (points.isEmpty())
        {
            curve.moveTo(trackHeaderWidth, automationY(laneY, baseValue));
            curve.lineTo(right, automationY(laneY, baseValue));
        }
        else
        {
            const AutomationPoint& first = points.first();
            const double firstX = trackHeaderWidth + first.beat * pixelsPerBeat;
            curve.moveTo(trackHeaderWidth, automationY(laneY, baseValue));
            if (first.beat > 0.0)
                curve.lineTo(firstX, automationY(laneY, baseValue));
            curve.lineTo(firstX, automationY(laneY, first.value));
            for (qsizetype index = 0; index + 1 < points.size(); ++index)
            {
                const AutomationPoint& from = points[index];
                const AutomationPoint& to = points[index + 1];
                const double endX = trackHeaderWidth + to.beat * pixelsPerBeat;
                if (from.curveToNext == AutomationPoint::Curve::Linear)
                {
                    curve.lineTo(endX, automationY(laneY, to.value));
                    continue;
                }
                if (from.curveToNext == AutomationPoint::Curve::Hold)
                {
                    curve.lineTo(endX, automationY(laneY, from.value));
                    curve.lineTo(endX, automationY(laneY, to.value));
                    continue;
                }
                constexpr int maximumSamplesPerSegment = 24;
                const int samplesPerSegment = std::clamp(
                    static_cast<int>(std::ceil((to.beat - from.beat) * pixelsPerBeat / 6.0)),
                    2, maximumSamplesPerSegment);
                for (int sample = 1; sample <= samplesPerSegment; ++sample)
                {
                    const double position = sample / static_cast<double>(samplesPerSegment);
                    const double beat = from.beat + (to.beat - from.beat) * position;
                    const double amount = automationCurveProgress(from.curveToNext, position);
                    const float value = static_cast<float>(from.value +
                        (to.value - from.value) * amount);
                    curve.lineTo(trackHeaderWidth + beat * pixelsPerBeat,
                                 automationY(laneY, value));
                }
            }
            curve.lineTo(right, automationY(laneY, points.last().value));
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(trackColour(trackIndex).lighter(125), 2));
        painter.drawPath(curve);

        for (qsizetype index = 0; index < points.size(); ++index)
        {
            const AutomationPoint& point = points[index];
            const QPointF center(trackHeaderWidth + point.beat * pixelsPerBeat,
                                 automationY(laneY, point.value));
            const bool selected = track.id == selectedAutomationTrackId && index == selectedAutomationPoint;
            painter.setPen(QPen(selected ? QColor(QStringLiteral("#f2f6f8")) :
                                          trackColour(trackIndex).lighter(150), selected ? 2 : 1));
            painter.setBrush(selected ? QColor(QStringLiteral("#f0cf82")) : trackColour(trackIndex));
            painter.drawEllipse(center, selected ? 5.5 : 4.0, selected ? 5.5 : 4.0);
        }
    }

    void drawBusAutomationLane(QPainter& painter, const MixerBus& bus, int rowY, int gridWidth) const
    {
        const int laneY = rowY + trackHeight;
        const QRect laneRect(trackHeaderWidth, laneY, gridWidth, automationLaneHeight - 1);
        painter.fillRect(laneRect, QColor(QStringLiteral("#141a21")));
        painter.setPen(QPen(QColor(QStringLiteral("#333d48")), 1));
        painter.drawLine(trackHeaderWidth, laneY + 7, trackHeaderWidth + gridWidth, laneY + 7);
        painter.drawLine(trackHeaderWidth, laneY + automationLaneHeight - 7,
                         trackHeaderWidth + gridWidth, laneY + automationLaneHeight - 7);
        const auto& points = busAutomationPoints(bus);
        if (!hasBusAutomationTarget(bus))
        {
            painter.setPen(QColor(QStringLiteral("#78838f")));
            painter.drawText(laneRect, Qt::AlignCenter, tr("No matching auxiliary send"));
            return;
        }
        const float base = automationParameter == mixerBusPanAutomationTarget ? bus.pan :
            automationParameter == mixerBusMuteAutomationTarget ? (bus.muted ? 1.0f : 0.0f) :
            automationParameter == mixerBusSoloAutomationTarget ? (bus.solo ? 1.0f : 0.0f) :
            isBusSendAutomationTarget()
                ? automationParameter - mixerSendLevelAutomationTargetBase < bus.sends.size()
                    ? bus.sends[automationParameter - mixerSendLevelAutomationTargetBase].gain : 0.0f
                : bus.gain;
        QPainterPath path;
        if (points.isEmpty())
        {
            path.moveTo(trackHeaderWidth, automationY(laneY, base));
            path.lineTo(trackHeaderWidth + gridWidth, automationY(laneY, base));
        }
        else
        {
            path.moveTo(trackHeaderWidth, automationY(laneY, points.first().value));
            for (qsizetype index = 0; index + 1 < points.size(); ++index)
            {
                const auto& a = points[index];
                const auto& b = points[index + 1];
                const QPointF start(trackHeaderWidth + a.beat * pixelsPerBeat, automationY(laneY, a.value));
                const QPointF end(trackHeaderWidth + b.beat * pixelsPerBeat, automationY(laneY, b.value));
                if (a.curveToNext == AutomationPoint::Curve::Hold)
                {
                    path.lineTo(end.x(), start.y());
                    path.lineTo(end);
                }
                else
                    path.lineTo(end);
            }
            const auto& last = points.last();
            path.lineTo(trackHeaderWidth + gridWidth, automationY(laneY, last.value));
        }
        painter.setPen(QPen(QColor(QStringLiteral("#e6b66f")), 2));
        painter.drawPath(path);
        for (qsizetype index = 0; index < points.size(); ++index)
        {
            const QPoint center(trackHeaderWidth + static_cast<int>(std::round(points[index].beat * pixelsPerBeat)),
                                automationY(laneY, points[index].value));
            painter.setPen(QPen(QColor(QStringLiteral("#172029")), 1.5));
            painter.setBrush(bus.id == selectedAutomationBusId && index == selectedAutomationPoint
                                 ? QColor(QStringLiteral("#fff0c2")) : QColor(QStringLiteral("#e6b66f")));
            painter.drawEllipse(QPointF(center), 4.5, 4.5);
        }
    }

    QRectF clipBounds(const MidiClip& clip, int trackIndex) const
    {
        return QRectF(trackHeaderWidth + clip.startBeat * pixelsPerBeat + 2,
                      rulerHeight + trackIndex * currentRowHeight() + 9,
                      std::max(18.0, clip.lengthBeats * pixelsPerBeat - 4), trackHeight - 18);
    }

    QRectF audioWaveBounds(const MidiClip& clip, int trackIndex) const
    {
        return clipBounds(clip, trackIndex).adjusted(6, 27, -6, -6);
    }

    QPointF fadeBezierHandlePosition(const MidiClip& clip, int trackIndex,
                                     bool fadeIn, int handleIndex) const
    {
        const double fadeBeats = fadeIn ? clip.audioFadeInBeats : clip.audioFadeOutBeats;
        const QVector<QPointF>& handles = fadeIn
            ? clip.audioFadeInBezierHandles : clip.audioFadeOutBezierHandles;
        const QRectF waveBounds = audioWaveBounds(clip, trackIndex);
        if (fadeBeats <= 0.0 || clip.lengthBeats <= 0.0 ||
            handles.size() != audioFadeBezierHandleCount ||
            handleIndex < 0 || handleIndex >= handles.size())
            return {};
        const double fadeWidth = std::min(waveBounds.width(),
            fadeBeats / clip.lengthBeats * waveBounds.width());
        const double fadeStartX = fadeIn ? waveBounds.left() : waveBounds.right() - fadeWidth;
        const QPointF handle = handles[handleIndex];
        const double shapedGain = fadeIn ? handle.y() : 1.0 - handle.y();
        return {fadeStartX + fadeWidth * handle.x(),
                waveBounds.bottom() - 1.0 - shapedGain * (waveBounds.height() - 2.0)};
    }

    void drawFadeBezierHandles(QPainter& painter, const MidiClip& clip, int trackIndex) const
    {
        painter.save();
        const QRectF waveBounds = audioWaveBounds(clip, trackIndex);
        for (const bool fadeIn : {true, false})
        {
            const AudioFadeCurve curve = fadeIn ? clip.audioFadeInCurve : clip.audioFadeOutCurve;
            const double beats = fadeIn ? clip.audioFadeInBeats : clip.audioFadeOutBeats;
            if (curve != AudioFadeCurve::Bezier || beats <= 0.0)
                continue;
            const double width = std::min(waveBounds.width(), beats / clip.lengthBeats * waveBounds.width());
            const double startX = fadeIn ? waveBounds.left() : waveBounds.right() - width;
            const QPointF start(fadeIn ? waveBounds.left() : startX,
                                fadeIn ? waveBounds.bottom() - 1.0 : waveBounds.top() + 1.0);
            const QPointF end(fadeIn ? startX + width : waveBounds.right(),
                              fadeIn ? waveBounds.top() + 1.0 : waveBounds.bottom() - 1.0);
            const QPointF first = fadeBezierHandlePosition(clip, trackIndex, fadeIn, 0);
            const QPointF second = fadeBezierHandlePosition(clip, trackIndex, fadeIn, 1);
            painter.setPen(QPen(QColor(QStringLiteral("#c7d2da")), 1, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawLine(start, first);
            painter.drawLine(first, second);
            painter.drawLine(second, end);
            painter.setPen(QPen(QColor(QStringLiteral("#18313a")), 1.5));
            painter.setBrush(QColor(QStringLiteral("#f0cf82")));
            painter.drawEllipse(first, 4.5, 4.5);
            painter.drawEllipse(second, 4.5, 4.5);
        }
        painter.restore();
    }

    bool fadeBezierHandleAt(const MidiClip& clip, int trackIndex, const QPoint& point,
                            bool& fadeIn, int& handleIndex) const
    {
        if (trackIndex < 0 || trackIndex >= project.tracks.size() || !clip.isAudioRegion() ||
            clip.id != selectedClipId || selectedTrackId != project.tracks[trackIndex].id)
            return false;
        for (const bool isFadeIn : {true, false})
        {
            const AudioFadeCurve curve = isFadeIn ? clip.audioFadeInCurve : clip.audioFadeOutCurve;
            const double beats = isFadeIn ? clip.audioFadeInBeats : clip.audioFadeOutBeats;
            if (curve != AudioFadeCurve::Bezier || beats <= 0.0)
                continue;
            for (int index = 0; index < audioFadeBezierHandleCount; ++index)
            {
                if (QLineF(point, fadeBezierHandlePosition(clip, trackIndex, isFadeIn, index)).length() <= 9.0)
                {
                    fadeIn = isFadeIn;
                    handleIndex = index;
                    return true;
                }
            }
        }
        return false;
    }

    void beginFadeBezierHandleDrag(quint64 trackId, const MidiClip& clip, bool fadeIn, int handleIndex)
    {
        draggingFadeBezierHandle = true;
        fadeBezierTrackId = trackId;
        fadeBezierClipId = clip.id;
        fadeBezierIn = fadeIn;
        fadeBezierHandleIndex = handleIndex;
        fadeBezierBefore = fadeIn ? clip.audioFadeInBezierHandles : clip.audioFadeOutBezierHandles;
        QVector<QPointF>& handles = fadeIn
            ? project.findClip(trackId, clip.id)->audioFadeInBezierHandles
            : project.findClip(trackId, clip.id)->audioFadeOutBezierHandles;
        normalizeAudioFadeBezierHandles(handles);
        setCursor(Qt::SizeAllCursor);
    }

    void updateFadeBezierHandle(const QPoint& point)
    {
        const int trackIndex = trackIndexForId(fadeBezierTrackId);
        MidiClip* clip = project.findClip(fadeBezierTrackId, fadeBezierClipId);
        if (trackIndex < 0 || clip == nullptr || fadeBezierHandleIndex < 0 ||
            fadeBezierHandleIndex >= audioFadeBezierHandleCount)
            return;
        QVector<QPointF>& handles = fadeBezierIn
            ? clip->audioFadeInBezierHandles : clip->audioFadeOutBezierHandles;
        normalizeAudioFadeBezierHandles(handles);
        const QRectF waveBounds = audioWaveBounds(*clip, trackIndex);
        const double fadeBeats = fadeBezierIn ? clip->audioFadeInBeats : clip->audioFadeOutBeats;
        const double fadeWidth = clip->lengthBeats > 0.0
            ? std::min(waveBounds.width(), fadeBeats / clip->lengthBeats * waveBounds.width()) : 0.0;
        if (fadeWidth < 2.0)
            return;
        const double fadeStartX = fadeBezierIn ? waveBounds.left() : waveBounds.right() - fadeWidth;
        double minimumX = fadeBezierHandleIndex == 0 ? 0.0 : handles[0].x() + 0.01;
        double maximumX = fadeBezierHandleIndex == 1 ? 1.0 : handles[1].x() - 0.01;
        if (minimumX > maximumX)
            return;
        const double x = std::clamp((point.x() - fadeStartX) / fadeWidth, minimumX, maximumX);
        const double gain = std::clamp((waveBounds.bottom() - 1.0 - point.y()) /
            std::max(1.0, waveBounds.height() - 2.0), 0.0, 1.0);
        handles[fadeBezierHandleIndex] = QPointF(x, fadeBezierIn ? gain : 1.0 - gain);
        QVector<float>& points = fadeBezierIn
            ? clip->audioFadeInCurvePoints : clip->audioFadeOutCurvePoints;
        points = audioFadeBezierCurvePoints(handles);
    }

    bool drawnFadeAt(const MidiClip& clip, int trackIndex, const QPoint& point,
                     bool& fadeIn) const
    {
        if (!audioWaveBounds(clip, trackIndex).contains(point))
            return false;
        const QRectF waveBounds = audioWaveBounds(clip, trackIndex);
        const double fadeInWidth = std::min(waveBounds.width(),
            clip.audioFadeInBeats / clip.lengthBeats * waveBounds.width());
        const double fadeOutWidth = std::min(waveBounds.width(),
            clip.audioFadeOutBeats / clip.lengthBeats * waveBounds.width());
        const bool inFadeIn = fadeInWidth >= 2.0 && point.x() <= waveBounds.left() + fadeInWidth;
        const bool inFadeOut = fadeOutWidth >= 2.0 && point.x() >= waveBounds.right() - fadeOutWidth;
        if (!inFadeIn && !inFadeOut)
            return false;
        if (inFadeIn && inFadeOut)
        {
            const bool inCurveDrawn = clip.audioFadeInCurve == AudioFadeCurve::Drawn;
            const bool outCurveDrawn = clip.audioFadeOutCurve == AudioFadeCurve::Drawn;
            fadeIn = inCurveDrawn != outCurveDrawn ? inCurveDrawn
                : point.x() - waveBounds.left() <= waveBounds.right() - point.x();
        }
        else
            fadeIn = inFadeIn;
        return (fadeIn ? clip.audioFadeInCurve : clip.audioFadeOutCurve) == AudioFadeCurve::Drawn;
    }

    void beginFadeCurveDrawing(quint64 trackId, const MidiClip& clip, bool fadeIn,
                               const QPoint& point)
    {
        drawingFadeCurve = true;
        drawnFadeTrackId = trackId;
        drawnFadeClipId = clip.id;
        drawnFadeIn = fadeIn;
        drawnFadeBefore = fadeIn ? clip.audioFadeInCurvePoints : clip.audioFadeOutCurvePoints;
        if (drawnFadeBefore.size() != audioFadeCurvePointCount)
            drawnFadeBefore = defaultAudioFadeCurvePoints();
        if (MidiClip* mutableClip = project.findClip(trackId, clip.id))
        {
            QVector<float>& points = fadeIn ? mutableClip->audioFadeInCurvePoints
                                            : mutableClip->audioFadeOutCurvePoints;
            points = drawnFadeBefore;
        }
        drawingFadeLastIndex = -1;
        updateFadeCurveDrawing(point);
        setCursor(Qt::CrossCursor);
    }

    void updateFadeCurveDrawing(const QPoint& point)
    {
        const int trackIndex = trackIndexForId(drawnFadeTrackId);
        MidiClip* clip = project.findClip(drawnFadeTrackId, drawnFadeClipId);
        if (trackIndex < 0 || clip == nullptr)
            return;
        const QRectF waveBounds = audioWaveBounds(*clip, trackIndex);
        const double fadeWidth = std::min(waveBounds.width(),
            (drawnFadeIn ? clip->audioFadeInBeats : clip->audioFadeOutBeats) /
                clip->lengthBeats * waveBounds.width());
        if (fadeWidth < 2.0)
            return;
        const double startX = drawnFadeIn ? waveBounds.left() : waveBounds.right() - fadeWidth;
        const double progress = std::clamp((point.x() - startX) / fadeWidth, 0.0, 1.0);
        const float value = static_cast<float>(std::clamp(
            (waveBounds.bottom() - 1.0 - point.y()) /
                std::max(1.0, waveBounds.height() - 2.0), 0.0, 1.0));
        const int index = static_cast<int>(std::round(progress * (audioFadeCurvePointCount - 1)));
        QVector<float>& points = drawnFadeIn ? clip->audioFadeInCurvePoints
                                             : clip->audioFadeOutCurvePoints;
        if (points.size() != audioFadeCurvePointCount)
            points = defaultAudioFadeCurvePoints();
        if (drawingFadeLastIndex >= 0 && drawingFadeLastIndex != index)
        {
            const int direction = index > drawingFadeLastIndex ? 1 : -1;
            const float previousValue = points[drawingFadeLastIndex];
            const int distance = std::abs(index - drawingFadeLastIndex);
            for (int step = 1; step < distance; ++step)
            {
                const int pointIndex = drawingFadeLastIndex + direction * step;
                const float amount = static_cast<float>(step) / distance;
                points[pointIndex] = previousValue + (value - previousValue) * amount;
            }
        }
        points[index] = value;
        normalizeAudioFadeCurvePoints(points);
        drawingFadeLastIndex = index;
    }

    int clipAt(const QPoint& point) const
    {
        const int row = (point.y() - rulerHeight) / currentRowHeight();
        if (row < 0 || row >= project.tracks.size())
            return -1;
        const InstrumentTrack& track = project.tracks[row];
        int hit = -1;
        int hitLayer = -1;
        for (int index = track.clips.size() - 1; index >= 0; --index)
        {
            const MidiClip& clip = track.clips[index];
            if (!clipBounds(clip, row).contains(point))
                continue;
            const int layer = clip.audioTakeGroupId == 0 ? 1 : clip.audioTakeActive ? 2 : 0;
            if (layer > hitLayer)
            {
                hit = index;
                hitLayer = layer;
            }
        }
        return hit;
    }

    void setLocalClipPosition(quint64 sourceTrackId, quint64 targetTrackId, quint64 clipId, double startBeat)
    {
        MidiClip movedClip;
        bool found = false;
        for (InstrumentTrack& track : project.tracks)
        {
            if (track.id != sourceTrackId) continue;
            for (MidiClip& clip : track.clips)
            {
                if (clip.id == clipId)
                {
                    movedClip = clip;
                    movedClip.startBeat = startBeat;
                    track.clips.erase(std::find_if(track.clips.begin(), track.clips.end(),
                        [clipId](const MidiClip& candidate) { return candidate.id == clipId; }));
                    found = true;
                    break;
                }
            }
            break;
        }
        if (!found) return;
        for (InstrumentTrack& track : project.tracks)
            if (track.id == targetTrackId)
            {
                track.clips.push_back(std::move(movedClip));
                return;
            }
    }

    void setLocalAudioTrim(quint64 trackId, quint64 clipId, double startBeat,
                           double lengthBeats, qint64 audioStartFrame)
    {
        MidiClip* clip = project.findClip(trackId, clipId);
        if (clip == nullptr)
            return;
        clip->startBeat = startBeat;
        clip->lengthBeats = lengthBeats;
        clip->audioStartFrame = audioStartFrame;
    }

    void updateCanvasSize()
    {
        const double quarterNotesPerBar = project.quarterNotesPerBar();
        double visibleBeats = std::max(project.songLengthBeats() + quarterNotesPerBar * 4.0,
                                       quarterNotesPerBar * 16.0);
        for (const TimelineMarker& marker : project.markers)
            visibleBeats = std::max(visibleBeats, marker.beat + quarterNotesPerBar * 2.0);
        for (const TempoChange& change : project.tempoChanges)
            visibleBeats = std::max(visibleBeats, change.beat + quarterNotesPerBar * 2.0);
        if (punchRangeEnabled)
            visibleBeats = std::max(visibleBeats, punchOutBeat + quarterNotesPerBar);
        const int width = trackHeaderWidth + static_cast<int>(std::ceil(visibleBeats * pixelsPerBeat));
        const int rows = static_cast<int>(project.tracks.size()) +
            ((automationViewEnabled && isBusAutomationTarget()) ? static_cast<int>(project.buses.size()) : 0);
        const int height = rulerHeight + std::max(1, rows) * currentRowHeight();
        setMinimumSize(width, height);
        resize(width, height);
        update();
    }

    ArrangementEditor* owner;
    ProjectData project;
    quint64 selectedTrackId = 0;
    quint64 selectedClipId = 0;
    quint64 sourceTrackId = 0;
    quint64 draggedTrackId = 0;
    quint64 draggedClipId = 0;
    double initialClipStart = 0.0;
    double initialClipLength = 0.0;
    qint64 initialAudioStartFrame = 0;
    int dragStartX = 0;
    int pixelsPerBeat = 28;
    double playheadBeat = -1.0;
    double punchInBeat = 0.0;
    double punchOutBeat = 0.0;
    int automationParameter = 0;
    float automationMinimum = 0.0f;
    float automationMaximum = 1.0f;
    float automationDefaultValue = 0.0f;
    AutomationPoint::Curve automationDefaultCurve = AutomationPoint::Curve::Linear;
    QString automationPluginUri;
    QString automationTargetLabel;
    int selectedAutomationPoint = -1;
    quint64 selectedAutomationTrackId = 0;
    quint64 selectedAutomationBusId = 0;
    quint64 automationDragTrackId = 0;
    quint64 automationDragBusId = 0;
    QVector<AutomationPoint> automationDragBefore;
    bool automationViewEnabled = false;
    bool punchRangeEnabled = false;
    bool draggingAutomationPoint = false;
    bool automationNeedsTrackSelection = false;
    bool movingClip = false;
    bool resizingAudioRegion = false;
    bool resizeLeftEdge = false;
    bool drawingFadeCurve = false;
    bool draggingFadeBezierHandle = false;
    bool drawnFadeIn = false;
    bool fadeBezierIn = false;
    int drawingFadeLastIndex = -1;
    int fadeBezierHandleIndex = -1;
    quint64 drawnFadeTrackId = 0;
    quint64 drawnFadeClipId = 0;
    quint64 fadeBezierTrackId = 0;
    quint64 fadeBezierClipId = 0;
    QVector<float> drawnFadeBefore;
    QVector<QPointF> fadeBezierBefore;
};

ArrangementEditor::ArrangementEditor(QWidget* parent) : QScrollArea(parent)
{
    setObjectName(QStringLiteral("arrangementEditor"));
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    canvas = new Canvas(this);
    setWidget(canvas);
}

void ArrangementEditor::setProject(const ProjectData& project)
{
    canvas->setProject(project);
}

void ArrangementEditor::setSelection(quint64 trackId, quint64 clipId)
{
    canvas->setSelection(trackId, clipId);
}

void ArrangementEditor::setPixelsPerBeat(int pixels)
{
    canvas->setPixelsPerBeat(pixels);
}

void ArrangementEditor::setPlayheadBeat(double beat)
{
    canvas->setPlayheadBeat(beat);
}

void ArrangementEditor::setPunchRange(bool enabled, double punchInBeat, double punchOutBeat)
{
    canvas->setPunchRange(enabled, punchInBeat, punchOutBeat);
}

void ArrangementEditor::setAutomationView(bool enabled, int parameter, float minimum,
                                          float maximum, float defaultValue,
                                          const QString& pluginUri,
                                          AutomationPoint::Curve defaultCurve,
                                          const QString& targetLabel)
{
    canvas->setAutomationView(enabled, parameter, minimum, maximum, defaultValue,
                              pluginUri, defaultCurve, targetLabel);
}
