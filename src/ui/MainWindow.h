#pragma once

#include "core/AudioEngine.h"
#include "core/Project.h"

#include <QMainWindow>
#include <QUndoStack>

class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QDockWidget;
class QLabel;
class QListWidget;
class QScrollArea;
class QSlider;
class QTabWidget;
class QTimer;
class MidiEditor;
class ArrangementEditor;
class StepSequencer;

class MainWindow final : public QMainWindow
{
    Q_OBJECT
    friend class NoteEditCommand;

public:
    explicit MainWindow(AudioEngine& audioEngine, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void createActions();
    void createToolBars();
    void createDocks();
    void createMenus();
    void setProject(const ProjectData& project, const QString& path = QString());
    void refreshProjectUi();
    void refreshMixer();
    void selectTrack(quint64 trackId);
    void selectClip(quint64 trackId, quint64 clipId, bool openPianoRoll);
    void addTrack();
    void addDrumTrack();
    void addClipAt(quint64 trackId, double startBeat);
    void addNewPattern();
    void removeClip(quint64 trackId, quint64 clipId);
    void moveClip(quint64 sourceTrackId, quint64 targetTrackId, quint64 clipId, double startBeat);
    InstrumentTrack* selectedTrack();
    MidiClip* selectedClip();
    bool maybeSave();
    bool saveToPath(const QString& path);
    void updateProjectTitle();
    void refreshDirtyState();
    void applyNoteHistory(quint64 trackId, quint64 clipId, const QVector<MidiNote>& notes);
    void exportAudio();
    void importMidi();
    void exportMidi();

    AudioEngine& audioEngine;
    ProjectData project;
    QString projectPath;
    bool settingsDirty = false;

    MidiEditor* midiEditor = nullptr;
    ArrangementEditor* arrangementEditor = nullptr;
    StepSequencer* stepSequencer = nullptr;
    QTabWidget* editorTabs = nullptr;
    QDockWidget* browserDock = nullptr;
    QDockWidget* mixerDock = nullptr;
    QScrollArea* mixerScrollArea = nullptr;
    QListWidget* trackList = nullptr;
    QDoubleSpinBox* tempoControl = nullptr;
    QDoubleSpinBox* clipLengthControl = nullptr;
    QComboBox* waveformControl = nullptr;
    QComboBox* timeSignatureControl = nullptr;
    QComboBox* countInControl = nullptr;
    QComboBox* snapControl = nullptr;
    QSlider* zoomControl = nullptr;
    QSlider* masterControl = nullptr;
    QLabel* masterGainReadout = nullptr;
    QLabel* playheadLabel = nullptr;
    QLabel* audioDeviceLabel = nullptr;
    QAction* playAction = nullptr;
    QAction* stopAction = nullptr;
    QAction* loopAction = nullptr;
    QAction* metronomeAction = nullptr;
    QAction* addTrackAction = nullptr;
    QAction* addDrumTrackAction = nullptr;
    QAction* addPatternAction = nullptr;
    QAction* undoAction = nullptr;
    QAction* redoAction = nullptr;
    QUndoStack undoStack;
    QTimer* uiTimer = nullptr;
    quint64 selectedTrackId = 0;
    quint64 selectedClipId = 0;
};
