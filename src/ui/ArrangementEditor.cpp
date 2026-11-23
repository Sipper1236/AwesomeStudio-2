#include "ArrangementEditor.h"

#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
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
QColor trackColour(int index)
{
    static const QColor palette[] = {
        QColor(QStringLiteral("#47a7bd")), QColor(QStringLiteral("#9a7bd1")),
        QColor(QStringLiteral("#d68756")), QColor(QStringLiteral("#66a879")),
        QColor(QStringLiteral("#d26f91")), QColor(QStringLiteral("#b4a04d"))
    };
    return palette[index % static_cast<int>(std::size(palette))];
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
        updateCanvasSize();
    }

    void setSelection(quint64 selectedTrack, quint64 selectedClip)
    {
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

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor(QStringLiteral("#131820")));

        const int trackCount = std::max(1, static_cast<int>(project.tracks.size()));
        const double beatsPerBar = project.beatsPerBar;
        const double visibleBeats = std::max(project.songLengthBeats() + beatsPerBar * 4.0,
                                             beatsPerBar * 16.0);
        const int gridWidth = static_cast<int>(std::ceil(visibleBeats * pixelsPerBeat));
        painter.fillRect(QRect(trackHeaderWidth, 0, gridWidth, rulerHeight), QColor(QStringLiteral("#202630")));
        painter.fillRect(QRect(0, 0, trackHeaderWidth, rulerHeight), QColor(QStringLiteral("#252d37")));

        for (int row = 0; row < trackCount; ++row)
        {
            const int y = rulerHeight + row * trackHeight;
            const bool selected = row < project.tracks.size() && project.tracks[row].id == selectedTrackId;
            painter.fillRect(QRect(trackHeaderWidth, y, gridWidth, trackHeight - 1),
                             row % 2 == 0 ? QColor(QStringLiteral("#191f27")) : QColor(QStringLiteral("#171d25")));
            painter.fillRect(QRect(0, y, trackHeaderWidth, trackHeight - 1),
                             selected ? QColor(QStringLiteral("#29343e")) : QColor(QStringLiteral("#222a33")));
            painter.setPen(QColor(QStringLiteral("#303944")));
            painter.drawLine(0, y + trackHeight - 1, width(), y + trackHeight - 1);

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
                             track.muted ? tr("MUTED") : tr("MIDI · %1 clips").arg(track.clips.size()));
        }

        const int totalBeats = static_cast<int>(std::ceil(visibleBeats));
        for (int beat = 0; beat <= totalBeats; ++beat)
        {
            const int x = trackHeaderWidth + beat * pixelsPerBeat;
            const bool bar = beat % project.beatsPerBar == 0;
            painter.setPen(bar ? QColor(QStringLiteral("#596572")) : QColor(QStringLiteral("#2b333d")));
            painter.drawLine(x, rulerHeight, x, rulerHeight + trackCount * trackHeight);
            if (bar && beat < visibleBeats)
            {
                painter.setPen(QColor(QStringLiteral("#e2e8ee")));
                painter.setFont(QFont(QStringLiteral("Sans Serif"), 8, QFont::DemiBold));
                painter.drawText(QRect(x + 5, 0, pixelsPerBeat * project.beatsPerBar - 10, rulerHeight),
                                 Qt::AlignVCenter, QString::number(beat / project.beatsPerBar + 1));
            }
        }

        for (int trackIndex = 0; trackIndex < project.tracks.size(); ++trackIndex)
        {
            const InstrumentTrack& track = project.tracks[trackIndex];
            const int rowY = rulerHeight + trackIndex * trackHeight;
            for (const MidiClip& clip : track.clips)
            {
                const QRectF bounds = clipBounds(clip, trackIndex);
                const bool selected = clip.id == selectedClipId && track.id == selectedTrackId;
                QColor fill = trackColour(trackIndex);
                fill.setAlpha(selected ? 225 : 165);
                painter.setPen(QPen(selected ? QColor(QStringLiteral("#d5fff7")) : fill.lighter(125), selected ? 2 : 1));
                painter.setBrush(fill);
                painter.drawRoundedRect(bounds, 4.0, 4.0);

                painter.setPen(QColor(QStringLiteral("#102126")));
                painter.setFont(QFont(QStringLiteral("Sans Serif"), 8, QFont::DemiBold));
                painter.drawText(bounds.adjusted(7, 3, -4, -3), Qt::AlignTop | Qt::AlignLeft,
                                 QStringLiteral("%1\n%2 notes").arg(clip.name).arg(clip.notes.size()));

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

        if (playheadBeat >= 0.0 && playheadBeat <= visibleBeats)
        {
            const int x = trackHeaderWidth + static_cast<int>(std::round(playheadBeat * pixelsPerBeat));
            painter.setPen(QPen(QColor(QStringLiteral("#f1a85c")), 2.0));
            painter.drawLine(x, rulerHeight, x, rulerHeight + trackCount * trackHeight);
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        setFocus(Qt::MouseFocusReason);
        const QPoint point = event->position().toPoint();
        const int row = (point.y() - rulerHeight) / trackHeight;
        if (row < 0 || row >= project.tracks.size())
            return;
        const InstrumentTrack& track = project.tracks[row];
        if (point.x() < trackHeaderWidth)
        {
            selectedTrackId = track.id;
            selectedClipId = 0;
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
                emit owner->clipRemoved(track.id, clip.id);
            }
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;

        selectedTrackId = track.id;
        if (hit >= 0)
        {
            const MidiClip& clip = track.clips[hit];
            selectedClipId = clip.id;
            sourceTrackId = track.id;
            draggedClipId = clip.id;
            draggedTrackId = track.id;
            initialClipStart = clip.startBeat;
            dragStartX = point.x();
            movingClip = true;
            emit owner->clipSelected(track.id, clip.id);
            update();
            return;
        }

        selectedClipId = 0;
        const double beat = std::max(0.0, (point.x() - trackHeaderWidth) / static_cast<double>(pixelsPerBeat));
        const double startBeat = std::floor(beat / project.beatsPerBar) * project.beatsPerBar;
        emit owner->clipAdded(track.id, startBeat);
        update();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!movingClip)
            return;
        const QPoint point = event->position().toPoint();
        const double delta = (point.x() - dragStartX) / static_cast<double>(pixelsPerBeat);
        const double snap = std::max(0.25, project.beatsPerBar / 4.0);
        const double startBeat = std::max(0.0, std::round((initialClipStart + delta) / snap) * snap);
        const int row = std::clamp((point.y() - rulerHeight) / trackHeight, 0,
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
        if (event->button() != Qt::LeftButton || !movingClip)
            return;
        movingClip = false;
        const MidiClip* clip = project.findClip(draggedTrackId, draggedClipId);
        if (clip != nullptr && (draggedTrackId != sourceTrackId ||
                                std::abs(clip->startBeat - initialClipStart) > 1.0e-9))
            emit owner->clipMoved(sourceTrackId, draggedTrackId, draggedClipId, clip->startBeat);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        const QPoint point = event->position().toPoint();
        const int row = (point.y() - rulerHeight) / trackHeight;
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
            if (selectedTrackId != 0 && selectedClipId != 0)
                emit owner->clipRemoved(selectedTrackId, selectedClipId);
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    QRectF clipBounds(const MidiClip& clip, int trackIndex) const
    {
        return QRectF(trackHeaderWidth + clip.startBeat * pixelsPerBeat + 2,
                      rulerHeight + trackIndex * trackHeight + 9,
                      std::max(18.0, clip.lengthBeats * pixelsPerBeat - 4), trackHeight - 18);
    }

    int clipAt(const QPoint& point) const
    {
        const int row = (point.y() - rulerHeight) / trackHeight;
        if (row < 0 || row >= project.tracks.size())
            return -1;
        const InstrumentTrack& track = project.tracks[row];
        for (int index = track.clips.size() - 1; index >= 0; --index)
            if (clipBounds(track.clips[index], row).contains(point)) return index;
        return -1;
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

    void updateCanvasSize()
    {
        const double beatsPerBar = project.beatsPerBar;
        const double visibleBeats = std::max(project.songLengthBeats() + beatsPerBar * 4.0,
                                             beatsPerBar * 16.0);
        const int width = trackHeaderWidth + static_cast<int>(std::ceil(visibleBeats * pixelsPerBeat));
        const int height = rulerHeight + std::max(1, static_cast<int>(project.tracks.size())) * trackHeight;
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
    int dragStartX = 0;
    int pixelsPerBeat = 28;
    double playheadBeat = -1.0;
    bool movingClip = false;
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
