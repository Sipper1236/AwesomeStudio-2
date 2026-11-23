#include "MainWindow.h"

#include "core/AudioFile.h"
#include "core/ProjectFile.h"
#include "core/MidiFile.h"
#include "core/WavExporter.h"
#include "ui/ArrangementEditor.h"
#include "ui/MidiEditor.h"
#include "ui/StepSequencer.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
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
#include <utility>

class NoteEditCommand final : public QUndoCommand
{
public:
    NoteEditCommand(MainWindow* window, quint64 trackId, quint64 clipId,
                    QVector<MidiNote> before, QVector<MidiNote> after)
        : window(window), trackId(trackId), clipId(clipId),
          before(std::move(before)), after(std::move(after))
    {
        setText(QObject::tr("Edit MIDI notes"));
    }

    void undo() override
    {
        if (window != nullptr)
            window->applyNoteHistory(trackId, clipId, before);
    }

    void redo() override
    {
        if (firstRedo)
        {
            firstRedo = false;
            return;
        }
        if (window != nullptr)
            window->applyNoteHistory(trackId, clipId, after);
    }

private:
    QPointer<MainWindow> window;
    quint64 trackId;
    quint64 clipId;
    QVector<MidiNote> before;
    QVector<MidiNote> after;
    bool firstRedo = true;
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
        audioEngine.triggerPreview(pitch, 96,
                                   track == nullptr ? SynthWaveform::Sine : track->waveform,
                                   track == nullptr ? 0.0f : track->pan,
                                   track != nullptr && track->instrumentType == InstrumentType::DrumRack);
    });
    connect(stepSequencer, &StepSequencer::previewRequested, this, [this](int pitch)
    {
        const InstrumentTrack* track = selectedTrack();
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
        audioEngine.stopPlayback();
        playAction->setChecked(false);
        midiEditor->setPlayheadBeat(0.0);
        arrangementEditor->setPlayheadBeat(0.0);
    });

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
    browserLayout->addStretch();
    auto* hint = new QLabel(tr("Import a WAV to add an audio track. Double-click MIDI clips to edit notes or drum steps."), browser);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #8996a2; font-size: 11px;"));
    browserLayout->addWidget(hint);
    browserDock->setWidget(browser);
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
    instrumentHeading->setText(track == nullptr ? tr("SELECTED INSTRUMENT") :
                               track->instrumentType == InstrumentType::DrumRack ? tr("DRUM RACK") :
                               track->instrumentType == InstrumentType::Audio ? tr("AUDIO SOURCE") :
                               tr("SELECTED INSTRUMENT"));

    const MidiClip* clip = selectedClip();
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
        connect(volume, &QSlider::valueChanged, this, [this, trackId, volumeLabel](int value)
        {
            InstrumentTrack* track = project.findTrack(trackId);
            if (track == nullptr) return;
            track->gain = value / 100.0f;
            volumeLabel->setText(tr("%1%").arg(value));
            audioEngine.setProject(project);
            settingsDirty = true;
            refreshDirtyState();
        });
        connect(pan, &QSlider::valueChanged, this, [this, trackId, panLabel](int value)
        {
            InstrumentTrack* track = project.findTrack(trackId);
            if (track == nullptr) return;
            track->pan = value / 100.0f;
            panLabel->setText(value < -2 ? tr("L %1").arg(-value) : value > 2 ? tr("R %1").arg(value) : tr("Center"));
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

bool MainWindow::maybeSave()
{
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
    int offlineRegions = 0;
    for (const InstrumentTrack& track : project.tracks)
        for (const MidiClip& clip : track.clips)
            if (clip.isAudioRegion() && clip.audioSource == nullptr)
                ++offlineRegions;
    if (offlineRegions > 0)
        statusBar()->showMessage(tr("Project opened with %1 offline WAV region(s); restore the source file to hear them.")
                                     .arg(offlineRegions), 6000);
}

bool MainWindow::saveToPath(const QString& path)
{
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

void MainWindow::applyNoteHistory(quint64 trackId, quint64 clipId, const QVector<MidiNote>& notes)
{
    MidiClip* clip = project.findClip(trackId, clipId);
    if (clip == nullptr)
        return;
    clip->notes = notes;
    if (selectedTrackId == trackId && selectedClipId == clipId)
        midiEditor->setNotes(notes);
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
    if (project.tracks.size() >= 256)
    {
        statusBar()->showMessage(tr("The project has reached the 256-track limit"), 3000);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Import WAV audio"), QString(),
                                                       tr("WAV audio (*.wav)"));
    if (path.isEmpty())
        return;

    std::shared_ptr<const AudioAsset> asset;
    QString error;
    if (!AudioFile::loadWav(path, asset, &error))
    {
        QMessageBox::critical(this, tr("Could not import WAV audio"), error);
        return;
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
