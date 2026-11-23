#pragma once

#include "core/AudioEngine.h"
#include "core/Project.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QUndoStack>

#include <array>
#include <memory>

class QCloseEvent;
class QMediaDevices;
class QComboBox;
class QDoubleSpinBox;
class QDockWidget;
class QLabel;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSlider;
class QTabWidget;
class QTimer;
class MidiEditor;
class ArrangementEditor;
class StepSequencer;
class AudioRecorder;
class MidiInput;

class MainWindow final : public QMainWindow
{
    Q_OBJECT
    friend class NoteEditCommand;
    friend class AutomationEditCommand;

public:
    explicit MainWindow(AudioEngine& audioEngine, QWidget* parent = nullptr);
    ~MainWindow() override;

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
    void trimAudioRegion(quint64 trackId, quint64 clipId, double startBeat,
                         double lengthBeats, qint64 audioStartFrame);
    void setSelectedAudioFade(bool fadeIn, double beats);
    void setSelectedSynthEnvelope(int parameter, double value);
    void refreshAutomationEditor();
    void addAutomationPoint();
    void removeAutomationPoint();
    void applyAutomationHistory(quint64 trackId, int parameter,
                                const QVector<AutomationPoint>& points);
    void beginAutomationWrite(quint64 trackId, int parameter, float value);
    void recordAutomationValue(quint64 trackId, int parameter, float value);
    void finishAutomationWrite();
    void importDrumSample(int pad);
    void clearDrumSample(int pad);
    quint64 projectAudioBytes() const;
    std::shared_ptr<const AudioAsset> findProjectAudioAsset(const QString& path) const;
    InstrumentTrack* selectedTrack();
    MidiClip* selectedClip();
    bool maybeSave();
    bool saveToPath(const QString& path);
    void updateProjectTitle();
    void refreshDirtyState();
    void applyNoteHistory(quint64 trackId, quint64 clipId, const QVector<MidiNote>& notes,
                          double clipLengthBeats = -1.0);
    void exportAudio();
    void importAudio();
    void importMidi();
    void exportMidi();
    void startAudioRecording();
    bool startPendingAudioRecording(double playheadBeat);
    void finishAudioRecording(bool keepTake = true);
    void refreshAudioInputs();
    void startMidiRecording();
    bool startPendingMidiRecording(double playheadBeat);
    void finishMidiRecording(bool keepTake = true);
    void refreshMidiInputs();
    void pollMidiInput();
    void setMidiMonitorEnabled(bool enabled);
    double currentMidiRecordingBeat();

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
    QWidget* audioRegionControls = nullptr;
    QWidget* synthEnvelopePanel = nullptr;
    QWidget* automationEditorPanel = nullptr;
    QWidget* drumSampleControls = nullptr;
    std::array<QDoubleSpinBox*, 4> envelopeControls{};
    QDoubleSpinBox* audioFadeInControl = nullptr;
    QDoubleSpinBox* audioFadeOutControl = nullptr;
    QComboBox* automationParameterControl = nullptr;
    QListWidget* automationPointsList = nullptr;
    QDoubleSpinBox* automationBeatControl = nullptr;
    QDoubleSpinBox* automationValueControl = nullptr;
    QLabel* automationValueLabel = nullptr;
    QPushButton* removeAutomationPointButton = nullptr;
    std::array<QPushButton*, 4> drumSampleButtons{};
    std::array<QPushButton*, 4> clearDrumSampleButtons{};
    QLabel* masterGainReadout = nullptr;
    QLabel* instrumentHeading = nullptr;
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
    QTimer* automationWriteIdleTimer = nullptr;
    QMediaDevices* mediaDevices = nullptr;
    QComboBox* audioInputControl = nullptr;
    QAction* recordAction = nullptr;
    QAction* midiRecordAction = nullptr;
    QAction* midiMonitorAction = nullptr;
    QAction* automationViewAction = nullptr;
    QAction* automationWriteAction = nullptr;
    QComboBox* automationLaneParameterControl = nullptr;
    std::unique_ptr<AudioRecorder> audioRecorder;
    std::unique_ptr<MidiInput> midiInput;
    QComboBox* midiInputControl = nullptr;
    QTimer* midiInputPollTimer = nullptr;
    QString activeMidiInputPortId;
    QString pendingRecordingPath;
    QByteArray pendingRecordingDeviceId;
    QString pendingRecordingDeviceName;
    double recordingStartBeat = 0.0;
    quint64 recordingDecodedByteLimit = 0;
    bool recordingPending = false;
    bool recordingTransportActive = false;
    struct OpenMidiNote
    {
        int pitch = 0;
        int velocity = 0;
        double startBeat = 0.0;
    };
    QHash<int, QVector<OpenMidiNote>> openMidiNotes;
    QVector<MidiNote> recordedMidiNotes;
    QElapsedTimer midiRecordClock;
    quint64 midiRecordingTrackId = 0;
    quint64 midiRecordingClipId = 0;
    quint64 midiAvailableNoteSlots = 0;
    quint64 midiOpenNoteCount = 0;
    double midiLastBeat = 0.0;
    double midiRecordStartBeat = 0.0;
    struct AutomationWriteSession
    {
        bool active = false;
        bool changed = false;
        quint64 trackId = 0;
        int parameter = 0;
        float initialValue = 0.0f;
        double lastSampleBeat = -1.0;
        double lastPointBeat = -1.0;
        QVector<AutomationPoint> before;
    } automationWriteSession;
    bool midiRecordingPending = false;
    bool midiRecordingActive = false;
    bool midiTransportActive = false;
    quint64 selectedTrackId = 0;
    quint64 selectedClipId = 0;
};
