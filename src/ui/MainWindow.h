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
class QCheckBox;
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
class QSpinBox;
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
    friend class StepDivisionCommand;
    friend class PitchBendRangeCommand;
    friend class AutomationEditCommand;
    friend class MarkerEditCommand;
    friend class TempoMapEditCommand;
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
        double fadeInShape = 1.0;
        double fadeOutShape = 1.0;
        QVector<float> fadeInCurvePoints;
        QVector<float> fadeOutCurvePoints;
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
    void addClapInstrumentTrack();
    void editSelectedClapInstrument();
    void addClipAt(quint64 trackId, double startBeat);
    void addNewPattern();
    void removeClip(quint64 trackId, quint64 clipId);
    void moveClip(quint64 sourceTrackId, quint64 targetTrackId, quint64 clipId, double startBeat);
    void trimAudioRegion(quint64 trackId, quint64 clipId, double startBeat,
                         double lengthBeats, qint64 audioStartFrame);
    void relinkAudioRegion(quint64 trackId, quint64 clipId);
    void setSelectedAudioFade(bool fadeIn, double beats);
    void setSelectedAudioFadeCurve(bool fadeIn, int curve);
    void setSelectedAudioFadeShape(bool fadeIn, double shape);
    void createSelectedAudioCrossfade();
    void applyAudioFadeHistory(quint64 trackId, const QVector<AudioFadeState>& fades);
    void setSelectedSynthEnvelope(int parameter, double value);
    void setSelectedSynthOscillator(int parameter, double value);
    void setSelectedSynthFilter(int parameter, double value);
    void setSelectedSynthFilterEnvelope(int parameter, double value);
    void applySelectedSynthPreset(int presetIndex);
    void populateSynthPresetControl(int selectedUserPresetIndex = -1);
    void saveSelectedSynthPreset();
    void showSynthPresetMenu(const QPoint& position);
    void importSynthPreset();
    void exportSelectedSynthPreset();
    void removeSelectedSynthPreset();
    void setSelectedTrackEqualizer(int band, double gainDb);
    void setSelectedTrackEqualizerCrossover(int band, double frequencyHz);
    void setSelectedTrackParametricEqualizer(int band, const ParametricEqBand& settings);
    void refreshParametricEqualizerControls();
    void refreshAutomationEditor();
    void refreshArrangementAutomationView();
    void addAutomationPoint();
    void removeAutomationPoint();
    void setSelectedAutomationCurve(int curve);
    void applyAutomationHistory(quint64 trackId, int parameter, const QString& pluginUri,
                                const QVector<AutomationPoint>& points);
    void addTimelineMarker(double beat);
    void renameTimelineMarker(double beat);
    void removeTimelineMarker(double beat);
    void seekToTimelineMarker(double beat);
    void applyMarkerHistory(const QVector<TimelineMarker>& markers);
    void setTimelineTempo(double beat);
    void removeTimelineTempo(double beat);
    void toggleTimelineTempoRamp(double beat);
    void applyTempoHistory(double baseTempo, const QVector<TempoChange>& changes);
    void beginAutomationWrite(quint64 trackId, int parameter, float value,
                              const QString& pluginUri = QString(),
                              qsizetype effectIndex = -1, qsizetype parameterIndex = -1,
                              float minimum = 0.0f, float maximum = 1.0f,
                              const QString& parameterName = QString());
    void recordAutomationValue(quint64 trackId, int parameter, float value);
    void finishAutomationWrite();
    void importDrumSample(int pad);
    void clearDrumSample(int pad);
    void importVeryLowVelocityDrumSample(int pad);
    void clearVeryLowVelocityDrumSample(int pad);
    void importLowVelocityDrumSample(int pad);
    void clearLowVelocityDrumSample(int pad);
    void importHighVelocityDrumSample(int pad);
    void clearHighVelocityDrumSample(int pad);
    void importVeryHighVelocityDrumSample(int pad);
    void clearVeryHighVelocityDrumSample(int pad);
    void importDrumVelocityLayer(int pad, int layer);
    void clearDrumVelocityLayer(int pad, int layer);
    void resetSelectedDrumLayerSettings(int pad);
    void setSelectedDrumSample(int pad, int parameter, double value);
    quint64 projectAudioBytes() const;
    std::shared_ptr<const AudioAsset> findProjectAudioAsset(const QString& path) const;
    InstrumentTrack* selectedTrack();
    MidiClip* selectedClip();
    bool maybeSave();
    bool saveToPath(const QString& path);
    void savePortableProjectCopy();
    void offerUnsavedSessionRecovery();
    void autosaveProject();
    void updateProjectTitle();
    void refreshDirtyState();
    void applyNoteHistory(quint64 trackId, quint64 clipId, const QVector<MidiNote>& notes,
                          double clipLengthBeats = -1.0,
                          const QVector<MidiPitchBend>* pitchBends = nullptr);
    void applyStepDivisionHistory(quint64 trackId, quint64 clipId, int stepsPerBar);
    void applyPitchBendRangeHistory(double semitones);
    void exportAudio();
    void exportAudioFlac();
    void exportAudioOgg();
    void exportAudioMp3();
    void exportStems();
    void showRenderQueue();
    void importAudio();
    void importMidi();
    void exportMidi();
    void startAudioRecording();
    bool startPendingAudioRecording(double playheadBeat);
    void finishAudioRecording(bool keepTake = true);
    void refreshPunchRangeUi();
    void refreshAudioInputs();
    void refreshAudioInputCompensation();
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
    void handleMidiControlChange(int channel, int controller, int value, bool nrpn = false);
    void applyMidiControllerTarget(int target, int value);
    double currentMidiRecordingBeat();

    enum class MidiControllerTarget : int
    {
        MasterLevel = 0,
        SelectedTrackLevel = 1,
        SelectedTrackPan = 2,
        Tempo = 3,
        SelectedTrackMute = 4,
        SelectedTrackSolo = 5,
        SelectedTrackFilterCutoff = 6,
        SelectedTrackFilterResonance = 7,
        SelectedTrackSynthAttack = 8,
        SelectedTrackSynthDecay = 9,
        SelectedTrackSynthSustain = 10,
        SelectedTrackSynthRelease = 11,
        SelectedTrackOscillatorMix = 12,
        SelectedTrackFilterEnvelopeAmount = 13,
        SelectedTrackFilterEnvelopeAttack = 14,
        SelectedTrackFilterEnvelopeDecay = 15,
        SelectedTrackFilterEnvelopeSustain = 16,
        SelectedTrackFilterEnvelopeRelease = 17,
        SelectedTrackFilterEnvelopeEnabled = 18,
        SelectedTrackOscillatorWaveform = 19,
        SelectedTrackSecondOscillatorEnabled = 20,
        SelectedTrackSecondOscillatorWaveform = 21,
        SelectedTrackSecondOscillatorDetune = 22,
        SelectedTrackFilterEnabled = 23,
        SelectedTrackFilterType = 24
    };
    struct MidiControllerMapping
    {
        int channel = -1;
        int controller = -1;
        bool nrpn = false;
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
    QCheckBox* secondOscillatorEnabledControl = nullptr;
    QComboBox* secondOscillatorWaveformControl = nullptr;
    QDoubleSpinBox* secondOscillatorDetuneControl = nullptr;
    QDoubleSpinBox* secondOscillatorMixControl = nullptr;
    QComboBox* synthPresetControl = nullptr;
    QCheckBox* synthFilterEnabledControl = nullptr;
    QComboBox* synthFilterTypeControl = nullptr;
    QDoubleSpinBox* synthFilterCutoffControl = nullptr;
    QDoubleSpinBox* synthFilterResonanceControl = nullptr;
    QCheckBox* synthFilterEnvelopeEnabledControl = nullptr;
    QComboBox* timeSignatureControl = nullptr;
    QComboBox* countInControl = nullptr;
    QComboBox* snapControl = nullptr;
    QSlider* zoomControl = nullptr;
    QSlider* masterControl = nullptr;
    QWidget* audioRegionControls = nullptr;
    QWidget* oscillatorPanel = nullptr;
    QWidget* synthPresetPanel = nullptr;
    QWidget* synthFilterPanel = nullptr;
    QWidget* synthFilterEnvelopePanel = nullptr;
    QWidget* synthEnvelopePanel = nullptr;
    QWidget* equalizerControls = nullptr;
    QWidget* equalizerResponseGraph = nullptr;
    QComboBox* parametricEqBandControl = nullptr;
    std::array<QDoubleSpinBox*, 3> parametricEqBandParameterControls{};
    QWidget* automationEditorPanel = nullptr;
    QWidget* drumSampleControls = nullptr;
    std::array<QDoubleSpinBox*, 4> envelopeControls{};
    std::array<QDoubleSpinBox*, 5> synthFilterEnvelopeControls{};
    std::array<QDoubleSpinBox*, 3> equalizerGainControls{};
    std::array<QDoubleSpinBox*, 2> equalizerCrossoverControls{};
    QDoubleSpinBox* audioFadeInControl = nullptr;
    QDoubleSpinBox* audioFadeOutControl = nullptr;
    QComboBox* audioFadeInCurveControl = nullptr;
    QComboBox* audioFadeOutCurveControl = nullptr;
    QDoubleSpinBox* audioFadeInShapeControl = nullptr;
    QDoubleSpinBox* audioFadeOutShapeControl = nullptr;
    QPushButton* audioCrossfadeButton = nullptr;
    QComboBox* automationParameterControl = nullptr;
    QListWidget* automationPointsList = nullptr;
    QDoubleSpinBox* automationBeatControl = nullptr;
    QDoubleSpinBox* automationValueControl = nullptr;
    QComboBox* automationCurveControl = nullptr;
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
    std::array<QPushButton*, 4> veryLowVelocityDrumSampleButtons{};
    std::array<QPushButton*, 4> clearVeryLowVelocityDrumSampleButtons{};
    std::array<QSpinBox*, 4> veryLowVelocityThresholdControls{};
    std::array<QPushButton*, 4> lowVelocityDrumSampleButtons{};
    std::array<QPushButton*, 4> clearLowVelocityDrumSampleButtons{};
    std::array<QSpinBox*, 4> lowVelocityThresholdControls{};
    std::array<QDoubleSpinBox*, 4> drumSampleTuneControls{};
    std::array<QDoubleSpinBox*, 4> drumSampleStartControls{};
    std::array<QDoubleSpinBox*, 4> drumSampleEndControls{};
    std::array<QWidget*, 4> drumSampleWaveformWidgets{};
    std::array<QCheckBox*, 4> drumSampleLoopControls{};
    std::array<QComboBox*, 4> drumLayerSettingsControls{};
    std::array<QPushButton*, 4> resetDrumLayerSettingsButtons{};
    std::array<QDoubleSpinBox*, 4> drumPadAttackControls{};
    std::array<QDoubleSpinBox*, 4> drumPadDecayControls{};
    std::array<QDoubleSpinBox*, 4> drumPadSustainControls{};
    std::array<QDoubleSpinBox*, 4> drumPadReleaseControls{};
    std::array<QPushButton*, 4> highVelocityDrumSampleButtons{};
    std::array<QPushButton*, 4> clearHighVelocityDrumSampleButtons{};
    std::array<QSpinBox*, 4> highVelocityThresholdControls{};
    std::array<QPushButton*, 4> veryHighVelocityDrumSampleButtons{};
    std::array<QPushButton*, 4> clearVeryHighVelocityDrumSampleButtons{};
    std::array<QSpinBox*, 4> veryHighVelocityThresholdControls{};
    QLabel* masterGainReadout = nullptr;
    QLabel* instrumentHeading = nullptr;
    QPushButton* clapInstrumentControlsButton = nullptr;
    QLabel* playheadLabel = nullptr;
    QLabel* audioDeviceLabel = nullptr;
    QAction* playAction = nullptr;
    QAction* stopAction = nullptr;
    QAction* loopAction = nullptr;
    QAction* metronomeAction = nullptr;
    QAction* addTrackAction = nullptr;
    QAction* addDrumTrackAction = nullptr;
    QAction* addClapInstrumentTrackAction = nullptr;
    QAction* addPatternAction = nullptr;
    QAction* undoAction = nullptr;
    QAction* redoAction = nullptr;
    QUndoStack undoStack;
    QTimer* uiTimer = nullptr;
    QTimer* autosaveTimer = nullptr;
    QTimer* automationWriteIdleTimer = nullptr;
    QMediaDevices* mediaDevices = nullptr;
    QComboBox* audioInputControl = nullptr;
    QDoubleSpinBox* audioInputCompensationControl = nullptr;
    QAction* recordAction = nullptr;
    QAction* punchRecordAction = nullptr;
    QAction* audioMonitorAction = nullptr;
    QAction* audioMonitorArmAction = nullptr;
    QAction* midiRecordAction = nullptr;
    QAction* midiMonitorAction = nullptr;
    QAction* automationViewAction = nullptr;
    QAction* automationWriteAction = nullptr;
    QComboBox* automationWriteModeControl = nullptr;
    QComboBox* automationLaneParameterControl = nullptr;
    QDoubleSpinBox* punchInControl = nullptr;
    QDoubleSpinBox* punchOutControl = nullptr;
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
    quint64 recordingTargetTrackId = 0;
    double recordingStartBeat = 0.0;
    double recordingInputCompensationMs = 0.0;
    double recordingPunchInBeat = 0.0;
    double recordingPunchOutBeat = 16.0;
    quint64 recordingDecodedByteLimit = 0;
    bool recordingPending = false;
    bool recordingTransportActive = false;
    bool recordingPunchEnabled = false;
    bool recordingPunchPriorLoopEnabled = false;
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
        QString pluginUri;
        qsizetype effectIndex = -1;
        qsizetype parameterIndex = -1;
        float minimum = 0.0f;
        float maximum = 1.0f;
        QString parameterName;
        float initialValue = 0.0f;
        float baseValue = 0.0f;
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
