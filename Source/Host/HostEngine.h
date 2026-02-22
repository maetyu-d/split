#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <cstdint>
#include <vector>

class HostEngine final : public juce::Component,
                         private juce::OSCReceiver,
                         private juce::OSCReceiver::Listener<juce::OSCReceiver::MessageLoopCallback>,
                         private juce::AudioIODeviceCallback,
                         private juce::Timer
{
public:
    HostEngine();
    ~HostEngine() override;

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;

private:
    class PluginEditorWindow;
    class LogWindow;
    static constexpr int numLanes = 10;
    static constexpr int numFxSlots = 3;
    static constexpr int numMasterFxSlots = 6;
    static constexpr int maxDueOffsPerBlock = 4096;
    static constexpr int rtEventFifoCapacity = 8192;

    enum class RtEventType : uint8_t
    {
        noteOn = 0,
        noteOff = 1
    };

    struct RtEvent
    {
        RtEventType type = RtEventType::noteOn;
        int lane = 0;
        int channel = 1;
        int note = 60;
        float velocity = 0.0f;
        float durationSec = -1.0f;
    };

    struct PendingMidiEvent
    {
        juce::MidiMessage message;
        uint64_t dueSample = 0;
    };

    struct Lane
    {
        juce::Synthesiser synth;
        juce::AudioBuffer<float> tempBuffer;
        std::unique_ptr<juce::AudioProcessorGraph> graph;
        juce::AudioPluginInstance* plugin = nullptr; // owned by graph node when present
        juce::String pluginName { "Built-in Sine" };
        std::array<std::unique_ptr<juce::AudioPluginInstance>, numFxSlots> fxPlugins;
        std::array<juce::String, numFxSlots> fxNames { "None", "None", "None" };
        std::atomic<float> gain { 1.0f };
        std::atomic<float> pan { 0.0f }; // -1..+1
        std::atomic<bool> muted { false };
        std::atomic<bool> solo { false };
        std::atomic<int> auditionSamplesRemaining { 0 };

        std::vector<PendingMidiEvent> midiQueue;
        juce::SpinLock midiQueueLock;
        std::array<uint8_t, 16 * 128> activeNotes {};
    };

    struct ScheduledNoteOff
    {
        int lane = 0;
        int channel = 1;
        int note = 60;
        float velocity = 0.0f;
        uint64_t dueSample = 0;
    };

    struct DueNoteOff
    {
        int lane = 0;
        int channel = 1;
        int note = 60;
        float velocity = 0.0f;
        int sampleOffset = 0;
    };

    void oscMessageReceived(const juce::OSCMessage& message) override;

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void timerCallback() override;

    void handleTrigger(const juce::OSCMessage& m);
    void handleNote(const juce::OSCMessage& m);
    void handleNoteOff(const juce::OSCMessage& m);
    void handleMod(const juce::OSCMessage& m);
    void handleTransport(const juce::OSCMessage& m);
    void handleTempo(const juce::OSCMessage& m);

    Lane* getLane(int index);
    bool enqueueMidiMessage(int laneIndex, const juce::MidiMessage& message, uint64_t dueSample = 0);
    void popMidiEventsForLane(Lane& lane, juce::MidiBuffer& out, uint64_t blockStartSample, int numSamples);
    bool pushRtEvent(const RtEvent& ev);
    void drainRtEventsToMidi(uint64_t blockStartSample);
    int collectDueNoteOffs(std::array<DueNoteOff, maxDueOffsPerBlock>& out, uint64_t blockStartSample, int numSamples);
    void scheduleNoteOffAtSample(int laneIndex, int midiChannel, int midiNote, float velocity, uint64_t dueSample);
    void scheduleNoteOff(int laneIndex, int midiChannel, int midiNote, float velocity, float durationSec);
    void markLaneOscActivity(int laneIndex);
    void appendLog(const juce::String& text);
    void refreshPluginCatalog(bool forceRescan = false);
    void rebuildCatalogFromKnownPlugins();
    juce::String computePluginScanSignature() const;
    bool loadPluginCatalogCache(const juce::String& signature);
    void savePluginCatalogCache(const juce::String& signature);
    bool loadInstrumentIntoLane(int laneIndex, int instrumentIndex);
    void unloadInstrumentFromLane(int laneIndex);
    bool loadPluginDescriptionIntoLane(int laneIndex, const juce::PluginDescription& desc, const juce::String& stateBase64);
    bool rebuildLaneGraphWithInstrument(int laneIndex, std::unique_ptr<juce::AudioPluginInstance> instrument);
    bool loadEffectIntoLaneSlot(int laneIndex, int slotIndex, int effectIndex);
    bool loadEffectDescriptionIntoLaneSlot(int laneIndex, int slotIndex, const juce::PluginDescription& desc, const juce::String& stateBase64);
    void unloadEffectFromLaneSlot(int laneIndex, int slotIndex);
    bool loadMasterEffectIntoSlot(int slotIndex, int effectIndex);
    bool loadMasterEffectDescriptionIntoSlot(int slotIndex, const juce::PluginDescription& desc, const juce::String& stateBase64);
    void unloadMasterEffectFromSlot(int slotIndex);
    void rebuildPluginMenus();
    void syncLaneRow(int laneIndex);
    void syncLaneFxUi(int laneIndex);
    void syncMasterFxUi();
    void syncAllLaneRows();
    void openPluginEditorForLane(int laneIndex);
    void closePluginEditorForLane(int laneIndex);
    void openEffectEditorForLaneSlot(int laneIndex, int slotIndex);
    void closeEffectEditorForLaneSlot(int laneIndex, int slotIndex);
    void openMasterEffectEditorForSlot(int slotIndex);
    void closeMasterEffectEditorForSlot(int slotIndex);
    void saveHostConfigDialog();
    void loadHostConfigDialog();
    bool saveHostConfigToFile(const juce::File& file);
    bool loadHostConfigFromFile(const juce::File& file);
    juce::ValueTree buildConfigTree();
    bool applyConfigTree(const juce::ValueTree& root);

    bool startRecording(const juce::File& destination);
    void stopRecording();
    void toggleRecordingFromUi();
    void openRecordingSettingsDialog();
    bool isRecordingActive();
    void updateAudioHeartbeatUi();

    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPluginList;
    std::vector<juce::PluginDescription> instrumentCatalog;
    std::vector<juce::PluginDescription> effectCatalog;
    juce::File deadMansPedalFile;
    juce::File pluginCacheFile;

    juce::TextButton refreshPluginsButton { "Refresh Plugins" };
    juce::TextButton audioSettingsButton { "Audio Settings" };
    juce::TextButton recordButton { "Record" };
    juce::TextButton recordSettingsButton { "Rec Settings" };
    juce::TextButton saveConfigButton { "Save Config" };
    juce::TextButton loadConfigButton { "Load Config" };
    juce::TextButton logDrawerButton { "Logs" };
    juce::Label uiStatus;
    juce::TextEditor log;
    std::unique_ptr<LogWindow> logWindow;
    juce::Label masterHeaderLabel;
    juce::Slider masterGainSlider;
    juce::Label laneHeaderLabel;
    juce::Label muteHeaderLabel;
    juce::Label soloHeaderLabel;
    juce::Label gainHeaderLabel;
    juce::Label panHeaderLabel;
    juce::Label pickerHeaderLabel;
    juce::Label editHeaderLabel;
    juce::Label loadedHeaderLabel;
    std::array<juce::Label, numLanes> laneLabels;
    std::array<juce::Label, numLanes> laneOscIndicators;
    std::array<juce::ToggleButton, numLanes> laneMuteButtons;
    std::array<juce::ToggleButton, numLanes> laneSoloButtons;
    std::array<juce::Slider, numLanes> laneGainSliders;
    std::array<juce::Slider, numLanes> lanePanSliders;
    std::array<juce::ComboBox, numLanes> lanePluginPickers;
    std::array<juce::TextButton, numLanes> laneEditButtons;
    std::array<juce::TextButton, numLanes> laneFxExpandButtons;
    std::array<bool, numLanes> laneFxExpanded {};
    std::array<juce::Label, numLanes> lanePluginNameLabels;
    std::array<juce::ComboBox, numLanes * numFxSlots> laneFxPickers;
    std::array<juce::TextButton, numLanes * numFxSlots> laneFxEditButtons;
    std::array<std::unique_ptr<PluginEditorWindow>, numLanes> laneEditorWindows;
    std::array<std::unique_ptr<PluginEditorWindow>, numLanes * numFxSlots> laneFxEditorWindows;
    juce::TextButton masterFxExpandButton { "FX +" };
    bool masterFxExpanded = false;
    std::array<juce::ComboBox, numMasterFxSlots> masterFxPickers;
    std::array<juce::TextButton, numMasterFxSlots> masterFxEditButtons;
    std::array<std::unique_ptr<PluginEditorWindow>, numMasterFxSlots> masterFxEditorWindows;
    std::array<std::unique_ptr<juce::AudioPluginInstance>, numMasterFxSlots> masterFxPlugins;
    std::array<juce::String, numMasterFxSlots> masterFxNames {};
    juce::AudioBuffer<float> masterTempBuffer;
    juce::Rectangle<int> masterMeterBounds;
    std::unique_ptr<juce::FileChooser> configLoadChooser;
    std::unique_ptr<juce::FileChooser> configSaveChooser;
    std::unique_ptr<juce::FileChooser> recordSettingsChooser;
    juce::File lastLoadedConfigFile;
    juce::File recordingTargetFile;
    int recordingBitDepth = 24;
    double recordingSampleRate = 48000.0;
    bool suppressUiCallbacks = false;
    bool logDrawerOpen = false;

    juce::CriticalSection laneLock;
    juce::SpinLock scheduledLock;
    juce::CriticalSection logLock;
    bool logIncomingOsc = false;
    std::array<Lane, numLanes> lanes;
    std::array<std::atomic<uint32_t>, numLanes> laneOscActiveUntilMs {};
    juce::AbstractFifo rtEventFifo { rtEventFifoCapacity };
    std::array<RtEvent, rtEventFifoCapacity> rtEventBuffer {};
    std::vector<ScheduledNoteOff> scheduledNoteOffs;
    std::atomic<int> scheduledNoteOffCount { 0 };
    double sampleRate = 48000.0;
    int expectedBlockSize = 512;
    std::atomic<float> masterGain { 1.0f };
    std::array<std::atomic<float>, 2> masterMeterPeaks { std::atomic<float>{ 0.0f }, std::atomic<float>{ 0.0f } };
    std::array<float, 2> masterMeterLevels { 0.0f, 0.0f };
    std::atomic<uint64_t> audioCallbackCounter { 0 };
    std::atomic<uint64_t> audioSampleCounter { 0 };
    std::array<uint64_t, numLanes * 16 * 128> lastNoteOnSample {};
    int noteOnDedupeSamples = 96;
    uint64_t lastAudioCallbackCounter = 0;
    uint32_t lastHeartbeatUiMs = 0;
    uint32_t lastAudioRestartAttemptMs = 0;
    bool lastAudioRunningState = true;
    float lastTempoUiBpm = -1.0f;
    double lastTempoUiMs = 0.0;

    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;
    juce::AudioBuffer<float> recordingStereoScratch;
    juce::TimeSliceThread writerThread { "wav-writer" };
    juce::CriticalSection writerLock;
};
