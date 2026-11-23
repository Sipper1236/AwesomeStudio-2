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
class QProgressBar;
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
    friend class PitchBendRangeCommand;
    friend class AutomationEditCommand;
    friend class MarkerEditCommand;
    friend class AudioFadeEditCommand;

public:
    explicit MainWindow(AudioEngine& audioEngine, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct AudioFadeState
    {
        quint64 clipId = 0;
        double fadeInBeats = 0.0;
        double fadeOutBeats = 0.0;
        AudioFadeCurve fadeInCurve = AudioFadeCurve::Linear;
        AudioFadeCurve fadeOutCurve = AudioFadeCurve::Linear;
    };

    void createActions();
    void createToolBars();
    void createDocks();
    void createMenus();
    void setProject(const ProjectData& project, const QString& path = QString());
    void refreshProjectUi();
    void refreshPianoRollGhostNotes();
    void refreshMixer();
    void refreshMixerMeters();
    void addMixerBus();
    void removeMixerBus(quint64 busId);
    bool mixerRouteWouldCycle(quint64 sourceBusId, quint64 targetBusId) const;
    void setMixerBusOutput(quint64 busId, quint64 outputBusId);
    void setMixerBusSend(quint64 busId, qsizetype sendIndex, quint64 targetBusId, float gain);
    void addMixerBusSend(quint64 busId);
    void removeMixerBusSend(quint64 busId, qsizetype sendIndex);
    void setAudioMonitoring(bool enabled);
    void finishAudioMonitoring();
    void setTrackOutputBus(quint64 trackId, quint64 busId);
    void setTrackSend(quint64 trackId, qsizetype sendIndex, quint64 busId, float gain);
    void addTrackSend(quint64 trackId);
    void removeTrackSend(quint64 trackId, qsizetype sendIndex);
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
    void setSelectedAudioFadeCurve(bool fadeIn, int curve);
    void createSelectedAudioCrossfade();
    void applyAudioFadeHistory(quint64 trackId, const QVector<AudioFadeState>& fades);
    void setSelectedSynthEnvelope(int parameter, double value);
    void setSelectedTrackEqualizer(int band, double gainDb);
    void refreshAutomationEditor();
    void addAutomationPoint();
    void removeAutomationPoint();
    void applyAutomationHistory(quint64 trackId, int parameter,
                                const QVector<AutomationPoint>& points);
    void addTimelineMarker(double beat);
    void renameTimelineMarker(double beat);
    void removeTimelineMarker(double beat);
    void seekToTimelineMarker(double beat);
    void applyMarkerHistory(const QVector<TimelineMarker>& markers);
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
                          double clipLengthBeats = -1.0,
                          const QVector<MidiPitchBend>* pitchBends = nullptr);
    void applyPitchBendRangeHistory(double semitones);
    void exportAudio();
    void exportStems();
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
    void setMidiControllerListening(bool enabled);
    void beginMidiControllerLearn(int target);
    void clearMidiControllerMapping(int target);
    void loadMidiControllerMappings();
    void saveMidiControllerMappings() const;
    void handleMidiControlChange(int channel, int controller, int value);
    void applyMidiControllerTarget(int target, int value);
    double currentMidiRecordingBeat();

    enum class MidiControllerTarget : int
    {
        MasterLevel = 0,
        SelectedTrackLevel = 1,
        SelectedTrackPan = 2,
        Tempo = 3
    };
    struct MidiControllerMapping
    {
        int channel = -1;
        int controller = -1;
    };

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
    QWidget* equalizerControls = nullptr;
    QWidget* automationEditorPanel = nullptr;
    QWidget* drumSampleControls = nullptr;
    std::array<QDoubleSpinBox*, 4> envelopeControls{};
    std::array<QDoubleSpinBox*, 3> equalizerGainControls{};
    QDoubleSpinBox* audioFadeInControl = nullptr;
    QDoubleSpinBox* audioFadeOutControl = nullptr;
    QComboBox* audioFadeInCurveControl = nullptr;
    QComboBox* audioFadeOutCurveControl = nullptr;
    QPushButton* audioCrossfadeButton = nullptr;
    QComboBox* automationParameterControl = nullptr;
    QListWidget* automationPointsList = nullptr;
    QDoubleSpinBox* automationBeatControl = nullptr;
    QDoubleSpinBox* automationValueControl = nullptr;
    QLabel* automationValueLabel = nullptr;
    QPushButton* removeAutomationPointButton = nullptr;
    struct MixerTrackMeterWidget
    {
        quint64 trackId = 0;
        QProgressBar* meter = nullptr;
    };
    struct MixerBusMeterWidget
    {
        size_t busIndex = 0;
        QProgressBar* meter = nullptr;
    };
    QVector<MixerTrackMeterWidget> mixerTrackMeters;
    QVector<MixerBusMeterWidget> mixerBusMeters;
    QProgressBar* masterPeakMeter = nullptr;
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
    QAction* audioMonitorAction = nullptr;
    QAction* midiRecordAction = nullptr;
    QAction* midiMonitorAction = nullptr;
    QAction* automationViewAction = nullptr;
    QAction* automationWriteAction = nullptr;
    QComboBox* automationWriteModeControl = nullptr;
    QComboBox* automationLaneParameterControl = nullptr;
    QComboBox* pianoRollKeyControl = nullptr;
    QComboBox* pianoRollScaleControl = nullptr;
    QAction* pianoRollScaleSnapAction = nullptr;
    QAction* pianoRollGhostNotesAction = nullptr;
    std::unique_ptr<AudioRecorder> audioRecorder;
    std::unique_ptr<AudioRecorder> audioMonitor;
    std::unique_ptr<MidiInput> midiInput;
    QComboBox* midiInputControl = nullptr;
    QTimer* midiInputPollTimer = nullptr;
    QString activeMidiInputPortId;
    QAction* midiControllerListenAction = nullptr;
    QHash<int, MidiControllerMapping> midiControllerMappings;
    int pendingMidiControllerTarget = -1;
    bool midiControllerListenerEnabled = false;
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
        int channel = 0;
        double startBeat = 0.0;
    };
    QHash<int, QVector<OpenMidiNote>> openMidiNotes;
    QVector<MidiNote> recordedMidiNotes;
    QVector<MidiPitchBend> recordedMidiPitchBends;
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
        float lastValue = 0.0f;
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
