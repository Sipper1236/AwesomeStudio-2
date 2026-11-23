#include "MainWindow.h"

#include "core/AudioFile.h"
#include "core/AudioRecorder.h"
#include "core/ProjectFile.h"
#include "core/MidiFile.h"
#include "core/MidiInput.h"
#include "core/WavExporter.h"
#include "ui/ArrangementEditor.h"
#include "ui/MidiEditor.h"
#include "ui/StepSequencer.h"

#include <QAction>
#include <QApplication>
#include <QAudioDevice>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMediaDevices>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSettings>
#include <QSlider>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace
{
constexpr double automationWritePointSpacingBeats = 0.04;

int drumSamplePadForPitch(int pitch)
{
    if (pitch == 35 || pitch == 36) return 0;
    if (pitch == 38 || pitch == 40) return 1;
    if (pitch == 42 || pitch == 44) return 2;
    if (pitch == 46) return 3;
    return -1;
}

bool sameAutomationPoints(const QVector<AutomationPoint>& left, const QVector<AutomationPoint>& right)
{
    if (left.size() != right.size())
        return false;
    for (qsizetype index = 0; index < left.size(); ++index)
    {
        if (std::abs(left[index].beat - right[index].beat) > 1.0e-9 ||
            std::abs(left[index].value - right[index].value) > 1.0e-6f)
            return false;
    }
    return true;
}

bool upsertAutomationPoint(QVector<AutomationPoint>& points, double beat, float value)
{
    auto position = std::lower_bound(points.begin(), points.end(), beat,
        [](const AutomationPoint& point, double targetBeat) { return point.beat < targetBeat; });
    if (position != points.end() && std::abs(position->beat - beat) < 0.001)
    {
        position->value = value;
        return true;
    }
    if (position != points.begin() && std::abs((position - 1)->beat - beat) < 0.001)
    {
        (position - 1)->value = value;
        return true;
    }
    if (points.size() >= 10000)
        return false;
    points.insert(position, {beat, value});
    return true;
}
}

class NoteEditCommand final : public QUndoCommand
{
public:
    NoteEditCommand(MainWindow* window, quint64 trackId, quint64 clipId,
                    QVector<MidiNote> before, QVector<MidiNote> after,
                    double beforeLengthBeats = -1.0, double afterLengthBeats = -1.0)
        : window(window), trackId(trackId), clipId(clipId),
          before(std::move(before)), after(std::move(after)),
          beforeLengthBeats(beforeLengthBeats), afterLengthBeats(afterLengthBeats)
    {
        setText(QObject::tr("Edit MIDI notes"));
    }

    void undo() override
    {
        if (window != nullptr)
            window->applyNoteHistory(trackId, clipId, before, beforeLengthBeats);
    }

    void redo() override
    {
        if (firstRedo)
        {
            firstRedo = false;
            return;
        }
        if (window != nullptr)
            window->applyNoteHistory(trackId, clipId, after, afterLengthBeats);
    }

private:
    QPointer<MainWindow> window;
    quint64 trackId;
    quint64 clipId;
    QVector<MidiNote> before;
    QVector<MidiNote> after;
    double beforeLengthBeats;
    double afterLengthBeats;
    bool firstRedo = true;
};

class AutomationEditCommand final : public QUndoCommand
{
public:
    AutomationEditCommand(MainWindow* window, quint64 trackId, int parameter,
                          QVector<AutomationPoint> before, QVector<AutomationPoint> after)
        : window(window), trackId(trackId), parameter(parameter),
          before(std::move(before)), after(std::move(after))
    {
        setText(QObject::tr("Edit track automation"));
    }

    void undo() override
    {
        if (window != nullptr)
            window->applyAutomationHistory(trackId, parameter, before);
    }

    void redo() override
    {
        if (window != nullptr)
            window->applyAutomationHistory(trackId, parameter, after);
    }

private:
    QPointer<MainWindow> window;
    quint64 trackId;
    int parameter;
    QVector<AutomationPoint> before;
    QVector<AutomationPoint> after;
};

MainWindow::MainWindow(AudioEngine& engine, QWidget* parent)
    : QMainWindow(parent), audioEngine(engine)
{
    setObjectName(QStringLiteral("mainWindow"));
    resize(1480, 920);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks |
                   QMainWindow::AllowTabbedDocks | QMainWindow::GroupedDragging);

    editorTabs = new QTabWidget(this);
    arrangementEditor = new ArrangementEditor(editorTabs);
    midiEditor = new MidiEditor(editorTabs);
    stepSequencer = new StepSequencer(editorTabs);
    editorTabs->addTab(arrangementEditor, tr("Playlist"));
    editorTabs->addTab(midiEditor, tr("Piano roll"));
    editorTabs->addTab(stepSequencer, tr("Step sequencer"));
    setCentralWidget(editorTabs);

    createActions();
    createToolBars();
    createDocks();
    createMenus();

    const auto applyEditedNotes = [this](const QVector<MidiNote>& before, const QVector<MidiNote>& after)
    {
        MidiClip* clip = selectedClip();
        if (clip == nullptr || selectedTrackId == 0)
            return;
        clip->notes = after;
        audioEngine.setProject(project);
        arrangementEditor->setProject(project);
        arrangementEditor->setSelection(selectedTrackId, selectedClipId);
        midiEditor->setNotes(after);
        stepSequencer->setPattern(after, clip->lengthBeats, project.quarterNotesPerBar(),
                                  project.nextNoteId(), true);
        undoStack.push(new NoteEditCommand(this, selectedTrackId, selectedClipId, before, after));
        refreshDirtyState();
        statusBar()->showMessage(tr("MIDI clip updated"), 1600);
    };
    connect(midiEditor, &MidiEditor::notesEdited, this, applyEditedNotes);
    connect(stepSequencer, &StepSequencer::notesEdited, this, applyEditedNotes);
    connect(midiEditor, &MidiEditor::previewRequested, this, [this](int pitch)
    {
        const InstrumentTrack* track = selectedTrack();
        if (track != nullptr && track->instrumentType == InstrumentType::DrumRack)
        {
            const int pad = drumSamplePadForPitch(pitch);
            if (pad >= 0 && !track->drumSamples[static_cast<size_t>(pad)].path.isEmpty())
            {
                const DrumPadSample& sample = track->drumSamples[static_cast<size_t>(pad)];
                if (sample.source != nullptr)
                    audioEngine.triggerSamplePreview(sample.source, 96, track->pan, track->gain);
                else
                    statusBar()->showMessage(tr("The assigned drum sample is offline"), 2000);
                return;
            }
        }
        audioEngine.triggerPreview(pitch, 96,
                                   track == nullptr ? SynthWaveform::Sine : track->waveform,
                                   track == nullptr ? 0.0f : track->pan,
                                   track != nullptr && track->instrumentType == InstrumentType::DrumRack);
    });
    connect(stepSequencer, &StepSequencer::previewRequested, this, [this](int pitch)
    {
        const InstrumentTrack* track = selectedTrack();
        if (track != nullptr && track->instrumentType == InstrumentType::DrumRack)
        {
            const int pad = drumSamplePadForPitch(pitch);
            if (pad >= 0 && !track->drumSamples[static_cast<size_t>(pad)].path.isEmpty())
            {
                const DrumPadSample& sample = track->drumSamples[static_cast<size_t>(pad)];
                if (sample.source != nullptr)
                    audioEngine.triggerSamplePreview(sample.source, 100, track->pan, track->gain);
                else
                    statusBar()->showMessage(tr("The assigned drum sample is offline"), 2000);
                return;
            }
        }
        audioEngine.triggerPreview(pitch, 100,
                                   track == nullptr ? SynthWaveform::Sine : track->waveform,
                                   track == nullptr ? 0.0f : track->pan,
                                   track != nullptr && track->instrumentType == InstrumentType::DrumRack);
    });
    connect(arrangementEditor, &ArrangementEditor::clipActivated, this,
            [this](quint64 trackId, quint64 clipId) { selectClip(trackId, clipId, true); });
    connect(arrangementEditor, &ArrangementEditor::clipSelected, this,
            [this](quint64 trackId, quint64 clipId) { selectClip(trackId, clipId, false); });
    connect(arrangementEditor, &ArrangementEditor::clipAdded, this,
            [this](quint64 trackId, double startBeat) { addClipAt(trackId, startBeat); });
    connect(arrangementEditor, &ArrangementEditor::clipRemoved, this,
            [this](quint64 trackId, quint64 clipId) { removeClip(trackId, clipId); });
    connect(arrangementEditor, &ArrangementEditor::clipMoved, this,
            [this](quint64 sourceTrackId, quint64 targetTrackId, quint64 clipId, double startBeat)
            { moveClip(sourceTrackId, targetTrackId, clipId, startBeat); });
    connect(arrangementEditor, &ArrangementEditor::audioRegionTrimmed, this,
            [this](quint64 trackId, quint64 clipId, double startBeat, double lengthBeats, qint64 audioStartFrame)
            { trimAudioRegion(trackId, clipId, startBeat, lengthBeats, audioStartFrame); });
    connect(arrangementEditor, &ArrangementEditor::automationChanged, this,
            [this](quint64 trackId, int parameter, const QVector<AutomationPoint>& before,
                   const QVector<AutomationPoint>& after)
    {
        if (project.findTrack(trackId) == nullptr)
            return;
        undoStack.push(new AutomationEditCommand(this, trackId, parameter, before, after));
    });
    connect(arrangementEditor, &ArrangementEditor::automationPointLimitReached, this, [this]
    {
        statusBar()->showMessage(tr("This track has reached the 10,000 keyframe limit for that parameter."), 4000);
    });
    connect(arrangementEditor, &ArrangementEditor::trackSelected, this,
            [this](quint64 trackId) { selectTrack(trackId); });
    connect(&undoStack, &QUndoStack::cleanChanged, this, [this] { refreshDirtyState(); });

    setStyleSheet(QStringLiteral(
        "QMainWindow { background: #12161c; color: #e8edf2; }"
        "QToolBar { background: #1b2028; border: 0; border-bottom: 1px solid #303743; spacing: 6px; padding: 5px; }"
        "QToolBar QToolButton { color: #d8e0e8; background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 5px 8px; }"
        "QToolBar QToolButton:hover { background: #29313b; border-color: #3b4653; }"
        "QToolBar QToolButton:checked { color: #101d20; background: #56d3c0; }"
        "QMenuBar, QMenu { background: #1b2028; color: #dce3e9; }"
        "QMenuBar::item:selected, QMenu::item:selected { background: #303944; }"
        "QDockWidget { color: #dce3e9; font-weight: 600; }"
        "QDockWidget::title { background: #1b2028; padding: 7px 9px; border-bottom: 1px solid #303743; }"
        "QStatusBar { background: #1b2028; color: #9da8b4; border-top: 1px solid #303743; }"
        "QComboBox, QDoubleSpinBox { color: #e8edf2; background: #252c35; border: 1px solid #3b4653; border-radius: 4px; padding: 4px 7px; min-height: 20px; }"
        "QComboBox QAbstractItemView { background: #252c35; color: #e8edf2; selection-background-color: #27796f; }"
        "QListWidget { background: #171c23; color: #cbd4dd; border: 0; outline: 0; }"
        "QListWidget::item { padding: 6px 8px; border-radius: 3px; }"
        "QListWidget::item:selected { background: #2a4d4d; color: #dbfff8; }"
        "QGroupBox { color: #dce4eb; border: 1px solid #343d48; border-radius: 4px; margin-top: 8px; padding-top: 8px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
        "QSlider::groove:vertical { width: 6px; background: #303944; border-radius: 3px; }"
        "QSlider::handle:vertical { height: 16px; margin: 0 -5px; background: #56d3c0; border-radius: 4px; }"
        "QSlider::groove:horizontal { height: 5px; background: #303944; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; margin: -4px 0; background: #56d3c0; border-radius: 3px; }"
    ));

    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("window/state")).toByteArray());

    if (!project.tracks.isEmpty())
    {
        selectedTrackId = project.tracks.first().id;
        if (!project.tracks.first().clips.isEmpty())
            selectedClipId = project.tracks.first().clips.first().id;
    }
    refreshProjectUi();

    if (audioEngine.initialize())
    {
        audioDeviceLabel->setText(tr("Audio · %1").arg(audioEngine.deviceName()));
        statusBar()->showMessage(tr("Ready · add clips in Playlist, then double-click one to edit MIDI"));
    }
    else
    {
        audioDeviceLabel->setText(tr("Audio unavailable"));
        statusBar()->showMessage(audioEngine.errorMessage() +
                                 tr(" The editor remains available; connect an output device to audition."));
    }

    updateProjectTitle();
    uiTimer = new QTimer(this);
    uiTimer->setInterval(33);
    connect(uiTimer, &QTimer::timeout, this, [this]
    {
        const double beat = audioEngine.positionBeats();
        if (automationWriteSession.active && !audioEngine.isPlaying())
            finishAutomationWrite();
        if (recordingPending)
        {
            if (!audioEngine.isPlaying())
            {
                finishAudioRecording(false);
                statusBar()->showMessage(tr("Playback stopped before recording could start."), 4000);
            }
            else if (beat >= 0.0 && !startPendingAudioRecording(beat))
            {
                finishAudioRecording(false);
            }
        }
        if (audioRecorder)
        {
            const QString inputIssue = audioRecorder->errorMessage();
            if (audioRecorder->maximumReached())
                finishAudioRecording(true);
            else if (!inputIssue.isEmpty())
                finishAudioRecording(true);
            else if (recordingTransportActive && !audioEngine.isPlaying())
                finishAudioRecording(true);
        }
        if (midiRecordingPending)
        {
            if (!midiTransportActive || !audioEngine.isPlaying())
            {
                finishMidiRecording(false);
                statusBar()->showMessage(tr("Playback stopped before MIDI recording could start."), 4000);
            }
            else if (beat >= 0.0 && !startPendingMidiRecording(beat))
            {
                finishMidiRecording(false);
            }
        }
        if (midiRecordingActive && midiTransportActive && !audioEngine.isPlaying())
            finishMidiRecording(true);
        if (beat < 0.0)
        {
            playheadLabel->setText(tr("COUNT-IN"));
            arrangementEditor->setPlayheadBeat(-1.0);
        }
        else
        {
            const double quarterNotesPerBar = project.quarterNotesPerBar();
            const double denominatorUnit = 4.0 / project.beatUnit;
            const int bar = static_cast<int>(beat / quarterNotesPerBar) + 1;
            const double beatInBar = std::fmod(beat, quarterNotesPerBar) / denominatorUnit + 1.0;
            playheadLabel->setText(tr("BAR %1  ·  BEAT %2")
                                       .arg(bar, 2, 10, QLatin1Char('0'))
                                       .arg(beatInBar, 0, 'f', 2));
            arrangementEditor->setPlayheadBeat(beat);
        }
        const MidiClip* clip = selectedClip();
        if (beat >= 0.0 && clip != nullptr && clip->lengthBeats > 0.0)
            midiEditor->setPlayheadBeat(std::fmod(std::max(0.0, beat - clip->startBeat), clip->lengthBeats));
        else
            midiEditor->setPlayheadBeat(0.0);
        if (!audioEngine.isPlaying() && playAction->isChecked())
            playAction->setChecked(false);
    });
    uiTimer->start();
}

MainWindow::~MainWindow() = default;

void MainWindow::createActions()
{
    auto* newAction = new QAction(style()->standardIcon(QStyle::SP_FileIcon), tr("New"), this);
    newAction->setShortcut(QKeySequence::New);
    newAction->setStatusTip(tr("Create a new arrangement"));
    connect(newAction, &QAction::triggered, this, [this]
    {
        if (maybeSave())
            setProject(ProjectData{});
    });

    auto* openAction = new QAction(style()->standardIcon(QStyle::SP_DialogOpenButton), tr("Open"), this);
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, [this]
    {
        if (!maybeSave())
            return;
        const QString path = QFileDialog::getOpenFileName(this, tr("Open project"), projectPath,
                                                          tr("Awesome Studio project (*.aws)"));
        if (path.isEmpty())
            return;
        ProjectData loaded;
        QString error;
        if (!ProjectFile::load(path, loaded, &error))
        {
            QMessageBox::critical(this, tr("Could not open project"), error);
            return;
        }
        setProject(loaded, path);
    });

    auto* saveAction = new QAction(style()->standardIcon(QStyle::SP_DialogSaveButton), tr("Save"), this);
    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, [this]
    {
        if (projectPath.isEmpty())
        {
            QString path = QFileDialog::getSaveFileName(this, tr("Save project"), QString(),
                                                         tr("Awesome Studio project (*.aws)"));
            if (!path.isEmpty())
            {
                if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".aws");
                saveToPath(path);
            }
        }
        else
        {
            saveToPath(projectPath);
        }
    });

    auto* exportAction = new QAction(tr("Export arrangement to WAV…"), this);
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportAudio);

    addTrackAction = new QAction(tr("Add track"), this);
    connect(addTrackAction, &QAction::triggered, this, &MainWindow::addTrack);
    addDrumTrackAction = new QAction(tr("Add drum track"), this);
    connect(addDrumTrackAction, &QAction::triggered, this, &MainWindow::addDrumTrack);
    addPatternAction = new QAction(tr("New pattern"), this);
    connect(addPatternAction, &QAction::triggered, this, &MainWindow::addNewPattern);

    playAction = new QAction(style()->standardIcon(QStyle::SP_MediaPlay), tr("Play"), this);
    playAction->setCheckable(true);
    playAction->setShortcut(Qt::Key_Space);
    playAction->setToolTip(tr("Play arrangement (Space)"));
    connect(playAction, &QAction::triggered, this, [this](bool enabled)
    {
        if (enabled)
        {
            if (!audioEngine.isAudioAvailable() && !audioEngine.initialize())
            {
                playAction->setChecked(false);
                audioDeviceLabel->setText(tr("Audio unavailable"));
                statusBar()->showMessage(audioEngine.errorMessage(), 5000);
                return;
            }
            audioEngine.setProject(project);
            audioEngine.startPlayback();
            statusBar()->showMessage(tr("Playing arrangement"));
        }
        else
        {
            finishAutomationWrite();
            if (audioRecorder || recordingPending)
                finishAudioRecording(true);
            if (midiRecordingActive || midiRecordingPending)
                finishMidiRecording(true);
            audioEngine.stopPlayback();
            midiEditor->setPlayheadBeat(0.0);
            arrangementEditor->setPlayheadBeat(0.0);
            statusBar()->showMessage(tr("Playback stopped"), 1800);
        }
    });

    stopAction = new QAction(style()->standardIcon(QStyle::SP_MediaStop), tr("Stop"), this);
    stopAction->setShortcut(Qt::Key_Escape);
    connect(stopAction, &QAction::triggered, this, [this]
    {
        finishAutomationWrite();
        if (audioRecorder || recordingPending)
            finishAudioRecording(true);
        if (midiRecordingActive || midiRecordingPending)
            finishMidiRecording(true);
        audioEngine.stopPlayback();
        playAction->setChecked(false);
        midiEditor->setPlayheadBeat(0.0);
        arrangementEditor->setPlayheadBeat(0.0);
    });

    recordAction = new QAction(tr("Record audio…"), this);
    recordAction->setCheckable(true);
    recordAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    recordAction->setToolTip(tr("Record the selected audio input to a new Playlist track"));
    connect(recordAction, &QAction::triggered, this, [this](bool enabled)
    {
        if (enabled)
            startAudioRecording();
        else
            finishAudioRecording(true);
    });

    midiRecordAction = new QAction(tr("Record MIDI"), this);
    midiRecordAction->setCheckable(true);
    midiRecordAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+R")));
    midiRecordAction->setToolTip(tr("Record MIDI notes from the selected input into an instrument pattern"));
    connect(midiRecordAction, &QAction::triggered, this, [this](bool enabled)
    {
        if (enabled)
            startMidiRecording();
        else
            finishMidiRecording(true);
    });

    midiMonitorAction = new QAction(tr("Monitor MIDI"), this);
    midiMonitorAction->setCheckable(true);
    midiMonitorAction->setToolTip(tr("Play incoming MIDI on the selected synth or drum rack without recording"));
    connect(midiMonitorAction, &QAction::toggled, this, &MainWindow::setMidiMonitorEnabled);

    loopAction = new QAction(tr("Loop"), this);
    loopAction->setCheckable(true);
    loopAction->setChecked(project.loopEnabled);
    loopAction->setToolTip(tr("Loop the full arrangement"));
    connect(loopAction, &QAction::toggled, this, [this](bool enabled)
    {
        project.loopEnabled = enabled;
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });

    metronomeAction = new QAction(tr("Metronome"), this);
    metronomeAction->setCheckable(true);
    metronomeAction->setChecked(project.metronomeEnabled);
    metronomeAction->setToolTip(tr("Play an accented click on each beat"));
    connect(metronomeAction, &QAction::toggled, this, [this](bool enabled)
    {
        project.metronomeEnabled = enabled;
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });

    undoAction = undoStack.createUndoAction(this, tr("Undo"));
    undoAction->setShortcut(QKeySequence::Undo);
    redoAction = undoStack.createRedoAction(this, tr("Redo"));
    redoAction->setShortcut(QKeySequence::Redo);
}

void MainWindow::createToolBars()
{
    auto* projectBar = addToolBar(tr("Project"));
    projectBar->setObjectName(QStringLiteral("projectToolbar"));
    for (QAction* action : findChildren<QAction*>())
    {
        if (action->text() == tr("New") || action->text() == tr("Open") || action->text() == tr("Save"))
            projectBar->addAction(action);
    }
    projectBar->setMovable(false);

    auto* arrangementBar = addToolBar(tr("Arrangement"));
    arrangementBar->setObjectName(QStringLiteral("arrangementToolbar"));
    arrangementBar->setMovable(false);
    arrangementBar->addAction(addTrackAction);
    arrangementBar->addAction(addDrumTrackAction);
    arrangementBar->addAction(addPatternAction);
    arrangementBar->addSeparator();
    arrangementBar->addWidget(new QLabel(tr("CLIP LENGTH"), arrangementBar));
    clipLengthControl = new QDoubleSpinBox(arrangementBar);
    clipLengthControl->setRange(0.25, 16384.0);
    clipLengthControl->setDecimals(2);
    clipLengthControl->setSingleStep(1.0);
    clipLengthControl->setSuffix(tr(" beats"));
    clipLengthControl->setFixedWidth(118);
    clipLengthControl->setToolTip(tr("Length of the selected MIDI clip in quarter-note beats"));
    arrangementBar->addWidget(clipLengthControl);
    connect(clipLengthControl, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double length)
    {
        MidiClip* clip = selectedClip();
        if (clip == nullptr)
            return;
        clip->lengthBeats = length;
        project.normalize();
        const MidiClip* updatedClip = selectedClip();
        midiEditor->setPatternLengthBeats(updatedClip == nullptr ? project.patternLengthBeats() : updatedClip->lengthBeats);
        midiEditor->setNotes(updatedClip == nullptr ? QVector<MidiNote>{} : updatedClip->notes);
        stepSequencer->setPattern(updatedClip == nullptr ? QVector<MidiNote>{} : updatedClip->notes,
                                  updatedClip == nullptr ? project.patternLengthBeats() : updatedClip->lengthBeats,
                                  project.quarterNotesPerBar(), project.nextNoteId(), updatedClip != nullptr);
        arrangementEditor->setProject(project);
        arrangementEditor->setSelection(selectedTrackId, selectedClipId);
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });

    arrangementBar->addSeparator();
    automationViewAction = new QAction(tr("Automation lanes"), this);
    automationViewAction->setCheckable(true);
    automationViewAction->setToolTip(tr("Show and edit volume or pan automation in the Playlist"));
    arrangementBar->addAction(automationViewAction);
    automationLaneParameterControl = new QComboBox(arrangementBar);
    automationLaneParameterControl->addItem(tr("Volume"), 0);
    automationLaneParameterControl->addItem(tr("Pan"), 1);
    automationLaneParameterControl->setFixedWidth(88);
    automationLaneParameterControl->setEnabled(false);
    automationLaneParameterControl->setToolTip(tr("Automation parameter shown in the Playlist lanes"));
    arrangementBar->addWidget(automationLaneParameterControl);
    automationWriteAction = new QAction(tr("Write automation"), this);
    automationWriteAction->setCheckable(true);
    automationWriteAction->setToolTip(tr("Record the selected volume or pan control while the transport plays"));
    arrangementBar->addAction(automationWriteAction);
    automationWriteIdleTimer = new QTimer(this);
    automationWriteIdleTimer->setSingleShot(true);
    connect(automationWriteIdleTimer, &QTimer::timeout, this, &MainWindow::finishAutomationWrite);
    connect(automationViewAction, &QAction::toggled, this, [this](bool enabled)
    {
        automationLaneParameterControl->setEnabled(enabled || automationWriteAction->isChecked());
        arrangementEditor->setAutomationView(enabled,
                                              automationLaneParameterControl->currentData().toInt());
        if (enabled)
            statusBar()->showMessage(tr("Click a lane to add a point, drag points to edit, and right-click or press Delete to remove one."), 6000);
    });
    connect(automationWriteAction, &QAction::toggled, this, [this](bool enabled)
    {
        automationLaneParameterControl->setEnabled(enabled || automationViewAction->isChecked());
        if (enabled)
            statusBar()->showMessage(tr("Write is armed. Start playback and move the selected track control in the Mixer."), 6000);
        else
            finishAutomationWrite();
    });
    connect(automationLaneParameterControl, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int index)
    {
        if (automationWriteSession.active &&
            automationWriteSession.parameter != automationLaneParameterControl->itemData(index).toInt())
            finishAutomationWrite();
        arrangementEditor->setAutomationView(automationViewAction->isChecked(),
                                              automationLaneParameterControl->itemData(index).toInt());
    });

    auto* transportBar = addToolBar(tr("Transport"));
    transportBar->setObjectName(QStringLiteral("transportToolbar"));
    transportBar->setMovable(false);
    transportBar->addAction(playAction);
    transportBar->addAction(stopAction);
    transportBar->addAction(loopAction);
    transportBar->addSeparator();
    transportBar->addWidget(new QLabel(tr("TEMPO"), transportBar));
    tempoControl = new QDoubleSpinBox(transportBar);
    tempoControl->setRange(20.0, 300.0);
    tempoControl->setDecimals(1);
    tempoControl->setSingleStep(1.0);
    tempoControl->setSuffix(tr(" BPM"));
    tempoControl->setValue(project.tempoBpm);
    tempoControl->setFixedWidth(104);
    transportBar->addWidget(tempoControl);
    connect(tempoControl, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value)
    {
        project.tempoBpm = value;
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });
    transportBar->addSeparator();
    transportBar->addWidget(new QLabel(tr("METER"), transportBar));
    timeSignatureControl = new QComboBox(transportBar);
    const QVector<QPair<int, int>> commonMeters = {
        {4, 4}, {3, 4}, {2, 4}, {5, 4}, {6, 8}, {7, 8}, {9, 8}, {12, 8}, {2, 2}, {3, 8}
    };
    for (const auto& meter : commonMeters)
        timeSignatureControl->addItem(QStringLiteral("%1/%2").arg(meter.first).arg(meter.second),
                                      QVariantList{meter.first, meter.second});
    timeSignatureControl->setFixedWidth(76);
    timeSignatureControl->setToolTip(tr("Arrangement time signature"));
    transportBar->addWidget(timeSignatureControl);
    connect(timeSignatureControl, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index)
    {
        if (index < 0)
            return;
        const QVariantList meter = timeSignatureControl->itemData(index).toList();
        if (meter.size() != 2)
            return;
        project.beatsPerBar = meter[0].toInt();
        project.beatUnit = meter[1].toInt();
        project.normalize();
        const MidiClip* clip = selectedClip();
        midiEditor->setTimeSignature(project.beatsPerBar, project.beatUnit);
        midiEditor->setPatternLengthBeats(clip == nullptr ? project.patternLengthBeats() : clip->lengthBeats);
        stepSequencer->setPattern(clip == nullptr ? QVector<MidiNote>{} : clip->notes,
                                  clip == nullptr ? project.patternLengthBeats() : clip->lengthBeats,
                                  project.quarterNotesPerBar(), project.nextNoteId(), clip != nullptr);
        arrangementEditor->setProject(project);
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });
    transportBar->addSeparator();
    transportBar->addAction(metronomeAction);
    transportBar->addWidget(new QLabel(tr("COUNT-IN"), transportBar));
    countInControl = new QComboBox(transportBar);
    countInControl->addItem(tr("Off"), 0);
    countInControl->addItem(tr("1 bar"), 1);
    countInControl->addItem(tr("2 bars"), 2);
    countInControl->addItem(tr("3 bars"), 3);
    countInControl->addItem(tr("4 bars"), 4);
    countInControl->setCurrentIndex(0);
    countInControl->setFixedWidth(78);
    countInControl->setToolTip(tr("Bars of metronome count-in before playback"));
    transportBar->addWidget(countInControl);
    connect(countInControl, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index)
    {
        if (index < 0)
            return;
        project.countInBars = countInControl->itemData(index).toInt();
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });

    auto* recordingBar = addToolBar(tr("Recording"));
    recordingBar->setObjectName(QStringLiteral("recordingToolbar"));
    recordingBar->setMovable(false);
    recordingBar->addAction(recordAction);
    recordingBar->addSeparator();
    recordingBar->addWidget(new QLabel(tr("INPUT"), recordingBar));
    audioInputControl = new QComboBox(recordingBar);
    audioInputControl->setMinimumWidth(190);
    audioInputControl->setToolTip(tr("Audio device used for new recordings"));
    recordingBar->addWidget(audioInputControl);
    recordingBar->addSeparator();
    recordingBar->addAction(midiRecordAction);
    recordingBar->addAction(midiMonitorAction);
    recordingBar->addWidget(new QLabel(tr("MIDI INPUT"), recordingBar));
    midiInputControl = new QComboBox(recordingBar);
    midiInputControl->setMinimumWidth(190);
    midiInputControl->setToolTip(tr("MIDI device used for live monitoring and recording"));
    recordingBar->addWidget(midiInputControl);
    mediaDevices = new QMediaDevices(this);
    connect(mediaDevices, &QMediaDevices::audioInputsChanged,
            this, &MainWindow::refreshAudioInputs);
    refreshAudioInputs();
    midiInput = std::make_unique<MidiInput>();
    refreshMidiInputs();
    midiInputPollTimer = new QTimer(this);
    midiInputPollTimer->setInterval(5);
    connect(midiInputPollTimer, &QTimer::timeout, this, &MainWindow::pollMidiInput);

    auto* editorBar = addToolBar(tr("Piano roll"));
    editorBar->setObjectName(QStringLiteral("pianoRollToolbar"));
    editorBar->setMovable(false);
    editorBar->addWidget(new QLabel(tr("SNAP"), editorBar));
    snapControl = new QComboBox(editorBar);
    snapControl->addItem(tr("1/4"), 1.0);
    snapControl->addItem(tr("1/8"), 0.5);
    snapControl->addItem(tr("1/16"), 0.25);
    snapControl->addItem(tr("1/32"), 0.125);
    snapControl->setCurrentIndex(2);
    snapControl->setToolTip(tr("Grid snap interval"));
    editorBar->addWidget(snapControl);
    connect(snapControl, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index)
    {
        midiEditor->setSnap(snapControl->itemData(index).toDouble());
    });
    editorBar->addSeparator();
    editorBar->addWidget(new QLabel(tr("ZOOM"), editorBar));
    zoomControl = new QSlider(Qt::Horizontal, editorBar);
    zoomControl->setRange(48, 132);
    zoomControl->setValue(84);
    zoomControl->setFixedWidth(110);
    zoomControl->setToolTip(tr("Piano roll zoom"));
    editorBar->addWidget(zoomControl);
    connect(zoomControl, &QSlider::valueChanged, midiEditor, &MidiEditor::setPixelsPerBeat);
}

void MainWindow::createDocks()
{
    browserDock = new QDockWidget(tr("Browser"), this);
    browserDock->setObjectName(QStringLiteral("browserDock"));
    browserDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    auto* browser = new QWidget(browserDock);
    auto* browserLayout = new QVBoxLayout(browser);
    browserLayout->setContentsMargins(10, 12, 10, 10);
    browserLayout->setSpacing(8);
    auto* trackHeading = new QLabel(tr("INSTRUMENT TRACKS"), browser);
    trackHeading->setStyleSheet(QStringLiteral("color: #8d9aa7; font-size: 10px; font-weight: 700; letter-spacing: 1px;"));
    browserLayout->addWidget(trackHeading);
    trackList = new QListWidget(browser);
    trackList->setMinimumHeight(160);
    browserLayout->addWidget(trackList);
    connect(trackList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current)
    {
        if (current != nullptr)
            selectTrack(current->data(Qt::UserRole).toULongLong());
    });

    auto* buttonRow = new QHBoxLayout;
    auto* addTrackButton = new QPushButton(tr("Add track"), browser);
    auto* addPatternButton = new QPushButton(tr("New pattern"), browser);
    connect(addTrackButton, &QPushButton::clicked, this, &MainWindow::addTrack);
    connect(addPatternButton, &QPushButton::clicked, this, &MainWindow::addNewPattern);
    buttonRow->addWidget(addTrackButton);
    buttonRow->addWidget(addPatternButton);
    browserLayout->addLayout(buttonRow);
    auto* addDrumButton = new QPushButton(tr("Add drum rack"), browser);
    connect(addDrumButton, &QPushButton::clicked, this, &MainWindow::addDrumTrack);
    browserLayout->addWidget(addDrumButton);
    auto* importAudioButton = new QPushButton(tr("Import WAV audio"), browser);
    connect(importAudioButton, &QPushButton::clicked, this, &MainWindow::importAudio);
    browserLayout->addWidget(importAudioButton);

    instrumentHeading = new QLabel(tr("SELECTED INSTRUMENT"), browser);
    instrumentHeading->setStyleSheet(QStringLiteral("color: #8d9aa7; font-size: 10px; font-weight: 700; margin-top: 10px;"));
    browserLayout->addWidget(instrumentHeading);
    waveformControl = new QComboBox(browser);
    waveformControl->addItem(tr("Sine"), static_cast<int>(SynthWaveform::Sine));
    waveformControl->addItem(tr("Triangle"), static_cast<int>(SynthWaveform::Triangle));
    waveformControl->addItem(tr("Saw"), static_cast<int>(SynthWaveform::Saw));
    waveformControl->addItem(tr("Square"), static_cast<int>(SynthWaveform::Square));
    browserLayout->addWidget(waveformControl);
    connect(waveformControl, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index)
    {
        InstrumentTrack* track = selectedTrack();
        if (track == nullptr) return;
        track->waveform = static_cast<SynthWaveform>(waveformControl->itemData(index).toInt());
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });

    auto* synthEnvelopeGroup = new QGroupBox(tr("SYNTH ENVELOPE"), browser);
    synthEnvelopePanel = synthEnvelopeGroup;
    auto* synthEnvelopeForm = new QFormLayout(synthEnvelopeGroup);
    const std::array<QString, 4> envelopeLabels = {
        tr("Attack (s)"), tr("Decay (s)"), tr("Sustain"), tr("Release (s)")
    };
    for (int parameter = 0; parameter < static_cast<int>(envelopeControls.size()); ++parameter)
    {
        QDoubleSpinBox* control = new QDoubleSpinBox(synthEnvelopeGroup);
        control->setDecimals(parameter == 2 ? 2 : 3);
        control->setSingleStep(parameter == 2 ? 0.05 : 0.01);
        if (parameter != 2)
        {
            control->setRange(0.0, parameter == 3 ? 10.0 : 5.0);
            control->setSuffix(tr(" s"));
        }
        else
        {
            control->setRange(0.0, 1.0);
        }
        envelopeControls[static_cast<size_t>(parameter)] = control;
        synthEnvelopeForm->addRow(envelopeLabels[static_cast<size_t>(parameter)], control);
        connect(control, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, parameter](double value) { setSelectedSynthEnvelope(parameter, value); });
    }
    browserLayout->addWidget(synthEnvelopeGroup);

    auto* automationGroup = new QGroupBox(tr("TRACK AUTOMATION"), browser);
    automationEditorPanel = automationGroup;
    auto* automationLayout = new QVBoxLayout(automationGroup);
    automationParameterControl = new QComboBox(automationGroup);
    automationParameterControl->addItem(tr("Volume"), 0);
    automationParameterControl->addItem(tr("Pan"), 1);
    automationLayout->addWidget(automationParameterControl);
    automationPointsList = new QListWidget(automationGroup);
    automationPointsList->setMaximumHeight(110);
    automationLayout->addWidget(automationPointsList);
    auto* automationForm = new QFormLayout;
    automationBeatControl = new QDoubleSpinBox(automationGroup);
    automationBeatControl->setRange(0.0, 16384.0);
    automationBeatControl->setDecimals(2);
    automationBeatControl->setSingleStep(1.0);
    automationBeatControl->setSuffix(tr(" beats"));
    automationValueControl = new QDoubleSpinBox(automationGroup);
    automationValueControl->setRange(0.0, 100.0);
    automationValueControl->setDecimals(0);
    automationValueControl->setSingleStep(5.0);
    automationValueControl->setSuffix(tr("%"));
    automationValueLabel = new QLabel(tr("Volume (%)"), automationGroup);
    automationForm->addRow(tr("Position"), automationBeatControl);
    automationForm->addRow(automationValueLabel, automationValueControl);
    automationLayout->addLayout(automationForm);
    auto* automationButtons = new QHBoxLayout;
    auto* addAutomationButton = new QPushButton(tr("Add / replace keyframe"), automationGroup);
    removeAutomationPointButton = new QPushButton(tr("Remove"), automationGroup);
    automationButtons->addWidget(addAutomationButton, 1);
    automationButtons->addWidget(removeAutomationPointButton);
    automationLayout->addLayout(automationButtons);
    browserLayout->addWidget(automationGroup);
    connect(automationParameterControl, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { refreshAutomationEditor(); });
    connect(automationPointsList, &QListWidget::currentRowChanged, this, [this](int row)
    {
        const QListWidgetItem* item = automationPointsList->item(row);
        if (item == nullptr)
        {
            removeAutomationPointButton->setEnabled(false);
            return;
        }
        const QSignalBlocker beatBlocker(automationBeatControl);
        const QSignalBlocker valueBlocker(automationValueControl);
        automationBeatControl->setValue(item->data(Qt::UserRole).toDouble());
        automationValueControl->setValue(item->data(Qt::UserRole + 1).toDouble());
        removeAutomationPointButton->setEnabled(true);
    });
    connect(addAutomationButton, &QPushButton::clicked, this, &MainWindow::addAutomationPoint);
    connect(removeAutomationPointButton, &QPushButton::clicked, this, &MainWindow::removeAutomationPoint);

    audioRegionControls = new QGroupBox(tr("AUDIO REGION"), browser);
    auto* audioFadeForm = new QFormLayout(audioRegionControls);
    audioFadeInControl = new QDoubleSpinBox(audioRegionControls);
    audioFadeOutControl = new QDoubleSpinBox(audioRegionControls);
    for (QDoubleSpinBox* control : {audioFadeInControl, audioFadeOutControl})
    {
        control->setRange(0.0, 16384.0);
        control->setDecimals(2);
        control->setSingleStep(0.25);
        control->setSuffix(tr(" beats"));
    }
    audioFadeForm->addRow(tr("Fade in"), audioFadeInControl);
    audioFadeForm->addRow(tr("Fade out"), audioFadeOutControl);
    browserLayout->addWidget(audioRegionControls);
    audioRegionControls->hide();
    connect(audioFadeInControl, &QDoubleSpinBox::valueChanged, this,
            [this](double beats) { setSelectedAudioFade(true, beats); });
    connect(audioFadeOutControl, &QDoubleSpinBox::valueChanged, this,
            [this](double beats) { setSelectedAudioFade(false, beats); });

    drumSampleControls = new QGroupBox(tr("DRUM SAMPLES"), browser);
    auto* drumSampleLayout = new QVBoxLayout(drumSampleControls);
    const std::array<QString, 4> drumPadNames = {
        tr("Kick"), tr("Snare"), tr("Closed hat"), tr("Open hat")
    };
    for (int pad = 0; pad < static_cast<int>(drumPadNames.size()); ++pad)
    {
        auto* row = new QWidget(drumSampleControls);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        drumSampleButtons[static_cast<size_t>(pad)] = new QPushButton(
            tr("Load %1 sample…").arg(drumPadNames[static_cast<size_t>(pad)]), row);
        clearDrumSampleButtons[static_cast<size_t>(pad)] = new QPushButton(tr("Clear"), row);
        rowLayout->addWidget(drumSampleButtons[static_cast<size_t>(pad)], 1);
        rowLayout->addWidget(clearDrumSampleButtons[static_cast<size_t>(pad)]);
        drumSampleLayout->addWidget(row);
        connect(drumSampleButtons[static_cast<size_t>(pad)], &QPushButton::clicked, this,
                [this, pad] { importDrumSample(pad); });
        connect(clearDrumSampleButtons[static_cast<size_t>(pad)], &QPushButton::clicked, this,
                [this, pad] { clearDrumSample(pad); });
    }
    browserLayout->addWidget(drumSampleControls);
    drumSampleControls->hide();

    browserLayout->addStretch();
    auto* hint = new QLabel(tr("Import a WAV to add an audio track. Double-click MIDI clips to edit notes or drum steps."), browser);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #8996a2; font-size: 11px;"));
    browserLayout->addWidget(hint);
    auto* browserScrollArea = new QScrollArea(browserDock);
    browserScrollArea->setWidgetResizable(true);
    browserScrollArea->setFrameShape(QFrame::NoFrame);
    browserScrollArea->setWidget(browser);
    browserDock->setWidget(browserScrollArea);
    addDockWidget(Qt::LeftDockWidgetArea, browserDock);

    mixerDock = new QDockWidget(tr("Mixer"), this);
    mixerDock->setObjectName(QStringLiteral("mixerDock"));
    mixerDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    mixerScrollArea = new QScrollArea(mixerDock);
    mixerScrollArea->setWidgetResizable(true);
    mixerScrollArea->setFrameShape(QFrame::NoFrame);
    mixerDock->setWidget(mixerScrollArea);
    addDockWidget(Qt::RightDockWidgetArea, mixerDock);

    audioDeviceLabel = new QLabel(this);
    audioDeviceLabel->setContentsMargins(8, 0, 8, 0);
    playheadLabel = new QLabel(tr("BAR 01  ·  BEAT 1.00"), this);
    playheadLabel->setContentsMargins(8, 0, 8, 0);
    playheadLabel->setStyleSheet(QStringLiteral("font-variant-numeric: tabular-nums; color: #d8e0e8;"));
    statusBar()->addPermanentWidget(playheadLabel);
    statusBar()->addPermanentWidget(audioDeviceLabel);
}

void MainWindow::createMenus()
{
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    for (QAction* action : findChildren<QAction*>())
    {
        if (action->text() == tr("New") || action->text() == tr("Open") || action->text() == tr("Save"))
            fileMenu->addAction(action);
    }
    auto* saveAsAction = fileMenu->addAction(tr("Save project as…"));
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, [this]
    {
        QString path = QFileDialog::getSaveFileName(this, tr("Save project as"), projectPath,
                                                     tr("Awesome Studio project (*.aws)"));
        if (!path.isEmpty())
        {
            if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".aws");
            saveToPath(path);
        }
    });
    fileMenu->addSeparator();
    fileMenu->addAction(tr("Import WAV audio…"), this, &MainWindow::importAudio);
    fileMenu->addAction(tr("Import MIDI…"), this, &MainWindow::importMidi);
    fileMenu->addAction(tr("Export arrangement to MIDI…"), this, &MainWindow::exportMidi);
    fileMenu->addAction(tr("Export arrangement to WAV…"), this, &MainWindow::exportAudio);
    fileMenu->addSeparator();
    auto* quitAction = fileMenu->addAction(tr("Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::closeAllWindows);

    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(undoAction);
    editMenu->addAction(redoAction);
    editMenu->addSeparator();
    editMenu->addAction(tr("Quantize note starts to grid"), midiEditor, &MidiEditor::quantizeNotesToGrid);

    QMenu* patternMenu = menuBar()->addMenu(tr("&Track"));
    patternMenu->addAction(addTrackAction);
    patternMenu->addAction(addDrumTrackAction);
    patternMenu->addAction(addPatternAction);

    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(browserDock->toggleViewAction());
    viewMenu->addAction(mixerDock->toggleViewAction());

    QMenu* transportMenu = menuBar()->addMenu(tr("&Transport"));
    transportMenu->addAction(playAction);
    transportMenu->addAction(stopAction);
    transportMenu->addAction(recordAction);
    transportMenu->addAction(midiRecordAction);
    transportMenu->addAction(midiMonitorAction);
    transportMenu->addAction(loopAction);
    transportMenu->addAction(metronomeAction);

    QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("About Awesome Studio"), this, [this]
    {
        QMessageBox::about(this, tr("About Awesome Studio"),
                           tr("Awesome Studio\nA pattern-based MIDI workstation with a Playlist, instrument tracks, and a built-in synth."));
    });
}

InstrumentTrack* MainWindow::selectedTrack()
{
    return project.findTrack(selectedTrackId);
}

MidiClip* MainWindow::selectedClip()
{
    return project.findClip(selectedTrackId, selectedClipId);
}

void MainWindow::refreshProjectUi()
{
    if (automationWriteSession.active)
        finishAutomationWrite();
    if (project.findTrack(selectedTrackId) == nullptr)
        selectedTrackId = project.tracks.isEmpty() ? 0 : project.tracks.first().id;
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr)
    {
        selectedClipId = 0;
    }
    else if (project.findClip(selectedTrackId, selectedClipId) == nullptr)
    {
        selectedClipId = track->clips.isEmpty() ? 0 : track->clips.first().id;
    }

    const QSignalBlocker trackListBlocker(trackList);
    trackList->clear();
    for (const InstrumentTrack& item : project.tracks)
    {
        auto* listItem = new QListWidgetItem(item.name, trackList);
        listItem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(item.id));
        if (item.id == selectedTrackId)
            trackList->setCurrentItem(listItem);
    }

    const QSignalBlocker waveformBlocker(waveformControl);
    const int waveformIndex = track == nullptr ? 0 :
        waveformControl->findData(static_cast<int>(track->waveform));
    waveformControl->setCurrentIndex(std::max(0, waveformIndex));
    waveformControl->setEnabled(track != nullptr && track->instrumentType == InstrumentType::Synth);
    const bool synthTrackSelected = track != nullptr && track->instrumentType == InstrumentType::Synth;
    synthEnvelopePanel->setVisible(synthTrackSelected);
    const std::array<float, 4> envelopeValues = track == nullptr
        ? std::array<float, 4>{0.008f, 0.0f, 1.0f, 0.06f}
        : std::array<float, 4>{track->attackSeconds, track->decaySeconds,
                               track->sustainLevel, track->releaseSeconds};
    for (size_t index = 0; index < envelopeControls.size(); ++index)
    {
        const QSignalBlocker envelopeBlocker(envelopeControls[index]);
        envelopeControls[index]->setValue(envelopeValues[index]);
        envelopeControls[index]->setEnabled(synthTrackSelected);
    }
    refreshAutomationEditor();
    instrumentHeading->setText(track == nullptr ? tr("SELECTED INSTRUMENT") :
                               track->instrumentType == InstrumentType::DrumRack ? tr("DRUM RACK") :
                               track->instrumentType == InstrumentType::Audio ? tr("AUDIO SOURCE") :
                               tr("SELECTED INSTRUMENT"));

    const MidiClip* clip = selectedClip();
    const QSignalBlocker audioFadeInBlocker(audioFadeInControl);
    const QSignalBlocker audioFadeOutBlocker(audioFadeOutControl);
    const bool audioRegionSelected = clip != nullptr && clip->isAudioRegion();
    audioRegionControls->setVisible(audioRegionSelected);
    audioFadeInControl->setMaximum(audioRegionSelected ? clip->lengthBeats : 0.0);
    audioFadeOutControl->setMaximum(audioRegionSelected ? clip->lengthBeats : 0.0);
    audioFadeInControl->setValue(audioRegionSelected ? clip->audioFadeInBeats : 0.0);
    audioFadeOutControl->setValue(audioRegionSelected ? clip->audioFadeOutBeats : 0.0);
    audioFadeInControl->setEnabled(audioRegionSelected);
    audioFadeOutControl->setEnabled(audioRegionSelected);
    const bool drumRackSelected = track != nullptr && track->instrumentType == InstrumentType::DrumRack;
    drumSampleControls->setVisible(drumRackSelected);
    const std::array<QString, 4> drumPadNames = {
        tr("Kick"), tr("Snare"), tr("Closed hat"), tr("Open hat")
    };
    for (size_t pad = 0; pad < drumSampleButtons.size(); ++pad)
    {
        const DrumPadSample* sample = track == nullptr ? nullptr : &track->drumSamples[pad];
        const QString padName = drumPadNames[pad];
        drumSampleButtons[pad]->setText(sample == nullptr || sample->path.isEmpty()
            ? tr("Load %1 sample…").arg(padName)
            : sample->source != nullptr ? QFileInfo(sample->path).fileName()
                                        : tr("%1 · OFFLINE").arg(padName));
        drumSampleButtons[pad]->setEnabled(drumRackSelected);
        clearDrumSampleButtons[pad]->setEnabled(drumRackSelected && sample != nullptr &&
                                                !sample->path.isEmpty());
    }
    midiEditor->setTimeSignature(project.beatsPerBar, project.beatUnit);
    midiEditor->setPatternLengthBeats(clip == nullptr ? project.patternLengthBeats() : clip->lengthBeats);
    midiEditor->setNotes(clip == nullptr ? QVector<MidiNote>{} : clip->notes);
    const bool drumTrack = track != nullptr && track->instrumentType == InstrumentType::DrumRack;
    editorTabs->setTabEnabled(editorTabs->indexOf(stepSequencer), drumTrack);
    stepSequencer->setEnabled(drumTrack);
    stepSequencer->setPattern(clip == nullptr ? QVector<MidiNote>{} : clip->notes,
                              clip == nullptr ? project.patternLengthBeats() : clip->lengthBeats,
                              project.quarterNotesPerBar(), project.nextNoteId(), clip != nullptr);
    arrangementEditor->setProject(project);
    arrangementEditor->setSelection(selectedTrackId, selectedClipId);

    const QSignalBlocker tempoBlocker(tempoControl);
    const QSignalBlocker meterBlocker(timeSignatureControl);
    const QSignalBlocker countInBlocker(countInControl);
    const QSignalBlocker clipLengthBlocker(clipLengthControl);
    const QSignalBlocker loopBlocker(loopAction);
    const QSignalBlocker metronomeBlocker(metronomeAction);
    tempoControl->setValue(project.tempoBpm);
    const QVariantList currentMeter{project.beatsPerBar, project.beatUnit};
    int meterIndex = timeSignatureControl->findData(currentMeter);
    if (meterIndex < 0)
    {
        timeSignatureControl->addItem(QStringLiteral("%1/%2").arg(project.beatsPerBar).arg(project.beatUnit),
                                      currentMeter);
        meterIndex = timeSignatureControl->count() - 1;
    }
    timeSignatureControl->setCurrentIndex(meterIndex);
    const int countInIndex = countInControl->findData(project.countInBars);
    countInControl->setCurrentIndex(countInIndex < 0 ? 0 : countInIndex);
    metronomeAction->setChecked(project.metronomeEnabled);
    clipLengthControl->setEnabled(clip != nullptr);
    clipLengthControl->setValue(clip == nullptr ? project.patternLengthBeats() : clip->lengthBeats);
    loopAction->setChecked(project.loopEnabled);
    refreshMixer();
    audioEngine.setProject(project);
    audioEngine.setMasterGain(project.masterGain);
}

void MainWindow::refreshMixer()
{
    if (mixerScrollArea == nullptr)
        return;

    auto* content = new QWidget(mixerScrollArea);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto* masterBox = new QGroupBox(tr("Master"), content);
    auto* masterForm = new QFormLayout(masterBox);
    masterControl = new QSlider(Qt::Horizontal, masterBox);
    masterControl->setRange(0, 100);
    masterControl->setValue(static_cast<int>(std::round(project.masterGain * 100.0f)));
    masterGainReadout = new QLabel(tr("%1%").arg(masterControl->value()), masterBox);
    auto* masterRow = new QWidget(masterBox);
    auto* masterRowLayout = new QHBoxLayout(masterRow);
    masterRowLayout->setContentsMargins(0, 0, 0, 0);
    masterRowLayout->addWidget(masterControl, 1);
    masterRowLayout->addWidget(masterGainReadout);
    masterForm->addRow(tr("Level"), masterRow);
    layout->addWidget(masterBox);
    connect(masterControl, &QSlider::valueChanged, this, [this](int value)
    {
        project.masterGain = value / 100.0f;
        masterGainReadout->setText(tr("%1%").arg(value));
        audioEngine.setMasterGain(project.masterGain);
        audioEngine.setProject(project);
        settingsDirty = true;
        refreshDirtyState();
    });

    for (const InstrumentTrack& item : project.tracks)
    {
        const quint64 trackId = item.id;
        auto* group = new QGroupBox(item.name, content);
        auto* form = new QFormLayout(group);
        auto* stateRow = new QWidget(group);
        auto* stateLayout = new QHBoxLayout(stateRow);
        stateLayout->setContentsMargins(0, 0, 0, 0);
        auto* mute = new QCheckBox(tr("Mute"), stateRow);
        auto* solo = new QCheckBox(tr("Solo"), stateRow);
        mute->setChecked(item.muted);
        solo->setChecked(item.solo);
        stateLayout->addWidget(mute);
        stateLayout->addWidget(solo);
        form->addRow(stateRow);

        auto* volume = new QSlider(Qt::Horizontal, group);
        volume->setRange(0, 100);
        volume->setValue(static_cast<int>(std::round(item.gain * 100.0f)));
        auto* volumeLabel = new QLabel(tr("%1%").arg(volume->value()), group);
        auto* volumeRow = new QWidget(group);
        auto* volumeLayout = new QHBoxLayout(volumeRow);
        volumeLayout->setContentsMargins(0, 0, 0, 0);
        volumeLayout->addWidget(volume, 1);
        volumeLayout->addWidget(volumeLabel);
        form->addRow(tr("Level"), volumeRow);

        auto* pan = new QSlider(Qt::Horizontal, group);
        pan->setRange(-100, 100);
        pan->setValue(static_cast<int>(std::round(item.pan * 100.0f)));
        auto* panLabel = new QLabel(item.pan < -0.02f ? tr("L %1").arg(std::abs(pan->value())) :
                                    item.pan > 0.02f ? tr("R %1").arg(pan->value()) : tr("Center"), group);
        auto* panRow = new QWidget(group);
        auto* panLayout = new QHBoxLayout(panRow);
        panLayout->setContentsMargins(0, 0, 0, 0);
        panLayout->addWidget(pan, 1);
        panLayout->addWidget(panLabel);
        form->addRow(tr("Pan"), panRow);
        layout->addWidget(group);

        connect(volume, &QSlider::sliderPressed, this, [this, trackId, volume]
        {
            beginAutomationWrite(trackId, 0, volume->value() / 100.0f);
        });
        connect(volume, &QSlider::sliderReleased, this, [this, trackId]
        {
            if (automationWriteSession.active && automationWriteSession.trackId == trackId &&
                automationWriteSession.parameter == 0)
                finishAutomationWrite();
        });
        connect(pan, &QSlider::sliderPressed, this, [this, trackId, pan]
        {
            beginAutomationWrite(trackId, 1, pan->value() / 100.0f);
        });
        connect(pan, &QSlider::sliderReleased, this, [this, trackId]
        {
            if (automationWriteSession.active && automationWriteSession.trackId == trackId &&
                automationWriteSession.parameter == 1)
                finishAutomationWrite();
        });

        connect(mute, &QCheckBox::toggled, this, [this, trackId](bool enabled)
        {
            InstrumentTrack* track = project.findTrack(trackId);
            if (track == nullptr) return;
            track->muted = enabled;
            audioEngine.setProject(project);
            settingsDirty = true;
            refreshDirtyState();
        });
        connect(solo, &QCheckBox::toggled, this, [this, trackId](bool enabled)
        {
            InstrumentTrack* track = project.findTrack(trackId);
            if (track == nullptr) return;
            track->solo = enabled;
            audioEngine.setProject(project);
            settingsDirty = true;
            refreshDirtyState();
        });
        connect(volume, &QSlider::valueChanged, this, [this, trackId, volume, volumeLabel](int value)
        {
            InstrumentTrack* track = project.findTrack(trackId);
            if (track == nullptr) return;
            const float level = value / 100.0f;
            volumeLabel->setText(tr("%1%").arg(value));
            if (automationWriteSession.active && automationWriteSession.trackId == trackId &&
                automationWriteSession.parameter == 0)
            {
                recordAutomationValue(trackId, 0, level);
                if (!volume->isSliderDown())
                    automationWriteIdleTimer->start(250);
                return;
            }
            if (automationWriteAction->isChecked() && automationLaneParameterControl->currentData().toInt() == 0 &&
                audioEngine.isPlaying())
            {
                beginAutomationWrite(trackId, 0, track->gain);
                if (automationWriteSession.active && automationWriteSession.trackId == trackId &&
                    automationWriteSession.parameter == 0)
                {
                    recordAutomationValue(trackId, 0, level);
                    if (!volume->isSliderDown())
                        automationWriteIdleTimer->start(250);
                    return;
                }
            }
            track->gain = value / 100.0f;
            audioEngine.setProject(project);
            settingsDirty = true;
            refreshDirtyState();
        });
        connect(pan, &QSlider::valueChanged, this, [this, trackId, pan, panLabel](int value)
        {
            InstrumentTrack* track = project.findTrack(trackId);
            if (track == nullptr) return;
            const float balance = value / 100.0f;
            panLabel->setText(value < -2 ? tr("L %1").arg(-value) : value > 2 ? tr("R %1").arg(value) : tr("Center"));
            if (automationWriteSession.active && automationWriteSession.trackId == trackId &&
                automationWriteSession.parameter == 1)
            {
                recordAutomationValue(trackId, 1, balance);
                if (!pan->isSliderDown())
                    automationWriteIdleTimer->start(250);
                return;
            }
            if (automationWriteAction->isChecked() && automationLaneParameterControl->currentData().toInt() == 1 &&
                audioEngine.isPlaying())
            {
                beginAutomationWrite(trackId, 1, track->pan);
                if (automationWriteSession.active && automationWriteSession.trackId == trackId &&
                    automationWriteSession.parameter == 1)
                {
                    recordAutomationValue(trackId, 1, balance);
                    if (!pan->isSliderDown())
                        automationWriteIdleTimer->start(250);
                    return;
                }
            }
            track->pan = value / 100.0f;
            audioEngine.setProject(project);
            settingsDirty = true;
            refreshDirtyState();
        });
    }
    layout->addStretch();
    mixerScrollArea->setWidget(content);
}

void MainWindow::selectTrack(quint64 trackId)
{
    InstrumentTrack* track = project.findTrack(trackId);
    if (track == nullptr)
        return;
    selectedTrackId = trackId;
    selectedClipId = track->clips.isEmpty() ? 0 : track->clips.first().id;
    refreshProjectUi();
    if (track->instrumentType == InstrumentType::DrumRack)
        editorTabs->setCurrentWidget(stepSequencer);
    else if (track->instrumentType == InstrumentType::Audio)
        editorTabs->setCurrentWidget(arrangementEditor);
}

void MainWindow::selectClip(quint64 trackId, quint64 clipId, bool openPianoRoll)
{
    if (project.findClip(trackId, clipId) == nullptr)
        return;
    selectedTrackId = trackId;
    selectedClipId = clipId;
    refreshProjectUi();
    if (openPianoRoll)
    {
        const InstrumentTrack* track = project.findTrack(trackId);
        if (track != nullptr && track->instrumentType == InstrumentType::Audio)
            editorTabs->setCurrentWidget(arrangementEditor);
        else
            editorTabs->setCurrentWidget(track != nullptr && track->instrumentType == InstrumentType::DrumRack
                                             ? static_cast<QWidget*>(stepSequencer)
                                             : static_cast<QWidget*>(midiEditor));
    }
}

void MainWindow::addTrack()
{
    if (project.tracks.size() >= 256)
    {
        statusBar()->showMessage(tr("The project has reached the 256-track limit"), 3000);
        return;
    }
    InstrumentTrack track;
    track.id = project.nextTrackId();
    track.name = tr("Synth %1").arg(track.id);
    project.tracks.push_back(track);
    selectedTrackId = track.id;
    selectedClipId = 0;
    settingsDirty = true;
    refreshProjectUi();
    statusBar()->showMessage(tr("Added %1").arg(track.name), 1800);
}

void MainWindow::addDrumTrack()
{
    if (project.tracks.size() >= 256)
    {
        statusBar()->showMessage(tr("The project has reached the 256-track limit"), 3000);
        return;
    }
    InstrumentTrack track;
    track.id = project.nextTrackId();
    track.name = tr("Drum Rack %1").arg(track.id);
    track.instrumentType = InstrumentType::DrumRack;
    track.gain = 0.8f;
    MidiClip clip;
    clip.id = project.nextClipId();
    clip.name = tr("Beat %1").arg(clip.id);
    clip.lengthBeats = project.patternLengthBeats();
    track.clips.push_back(std::move(clip));
    project.tracks.push_back(std::move(track));
    selectedTrackId = project.tracks.last().id;
    selectedClipId = project.tracks.last().clips.first().id;
    settingsDirty = true;
    refreshProjectUi();
    editorTabs->setCurrentWidget(stepSequencer);
    statusBar()->showMessage(tr("Added a drum rack with kick, snare, closed-hat, and open-hat sounds"), 2500);
}

void MainWindow::addClipAt(quint64 trackId, double startBeat)
{
    InstrumentTrack* track = project.findTrack(trackId);
    if (track == nullptr)
        return;
    if (track->instrumentType == InstrumentType::Audio)
    {
        statusBar()->showMessage(tr("Import a WAV file to add an audio region to this track"), 2500);
        return;
    }
    MidiClip clip;
    clip.id = project.nextClipId();
    clip.name = tr("Pattern %1").arg(clip.id);
    clip.startBeat = std::max(0.0, startBeat);
    clip.lengthBeats = project.patternLengthBeats();
    track->clips.push_back(clip);
    selectedTrackId = trackId;
    selectedClipId = clip.id;
    settingsDirty = true;
    refreshProjectUi();
    statusBar()->showMessage(tr("Added %1 to the Playlist").arg(clip.name), 1800);
}

void MainWindow::addNewPattern()
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr)
    {
        addTrack();
        track = selectedTrack();
    }
    if (track == nullptr)
        return;
    if (track->instrumentType == InstrumentType::Audio)
    {
        statusBar()->showMessage(tr("Use Import WAV audio to add another audio region"), 2500);
        return;
    }

    double startBeat = 0.0;
    for (const MidiClip& clip : track->clips)
        startBeat = std::max(startBeat, clip.startBeat + clip.lengthBeats);
    const double barLength = project.quarterNotesPerBar();
    startBeat = std::ceil(startBeat / barLength) * barLength;
    addClipAt(track->id, startBeat);
}

void MainWindow::removeClip(quint64 trackId, quint64 clipId)
{
    InstrumentTrack* track = project.findTrack(trackId);
    if (track == nullptr)
        return;
    for (auto it = track->clips.begin(); it != track->clips.end(); ++it)
    {
        if (it->id == clipId)
        {
            track->clips.erase(it);
            if (selectedTrackId == trackId && selectedClipId == clipId)
                selectedClipId = track->clips.isEmpty() ? 0 : track->clips.first().id;
            settingsDirty = true;
            refreshProjectUi();
            statusBar()->showMessage(tr("Pattern removed"), 1600);
            return;
        }
    }
}

void MainWindow::moveClip(quint64 sourceTrackId, quint64 targetTrackId, quint64 clipId, double startBeat)
{
    InstrumentTrack* source = project.findTrack(sourceTrackId);
    InstrumentTrack* target = project.findTrack(targetTrackId);
    if (source == nullptr || target == nullptr)
        return;
    const MidiClip* sourceClip = project.findClip(sourceTrackId, clipId);
    if (sourceClip == nullptr || sourceClip->isAudioRegion() !=
        (target->instrumentType == InstrumentType::Audio))
    {
        statusBar()->showMessage(tr("Audio regions stay on audio tracks; MIDI patterns stay on instrument tracks"), 2500);
        return;
    }
    MidiClip movedClip;
    bool found = false;
    for (auto it = source->clips.begin(); it != source->clips.end(); ++it)
    {
        if (it->id == clipId)
        {
            movedClip = *it;
            source->clips.erase(it);
            found = true;
            break;
        }
    }
    if (!found)
        return;
    movedClip.startBeat = std::clamp(startBeat, 0.0, 100000.0);
    target->clips.push_back(movedClip);
    selectedTrackId = targetTrackId;
    selectedClipId = clipId;
    settingsDirty = true;
    refreshProjectUi();
    refreshDirtyState();
}

void MainWindow::trimAudioRegion(quint64 trackId, quint64 clipId, double startBeat,
                                 double lengthBeats, qint64 audioStartFrame)
{
    InstrumentTrack* track = project.findTrack(trackId);
    MidiClip* clip = project.findClip(trackId, clipId);
    if (track == nullptr || clip == nullptr || track->instrumentType != InstrumentType::Audio ||
        !clip->isAudioRegion())
        return;

    clip->startBeat = std::clamp(startBeat, 0.0, 16384.0);
    clip->lengthBeats = std::clamp(lengthBeats, 0.25, 16384.0);
    clip->audioStartFrame = std::max<qint64>(0, audioStartFrame);
    project.normalize();
    settingsDirty = true;
    refreshProjectUi();
    refreshDirtyState();
    statusBar()->showMessage(tr("Audio region trimmed"), 1800);
}

void MainWindow::setSelectedAudioFade(bool fadeIn, double beats)
{
    MidiClip* clip = selectedClip();
    if (clip == nullptr || !clip->isAudioRegion())
        return;

    const double fadeBeats = std::clamp(beats, 0.0, clip->lengthBeats);
    if (fadeIn)
        clip->audioFadeInBeats = fadeBeats;
    else
        clip->audioFadeOutBeats = fadeBeats;
    audioEngine.setProject(project);
    arrangementEditor->setProject(project);
    arrangementEditor->setSelection(selectedTrackId, selectedClipId);
    settingsDirty = true;
    refreshDirtyState();
}

void MainWindow::setSelectedSynthEnvelope(int parameter, double value)
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType != InstrumentType::Synth)
        return;
    switch (parameter)
    {
        case 0: track->attackSeconds = static_cast<float>(value); break;
        case 1: track->decaySeconds = static_cast<float>(value); break;
        case 2: track->sustainLevel = static_cast<float>(value); break;
        case 3: track->releaseSeconds = static_cast<float>(value); break;
        default: return;
    }
    audioEngine.setProject(project);
    settingsDirty = true;
    refreshDirtyState();
}

void MainWindow::refreshAutomationEditor()
{
    if (automationPointsList == nullptr)
        return;
    InstrumentTrack* track = selectedTrack();
    const bool volume = automationParameterControl->currentData().toInt() == 0;
    QVector<AutomationPoint>* points = track == nullptr ? nullptr :
        (volume ? &track->volumeAutomation : &track->panAutomation);
    automationEditorPanel->setEnabled(track != nullptr);
    automationValueLabel->setText(volume ? tr("Volume (%)") : tr("Pan (%)"));
    automationValueControl->setRange(volume ? 0.0 : -100.0, 100.0);
    automationValueControl->setSingleStep(volume ? 5.0 : 10.0);

    const QSignalBlocker listBlocker(automationPointsList);
    automationPointsList->clear();
    if (points != nullptr)
    {
        for (const AutomationPoint& point : *points)
        {
            auto* item = new QListWidgetItem(automationPointsList);
            const double displayedValue = point.value * 100.0;
            const QString valueText = volume ? tr("%1%").arg(displayedValue, 0, 'f', 0) :
                displayedValue < -1.0 ? tr("L %1").arg(std::abs(displayedValue), 0, 'f', 0) :
                displayedValue > 1.0 ? tr("R %1").arg(displayedValue, 0, 'f', 0) : tr("Center");
            item->setText(tr("Beat %1  ·  %2").arg(point.beat, 0, 'f', 2).arg(valueText));
            item->setData(Qt::UserRole, point.beat);
            item->setData(Qt::UserRole + 1, displayedValue);
        }
    }
    automationBeatControl->setEnabled(track != nullptr);
    automationValueControl->setEnabled(track != nullptr);
    removeAutomationPointButton->setEnabled(false);
}

void MainWindow::addAutomationPoint()
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr)
        return;
    const bool volume = automationParameterControl->currentData().toInt() == 0;
    const int parameter = volume ? 0 : 1;
    const QVector<AutomationPoint>& currentPoints = volume ? track->volumeAutomation : track->panAutomation;
    const QVector<AutomationPoint> before = currentPoints;
    QVector<AutomationPoint> after = before;
    const AutomationPoint point{automationBeatControl->value(),
                                static_cast<float>(automationValueControl->value() / 100.0)};
    auto position = std::lower_bound(after.begin(), after.end(), point.beat,
        [](const AutomationPoint& current, double beat) { return current.beat < beat; });
    if (position != after.end() && std::abs(position->beat - point.beat) < 1.0e-9)
    {
        if (std::abs(position->value - point.value) < 1.0e-6f)
            return;
        position->value = point.value;
    }
    else
    {
        if (after.size() >= 10000)
        {
            statusBar()->showMessage(tr("This track has reached the 10,000 keyframe limit for that parameter."), 4000);
            return;
        }
        after.insert(position, point);
    }
    undoStack.push(new AutomationEditCommand(this, track->id, parameter, before, after));
}

void MainWindow::removeAutomationPoint()
{
    InstrumentTrack* track = selectedTrack();
    const QListWidgetItem* item = automationPointsList->currentItem();
    if (track == nullptr || item == nullptr)
        return;
    const bool volume = automationParameterControl->currentData().toInt() == 0;
    const int parameter = volume ? 0 : 1;
    const QVector<AutomationPoint>& currentPoints = volume ? track->volumeAutomation : track->panAutomation;
    const QVector<AutomationPoint> before = currentPoints;
    QVector<AutomationPoint> after = before;
    const double beat = item->data(Qt::UserRole).toDouble();
    after.erase(std::remove_if(after.begin(), after.end(), [beat](const AutomationPoint& point)
    {
        return std::abs(point.beat - beat) < 1.0e-9;
    }), after.end());
    if (after.size() == before.size())
        return;
    undoStack.push(new AutomationEditCommand(this, track->id, parameter, before, after));
}

void MainWindow::beginAutomationWrite(quint64 trackId, int parameter, float value)
{
    if (automationWriteSession.active || automationWriteAction == nullptr ||
        !automationWriteAction->isChecked() || !audioEngine.isPlaying() ||
        automationLaneParameterControl->currentData().toInt() != parameter)
        return;
    InstrumentTrack* track = project.findTrack(trackId);
    const double beat = audioEngine.positionBeats();
    if (track == nullptr || beat < 0.0)
        return;

    QVector<AutomationPoint>& points = parameter == 0 ? track->volumeAutomation : track->panAutomation;
    automationWriteSession.active = true;
    automationWriteSession.changed = false;
    automationWriteSession.trackId = trackId;
    automationWriteSession.parameter = parameter;
    automationWriteSession.initialValue = value;
    automationWriteSession.lastSampleBeat = beat;
    automationWriteSession.lastPointBeat = beat;
    automationWriteSession.before = points;
    if (!upsertAutomationPoint(points, beat, value))
    {
        automationWriteSession = {};
        statusBar()->showMessage(tr("This track has reached the 10,000 keyframe limit for that parameter."), 4000);
        return;
    }
    audioEngine.setLiveTrackMixOverride(trackId, parameter, value);
}

void MainWindow::recordAutomationValue(quint64 trackId, int parameter, float value)
{
    if (!automationWriteSession.active || automationWriteSession.trackId != trackId ||
        automationWriteSession.parameter != parameter)
        return;
    const double beat = audioEngine.positionBeats();
    if (beat < 0.0)
        return;
    InstrumentTrack* track = project.findTrack(trackId);
    if (track == nullptr)
        return;

    if (beat + 1.0e-6 < automationWriteSession.lastSampleBeat)
    {
        automationWriteSession.lastPointBeat = -1.0;
        automationWriteSession.lastSampleBeat = -1.0;
    }
    double writeBeat = beat;
    const bool coalesce = automationWriteSession.lastPointBeat >= 0.0 &&
                          beat >= automationWriteSession.lastPointBeat &&
                          beat - automationWriteSession.lastPointBeat < automationWritePointSpacingBeats;
    if (coalesce)
        writeBeat = automationWriteSession.lastPointBeat;

    QVector<AutomationPoint>& points = parameter == 0 ? track->volumeAutomation : track->panAutomation;
    if (!upsertAutomationPoint(points, writeBeat, value))
        statusBar()->showMessage(tr("This track has reached the 10,000 keyframe limit for that parameter."), 4000);
    else if (!coalesce)
        automationWriteSession.lastPointBeat = writeBeat;
    automationWriteSession.lastSampleBeat = beat;
    automationWriteSession.changed = automationWriteSession.changed ||
                                     std::abs(value - automationWriteSession.initialValue) > 1.0e-6f;
    audioEngine.setLiveTrackMixOverride(trackId, parameter, value);
}

void MainWindow::finishAutomationWrite()
{
    if (!automationWriteSession.active)
        return;
    automationWriteIdleTimer->stop();
    AutomationWriteSession session = automationWriteSession;
    automationWriteSession = {};
    audioEngine.clearLiveTrackMixOverride(session.trackId, session.parameter);

    InstrumentTrack* track = project.findTrack(session.trackId);
    if (track == nullptr)
        return;
    const QVector<AutomationPoint>& current = session.parameter == 0
        ? track->volumeAutomation : track->panAutomation;
    if (!session.changed || sameAutomationPoints(session.before, current))
    {
        if (session.parameter == 0)
            track->volumeAutomation = session.before;
        else
            track->panAutomation = session.before;
        audioEngine.setProject(project);
        return;
    }

    const QVector<AutomationPoint> after = current;
    undoStack.push(new AutomationEditCommand(this, session.trackId, session.parameter,
                                             session.before, after));
    statusBar()->showMessage(tr("Recorded %1 automation for %2")
                                 .arg(session.parameter == 0 ? tr("volume") : tr("pan"))
                                 .arg(track->name), 2500);
}

void MainWindow::applyAutomationHistory(quint64 trackId, int parameter,
                                        const QVector<AutomationPoint>& points)
{
    InstrumentTrack* track = project.findTrack(trackId);
    if (track == nullptr)
        return;
    if (parameter == 0)
        track->volumeAutomation = points;
    else if (parameter == 1)
        track->panAutomation = points;
    else
        return;
    audioEngine.setProject(project);
    arrangementEditor->setProject(project);
    arrangementEditor->setSelection(selectedTrackId, selectedClipId);
    refreshAutomationEditor();
    refreshDirtyState();
}

bool MainWindow::maybeSave()
{
    finishAutomationWrite();
    if (audioRecorder || recordingPending)
        finishAudioRecording(true);
    if (midiRecordingActive || midiRecordingPending)
        finishMidiRecording(true);
    if (!isWindowModified())
        return true;

    const QMessageBox::StandardButton choice = QMessageBox::warning(
        this, tr("Unsaved project"), tr("Save changes to %1?").arg(project.title),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Save)
    {
        if (projectPath.isEmpty())
        {
            QString path = QFileDialog::getSaveFileName(this, tr("Save project"), QString(),
                                                         tr("Awesome Studio project (*.aws)"));
            if (path.isEmpty()) return false;
            if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".aws");
            return saveToPath(path);
        }
        return saveToPath(projectPath);
    }
    return choice == QMessageBox::Discard;
}

void MainWindow::setProject(const ProjectData& incoming, const QString& path)
{
    finishAutomationWrite();
    audioEngine.stopPlayback();
    project = incoming;
    project.normalize();
    projectPath = path;
    settingsDirty = false;
    undoStack.clear();
    undoStack.setClean();
    selectedTrackId = project.tracks.isEmpty() ? 0 : project.tracks.first().id;
    selectedClipId = project.tracks.isEmpty() || project.tracks.first().clips.isEmpty()
        ? 0 : project.tracks.first().clips.first().id;
    refreshProjectUi();
    playAction->setChecked(false);
    midiEditor->setPlayheadBeat(0.0);
    arrangementEditor->setPlayheadBeat(0.0);
    updateProjectTitle();
    int offlineSources = 0;
    for (const InstrumentTrack& track : project.tracks)
    {
        for (const MidiClip& clip : track.clips)
            if (clip.isAudioRegion() && clip.audioSource == nullptr)
                ++offlineSources;
        for (const DrumPadSample& sample : track.drumSamples)
            if (!sample.path.isEmpty() && sample.source == nullptr)
                ++offlineSources;
    }
    if (offlineSources > 0)
        statusBar()->showMessage(tr("Project opened with %1 offline audio source reference(s); restore media or reduce loaded audio.")
                                     .arg(offlineSources), 6000);
}

bool MainWindow::saveToPath(const QString& path)
{
    finishAutomationWrite();
    if (audioRecorder || recordingPending)
        finishAudioRecording(true);
    if (midiRecordingActive || midiRecordingPending)
        finishMidiRecording(true);
    QString savePath = path;
    if (QFileInfo(savePath).suffix().isEmpty())
        savePath += QStringLiteral(".aws");
    const QString previousTitle = project.title;
    project.title = QFileInfo(savePath).completeBaseName();
    QString error;
    if (!ProjectFile::save(savePath, project, &error))
    {
        project.title = previousTitle;
        QMessageBox::critical(this, tr("Could not save project"), error);
        return false;
    }

    projectPath = savePath;
    settingsDirty = false;
    undoStack.setClean();
    audioEngine.setProject(project);
    updateProjectTitle();
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(savePath).fileName()), 2500);
    return true;
}

void MainWindow::updateProjectTitle()
{
    const QString name = projectPath.isEmpty() ? project.title : QFileInfo(projectPath).completeBaseName();
    setWindowTitle(tr("Awesome Studio — %1[*]").arg(name));
    refreshDirtyState();
}

void MainWindow::refreshDirtyState()
{
    setWindowModified(settingsDirty || !undoStack.isClean());
}

void MainWindow::applyNoteHistory(quint64 trackId, quint64 clipId, const QVector<MidiNote>& notes,
                                  double clipLengthBeats)
{
    MidiClip* clip = project.findClip(trackId, clipId);
    if (clip == nullptr)
        return;
    clip->notes = notes;
    if (selectedTrackId == trackId && selectedClipId == clipId)
    {
        if (clipLengthBeats >= 0.0)
        {
            clip->lengthBeats = std::clamp(clipLengthBeats, 0.25, 16384.0);
            midiEditor->setPatternLengthBeats(clip->lengthBeats);
            const QSignalBlocker lengthBlocker(clipLengthControl);
            clipLengthControl->setValue(clip->lengthBeats);
        }
        midiEditor->setNotes(notes);
    }
    else if (clipLengthBeats >= 0.0)
    {
        clip->lengthBeats = std::clamp(clipLengthBeats, 0.25, 16384.0);
    }
    const MidiClip* currentClip = selectedClip();
    stepSequencer->setPattern(currentClip == nullptr ? QVector<MidiNote>{} : currentClip->notes,
                              currentClip == nullptr ? project.patternLengthBeats() : currentClip->lengthBeats,
                              project.quarterNotesPerBar(), project.nextNoteId(), currentClip != nullptr);
    arrangementEditor->setProject(project);
    arrangementEditor->setSelection(selectedTrackId, selectedClipId);
    audioEngine.setProject(project);
    refreshDirtyState();
}

void MainWindow::exportAudio()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Export arrangement to WAV"),
                                                 project.title + QStringLiteral(".wav"),
                                                 tr("WAV audio (*.wav)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".wav");

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool success = WavExporter::exportProject(project, path, &error);
    QApplication::restoreOverrideCursor();
    if (!success)
    {
        QMessageBox::critical(this, tr("Could not export WAV"), error);
        return;
    }
    statusBar()->showMessage(tr("Exported %1").arg(QFileInfo(path).fileName()), 3500);
}

void MainWindow::importAudio()
{
    if (audioRecorder || recordingPending)
    {
        statusBar()->showMessage(tr("Finish the active recording before importing more audio."), 3000);
        return;
    }
    if (project.tracks.size() >= 256)
    {
        statusBar()->showMessage(tr("The project has reached the 256-track limit"), 3000);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Import WAV audio"), QString(),
                                                       tr("WAV audio (*.wav)"));
    if (path.isEmpty())
        return;

    std::shared_ptr<const AudioAsset> asset = findProjectAudioAsset(path);
    QString error;
    if (asset == nullptr && !AudioFile::loadWav(path, asset, &error))
    {
        QMessageBox::critical(this, tr("Could not import WAV audio"), error);
        return;
    }
    if (findProjectAudioAsset(path) == nullptr)
    {
        const quint64 currentBytes = projectAudioBytes();
        const quint64 assetBytes = static_cast<quint64>(asset->samples.size() + asset->waveformPeaks.size()) *
                                   sizeof(float);
        if (currentBytes > AudioFile::maximumProjectDecodedBytes ||
            assetBytes > AudioFile::maximumProjectDecodedBytes - currentBytes)
        {
            QMessageBox::warning(this, tr("Audio memory limit"),
                                 tr("This project can load up to 512 MiB of decoded audio. Remove another audio source first."));
            return;
        }
    }

    InstrumentTrack track;
    track.id = project.nextTrackId();
    track.name = QFileInfo(path).completeBaseName();
    track.instrumentType = InstrumentType::Audio;
    MidiClip clip;
    clip.id = project.nextClipId();
    clip.name = QFileInfo(path).fileName();
    clip.audioSourcePath = asset->path;
    clip.audioSource = std::move(asset);
    const double durationBeats = static_cast<double>(clip.audioSource->frameCount()) /
                                 clip.audioSource->sampleRate * project.tempoBpm / 60.0;
    clip.lengthBeats = std::clamp(durationBeats, 0.25, 16384.0);
    track.clips.push_back(std::move(clip));
    project.tracks.push_back(std::move(track));
    selectedTrackId = project.tracks.last().id;
    selectedClipId = project.tracks.last().clips.first().id;
    settingsDirty = true;
    refreshProjectUi();
    editorTabs->setCurrentWidget(arrangementEditor);
    statusBar()->showMessage(tr("Imported %1 into an audio track").arg(QFileInfo(path).fileName()), 3000);
}

void MainWindow::importDrumSample(int pad)
{
    if (audioRecorder || recordingPending)
    {
        statusBar()->showMessage(tr("Finish the active recording before loading more audio."), 3000);
        return;
    }
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType != InstrumentType::DrumRack ||
        pad < 0 || pad >= static_cast<int>(track->drumSamples.size()))
        return;

    const QString path = QFileDialog::getOpenFileName(this, tr("Load drum sample"), QString(),
                                                       tr("WAV audio (*.wav)"));
    if (path.isEmpty())
        return;

    std::shared_ptr<const AudioAsset> asset = findProjectAudioAsset(path);
    QString error;
    if (asset == nullptr && !AudioFile::loadWav(path, asset, &error))
    {
        QMessageBox::critical(this, tr("Could not load drum sample"), error);
        return;
    }
    DrumPadSample& drumSample = track->drumSamples[static_cast<size_t>(pad)];
    const DrumPadSample previousSample = drumSample;
    drumSample.path = asset->path;
    drumSample.source = asset;
    if (projectAudioBytes() > AudioFile::maximumProjectDecodedBytes)
    {
        drumSample = previousSample;
        QMessageBox::warning(this, tr("Audio memory limit"),
                             tr("This project can load up to 512 MiB of decoded audio. Remove another audio source first."));
        return;
    }
    settingsDirty = true;
    refreshProjectUi();
    statusBar()->showMessage(tr("Drum sample set: %1")
                                 .arg(QFileInfo(drumSample.path).fileName()), 2500);
}

void MainWindow::clearDrumSample(int pad)
{
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType != InstrumentType::DrumRack ||
        pad < 0 || pad >= static_cast<int>(track->drumSamples.size()))
        return;

    track->drumSamples[static_cast<size_t>(pad)] = DrumPadSample{};
    settingsDirty = true;
    refreshProjectUi();
    statusBar()->showMessage(tr("Using synthesized sound for this drum pad"), 2000);
}

quint64 MainWindow::projectAudioBytes() const
{
    std::unordered_set<const AudioAsset*> uniqueAssets;
    quint64 totalBytes = 0;
    const auto addAsset = [&uniqueAssets, &totalBytes](const std::shared_ptr<const AudioAsset>& asset)
    {
        if (asset == nullptr || !uniqueAssets.insert(asset.get()).second)
            return;
        const quint64 sampleCount = static_cast<quint64>(asset->samples.size()) +
                                    static_cast<quint64>(asset->waveformPeaks.size());
        if (totalBytes > AudioFile::maximumProjectDecodedBytes ||
            sampleCount > (AudioFile::maximumProjectDecodedBytes - totalBytes) / sizeof(float))
            totalBytes = AudioFile::maximumProjectDecodedBytes + 1;
        else
            totalBytes += sampleCount * sizeof(float);
    };

    for (const InstrumentTrack& track : project.tracks)
    {
        for (const MidiClip& clip : track.clips)
            addAsset(clip.audioSource);
        for (const DrumPadSample& sample : track.drumSamples)
            addAsset(sample.source);
    }
    return totalBytes;
}

std::shared_ptr<const AudioAsset> MainWindow::findProjectAudioAsset(const QString& path) const
{
    const QFileInfo requestedInfo(path);
    const QString requestedPath = requestedInfo.canonicalFilePath().isEmpty()
        ? requestedInfo.absoluteFilePath() : requestedInfo.canonicalFilePath();
    const auto matchesPath = [&requestedPath](const std::shared_ptr<const AudioAsset>& asset)
    {
        if (asset == nullptr)
            return false;
        const QFileInfo assetInfo(asset->path);
        const QString assetPath = assetInfo.canonicalFilePath().isEmpty()
            ? assetInfo.absoluteFilePath() : assetInfo.canonicalFilePath();
        return assetPath == requestedPath;
    };

    for (const InstrumentTrack& track : project.tracks)
    {
        for (const MidiClip& clip : track.clips)
            if (matchesPath(clip.audioSource)) return clip.audioSource;
        for (const DrumPadSample& sample : track.drumSamples)
            if (matchesPath(sample.source)) return sample.source;
    }
    return {};
}

void MainWindow::importMidi()
{
    if (!maybeSave())
        return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Import MIDI file"), QString(),
                                                       tr("Standard MIDI files (*.mid *.midi)"));
    if (path.isEmpty())
        return;

    ProjectData imported;
    QString error;
    if (!MidiFile::load(path, imported, &error))
    {
        QMessageBox::critical(this, tr("Could not import MIDI"), error);
        return;
    }
    setProject(imported);
    statusBar()->showMessage(tr("Imported %1 MIDI tracks from %2")
                                 .arg(project.tracks.size()).arg(QFileInfo(path).fileName()), 4000);
}

void MainWindow::exportMidi()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Export arrangement to MIDI"),
                                                 project.title + QStringLiteral(".mid"),
                                                 tr("Standard MIDI files (*.mid *.midi)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".mid");

    QString error;
    if (!MidiFile::save(path, project, &error))
    {
        QMessageBox::critical(this, tr("Could not export MIDI"), error);
        return;
    }
    statusBar()->showMessage(tr("Exported %1").arg(QFileInfo(path).fileName()), 3500);
}

void MainWindow::refreshAudioInputs()
{
    if (audioInputControl == nullptr)
        return;

    const QByteArray previousId = audioInputControl->currentData().toByteArray();
    const QList<QAudioDevice> devices = QMediaDevices::audioInputs();
    const QSignalBlocker blocker(audioInputControl);
    audioInputControl->clear();

    int selectedIndex = -1;
    int defaultIndex = -1;
    for (const QAudioDevice& device : devices)
    {
        audioInputControl->addItem(device.description(), device.id());
        const int index = audioInputControl->count() - 1;
        if (device.id() == previousId)
            selectedIndex = index;
        if (device.isDefault())
            defaultIndex = index;
    }

    if (devices.isEmpty())
    {
        audioInputControl->addItem(tr("No input devices"), QByteArray{});
        audioInputControl->setCurrentIndex(0);
    }
    else if (selectedIndex >= 0)
    {
        audioInputControl->setCurrentIndex(selectedIndex);
    }
    else
    {
        audioInputControl->setCurrentIndex(defaultIndex >= 0 ? defaultIndex : 0);
    }
    audioInputControl->setEnabled(!devices.isEmpty() && !audioRecorder && !recordingPending &&
                                  !midiRecordingActive && !midiRecordingPending);
    recordAction->setEnabled(!midiRecordingActive && !midiRecordingPending &&
                             (audioRecorder || recordingPending || !devices.isEmpty()));
}

void MainWindow::refreshMidiInputs()
{
    if (midiInputControl == nullptr)
        return;

    const QString previousId = midiInputControl->currentData().toString();
    QString error;
    const QList<MidiInputPort> ports = MidiInput::availablePorts(&error);
    const QSignalBlocker blocker(midiInputControl);
    midiInputControl->clear();
    int selectedIndex = -1;
    for (const MidiInputPort& port : ports)
    {
        midiInputControl->addItem(port.name, port.id);
        if (port.id == previousId)
            selectedIndex = midiInputControl->count() - 1;
    }
    if (ports.isEmpty())
    {
        midiInputControl->addItem(MidiInput::isSupported()
                                      ? tr("No MIDI input devices")
                                      : tr("MIDI input unavailable in this build"),
                                  QString{});
        midiInputControl->setCurrentIndex(0);
    }
    else
    {
        midiInputControl->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    }
    midiInputControl->setEnabled(!ports.isEmpty() && !audioRecorder && !recordingPending &&
                                 !midiMonitorAction->isChecked() &&
                                 !midiRecordingActive && !midiRecordingPending);
    midiRecordAction->setEnabled(!audioRecorder && !recordingPending &&
                                 (midiRecordingActive || midiRecordingPending || MidiInput::isSupported()));
    midiMonitorAction->setEnabled((!audioRecorder && !recordingPending) ||
                                  midiMonitorAction->isChecked());
}

void MainWindow::setMidiMonitorEnabled(bool enabled)
{
    const auto rejectEnable = [this](const QString& message)
    {
        const QSignalBlocker blocker(midiMonitorAction);
        midiMonitorAction->setChecked(false);
        statusBar()->showMessage(message, 5000);
        refreshMidiInputs();
    };

    if (!enabled)
    {
        audioEngine.stopLiveMidiNotes();
        if (!midiRecordingActive && !midiRecordingPending)
        {
            if (midiInputPollTimer != nullptr)
                midiInputPollTimer->stop();
            if (midiInput != nullptr)
                midiInput->disconnect();
            activeMidiInputPortId.clear();
        }
        refreshMidiInputs();
        statusBar()->showMessage(tr("MIDI monitoring stopped"), 1800);
        return;
    }

    if (audioRecorder || recordingPending)
    {
        rejectEnable(tr("Stop audio recording before enabling MIDI monitoring."));
        return;
    }
    const InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType == InstrumentType::Audio)
    {
        rejectEnable(tr("Select a synth or drum rack track before monitoring MIDI."));
        return;
    }
    const QString portId = midiInputControl->currentData().toString();
    if (portId.isEmpty())
    {
        rejectEnable(MidiInput::isSupported()
            ? tr("No MIDI input devices are available. Connect a device and try again.")
            : tr("MIDI monitoring requires an ALSA enabled Linux build."));
        return;
    }
    if (!audioEngine.isAudioAvailable() && !audioEngine.initialize())
    {
        rejectEnable(audioEngine.errorMessage());
        return;
    }
    if (midiInput == nullptr)
        midiInput = std::make_unique<MidiInput>();
    if (midiInput->isConnected() && activeMidiInputPortId != portId)
    {
        if (midiRecordingActive || midiRecordingPending)
        {
            rejectEnable(tr("The MIDI recording is using a different input device."));
            return;
        }
        midiInput->disconnect();
    }
    if (!midiInput->isConnected())
    {
        QString error;
        if (!midiInput->connectTo(portId, &error))
        {
            rejectEnable(tr("Could not open MIDI input: %1").arg(error));
            return;
        }
    }
    activeMidiInputPortId = portId;
    midiInputPollTimer->start();
    refreshMidiInputs();
    statusBar()->showMessage(tr("Monitoring MIDI on %1").arg(track->name), 2500);
}

void MainWindow::startAudioRecording()
{
    if (audioRecorder || recordingPending || midiRecordingActive || midiRecordingPending)
        return;
    if (project.tracks.size() >= 256)
    {
        QMessageBox::warning(this, tr("Track limit"), tr("A project can contain up to 256 tracks."));
        QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
        return;
    }

    const QByteArray selectedDeviceId = audioInputControl == nullptr
        ? QByteArray{} : audioInputControl->currentData().toByteArray();
    QAudioDevice selectedDevice;
    for (const QAudioDevice& device : QMediaDevices::audioInputs())
    {
        if (device.id() == selectedDeviceId)
        {
            selectedDevice = device;
            break;
        }
    }
    if (selectedDevice.isNull())
    {
        QMessageBox::warning(this, tr("No audio input"),
                             tr("Connect or select an audio input device before recording."));
        QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
        return;
    }

    const QString defaultDirectory = projectPath.isEmpty()
        ? QDir::homePath() : QFileInfo(projectPath).absolutePath();
    QString path = QFileDialog::getSaveFileName(this, tr("Save recorded take"),
                                                 QDir(defaultDirectory).filePath(tr("Recording.wav")),
                                                 tr("WAV audio (*.wav)"));
    if (path.isEmpty())
    {
        QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
        return;
    }
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".wav");
    else if (QFileInfo(path).suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) != 0)
    {
        QMessageBox::warning(this, tr("Unsupported recording format"),
                             tr("Recordings are saved as WAV files. Choose a .wav filename."));
        QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
        return;
    }
    if (QFileInfo::exists(path) &&
        QMessageBox::question(this, tr("Replace recording?"),
                              tr("%1 already exists. Replace it when the take is saved?")
                                  .arg(QFileInfo(path).fileName()),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
    {
        QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
        return;
    }
    if (findProjectAudioAsset(path) != nullptr)
    {
        QMessageBox::warning(this, tr("Recording is already in use"),
                             tr("Choose a new WAV filename. This path is already used by audio in the current project."));
        QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
        return;
    }

    const bool playbackAvailable = audioEngine.isAudioAvailable() || audioEngine.initialize();
    const quint64 usedAudioBytes = projectAudioBytes();
    recordingDecodedByteLimit = usedAudioBytes >= AudioFile::maximumProjectDecodedBytes
        ? 0 : AudioFile::maximumProjectDecodedBytes - usedAudioBytes;
    if (recordingDecodedByteLimit == 0)
    {
        QMessageBox::warning(this, tr("Audio memory limit"),
                             tr("This project has reached its 512 MiB decoded audio limit. Remove an audio source before recording another take."));
        QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
        return;
    }

    pendingRecordingPath = path;
    pendingRecordingDeviceId = selectedDevice.id();
    pendingRecordingDeviceName = selectedDevice.description();
    recordingPending = true;
    recordingTransportActive = playbackAvailable;
    audioInputControl->setEnabled(false);
    tempoControl->setEnabled(false);
    timeSignatureControl->setEnabled(false);
    countInControl->setEnabled(false);
    loopAction->setEnabled(false);
    metronomeAction->setEnabled(false);
    midiInputControl->setEnabled(false);
    midiRecordAction->setEnabled(false);
    if (playbackAvailable)
    {
        audioEngine.setProject(project);
        audioEngine.startPlayback();
        playAction->setChecked(true);
        statusBar()->showMessage(project.countInBars > 0
            ? tr("Waiting for count-in before recording to %1…").arg(QFileInfo(path).fileName())
            : tr("Starting recording to %1…").arg(QFileInfo(path).fileName()));
    }
    else
    {
        statusBar()->showMessage(tr("Audio output is unavailable; recording starts now without count-in or playback."), 5000);
        if (!startPendingAudioRecording(0.0))
            finishAudioRecording(false);
    }
}

bool MainWindow::startPendingAudioRecording(double playheadBeat)
{
    QAudioDevice selectedDevice;
    for (const QAudioDevice& device : QMediaDevices::audioInputs())
    {
        if (device.id() == pendingRecordingDeviceId)
        {
            selectedDevice = device;
            break;
        }
    }
    if (selectedDevice.isNull())
    {
        statusBar()->showMessage(tr("The selected audio input was disconnected."), 5000);
        return false;
    }

    audioRecorder = std::make_unique<AudioRecorder>();
    QString error;
    if (!audioRecorder->start(selectedDevice, pendingRecordingPath, recordingDecodedByteLimit, &error))
    {
        audioRecorder.reset();
        statusBar()->showMessage(tr("Could not start recording: %1").arg(error), 6000);
        return false;
    }

    recordingStartBeat = std::max(0.0, playheadBeat);
    recordingPending = false;
    pendingRecordingPath.clear();
    pendingRecordingDeviceId.clear();
    recordAction->setEnabled(true);
    statusBar()->showMessage(recordingTransportActive
        ? tr("Recording from %1…").arg(pendingRecordingDeviceName)
        : tr("Recording from %1 without playback…").arg(pendingRecordingDeviceName));
    pendingRecordingDeviceName.clear();
    return true;
}

void MainWindow::finishAudioRecording(bool keepTake)
{
    if (!audioRecorder && !recordingPending)
        return;

    const bool wasWaitingForCountIn = recordingPending;
    const QString path = audioRecorder ? audioRecorder->path() : pendingRecordingPath;
    const bool reachedLimit = audioRecorder && audioRecorder->maximumReached();
    const QString captureIssue = audioRecorder ? audioRecorder->errorMessage() : QString{};
    bool finished = false;
    QString error;
    if (audioRecorder && keepTake)
        finished = audioRecorder->finish(&error);
    audioRecorder.reset();
    recordingPending = false;
    recordingTransportActive = false;
    pendingRecordingPath.clear();
    pendingRecordingDeviceId.clear();
    pendingRecordingDeviceName.clear();
    recordingDecodedByteLimit = 0;
    tempoControl->setEnabled(true);
    timeSignatureControl->setEnabled(true);
    countInControl->setEnabled(true);
    loopAction->setEnabled(true);
    metronomeAction->setEnabled(true);
    audioEngine.stopPlayback();
    playAction->setChecked(false);
    {
        const QSignalBlocker blocker(recordAction);
        recordAction->setChecked(false);
    }
    refreshAudioInputs();
    refreshMidiInputs();

    if (!keepTake)
        return;
    if (wasWaitingForCountIn)
    {
        statusBar()->showMessage(tr("Recording stopped before the count-in finished; no take was created."), 3500);
        return;
    }
    if (!finished)
    {
        statusBar()->showMessage(tr("Recording was not saved: %1").arg(error), 6000);
        return;
    }

    std::shared_ptr<const AudioAsset> asset;
    if (!AudioFile::loadWav(path, asset, &error))
    {
        statusBar()->showMessage(tr("The take was written, but could not be added to the project: %1").arg(error), 6000);
        return;
    }

    const QString takeName = QFileInfo(path).completeBaseName();
    InstrumentTrack track;
    track.id = project.nextTrackId();
    track.name = tr("Audio · %1").arg(takeName);
    track.instrumentType = InstrumentType::Audio;

    MidiClip clip;
    clip.id = project.nextClipId();
    clip.name = takeName;
    clip.startBeat = recordingStartBeat;
    const double durationBeats = static_cast<double>(asset->frameCount()) / asset->sampleRate *
                                 project.tempoBpm / 60.0;
    clip.lengthBeats = std::max(0.25, durationBeats);
    clip.audioSourcePath = asset->path;
    clip.audioSource = asset;
    track.clips.push_back(std::move(clip));
    project.tracks.push_back(std::move(track));
    project.normalize();
    selectedTrackId = project.tracks.last().id;
    selectedClipId = project.tracks.last().clips.first().id;
    settingsDirty = true;
    refreshProjectUi();
    editorTabs->setCurrentWidget(arrangementEditor);
    refreshDirtyState();

    if (reachedLimit)
        statusBar()->showMessage(tr("Recording stopped at the project audio memory or WAV size limit; the take was added."), 6000);
    else if (!captureIssue.isEmpty())
        statusBar()->showMessage(tr("A partial take was added after an input error: %1").arg(captureIssue), 6000);
    else
        statusBar()->showMessage(tr("Recorded %1 to a new audio track").arg(QFileInfo(path).fileName()), 3500);
}

void MainWindow::startMidiRecording()
{
    if (audioRecorder || recordingPending || midiRecordingActive || midiRecordingPending)
        return;

    refreshMidiInputs();
    InstrumentTrack* track = selectedTrack();
    if (track == nullptr || track->instrumentType == InstrumentType::Audio)
    {
        QMessageBox::warning(this, tr("Select an instrument track"),
                             tr("Select a synth or drum rack track before recording MIDI."));
        QSignalBlocker blocker(midiRecordAction);
        midiRecordAction->setChecked(false);
        return;
    }
    const QString portId = midiInputControl->currentData().toString();
    if (portId.isEmpty())
    {
        statusBar()->showMessage(MidiInput::isSupported()
            ? tr("No MIDI input devices are available. Connect a device and try again.")
            : tr("MIDI input requires an ALSA enabled Linux build."), 5000);
        QSignalBlocker blocker(midiRecordAction);
        midiRecordAction->setChecked(false);
        return;
    }

    const MidiClip* selected = selectedClip();
    quint64 targetClipId = selected != nullptr && !selected->isAudioRegion() &&
                           std::abs(selected->startBeat) < 1.0e-9 ? selected->id : 0;
    if (targetClipId == 0)
    {
        for (const MidiClip& clip : track->clips)
        {
            if (!clip.isAudioRegion() && std::abs(clip.startBeat) < 1.0e-9)
            {
                targetClipId = clip.id;
                break;
            }
        }
    }
    quint64 usedNotes = 0;
    if (targetClipId != 0)
    {
        const MidiClip* targetClip = project.findClip(track->id, targetClipId);
        if (targetClip != nullptr)
            usedNotes = static_cast<quint64>(targetClip->notes.size());
    }
    midiAvailableNoteSlots = usedNotes >= 100000 ? 0 : 100000 - usedNotes;
    if (midiAvailableNoteSlots == 0)
    {
        statusBar()->showMessage(tr("The target pattern already has the 100,000 note limit."), 4000);
        QSignalBlocker blocker(midiRecordAction);
        midiRecordAction->setChecked(false);
        return;
    }

    QString error;
    if ((!midiInput->isConnected() || activeMidiInputPortId != portId) &&
        !midiInput->connectTo(portId, &error))
    {
        statusBar()->showMessage(tr("Could not open MIDI input: %1").arg(error), 6000);
        QSignalBlocker blocker(midiRecordAction);
        midiRecordAction->setChecked(false);
        refreshMidiInputs();
        return;
    }
    activeMidiInputPortId = portId;

    midiRecordingTrackId = track->id;
    midiRecordingClipId = targetClipId;
    midiLastBeat = 0.0;
    midiOpenNoteCount = 0;
    openMidiNotes.clear();
    recordedMidiNotes.clear();
    const bool playbackAvailable = audioEngine.isAudioAvailable() || audioEngine.initialize();
    midiTransportActive = playbackAvailable;
    midiRecordingPending = playbackAvailable;
    midiRecordingActive = false;
    tempoControl->setEnabled(false);
    timeSignatureControl->setEnabled(false);
    countInControl->setEnabled(false);
    loopAction->setEnabled(false);
    metronomeAction->setEnabled(false);
    audioInputControl->setEnabled(false);
    midiInputControl->setEnabled(false);
    recordAction->setEnabled(false);
    midiInputPollTimer->start();

    if (playbackAvailable)
    {
        audioEngine.setProject(project);
        audioEngine.startPlayback();
        playAction->setChecked(true);
        statusBar()->showMessage(project.countInBars > 0
            ? tr("Waiting for count-in before MIDI recording…")
            : tr("Starting MIDI recording…"));
    }
    else
    {
        statusBar()->showMessage(tr("Audio output is unavailable; MIDI recording starts now without count-in or playback."), 5000);
        if (!startPendingMidiRecording(0.0))
            finishMidiRecording(false);
    }
}

bool MainWindow::startPendingMidiRecording(double playheadBeat)
{
    if (midiInput == nullptr || !midiInput->isConnected())
    {
        statusBar()->showMessage(tr("The MIDI input disconnected before recording could start."), 5000);
        return false;
    }
    midiRecordingPending = false;
    midiRecordingActive = true;
    midiRecordStartBeat = std::max(0.0, playheadBeat);
    midiLastBeat = midiRecordStartBeat;
    midiRecordClock.start();
    statusBar()->showMessage(midiTransportActive
        ? tr("Recording MIDI into the bar 1 pattern…")
        : tr("Recording MIDI without playback…"));
    return true;
}

void MainWindow::pollMidiInput()
{
    if (midiInput == nullptr || !midiInput->isConnected())
        return;
    const QVector<MidiInputEvent> events = midiInput->readAvailable();
    if (events.isEmpty())
        return;

    for (const MidiInputEvent& event : events)
    {
        if (midiMonitorAction->isChecked())
        {
            if (event.noteOn)
            {
                const InstrumentTrack* track = selectedTrack();
                if (track != nullptr && track->instrumentType != InstrumentType::Audio)
                    audioEngine.triggerLiveMidiNoteOn(event.channel, event.pitch, event.velocity, *track);
            }
            else
            {
                audioEngine.triggerLiveMidiNoteOff(event.channel, event.pitch);
            }
        }

        if (!midiRecordingActive)
            continue;
        const int key = event.channel * 128 + event.pitch;
        if (event.noteOn)
        {
            if (static_cast<quint64>(recordedMidiNotes.size()) + midiOpenNoteCount >= midiAvailableNoteSlots)
            {
                finishMidiRecording(true);
                statusBar()->showMessage(tr("MIDI recording stopped at the pattern note limit."), 5000);
                return;
            }
            OpenMidiNote note;
            note.pitch = event.pitch;
            note.velocity = std::clamp(event.velocity, 1, 127);
            note.startBeat = currentMidiRecordingBeat();
            openMidiNotes[key].push_back(note);
            ++midiOpenNoteCount;
        }
        else
        {
            auto open = openMidiNotes.find(key);
            if (open == openMidiNotes.end() || open->isEmpty())
                continue;
            const OpenMidiNote note = open->takeLast();
            if (open->isEmpty())
                openMidiNotes.erase(open);
            --midiOpenNoteCount;
            const double endBeat = currentMidiRecordingBeat();
            recordedMidiNotes.push_back({0, note.pitch, note.velocity, note.startBeat,
                                         std::max(1.0 / 64.0, endBeat - note.startBeat)});
        }
    }
}

double MainWindow::currentMidiRecordingBeat()
{
    if (midiRecordClock.isValid())
    {
        const double elapsedBeats = static_cast<double>(midiRecordClock.nsecsElapsed()) *
                                    project.tempoBpm / 60000000000.0;
        midiLastBeat = std::max(midiLastBeat, midiRecordStartBeat + elapsedBeats);
    }
    return midiLastBeat;
}

void MainWindow::finishMidiRecording(bool keepTake)
{
    if (!midiRecordingActive && !midiRecordingPending)
        return;

    const bool wasPending = midiRecordingPending;
    if (midiRecordingActive)
    {
        const double endBeat = currentMidiRecordingBeat();
        for (auto notes = openMidiNotes.begin(); notes != openMidiNotes.end(); ++notes)
        {
            for (const OpenMidiNote& open : notes.value())
            {
                if (static_cast<quint64>(recordedMidiNotes.size()) >= midiAvailableNoteSlots)
                    break;
                recordedMidiNotes.push_back({0, open.pitch, open.velocity, open.startBeat,
                                             std::max(1.0 / 64.0, endBeat - open.startBeat)});
            }
        }
    }

    midiRecordingActive = false;
    midiRecordingPending = false;
    if (!midiMonitorAction->isChecked())
    {
        if (midiInputPollTimer != nullptr)
            midiInputPollTimer->stop();
        if (midiInput != nullptr)
            midiInput->disconnect();
        activeMidiInputPortId.clear();
        audioEngine.stopLiveMidiNotes();
    }
    else if (midiInputPollTimer != nullptr)
    {
        midiInputPollTimer->start();
    }
    openMidiNotes.clear();
    midiOpenNoteCount = 0;
    if (midiTransportActive)
    {
        audioEngine.stopPlayback();
        playAction->setChecked(false);
    }
    midiTransportActive = false;
    tempoControl->setEnabled(true);
    timeSignatureControl->setEnabled(true);
    countInControl->setEnabled(true);
    loopAction->setEnabled(true);
    metronomeAction->setEnabled(true);
    {
        const QSignalBlocker blocker(midiRecordAction);
        midiRecordAction->setChecked(false);
    }
    refreshAudioInputs();
    refreshMidiInputs();

    if (!keepTake)
    {
        recordedMidiNotes.clear();
        return;
    }
    if (wasPending)
    {
        statusBar()->showMessage(tr("MIDI recording stopped before the count-in finished; no notes were added."), 3500);
        recordedMidiNotes.clear();
        return;
    }
    if (recordedMidiNotes.isEmpty())
    {
        statusBar()->showMessage(tr("No MIDI notes were captured."), 3500);
        return;
    }

    InstrumentTrack* track = project.findTrack(midiRecordingTrackId);
    if (track == nullptr || track->instrumentType == InstrumentType::Audio)
    {
        statusBar()->showMessage(tr("The target instrument track is no longer available."), 5000);
        recordedMidiNotes.clear();
        return;
    }
    MidiClip* clip = midiRecordingClipId == 0
        ? nullptr : project.findClip(midiRecordingTrackId, midiRecordingClipId);
    bool createdClip = false;
    if (clip == nullptr)
    {
        if (track->clips.size() >= 10000)
        {
            statusBar()->showMessage(tr("The target track has reached the 10,000 pattern limit."), 5000);
            recordedMidiNotes.clear();
            return;
        }
        MidiClip newClip;
        newClip.id = project.nextClipId();
        newClip.name = tr("MIDI Take");
        newClip.lengthBeats = project.patternLengthBeats();
        track->clips.push_back(std::move(newClip));
        clip = &track->clips.last();
        createdClip = true;
    }

    const QVector<MidiNote> beforeNotes = clip->notes;
    const double beforeLength = clip->lengthBeats;
    quint64 nextNoteId = project.nextNoteId();
    double maximumEndBeat = clip->lengthBeats;
    std::sort(recordedMidiNotes.begin(), recordedMidiNotes.end(), [](const MidiNote& left, const MidiNote& right)
    {
        if (left.startBeat != right.startBeat) return left.startBeat < right.startBeat;
        return left.pitch < right.pitch;
    });
    for (MidiNote note : recordedMidiNotes)
    {
        if (static_cast<quint64>(clip->notes.size()) >= 100000)
            break;
        if (note.startBeat >= 16384.0)
            continue;
        note.id = nextNoteId++;
        note.startBeat = std::clamp(note.startBeat, 0.0, 16384.0 - 1.0 / 64.0);
        note.lengthBeats = std::clamp(note.lengthBeats, 1.0 / 64.0,
                                      16384.0 - note.startBeat);
        maximumEndBeat = std::max(maximumEndBeat, note.startBeat + note.lengthBeats);
        clip->notes.push_back(note);
    }
    if (clip->notes.size() == beforeNotes.size())
    {
        if (createdClip)
            track->clips.removeLast();
        statusBar()->showMessage(tr("No MIDI notes fit in the target pattern."), 3500);
        recordedMidiNotes.clear();
        return;
    }
    std::sort(clip->notes.begin(), clip->notes.end(), [](const MidiNote& left, const MidiNote& right)
    {
        if (left.startBeat != right.startBeat) return left.startBeat < right.startBeat;
        return left.pitch < right.pitch;
    });

    const double barLength = project.quarterNotesPerBar();
    const double afterLength = std::clamp(std::ceil(maximumEndBeat / barLength) * barLength,
                                          std::max(0.25, beforeLength), 16384.0);
    clip->lengthBeats = afterLength;
    const quint64 targetClipId = clip->id;
    const quint64 targetTrackId = track->id;
    const InstrumentType targetInstrumentType = track->instrumentType;
    const QString targetClipName = clip->name;
    const QVector<MidiNote> afterNotes = clip->notes;
    midiRecordingTrackId = 0;
    midiRecordingClipId = 0;
    selectedTrackId = targetTrackId;
    selectedClipId = targetClipId;
    if (createdClip)
        settingsDirty = true;
    refreshProjectUi();
    undoStack.push(new NoteEditCommand(this, targetTrackId, targetClipId,
                                       beforeNotes, afterNotes, beforeLength, afterLength));
    editorTabs->setCurrentWidget(targetInstrumentType == InstrumentType::DrumRack
                                     ? static_cast<QWidget*>(stepSequencer)
                                     : static_cast<QWidget*>(midiEditor));
    refreshDirtyState();
    statusBar()->showMessage(tr("Recorded %1 MIDI note(s) into %2")
                                 .arg(afterNotes.size() - beforeNotes.size())
                                 .arg(targetClipName), 4500);
    recordedMidiNotes.clear();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (!maybeSave())
    {
        event->ignore();
        return;
    }

    audioEngine.stopPlayback();
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState());
    event->accept();
}
