#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <cstdint>
#include <vector>

class HostEngine final : public juce::Component,
                         private juce::OSCReceiver,
                         private juce::OSCReceiver::Listener<juce::OSCReceiver::MessageLoopCallback>,
                         private juce::AudioIODeviceCallback
{
public:
    HostEngine();
    ~HostEngine() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class PluginEditorWindow;
    class LogWindow;
    static constexpr int numLanes = 10;
    static constexpr int numFxSlots = 3;
    static constexpr int numMasterFxSlots = 6;
    static constexpr int midiQueueCapacity = 2048;
    static constexpr int maxDueOffsPerBlock = 256;

    struct RtMidiEvent
    {
        uint8_t status = 0;
        uint8_t data1 = 0;
        uint8_t data2 = 0;
    };

    struct Lane
    {
        juce::Synthesiser synth;
        juce::AudioBuffer<float> tempBuffer;
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::String pluginName { "Built-in Sine" };
        std::array<std::unique_ptr<juce::AudioPluginInstance>, numFxSlots> fxPlugins;
        std::array<juce::String, numFxSlots> fxNames { "None", "None", "None" };
        std::atomic<float> gain { 1.0f };
        std::atomic<float> pan { 0.0f }; // -1..+1
        std::atomic<bool> muted { false };
        std::atomic<bool> solo { false };

        std::array<RtMidiEvent, midiQueueCapacity> midiQueue {};
        std::atomic<uint32_t> midiWriteIndex { 0 };
        std::atomic<uint32_t> midiReadIndex { 0 };
    };

    struct ScheduledNoteOff
    {
        int lane = 0;
        int note = 60;
        float velocity = 0.0f;
        double dueMs = 0.0;
    };

    struct DueNoteOff
    {
        int lane = 0;
        int note = 60;
        float velocity = 0.0f;
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

    void handleTrigger(const juce::OSCMessage& m);
    void handleNote(const juce::OSCMessage& m);
    void handleNoteOff(const juce::OSCMessage& m);
    void handleMod(const juce::OSCMessage& m);
    void handleTransport(const juce::OSCMessage& m);
    void handleTempo(const juce::OSCMessage& m);

    Lane* getLane(int index);
    bool enqueueMidiMessage(int laneIndex, const juce::MidiMessage& message);
    void popMidiEventsForLane(Lane& lane, juce::MidiBuffer& out);
    int collectDueNoteOffs(std::array<DueNoteOff, maxDueOffsPerBlock>& out);
    void scheduleNoteOff(int laneIndex, int midiNote, float velocity, float durationSec);
    void appendLog(const juce::String& text);
    void refreshPluginCatalog();
    bool loadInstrumentIntoLane(int laneIndex, int instrumentIndex);
    void unloadInstrumentFromLane(int laneIndex);
    bool loadPluginDescriptionIntoLane(int laneIndex, const juce::PluginDescription& desc, const juce::String& stateBase64);
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

    void startRecording(const juce::File& destination);
    void stopRecording();

    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPluginList;
    std::vector<juce::PluginDescription> instrumentCatalog;
    std::vector<juce::PluginDescription> effectCatalog;
    juce::File deadMansPedalFile;

    juce::TextButton refreshPluginsButton { "Refresh Plugins" };
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
    std::unique_ptr<juce::FileChooser> configLoadChooser;
    std::unique_ptr<juce::FileChooser> configSaveChooser;
    bool suppressUiCallbacks = false;
    bool logDrawerOpen = false;

    juce::CriticalSection laneLock;
    juce::SpinLock scheduledLock;
    juce::CriticalSection logLock;
    bool logIncomingOsc = false;
    std::array<Lane, numLanes> lanes;
    std::vector<ScheduledNoteOff> scheduledNoteOffs;
    double sampleRate = 48000.0;
    int expectedBlockSize = 512;
    std::atomic<float> masterGain { 1.0f };
    float lastTempoUiBpm = -1.0f;
    double lastTempoUiMs = 0.0;

    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;
    juce::TimeSliceThread writerThread { "wav-writer" };
    juce::CriticalSection writerLock;
};
