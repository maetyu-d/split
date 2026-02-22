#include "HostEngine.h"
#include "../Common/OscProtocol.h"

#include <algorithm>
#include <cmath>

#if SPLIT_ENABLE_ABLETON_LINK
 #if __has_include(<ableton/Link.hpp>)
  #include <ableton/Link.hpp>
  #define SPLIT_HAS_ABLETON_LINK_BACKEND 1
 #else
  #define SPLIT_HAS_ABLETON_LINK_BACKEND 0
 #endif
#else
 #define SPLIT_HAS_ABLETON_LINK_BACKEND 0
#endif

class AbletonLinkBackend
{
public:
    AbletonLinkBackend() = default;
    virtual ~AbletonLinkBackend() = default;
    virtual bool isAvailable() const = 0;
    virtual void setEnabled(bool enabled) = 0;
    virtual bool isEnabled() const = 0;
    virtual void setTempo(double bpm) = 0;
    virtual double getTempo() = 0;
};

#if SPLIT_HAS_ABLETON_LINK_BACKEND
class AbletonLinkBackendImpl final : public AbletonLinkBackend
{
public:
    AbletonLinkBackendImpl()
        : link(120.0)
    {
        link.enable(false);
        link.enableStartStopSync(true);
    }

    bool isAvailable() const override { return true; }

    void setEnabled(bool enabled) override
    {
        link.enable(enabled);
    }

    bool isEnabled() const override
    {
        return link.isEnabled();
    }

    void setTempo(double bpm) override
    {
        const auto micros = link.clock().micros();
        auto state = link.captureAppSessionState();
        state.setTempo(juce::jlimit(20.0, 300.0, bpm), micros);
        link.commitAppSessionState(state);
    }

    double getTempo() override
    {
        const auto state = link.captureAppSessionState();
        return state.tempo();
    }

private:
    ableton::Link link;
};
#else
class AbletonLinkBackendImpl final : public AbletonLinkBackend
{
public:
    bool isAvailable() const override { return false; }
    void setEnabled(bool) override {}
    bool isEnabled() const override { return false; }
    void setTempo(double) override {}
    double getTempo() override { return 0.0; }
};
#endif

namespace
{
class SineSound final : public juce::SynthesiserSound
{
public:
    bool appliesToNote(int) override { return true; }
    bool appliesToChannel(int) override { return true; }
};

class SineVoice final : public juce::SynthesiserVoice
{
public:
    bool canPlaySound(juce::SynthesiserSound* s) override
    {
        return dynamic_cast<SineSound*> (s) != nullptr;
    }

    void startNote(int midiNoteNumber, float velocity, juce::SynthesiserSound*, int) override
    {
        currentAngle = 0.0;
        level = velocity * 0.18;
        tailOff = 0.0;

        const auto cyclesPerSecond = juce::MidiMessage::getMidiNoteInHertz(midiNoteNumber);
        const auto cyclesPerSample = cyclesPerSecond / getSampleRate();
        angleDelta = cyclesPerSample * juce::MathConstants<double>::twoPi;
    }

    void stopNote(float, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            if (tailOff == 0.0)
                tailOff = 1.0;
        }
        else
        {
            clearCurrentNote();
            angleDelta = 0.0;
        }
    }

    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}

    void renderNextBlock(juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples) override
    {
        if (angleDelta == 0.0)
            return;

        if (tailOff > 0.0)
        {
            while (--numSamples >= 0)
            {
                const auto currentSample = static_cast<float> (std::sin(currentAngle) * level * tailOff);
                for (int i = outputBuffer.getNumChannels(); --i >= 0;)
                    outputBuffer.addSample(i, startSample, currentSample);

                currentAngle += angleDelta;
                ++startSample;
                tailOff *= 0.99;

                if (tailOff <= 0.005)
                {
                    clearCurrentNote();
                    angleDelta = 0.0;
                    break;
                }
            }
            return;
        }

        while (--numSamples >= 0)
        {
            const auto currentSample = static_cast<float> (std::sin(currentAngle) * level);
            for (int i = outputBuffer.getNumChannels(); --i >= 0;)
                outputBuffer.addSample(i, startSample, currentSample);

            currentAngle += angleDelta;
            ++startSample;
        }
    }

private:
    double currentAngle = 0.0;
    double angleDelta = 0.0;
    double level = 0.0;
    double tailOff = 0.0;
};

juce::String messageToString(const juce::OSCMessage& m)
{
    juce::String out;
    out << m.getAddressPattern().toString();
    for (auto i = 0; i < m.size(); ++i)
    {
        out << " ";
        const auto arg = m[i];
        if (arg.isInt32()) out << arg.getInt32();
        else if (arg.isFloat32()) out << arg.getFloat32();
        else if (arg.isString()) out << arg.getString();
        else out << "<unsupported>";
    }
    return out;
}

int getIntArg(const juce::OSCMessage& m, int index, int fallback)
{
    if (index >= m.size())
        return fallback;
    if (m[index].isInt32())
        return m[index].getInt32();
    if (m[index].isFloat32())
        return static_cast<int> (m[index].getFloat32());
    return fallback;
}

float getFloatArg(const juce::OSCMessage& m, int index, float fallback)
{
    if (index >= m.size())
        return fallback;
    if (m[index].isFloat32())
        return m[index].getFloat32();
    if (m[index].isInt32())
        return static_cast<float> (m[index].getInt32());
    return fallback;
}

juce::String getStringArg(const juce::OSCMessage& m, int index)
{
    if (index >= m.size() || !m[index].isString())
        return {};
    return m[index].getString();
}

bool configurePluginBuses(juce::AudioPluginInstance& plugin, bool isInstrument)
{
    plugin.enableAllBuses();

    if (isInstrument)
        return true; // Leave instrument bus layout untouched; many commercial synths expect native defaults.

    plugin.disableNonMainBuses();

    const bool hasInput = plugin.getBusCount(true) > 0;
    const bool hasOutput = plugin.getBusCount(false) > 0;
    auto base = plugin.getBusesLayout();

    if (! hasOutput)
        return true;

    const auto inDisabled = juce::AudioChannelSet::disabled();
    const auto inMono = juce::AudioChannelSet::mono();
    const auto inStereo = juce::AudioChannelSet::stereo();
    const auto outMono = juce::AudioChannelSet::mono();
    const auto outStereo = juce::AudioChannelSet::stereo();

    std::vector<std::pair<juce::AudioChannelSet, juce::AudioChannelSet>> candidates;
    if (isInstrument)
    {
        candidates.push_back({ inDisabled, outStereo });
        candidates.push_back({ inDisabled, outMono });
        candidates.push_back({ inStereo, outStereo });
        candidates.push_back({ inMono, outMono });
    }
    else
    {
        candidates.push_back({ inStereo, outStereo });
        candidates.push_back({ inMono, outMono });
        candidates.push_back({ inStereo, outMono });
        candidates.push_back({ inMono, outStereo });
    }

    for (const auto& candidate : candidates)
    {
        auto attempt = base;
        if (hasInput)
            attempt.getMainInputChannelSet() = candidate.first;
        if (hasOutput)
            attempt.getMainOutputChannelSet() = candidate.second;

        if (plugin.checkBusesLayoutSupported(attempt) && plugin.setBusesLayout(attempt))
        {
            if (hasInput)
                if (auto* inBus = plugin.getBus(true, 0))
                    inBus->enable(! isInstrument);
            if (hasOutput)
                if (auto* outBus = plugin.getBus(false, 0))
                    outBus->enable(true);
            return true;
        }
    }

    auto lastResort = plugin.getBusesLayout();
    if (hasOutput && lastResort.getMainOutputChannelSet().isDisabled())
        lastResort.getMainOutputChannelSet() = outStereo;

    if (hasInput)
        lastResort.getMainInputChannelSet() = isInstrument ? inDisabled : inStereo;

    if (plugin.checkBusesLayoutSupported(lastResort) && plugin.setBusesLayout(lastResort))
    {
        if (hasInput)
            if (auto* inBus = plugin.getBus(true, 0))
                inBus->enable(! isInstrument);
        if (hasOutput)
            if (auto* outBus = plugin.getBus(false, 0))
                outBus->enable(true);
        return true;
    }

    return false;
}

juce::String channelSetToString(const juce::AudioChannelSet& set)
{
    if (set.isDisabled())
        return "disabled";
    return set.getDescription();
}

juce::String pluginIoSummary(juce::AudioPluginInstance& plugin)
{
    juce::String s;
    s << "inBuses=" << plugin.getBusCount(true)
      << " outBuses=" << plugin.getBusCount(false)
      << " acceptsMidi=" << (plugin.acceptsMidi() ? "1" : "0")
      << " producesMidi=" << (plugin.producesMidi() ? "1" : "0")
      << " isMidiEffect=" << (plugin.isMidiEffect() ? "1" : "0")
      << " totalInCh=" << plugin.getTotalNumInputChannels()
      << " totalOutCh=" << plugin.getTotalNumOutputChannels();

    const auto layout = plugin.getBusesLayout();
    if (plugin.getBusCount(true) > 0)
        s << " mainIn=" << channelSetToString(layout.getMainInputChannelSet());
    if (plugin.getBusCount(false) > 0)
        s << " mainOut=" << channelSetToString(layout.getMainOutputChannelSet());
    return s;
}

bool preparePluginInstance(juce::AudioPluginInstance& plugin, bool isInstrument, double sampleRate, int blockSize)
{
    plugin.releaseResources();
    plugin.setNonRealtime(false);
    plugin.suspendProcessing(false);
    const auto busesOk = configurePluginBuses(plugin, isInstrument);
    const auto totalIns = plugin.getTotalNumInputChannels();
    const auto totalOuts = plugin.getTotalNumOutputChannels();
    plugin.setPlayConfigDetails(totalIns, totalOuts, sampleRate, blockSize);
    plugin.setRateAndBufferSizeDetails(sampleRate, blockSize);
    plugin.prepareToPlay(sampleRate, blockSize);
    plugin.reset();
    return busesOk;
}

constexpr int kNumFxSlots = 3;
constexpr int kNumMasterFxSlots = 6;
constexpr int kHostProcessChannels = 16;

int fxUiIndex(int laneIndex, int slotIndex)
{
    return (laneIndex * kNumFxSlots) + slotIndex;
}

juce::String gainToDbText(double gainLinear)
{
    if (gainLinear <= 0.000001)
        return "-inf dB";

    const auto db = juce::Decibels::gainToDecibels(static_cast<float> (gainLinear), -100.0f);
    return juce::String(db, 1) + " dB";
}

double dbTextToGain(const juce::String& text)
{
    auto trimmed = text.trim().toLowerCase();
    trimmed = trimmed.upToFirstOccurrenceOf("db", false, false).trim();
    if (trimmed.isEmpty() || trimmed == "-inf" || trimmed == "-infinity")
        return 0.0;

    const auto db = static_cast<float> (trimmed.getDoubleValue());
    return juce::jlimit(0.0, 2.0, static_cast<double> (juce::Decibels::decibelsToGain(db, -100.0f)));
}

class ZeroDbSliderLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawLinearSlider(juce::Graphics& g,
                          int x, int y, int width, int height,
                          float sliderPos,
                          float minSliderPos,
                          float maxSliderPos,
                          const juce::Slider::SliderStyle style,
                          juce::Slider& slider) override
    {
        juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);

        if (style != juce::Slider::LinearVertical)
            return;

        const auto minV = static_cast<float> (slider.getMinimum());
        const auto maxV = static_cast<float> (slider.getMaximum());
        if (! (minV < 1.0f && maxV > 1.0f))
            return;

        const auto zeroPos = juce::jmap(1.0f, minV, maxV, static_cast<float> (y + height), static_cast<float> (y));
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.drawLine(static_cast<float> (x + 3), zeroPos, static_cast<float> (x + width - 3), zeroPos, 1.0f);
    }
};

ZeroDbSliderLookAndFeel zeroDbSliderLookAndFeel;
} // namespace

class HostEngine::PluginEditorWindow final : public juce::DocumentWindow
{
public:
    PluginEditorWindow(const juce::String& name, std::function<void()> onCloseIn)
        : juce::DocumentWindow(name,
                               juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
                               juce::DocumentWindow::closeButton),
          onClose(std::move(onCloseIn))
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
    }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose();
    }

private:
    std::function<void()> onClose;
};

class HostEngine::LogWindow final : public juce::DocumentWindow
{
public:
    LogWindow(juce::TextEditor& editorIn, std::function<void(bool)> onVisibleChangeIn)
        : juce::DocumentWindow("Host Logs",
                               juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
                               juce::DocumentWindow::allButtons),
          editor(editorIn),
          onVisibleChange(std::move(onVisibleChangeIn))
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        setContentNonOwned(&editor, false);
        setVisible(false);
    }

    void closeButtonPressed() override
    {
        setVisible(false);
        if (onVisibleChange)
            onVisibleChange(false);
    }

private:
    juce::TextEditor& editor;
    std::function<void(bool)> onVisibleChange;
};

HostEngine::HostEngine()
{
    linkBackend = std::make_unique<AbletonLinkBackendImpl>();
    linkAvailable = (linkBackend != nullptr && linkBackend->isAvailable());

   #if JUCE_PLUGINHOST_AU
    formatManager.addFormat(std::make_unique<juce::AudioUnitPluginFormat>());
   #endif
   #if JUCE_PLUGINHOST_VST3
    formatManager.addFormat(std::make_unique<juce::VST3PluginFormat>());
   #endif

    deadMansPedalFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("split-audio-deadmanspedal.txt");
    pluginCacheFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                          .getChildFile("SplitAudioProgramming")
                          .getChildFile("plugin-scan-cache.xml");

    for (auto& lane : lanes)
    {
        lane.synth.clearSounds();
        lane.synth.clearVoices();
        for (int v = 0; v < 8; ++v)
            lane.synth.addVoice(new SineVoice());
        lane.synth.addSound(new SineSound());
        lane.midiQueue.reserve(4096);
    }
    scheduledNoteOffs.reserve(8192);

    setOpaque(true);

    auto styleTopButton = [] (juce::TextButton& b)
    {
        b.setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(43, 49, 58));
        b.setColour(juce::TextButton::buttonOnColourId, juce::Colour::fromRGB(59, 68, 82));
        b.setColour(juce::TextButton::textColourOffId, juce::Colour::fromRGB(225, 230, 238));
        b.setColour(juce::TextButton::textColourOnId, juce::Colour::fromRGB(243, 247, 252));
    };

    addAndMakeVisible(refreshPluginsButton);
    styleTopButton(refreshPluginsButton);
    refreshPluginsButton.onClick = [this]
    {
        refreshPluginCatalog(true);
    };

    addAndMakeVisible(audioSettingsButton);
    styleTopButton(audioSettingsButton);
    audioSettingsButton.onClick = [this]
    {
        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle = "Audio Settings";
        options.dialogBackgroundColour = juce::Colour::fromRGB(24, 28, 36);
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = false;

        auto* selector = new juce::AudioDeviceSelectorComponent(deviceManager,
                                                                0, 2,   // min/max inputs
                                                                0, 2,   // min/max outputs
                                                                false,  // hide midi input options
                                                                false,  // hide midi output selector
                                                                true,   // show channels as stereo pairs
                                                                false); // hide advanced options
        selector->setSize(560, 420);
        options.content.setOwned(selector);
        options.componentToCentreAround = this;
        options.launchAsync();
    };

    addAndMakeVisible(linkButton);
    styleTopButton(linkButton);
    linkButton.setEnabled(linkAvailable);
    linkButton.setButtonText(linkAvailable ? "Link Off" : "Link N/A");
    linkButton.onClick = [this]
    {
        setLinkEnabled(! linkEnabled);
    };

    addAndMakeVisible(recordButton);
    recordButton.setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(74, 46, 46));
    recordButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour::fromRGB(138, 52, 52));
    recordButton.setColour(juce::TextButton::textColourOffId, juce::Colour::fromRGB(244, 210, 210));
    recordButton.setColour(juce::TextButton::textColourOnId, juce::Colour::fromRGB(255, 234, 234));
    recordButton.onClick = [this] { toggleRecordingFromUi(); };

    addAndMakeVisible(recordSettingsButton);
    styleTopButton(recordSettingsButton);
    recordSettingsButton.onClick = [this] { openRecordingSettingsDialog(); };

    addAndMakeVisible(saveConfigButton);
    styleTopButton(saveConfigButton);
    saveConfigButton.onClick = [this]
    {
        saveHostConfigDialog();
    };

    addAndMakeVisible(loadConfigButton);
    styleTopButton(loadConfigButton);
    loadConfigButton.onClick = [this]
    {
        loadHostConfigDialog();
    };

    addAndMakeVisible(logDrawerButton);
    styleTopButton(logDrawerButton);
    logDrawerButton.onClick = [this]
    {
        logDrawerOpen = ! logDrawerOpen;
        logDrawerButton.setButtonText(logDrawerOpen ? "Hide Logs" : "Logs");

        if (logWindow != nullptr)
        {
            if (logDrawerOpen)
            {
                const auto hostBounds = getScreenBounds();
                const auto w = juce::jmax(640, hostBounds.getWidth());
                logWindow->setBounds(hostBounds.withHeight(280).withWidth(w).withY(hostBounds.getBottom() + 8).withX(hostBounds.getX()));
                logWindow->setVisible(true);
                logWindow->toFront(true);
            }
            else
            {
                logWindow->setVisible(false);
            }
        }
    };

    addAndMakeVisible(uiStatus);
    uiStatus.setJustificationType(juce::Justification::centredLeft);
    uiStatus.setColour(juce::Label::textColourId, juce::Colour::fromRGB(216, 223, 234));
    uiStatus.setColour(juce::Label::backgroundColourId, juce::Colour::fromRGB(27, 33, 42).withAlpha(0.72f));
    uiStatus.setColour(juce::Label::outlineColourId, juce::Colour::fromRGB(66, 78, 96).withAlpha(0.55f));
    uiStatus.setOpaque(true);
    uiStatus.setText("Ready. Click Reload Plugins to scan AU/VST3.", juce::dontSendNotification);

    addAndMakeVisible(topRightStatusBox);
    topRightStatusBox.setJustificationType(juce::Justification::centredLeft);
    topRightStatusBox.setColour(juce::Label::backgroundColourId, juce::Colour::fromRGB(24, 30, 39).withAlpha(0.88f));
    topRightStatusBox.setColour(juce::Label::outlineColourId, juce::Colour::fromRGB(66, 78, 96).withAlpha(0.72f));
    topRightStatusBox.setColour(juce::Label::textColourId, juce::Colours::transparentBlack);
    topRightStatusBox.setOpaque(true);
    topRightStatusBox.setText({}, juce::dontSendNotification);

    auto styleStateLine = [] (juce::Label& l)
    {
        l.setJustificationType(juce::Justification::centredLeft);
        l.setColour(juce::Label::textColourId, juce::Colour::fromRGB(214, 222, 235));
        l.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        l.setFont(juce::Font(juce::FontOptions("Menlo", 11.5f, juce::Font::plain)));
    };

    addAndMakeVisible(audioStateLabel);
    styleStateLine(audioStateLabel);
    addAndMakeVisible(oscStateLabel);
    styleStateLine(oscStateLabel);
    addAndMakeVisible(linkStateLabel);
    styleStateLine(linkStateLabel);

    addAndMakeVisible(masterHeaderLabel);
    masterHeaderLabel.setText("Master", juce::dontSendNotification);
    masterHeaderLabel.setJustificationType(juce::Justification::centred);
    masterHeaderLabel.setColour(juce::Label::textColourId, juce::Colour::fromRGB(214, 188, 115));
    masterHeaderLabel.setColour(juce::Label::backgroundColourId, juce::Colour::fromRGB(61, 54, 35).withAlpha(0.45f));
    masterHeaderLabel.setOpaque(true);
    addAndMakeVisible(masterGainSlider);
    masterGainSlider.setLookAndFeel(&zeroDbSliderLookAndFeel);
    masterGainSlider.setSliderStyle(juce::Slider::LinearVertical);
    masterGainSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 68, 18);
    masterGainSlider.setColour(juce::Slider::trackColourId, juce::Colour::fromRGB(205, 173, 95));
    masterGainSlider.setColour(juce::Slider::thumbColourId, juce::Colour::fromRGB(224, 199, 133));
    masterGainSlider.setColour(juce::Slider::backgroundColourId, juce::Colour::fromRGB(48, 44, 33));
    masterGainSlider.setColour(juce::Slider::textBoxTextColourId, juce::Colour::fromRGB(214, 188, 115));
    masterGainSlider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colour::fromRGB(138, 116, 70));
    masterGainSlider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour::fromRGB(39, 36, 27));
    masterGainSlider.setRange(0.0, 2.0, 0.001);
    masterGainSlider.setDoubleClickReturnValue(true, 1.0);
    masterGainSlider.textFromValueFunction = [] (double v) { return gainToDbText(v); };
    masterGainSlider.valueFromTextFunction = [] (const juce::String& t) { return dbTextToGain(t); };
    masterGainSlider.setValue(masterGain.load(), juce::dontSendNotification);
    masterGainSlider.onValueChange = [this]
    {
        if (suppressUiCallbacks)
            return;

        masterGain.store(static_cast<float> (masterGainSlider.getValue()));
    };

    addAndMakeVisible(masterFxExpandButton);
    masterFxExpandButton.setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(73, 63, 39));
    masterFxExpandButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour::fromRGB(92, 79, 48));
    masterFxExpandButton.setColour(juce::TextButton::textColourOnId, juce::Colour::fromRGB(221, 198, 138));
    masterFxExpandButton.setColour(juce::TextButton::textColourOffId, juce::Colour::fromRGB(210, 186, 122));
    masterFxExpandButton.onClick = [this]
    {
        masterFxExpanded = ! masterFxExpanded;
        masterFxExpandButton.setButtonText(masterFxExpanded ? "FX -" : "FX +");
        resized();
    };
    masterFxExpandButton.setButtonText("FX +");

    addAndMakeVisible(laneHeaderLabel);
    laneHeaderLabel.setText("Lane", juce::dontSendNotification);
    addAndMakeVisible(muteHeaderLabel);
    muteHeaderLabel.setText("Mute", juce::dontSendNotification);
    addAndMakeVisible(soloHeaderLabel);
    soloHeaderLabel.setText("Solo", juce::dontSendNotification);
    addAndMakeVisible(gainHeaderLabel);
    gainHeaderLabel.setText("Gain", juce::dontSendNotification);
    addAndMakeVisible(panHeaderLabel);
    panHeaderLabel.setText("Pan", juce::dontSendNotification);
    addAndMakeVisible(pickerHeaderLabel);
    pickerHeaderLabel.setText("Picker", juce::dontSendNotification);
    addAndMakeVisible(editHeaderLabel);
    editHeaderLabel.setText("Edit", juce::dontSendNotification);
    addAndMakeVisible(loadedHeaderLabel);
    loadedHeaderLabel.setText("Loaded", juce::dontSendNotification);
    laneHeaderLabel.setVisible(false);
    muteHeaderLabel.setVisible(false);
    soloHeaderLabel.setVisible(false);
    gainHeaderLabel.setVisible(false);
    panHeaderLabel.setVisible(false);
    pickerHeaderLabel.setVisible(false);
    editHeaderLabel.setVisible(false);
    loadedHeaderLabel.setVisible(false);

    for (int i = 0; i < static_cast<int> (lanes.size()); ++i)
    {
        addAndMakeVisible(laneLabels[static_cast<size_t> (i)]);
        laneLabels[static_cast<size_t> (i)].setText("T" + juce::String(i + 1), juce::dontSendNotification);
        laneLabels[static_cast<size_t> (i)].setJustificationType(juce::Justification::centred);
        laneLabels[static_cast<size_t> (i)].setColour(juce::Label::textColourId, juce::Colour::fromRGB(216, 220, 228));

        addAndMakeVisible(laneOscIndicators[static_cast<size_t> (i)]);
        laneOscIndicators[static_cast<size_t> (i)].setText("●", juce::dontSendNotification);
        laneOscIndicators[static_cast<size_t> (i)].setJustificationType(juce::Justification::centred);
        laneOscIndicators[static_cast<size_t> (i)].setColour(juce::Label::textColourId, juce::Colour::fromRGB(112, 128, 148));
        laneOscIndicators[static_cast<size_t> (i)].setAlpha(0.28f);

        addAndMakeVisible(laneMuteButtons[static_cast<size_t> (i)]);
        laneMuteButtons[static_cast<size_t> (i)].setButtonText("M");
        laneMuteButtons[static_cast<size_t> (i)].setClickingTogglesState(true);
        laneMuteButtons[static_cast<size_t> (i)].setColour(juce::ToggleButton::textColourId, juce::Colour::fromRGB(245, 173, 173));
        laneMuteButtons[static_cast<size_t> (i)].setColour(juce::ToggleButton::tickColourId, juce::Colour::fromRGB(235, 86, 86));
        laneMuteButtons[static_cast<size_t> (i)].setColour(juce::ToggleButton::tickDisabledColourId, juce::Colour::fromRGB(96, 76, 76));
        laneMuteButtons[static_cast<size_t> (i)].onClick = [this, i]
        {
            if (suppressUiCallbacks)
                return;

            {
                const juce::ScopedLock scope(laneLock);
                if (auto* lane = getLane(i))
                    lane->muted.store(laneMuteButtons[static_cast<size_t> (i)].getToggleState());
            }

            syncLaneRow(i);
        };

        addAndMakeVisible(laneSoloButtons[static_cast<size_t> (i)]);
        laneSoloButtons[static_cast<size_t> (i)].setButtonText("S");
        laneSoloButtons[static_cast<size_t> (i)].setClickingTogglesState(true);
        laneSoloButtons[static_cast<size_t> (i)].setColour(juce::ToggleButton::textColourId, juce::Colour::fromRGB(176, 222, 178));
        laneSoloButtons[static_cast<size_t> (i)].setColour(juce::ToggleButton::tickColourId, juce::Colour::fromRGB(94, 204, 117));
        laneSoloButtons[static_cast<size_t> (i)].setColour(juce::ToggleButton::tickDisabledColourId, juce::Colour::fromRGB(68, 92, 68));
        laneSoloButtons[static_cast<size_t> (i)].onClick = [this, i]
        {
            if (suppressUiCallbacks)
                return;

            const juce::ScopedLock scope(laneLock);
            if (auto* lane = getLane(i))
                lane->solo.store(laneSoloButtons[static_cast<size_t> (i)].getToggleState());
        };

        addAndMakeVisible(laneGainSliders[static_cast<size_t> (i)]);
        laneGainSliders[static_cast<size_t> (i)].setLookAndFeel(&zeroDbSliderLookAndFeel);
        laneGainSliders[static_cast<size_t> (i)].setSliderStyle(juce::Slider::LinearVertical);
        laneGainSliders[static_cast<size_t> (i)].setTextBoxStyle(juce::Slider::TextBoxBelow, false, 68, 16);
        laneGainSliders[static_cast<size_t> (i)].setColour(juce::Slider::trackColourId, juce::Colour::fromRGB(91, 171, 245));
        laneGainSliders[static_cast<size_t> (i)].setColour(juce::Slider::thumbColourId, juce::Colour::fromRGB(146, 202, 252));
        laneGainSliders[static_cast<size_t> (i)].setColour(juce::Slider::backgroundColourId, juce::Colour::fromRGB(36, 42, 52));
        laneGainSliders[static_cast<size_t> (i)].setColour(juce::Slider::textBoxTextColourId, juce::Colour::fromRGB(210, 218, 230));
        laneGainSliders[static_cast<size_t> (i)].setColour(juce::Slider::textBoxOutlineColourId, juce::Colour::fromRGB(74, 86, 104));
        laneGainSliders[static_cast<size_t> (i)].setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour::fromRGB(33, 37, 45));
        laneGainSliders[static_cast<size_t> (i)].setRange(0.0, 2.0, 0.001);
        laneGainSliders[static_cast<size_t> (i)].setDoubleClickReturnValue(true, 1.0);
        laneGainSliders[static_cast<size_t> (i)].textFromValueFunction = [] (double v) { return gainToDbText(v); };
        laneGainSliders[static_cast<size_t> (i)].valueFromTextFunction = [] (const juce::String& t) { return dbTextToGain(t); };
        laneGainSliders[static_cast<size_t> (i)].onValueChange = [this, i]
        {
            if (suppressUiCallbacks)
                return;

            const juce::ScopedLock scope(laneLock);
            if (auto* lane = getLane(i))
                lane->gain.store(static_cast<float> (laneGainSliders[static_cast<size_t> (i)].getValue()));
        };

        addAndMakeVisible(lanePanSliders[static_cast<size_t> (i)]);
        lanePanSliders[static_cast<size_t> (i)].setSliderStyle(juce::Slider::LinearHorizontal);
        lanePanSliders[static_cast<size_t> (i)].setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 16);
        lanePanSliders[static_cast<size_t> (i)].setColour(juce::Slider::trackColourId, juce::Colour::fromRGB(140, 151, 172));
        lanePanSliders[static_cast<size_t> (i)].setColour(juce::Slider::thumbColourId, juce::Colour::fromRGB(201, 209, 224));
        lanePanSliders[static_cast<size_t> (i)].setColour(juce::Slider::backgroundColourId, juce::Colour::fromRGB(36, 42, 52));
        lanePanSliders[static_cast<size_t> (i)].setColour(juce::Slider::textBoxTextColourId, juce::Colour::fromRGB(210, 218, 230));
        lanePanSliders[static_cast<size_t> (i)].setColour(juce::Slider::textBoxOutlineColourId, juce::Colour::fromRGB(74, 86, 104));
        lanePanSliders[static_cast<size_t> (i)].setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour::fromRGB(33, 37, 45));
        lanePanSliders[static_cast<size_t> (i)].setRange(-1.0, 1.0, 0.001);
        lanePanSliders[static_cast<size_t> (i)].onValueChange = [this, i]
        {
            if (suppressUiCallbacks)
                return;

            const juce::ScopedLock scope(laneLock);
            if (auto* lane = getLane(i))
                lane->pan.store(static_cast<float> (lanePanSliders[static_cast<size_t> (i)].getValue()));
        };

        addAndMakeVisible(lanePluginPickers[static_cast<size_t> (i)]);
        lanePluginPickers[static_cast<size_t> (i)].setColour(juce::ComboBox::backgroundColourId, juce::Colour::fromRGB(35, 39, 48));
        lanePluginPickers[static_cast<size_t> (i)].setColour(juce::ComboBox::textColourId, juce::Colour::fromRGB(219, 223, 230));
        lanePluginPickers[static_cast<size_t> (i)].setColour(juce::ComboBox::outlineColourId, juce::Colour::fromRGB(74, 86, 104));
        lanePluginPickers[static_cast<size_t> (i)].onChange = [this, i]
        {
            if (suppressUiCallbacks)
                return;

            const auto selectedId = lanePluginPickers[static_cast<size_t> (i)].getSelectedId();
            if (selectedId <= 0)
                return;

            if (selectedId == 1)
            {
                unloadInstrumentFromLane(i);
                syncLaneRow(i);
                return;
            }

            const auto catalogIndex = selectedId - 2;
            if (loadInstrumentIntoLane(i, catalogIndex))
                syncLaneRow(i);
            else
                syncLaneRow(i);
        };

        addAndMakeVisible(laneEditButtons[static_cast<size_t> (i)]);
        laneEditButtons[static_cast<size_t> (i)].setButtonText("E");
        laneEditButtons[static_cast<size_t> (i)].setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(45, 50, 58));
        laneEditButtons[static_cast<size_t> (i)].setColour(juce::TextButton::buttonOnColourId, juce::Colour::fromRGB(62, 71, 84));
        laneEditButtons[static_cast<size_t> (i)].setColour(juce::TextButton::textColourOffId, juce::Colour::fromRGB(228, 233, 241));
        laneEditButtons[static_cast<size_t> (i)].setColour(juce::TextButton::textColourOnId, juce::Colour::fromRGB(236, 241, 249));
        laneEditButtons[static_cast<size_t> (i)].onClick = [this, i]
        {
            openPluginEditorForLane(i);
        };

        addAndMakeVisible(laneFxExpandButtons[static_cast<size_t> (i)]);
        laneFxExpandButtons[static_cast<size_t> (i)].setButtonText("FX +");
        laneFxExpandButtons[static_cast<size_t> (i)].setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(38, 60, 72));
        laneFxExpandButtons[static_cast<size_t> (i)].setColour(juce::TextButton::buttonOnColourId, juce::Colour::fromRGB(48, 81, 97));
        laneFxExpandButtons[static_cast<size_t> (i)].setColour(juce::TextButton::textColourOffId, juce::Colour::fromRGB(179, 229, 250));
        laneFxExpandButtons[static_cast<size_t> (i)].setColour(juce::TextButton::textColourOnId, juce::Colour::fromRGB(203, 239, 252));
        laneFxExpandButtons[static_cast<size_t> (i)].onClick = [this, i]
        {
            laneFxExpanded[static_cast<size_t> (i)] = ! laneFxExpanded[static_cast<size_t> (i)];
            laneFxExpandButtons[static_cast<size_t> (i)].setButtonText(laneFxExpanded[static_cast<size_t> (i)] ? "FX -" : "FX +");
            resized();
        };

        addAndMakeVisible(lanePluginNameLabels[static_cast<size_t> (i)]);
        lanePluginNameLabels[static_cast<size_t> (i)].setJustificationType(juce::Justification::centred);
        lanePluginNameLabels[static_cast<size_t> (i)].setText("Built-in Sine", juce::dontSendNotification);
        lanePluginNameLabels[static_cast<size_t> (i)].setMinimumHorizontalScale(0.7f);
        lanePluginNameLabels[static_cast<size_t> (i)].setColour(juce::Label::textColourId, juce::Colour::fromRGB(160, 170, 186));

        for (int slot = 0; slot < kNumFxSlots; ++slot)
        {
            auto& picker = laneFxPickers[static_cast<size_t> (fxUiIndex(i, slot))];
            auto& editButton = laneFxEditButtons[static_cast<size_t> (fxUiIndex(i, slot))];
            addAndMakeVisible(picker);
            picker.setColour(juce::ComboBox::backgroundColourId, juce::Colour::fromRGB(35, 39, 48));
            picker.setColour(juce::ComboBox::textColourId, juce::Colour::fromRGB(208, 214, 224));
            picker.setColour(juce::ComboBox::outlineColourId, juce::Colour::fromRGB(67, 78, 94));
            picker.setVisible(false);
            picker.onChange = [this, i, slot]
            {
                if (suppressUiCallbacks)
                    return;

                const auto selectedId = laneFxPickers[static_cast<size_t> (fxUiIndex(i, slot))].getSelectedId();
                if (selectedId <= 0)
                    return;

                if (selectedId == 1)
                {
                    unloadEffectFromLaneSlot(i, slot);
                    syncLaneFxUi(i);
                    return;
                }

                const auto effectIndex = selectedId - 2;
                if (loadEffectIntoLaneSlot(i, slot, effectIndex))
                    syncLaneFxUi(i);
                else
                    syncLaneFxUi(i);
            };

            addAndMakeVisible(editButton);
            editButton.setButtonText("E");
            editButton.setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(45, 50, 58));
            editButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour::fromRGB(62, 71, 84));
            editButton.setColour(juce::TextButton::textColourOffId, juce::Colour::fromRGB(228, 233, 241));
            editButton.setColour(juce::TextButton::textColourOnId, juce::Colour::fromRGB(236, 241, 249));
            editButton.setVisible(false);
            editButton.onClick = [this, i, slot]
            {
                openEffectEditorForLaneSlot(i, slot);
            };
        }
    }

    for (int slot = 0; slot < kNumMasterFxSlots; ++slot)
    {
        masterFxNames[static_cast<size_t> (slot)] = "None";
        auto& picker = masterFxPickers[static_cast<size_t> (slot)];
        auto& editButton = masterFxEditButtons[static_cast<size_t> (slot)];
        addAndMakeVisible(picker);
        picker.setColour(juce::ComboBox::outlineColourId, juce::Colour::fromRGB(126, 108, 69));
        picker.setColour(juce::ComboBox::textColourId, juce::Colour::fromRGB(214, 188, 115));
        picker.setColour(juce::ComboBox::backgroundColourId, juce::Colour::fromRGB(45, 41, 31));
        picker.setVisible(false);
        picker.onChange = [this, slot]
        {
            if (suppressUiCallbacks)
                return;

            const auto selectedId = masterFxPickers[static_cast<size_t> (slot)].getSelectedId();
            if (selectedId <= 0)
                return;

            if (selectedId == 1)
            {
                unloadMasterEffectFromSlot(slot);
                syncMasterFxUi();
                return;
            }

            const auto effectIndex = selectedId - 2;
            if (loadMasterEffectIntoSlot(slot, effectIndex))
                syncMasterFxUi();
            else
                syncMasterFxUi();
        };

        addAndMakeVisible(editButton);
        editButton.setButtonText("E");
        editButton.setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(73, 63, 39));
        editButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour::fromRGB(92, 79, 48));
        editButton.setColour(juce::TextButton::textColourOnId, juce::Colour::fromRGB(221, 198, 138));
        editButton.setColour(juce::TextButton::textColourOffId, juce::Colour::fromRGB(210, 186, 122));
        editButton.setVisible(false);
        editButton.onClick = [this, slot]
        {
            openMasterEffectEditorForSlot(slot);
        };
    }

    log.setMultiLine(true);
    log.setReadOnly(true);
    log.setScrollbarsShown(true);
    log.setCaretVisible(false);
    log.setText("Host started (" __DATE__ " " __TIME__ ")\n");
    logWindow = std::make_unique<LogWindow>(log, [this] (bool visible)
    {
        logDrawerOpen = visible;
        logDrawerButton.setButtonText(logDrawerOpen ? "Hide Logs" : "Logs");
    });

    rebuildPluginMenus();
    syncAllLaneRows();

    writerThread.startThread();
    recordingTargetFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                              .getNonexistentChildFile("split-audio-take", ".wav");

    const auto initError = deviceManager.initialiseWithDefaultDevices(0, 2);
    if (initError.isNotEmpty())
    {
        appendLog("Audio init error: " + initError);
        uiStatus.setText("Audio init error: " + initError, juce::dontSendNotification);
    }
    else
    {
        appendLog("Audio init ok");
    }

    if (auto* dev = deviceManager.getCurrentAudioDevice())
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        deviceManager.getAudioDeviceSetup(setup);
        setup.useDefaultInputChannels = false;
        setup.useDefaultOutputChannels = true;
        setup.bufferSize = juce::jmax(128, setup.bufferSize);
        const auto setupError = deviceManager.setAudioDeviceSetup(setup, true);
        if (setupError.isNotEmpty())
            appendLog("Audio setup warning: " + setupError);

        deviceManager.restartLastAudioDevice();

        dev = deviceManager.getCurrentAudioDevice();
        appendLog("Audio device: " + dev->getName()
                  + " sr=" + juce::String(dev->getCurrentSampleRate(), 1)
                  + " block=" + juce::String(dev->getCurrentBufferSizeSamples())
                  + " activeOut=" + juce::String(dev->getActiveOutputChannels().countNumberOfSetBits())
                  + " namedOut=" + juce::String(dev->getOutputChannelNames().size()));
        recordingSampleRate = dev->getCurrentSampleRate();
    }
    else
    {
        appendLog("Audio device: none");
        uiStatus.setText("No active audio output device", juce::dontSendNotification);
    }

    deviceManager.addAudioCallback(this);

    oscConnected = connect(oscproto::defaultPort);
    if (oscConnected)
        addListener(this);
    else
        appendLog("OSC bind failed on port " + juce::String(oscproto::defaultPort));
    startTimerHz(20);

    refreshPluginCatalog(false);
    if (oscConnected)
        appendLog("Listening OSC on port " + juce::String(oscproto::defaultPort));
    refreshTopRightStatusBox();
}

HostEngine::~HostEngine()
{
    stopTimer();

    masterGainSlider.setLookAndFeel(nullptr);
    for (auto& s : laneGainSliders)
        s.setLookAndFeel(nullptr);

    logWindow.reset();

    for (int i = 0; i < static_cast<int> (laneEditorWindows.size()); ++i)
        closePluginEditorForLane(i);
    for (int i = 0; i < static_cast<int> (lanes.size()); ++i)
        for (int slot = 0; slot < kNumFxSlots; ++slot)
            closeEffectEditorForLaneSlot(i, slot);
    for (int slot = 0; slot < kNumMasterFxSlots; ++slot)
        closeMasterEffectEditorForSlot(slot);

    stopRecording();
    deviceManager.removeAudioCallback(this);
    disconnect();
    writerThread.stopThread(1000);
}

void HostEngine::timerCallback()
{
    updateAudioHeartbeatUi();

    const auto now = juce::Time::getMillisecondCounter();
    for (int i = 0; i < static_cast<int> (laneOscIndicators.size()); ++i)
    {
        const auto activeUntil = laneOscActiveUntilMs[static_cast<size_t> (i)].load(std::memory_order_relaxed);
        const auto active = (activeUntil > now);
        laneOscIndicators[static_cast<size_t> (i)].setColour(juce::Label::textColourId,
                                                             active ? juce::Colour::fromRGB(130, 242, 186)
                                                                    : juce::Colour::fromRGB(112, 128, 148));
        laneOscIndicators[static_cast<size_t> (i)].setAlpha(active ? 1.0f : 0.28f);
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto blockPeak = masterMeterPeaks[static_cast<size_t> (ch)].exchange(0.0f, std::memory_order_relaxed);
        auto& level = masterMeterLevels[static_cast<size_t> (ch)];
        level = juce::jmax(blockPeak, level * 0.82f);
        level = juce::jlimit(0.0f, 1.0f, level);
    }
    if (! masterMeterBounds.isEmpty())
        repaint(masterMeterBounds.expanded(4));
}

void HostEngine::setLinkEnabled(bool shouldEnable)
{
    if (! linkAvailable || linkBackend == nullptr)
        return;

    linkEnabled = shouldEnable;
    linkBackend->setEnabled(shouldEnable);
    linkButton.setButtonText(linkEnabled ? "Link On" : "Link Off");
    appendLog(linkEnabled ? "Ableton Link enabled" : "Ableton Link disabled");

    if (linkEnabled)
        pushTempoToLink(transportTempoBpm.load(std::memory_order_relaxed));

    refreshTopRightStatusBox();
}

void HostEngine::pushTempoToLink(double bpm)
{
    if (! linkEnabled || linkBackend == nullptr)
        return;
    linkBackend->setTempo(juce::jlimit(20.0, 300.0, bpm));
}

double HostEngine::pullTempoFromLink()
{
    if (! linkEnabled || linkBackend == nullptr)
        return 0.0;

    const auto bpm = linkBackend->getTempo();
    return juce::jlimit(20.0, 300.0, bpm);
}

void HostEngine::refreshTopRightStatusBox()
{
    const auto bpm = transportTempoBpm.load(std::memory_order_relaxed);
    audioStateLabel.setText(lastAudioRunningState ? "Audio: Running" : "Audio: Stopped", juce::dontSendNotification);
    audioStateLabel.setColour(juce::Label::textColourId, lastAudioRunningState
                                                            ? juce::Colour::fromRGB(165, 232, 182)
                                                            : juce::Colour::fromRGB(240, 170, 170));

    oscStateLabel.setText(oscConnected
                              ? ("OSC: Listening " + juce::String(oscproto::defaultPort))
                              : ("OSC: Disconnected " + juce::String(oscproto::defaultPort)),
                          juce::dontSendNotification);
    oscStateLabel.setColour(juce::Label::textColourId, oscConnected
                                                          ? juce::Colour::fromRGB(172, 214, 255)
                                                          : juce::Colour::fromRGB(240, 170, 170));

    if (! linkAvailable)
    {
        linkStateLabel.setText("Link: N/A", juce::dontSendNotification);
        linkStateLabel.setColour(juce::Label::textColourId, juce::Colour::fromRGB(162, 174, 190));
    }
    else if (linkEnabled)
    {
        linkStateLabel.setText("Link: On  " + juce::String(bpm, 2) + " BPM", juce::dontSendNotification);
        linkStateLabel.setColour(juce::Label::textColourId, juce::Colour::fromRGB(220, 196, 132));
    }
    else
    {
        linkStateLabel.setText("Link: Off  " + juce::String(bpm, 2) + " BPM", juce::dontSendNotification);
        linkStateLabel.setColour(juce::Label::textColourId, juce::Colour::fromRGB(196, 179, 140));
    }
}

void HostEngine::updateAudioHeartbeatUi()
{
    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastHeartbeatUiMs < 500)
        return;
    lastHeartbeatUiMs = now;

    const auto c = audioCallbackCounter.load(std::memory_order_relaxed);
    const auto running = (c != lastAudioCallbackCounter);
    const auto stateChanged = (running != lastAudioRunningState);
    lastAudioCallbackCounter = c;
    if (stateChanged)
    {
        appendLog(running ? "Audio callback: running" : "Audio callback: stopped");
        lastAudioRunningState = running;
        uiStatus.setText(running ? "Audio callback: running" : "Audio callback: stopped", juce::dontSendNotification);
    }

    if (linkEnabled)
    {
        const auto linkTempo = pullTempoFromLink();
        if (linkTempo > 0.0)
            transportTempoBpm.store(static_cast<float> (linkTempo), std::memory_order_relaxed);
    }

    refreshTopRightStatusBox();

}

void HostEngine::markLaneOscActivity(int laneIndex)
{
    if (! juce::isPositiveAndBelow(laneIndex, static_cast<int> (laneOscActiveUntilMs.size())))
        return;

    laneOscActiveUntilMs[static_cast<size_t> (laneIndex)].store(juce::Time::getMillisecondCounter() + 180u,
                                                                 std::memory_order_relaxed);
}

void HostEngine::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(juce::Colour::fromRGB(20, 23, 30), 0.0f, 0.0f,
                            juce::Colour::fromRGB(13, 15, 19), 0.0f, static_cast<float> (getHeight()), false);
    g.setGradientFill(bg);
    g.fillAll();

    auto bounds = getLocalBounds().reduced(8);
    const auto topBar = bounds.removeFromTop(56);
    g.setColour(juce::Colour::fromRGB(33, 38, 47));
    g.fillRoundedRectangle(topBar.toFloat(), 6.0f);
    g.setColour(juce::Colour::fromRGB(56, 64, 77).withAlpha(0.72f));
    g.drawRoundedRectangle(topBar.toFloat().reduced(0.5f), 6.0f, 1.0f);
    g.setColour(juce::Colour::fromRGB(95, 108, 128).withAlpha(0.18f));
    g.drawLine(static_cast<float> (topBar.getX() + 8), static_cast<float> (topBar.getY() + 1),
               static_cast<float> (topBar.getRight() - 8), static_cast<float> (topBar.getY() + 1), 1.0f);

    auto drawTopSep = [&] (int x)
    {
        g.setColour(juce::Colour::fromRGB(86, 97, 116).withAlpha(0.45f));
        g.drawLine(static_cast<float> (x), static_cast<float> (topBar.getY() + 8),
                   static_cast<float> (x), static_cast<float> (topBar.getBottom() - 8), 1.0f);
    };
    drawTopSep(recordButton.getX() - 4);
    drawTopSep(audioSettingsButton.getX() - 4);
    drawTopSep(logDrawerButton.getX() - 8);

    g.setColour(juce::Colour::fromRGB(170, 182, 200).withAlpha(0.42f));
    g.setFont(juce::Font(juce::FontOptions("Menlo", 11.0f, juce::Font::plain)));
    g.drawText("by matd.space",
               topBar.toNearestInt().removeFromRight(126).reduced(4, 0),
               juce::Justification::centredRight,
               false);

    g.setColour(juce::Colour::fromRGB(18, 22, 29).withAlpha(0.42f));
    g.fillRoundedRectangle(bounds.toFloat(), 8.0f);
    g.setColour(juce::Colour::fromRGB(53, 61, 74).withAlpha(0.55f));
    g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), 8.0f, 1.0f);

    const auto masterArea = masterHeaderLabel.getBounds()
                                .getUnion(masterFxExpandButton.getBounds())
                                .getUnion(masterGainSlider.getBounds())
                                .getUnion(masterMeterBounds)
                                .expanded(4, 4);
    g.setColour(juce::Colour::fromRGB(64, 58, 44).withAlpha(0.18f));
    g.fillRoundedRectangle(masterArea.toFloat(), 7.0f);

}

void HostEngine::paintOverChildren(juce::Graphics& g)
{
    if (masterMeterBounds.isEmpty())
        return;

    const auto fullMeterRect = masterMeterBounds;
    g.setColour(juce::Colour::fromRGB(24, 28, 34).withAlpha(0.9f));
    g.fillRoundedRectangle(fullMeterRect.toFloat(), 2.0f);
    g.setColour(juce::Colour::fromRGB(74, 86, 104).withAlpha(0.85f));
    g.drawRoundedRectangle(fullMeterRect.toFloat().reduced(0.5f), 2.0f, 1.0f);

    auto meterRect = fullMeterRect;
    auto leftBar = meterRect.removeFromLeft(meterRect.getWidth() / 2).reduced(1, 1);
    auto rightBar = meterRect.reduced(1, 1);

    auto drawBar = [&] (juce::Rectangle<int> r, float level)
    {
        const auto lv = juce::jlimit(0.0f, 1.0f, level);
        const auto fillH = static_cast<int> (std::round(lv * static_cast<float> (r.getHeight())));
        if (fillH <= 0)
            return;
        auto fill = r.withTrimmedTop(juce::jmax(0, r.getHeight() - fillH));
        auto grad = juce::ColourGradient(juce::Colour::fromRGB(94, 204, 117),
                                         static_cast<float> (fill.getX()),
                                         static_cast<float> (fill.getBottom()),
                                         juce::Colour::fromRGB(235, 185, 94),
                                         static_cast<float> (fill.getX()),
                                         static_cast<float> (fill.getY()),
                                         false);
        grad.addColour(0.88, juce::Colour::fromRGB(228, 102, 92));
        g.setGradientFill(grad);
        g.fillRoundedRectangle(fill.toFloat(), 1.2f);
    };
    drawBar(leftBar, masterMeterLevels[0]);
    drawBar(rightBar, masterMeterLevels[1]);

    // Subtle 0 dB reference (top of digital full-scale meter).
    const auto zeroDbY = fullMeterRect.getY() + 1;
    g.setColour(juce::Colour::fromRGB(214, 188, 115).withAlpha(0.42f));
    g.drawLine(static_cast<float> (fullMeterRect.getX() + 1),
               static_cast<float> (zeroDbY),
               static_cast<float> (fullMeterRect.getRight() - 1),
               static_cast<float> (zeroDbY),
               1.0f);

    const auto minus3Linear = std::pow(10.0f, -3.0f / 20.0f); // ~0.7079
    const auto innerH = juce::jmax(1, fullMeterRect.getHeight() - 2);
    const auto minus3Y = fullMeterRect.getBottom() - 1 - static_cast<int> (std::round(minus3Linear * static_cast<float> (innerH)));
    g.setColour(juce::Colour::fromRGB(148, 184, 150).withAlpha(0.28f));
    g.drawLine(static_cast<float> (fullMeterRect.getX() + 1),
               static_cast<float> (minus3Y),
               static_cast<float> (fullMeterRect.getRight() - 1),
               static_cast<float> (minus3Y),
               1.0f);
}

void HostEngine::resized()
{
    auto area = getLocalBounds().reduced(8);
    const bool compactWidth = area.getWidth() < 980;
    const bool veryCompactWidth = area.getWidth() < 780;
    const bool compactHeight = area.getHeight() < 420;
    const int topBarHeight = compactHeight ? 46 : (compactWidth ? 50 : 56);
    const int laneGap = veryCompactWidth ? 3 : (compactWidth ? 5 : 8);

    auto top = area.removeFromTop(topBarHeight);
    auto controls = top;

    const int audioW = compactWidth ? 112 : 136;       // system
    const int linkW = compactWidth ? 86 : 104;         // sync
    const int refreshW = compactWidth ? 104 : 130;     // system
    const int recW = compactWidth ? 72 : 92;           // recording
    const int recSettingsW = compactWidth ? 96 : 118;  // recording
    const int saveW = compactWidth ? 86 : 110;         // config
    const int loadW = compactWidth ? 86 : 110;         // config
    const int logsW = compactWidth ? 64 : 90;          // utility
    const int statusBoxW = compactWidth ? 210 : 260;   // separated Audio/OSC/Link lines
    const int groupGap = compactWidth ? 4 : 8;
    const int controlVMargin = compactHeight ? 6 : 8;

    // Group 1: system
    refreshPluginsButton.setBounds(controls.removeFromLeft(refreshW).reduced(2, controlVMargin));
    controls.removeFromLeft(groupGap);

    // Group 2: recording
    recordButton.setBounds(controls.removeFromLeft(recW).reduced(2, controlVMargin));
    recordSettingsButton.setBounds(controls.removeFromLeft(recSettingsW).reduced(2, controlVMargin));
    controls.removeFromLeft(groupGap);

    // Group 3: audio + sync + config
    audioSettingsButton.setBounds(controls.removeFromLeft(audioW).reduced(2, controlVMargin));
    linkButton.setBounds(controls.removeFromLeft(linkW).reduced(2, controlVMargin));
    controls.removeFromLeft(groupGap);
    loadConfigButton.setBounds(controls.removeFromLeft(loadW).reduced(2, controlVMargin));
    saveConfigButton.setBounds(controls.removeFromLeft(saveW).reduced(2, controlVMargin));

    // Group 4: utility on right edge
    auto rightControls = controls.removeFromRight(logsW);
    logDrawerButton.setBounds(rightControls.reduced(2, controlVMargin));
    auto statusBoxArea = controls.removeFromRight(statusBoxW).reduced(2, controlVMargin);
    topRightStatusBox.setBounds(statusBoxArea);
    auto statusInner = statusBoxArea.reduced(8, compactHeight ? 3 : 4);
    const auto rowH = juce::jmax(10, statusInner.getHeight() / 3);
    audioStateLabel.setBounds(statusInner.removeFromTop(rowH));
    oscStateLabel.setBounds(statusInner.removeFromTop(rowH));
    linkStateLabel.setBounds(statusInner);

    uiStatus.setBounds(controls.reduced(8, 8));

    const auto laneCount = static_cast<int> (lanes.size());
    const auto columnCount = laneCount + 1; // +1 for master lane
    const auto totalGap = laneGap * (columnCount - 1);
    const auto usableWidth = juce::jmax(0, area.getWidth() - totalGap);
    const auto laneWidth = juce::jmax(1, usableWidth / columnCount);
    const bool hidePluginNames = laneWidth < 78;
    const float laneNameFont = laneWidth < 70 ? 11.0f : 13.0f;
    const float pluginNameFont = laneWidth < 70 ? 10.0f : 11.0f;
    const int laneLabelH = laneWidth < 70 ? 18 : 20;
    const int pickerH = laneWidth < 70 ? 24 : 26;
    const int pluginNameH = hidePluginNames ? 0 : (laneWidth < 70 ? 14 : 18);
    const int editH = laneWidth < 70 ? 22 : 24;
    const int muteSoloH = laneWidth < 70 ? 22 : 24;
    const int panH = compactHeight ? 42 : 46;

    for (int i = 0; i < laneCount; ++i)
    {
        auto col = area.removeFromLeft(laneWidth);
        if (i < laneCount - 1)
            area.removeFromLeft(laneGap);

        auto laneHead = col.removeFromTop(laneLabelH);
        laneOscIndicators[static_cast<size_t> (i)].setBounds(laneHead.removeFromRight(14));
        laneLabels[static_cast<size_t> (i)].setBounds(laneHead);
        laneLabels[static_cast<size_t> (i)].setFont(juce::FontOptions(laneNameFont));
        lanePluginPickers[static_cast<size_t> (i)].setBounds(col.removeFromTop(pickerH).reduced(0, 1));
        lanePluginNameLabels[static_cast<size_t> (i)].setVisible(! hidePluginNames);
        if (! hidePluginNames)
        {
            lanePluginNameLabels[static_cast<size_t> (i)].setBounds(col.removeFromTop(pluginNameH));
            lanePluginNameLabels[static_cast<size_t> (i)].setFont(juce::FontOptions(pluginNameFont));
        }
        else
        {
            lanePluginNameLabels[static_cast<size_t> (i)].setBounds({});
        }

        auto editRow = col.removeFromTop(editH);
        auto editArea = editRow.reduced(2, 1);
        laneEditButtons[static_cast<size_t> (i)].setBounds(editArea.removeFromLeft(editArea.getWidth() / 2).reduced(1, 0));
        laneFxExpandButtons[static_cast<size_t> (i)].setBounds(editArea.reduced(1, 0));

        auto msRow = col.removeFromTop(muteSoloH);
        laneMuteButtons[static_cast<size_t> (i)].setBounds(msRow.removeFromLeft(msRow.getWidth() / 2).reduced(2, 0));
        laneSoloButtons[static_cast<size_t> (i)].setBounds(msRow.reduced(2, 0));

        col.removeFromTop(compactHeight ? 2 : 4);
        const bool fxExpanded = laneFxExpanded[static_cast<size_t> (i)];
        const int fxRowH = laneWidth < 70 ? 20 : 22;
        const int fxPanelH = fxExpanded ? ((fxRowH + 2) * kNumFxSlots + 2) : 0;

        if (fxExpanded)
        {
            auto fxPanel = col.removeFromBottom(fxPanelH);
            for (int slot = 0; slot < kNumFxSlots; ++slot)
            {
                auto row = fxPanel.removeFromTop(fxRowH);
                fxPanel.removeFromTop(2);
                auto& picker = laneFxPickers[static_cast<size_t> (fxUiIndex(i, slot))];
                auto& editButton = laneFxEditButtons[static_cast<size_t> (fxUiIndex(i, slot))];
                editButton.setVisible(true);
                picker.setVisible(true);
                editButton.setBounds(row.removeFromRight(24).reduced(1));
                picker.setBounds(row.reduced(1, 0));
            }
            col.removeFromBottom(2);
        }
        else
        {
            for (int slot = 0; slot < kNumFxSlots; ++slot)
            {
                laneFxPickers[static_cast<size_t> (fxUiIndex(i, slot))].setVisible(false);
                laneFxEditButtons[static_cast<size_t> (fxUiIndex(i, slot))].setVisible(false);
                laneFxPickers[static_cast<size_t> (fxUiIndex(i, slot))].setBounds({});
                laneFxEditButtons[static_cast<size_t> (fxUiIndex(i, slot))].setBounds({});
            }
        }

        lanePanSliders[static_cast<size_t> (i)].setBounds(col.removeFromBottom(panH).reduced(2, 0));
        laneGainSliders[static_cast<size_t> (i)].setBounds(col.reduced(2, 0));
    }

    if (laneCount > 0)
        area.removeFromLeft(laneGap);

    auto masterCol = area.removeFromLeft(laneWidth);
    const int masterLabelH = laneWidth < 70 ? 18 : 20;
    const int masterButtonH = laneWidth < 70 ? 22 : 24;
    const int masterFxRowH = laneWidth < 70 ? 20 : 22;

    masterHeaderLabel.setBounds(masterCol.removeFromTop(masterLabelH));
    masterFxExpandButton.setBounds(masterCol.removeFromTop(masterButtonH).reduced(4, 1));
    masterCol.removeFromTop(4);

    if (masterFxExpanded)
    {
        const int fxPanelH = ((masterFxRowH + 2) * kNumMasterFxSlots) + 2;
        auto fxPanel = masterCol.removeFromBottom(fxPanelH);
        for (int slot = 0; slot < kNumMasterFxSlots; ++slot)
        {
            auto row = fxPanel.removeFromTop(masterFxRowH);
            fxPanel.removeFromTop(2);
            auto& picker = masterFxPickers[static_cast<size_t> (slot)];
            auto& editButton = masterFxEditButtons[static_cast<size_t> (slot)];
            picker.setVisible(true);
            editButton.setVisible(true);
            editButton.setBounds(row.removeFromRight(24).reduced(1));
            picker.setBounds(row.reduced(1, 0));
        }
        masterCol.removeFromBottom(4);
    }
    else
    {
        for (int slot = 0; slot < kNumMasterFxSlots; ++slot)
        {
            masterFxPickers[static_cast<size_t> (slot)].setVisible(false);
            masterFxEditButtons[static_cast<size_t> (slot)].setVisible(false);
            masterFxPickers[static_cast<size_t> (slot)].setBounds({});
            masterFxEditButtons[static_cast<size_t> (slot)].setBounds({});
        }
    }

    auto masterArea = masterCol.reduced(6, 2);
    masterGainSlider.setBounds(masterArea);
    const int meterW = 10;
    masterMeterBounds = masterGainSlider.getBounds()
                           .withTrimmedLeft(juce::jmax(0, masterGainSlider.getWidth() - meterW))
                           .reduced(1, 8);

    laneHeaderLabel.setBounds({});
    muteHeaderLabel.setBounds({});
    soloHeaderLabel.setBounds({});
    gainHeaderLabel.setBounds({});
    panHeaderLabel.setBounds({});
    pickerHeaderLabel.setBounds({});
    editHeaderLabel.setBounds({});
    loadedHeaderLabel.setBounds({});
}

void HostEngine::oscMessageReceived(const juce::OSCMessage& message)
{
    if (logIncomingOsc)
        appendLog(messageToString(message));

    const auto address = message.getAddressPattern().toString();
    if (address == oscproto::triggerAddress) { handleTrigger(message); return; }
    if (address == oscproto::noteOnAddress) { handleNote(message); return; }
    if (address == oscproto::noteOffAddress) { handleNoteOff(message); return; }
    if (address == oscproto::modAddress) { handleMod(message); return; }
    if (address == oscproto::transportAddress) { handleTransport(message); return; }
    if (address == oscproto::tempoAddress) { handleTempo(message); return; }
}

void HostEngine::audioDeviceIOCallbackWithContext(const float* const*,
                                                  int,
                                                  float* const* outputChannelData,
                                                  int numOutputChannels,
                                                  int numSamples,
                                                  const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;
    audioCallbackCounter.fetch_add(1, std::memory_order_relaxed);
    const auto blockStartSample = audioSampleCounter.fetch_add(static_cast<uint64_t> (numSamples), std::memory_order_relaxed);

    juce::AudioBuffer<float> outputBuffer(outputChannelData, numOutputChannels, numSamples);
    outputBuffer.clear();

    drainRtEventsToMidi(blockStartSample);

    std::array<DueNoteOff, maxDueOffsPerBlock> dueOffs {};
    const auto dueCount = collectDueNoteOffs(dueOffs, blockStartSample, numSamples);

    bool anySoloed = false;
    {
        anySoloed = std::any_of(lanes.begin(), lanes.end(), [] (const auto& lane) { return lane.solo.load(); });
    }

    for (int laneIndex = 0; laneIndex < static_cast<int> (lanes.size()); ++laneIndex)
    {
        auto& lane = lanes[static_cast<size_t> (laneIndex)];
        if (lane.tempBuffer.getNumSamples() < numSamples)
            continue; // Avoid allocating in audio thread.

        juce::AudioBuffer<float> laneBlock(lane.tempBuffer.getArrayOfWritePointers(),
                                           lane.tempBuffer.getNumChannels(),
                                           numSamples);
        laneBlock.clear();

        juce::MidiBuffer midi;
        popMidiEventsForLane(lane, midi, blockStartSample, numSamples);
        for (int i = 0; i < dueCount; ++i)
            if (dueOffs[static_cast<size_t> (i)].lane == laneIndex)
                midi.addEvent(juce::MidiMessage::noteOff(dueOffs[static_cast<size_t> (i)].channel,
                                                         dueOffs[static_cast<size_t> (i)].note,
                                                         dueOffs[static_cast<size_t> (i)].velocity),
                              dueOffs[static_cast<size_t> (i)].sampleOffset);

        const auto gain = lane.gain.load();
        const auto pan = lane.pan.load();
        const auto muted = lane.muted.load();
        const auto solo = lane.solo.load();
        auto* plugin = lane.plugin;
        const auto auditionRemaining = lane.auditionSamplesRemaining.load(std::memory_order_relaxed);
        const auto auditionActive = auditionRemaining > 0;
        if (auditionActive)
            lane.auditionSamplesRemaining.store(juce::jmax(0, auditionRemaining - numSamples), std::memory_order_relaxed);

        if (! auditionActive && (muted || (anySoloed && ! solo)))
            continue;

        if (lane.graph != nullptr && plugin != nullptr)
            lane.graph->processBlock(laneBlock, midi);
        else
            lane.synth.renderNextBlock(laneBlock, midi, 0, numSamples);

        juce::MidiBuffer fxMidi;
        for (auto& fx : lane.fxPlugins)
            if (fx != nullptr && ! fx->isSuspended())
                fx->processBlock(laneBlock, fxMidi);

        const auto clampedPan = juce::jlimit(-1.0f, 1.0f, pan);
        const auto leftGain = gain * juce::jlimit(0.0f, 1.0f, 1.0f - clampedPan);
        const auto rightGain = gain * juce::jlimit(0.0f, 1.0f, 1.0f + clampedPan);

        if (numOutputChannels > 0)
            outputBuffer.addFrom(0, 0, laneBlock, 0, 0, numSamples, leftGain);
        if (numOutputChannels > 1)
            outputBuffer.addFrom(1, 0, laneBlock, juce::jmin(1, laneBlock.getNumChannels() - 1), 0, numSamples, rightGain);
        for (int ch = 2; ch < numOutputChannels; ++ch)
            outputBuffer.addFrom(ch, 0, laneBlock, juce::jmin(ch, laneBlock.getNumChannels() - 1), 0, numSamples, gain);
    }

    if (masterTempBuffer.getNumSamples() >= numSamples)
    {
        juce::AudioBuffer<float> masterBlock(masterTempBuffer.getArrayOfWritePointers(),
                                             masterTempBuffer.getNumChannels(),
                                             numSamples);
        masterBlock.clear();
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (ch < masterBlock.getNumChannels())
                masterBlock.copyFrom(ch, 0, outputBuffer, ch, 0, numSamples);

        juce::MidiBuffer masterFxMidi;
        for (auto& fx : masterFxPlugins)
            if (fx != nullptr && ! fx->isSuspended())
                fx->processBlock(masterBlock, masterFxMidi);

        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (ch < masterBlock.getNumChannels())
                outputBuffer.copyFrom(ch, 0, masterBlock, ch, 0, numSamples);
    }

    outputBuffer.applyGain(masterGain.load());

    float blockPeakL = 0.0f;
    float blockPeakR = 0.0f;
    if (outputBuffer.getNumChannels() > 0)
    {
        const auto* l = outputBuffer.getReadPointer(0);
        for (int i = 0; i < numSamples; ++i)
            blockPeakL = juce::jmax(blockPeakL, std::abs(l[i]));
    }
    if (outputBuffer.getNumChannels() > 1)
    {
        const auto* r = outputBuffer.getReadPointer(1);
        for (int i = 0; i < numSamples; ++i)
            blockPeakR = juce::jmax(blockPeakR, std::abs(r[i]));
    }
    else
    {
        blockPeakR = blockPeakL;
    }

    auto pushPeak = [] (std::atomic<float>& dst, float peak)
    {
        auto prev = dst.load(std::memory_order_relaxed);
        while (peak > prev && ! dst.compare_exchange_weak(prev, peak, std::memory_order_relaxed))
        {
        }
    };
    pushPeak(masterMeterPeaks[0], blockPeakL);
    pushPeak(masterMeterPeaks[1], blockPeakR);

    if (writerLock.tryEnter())
    {
        if (threadedWriter != nullptr)
        {
            if (recordingStereoScratch.getNumChannels() != 2 || recordingStereoScratch.getNumSamples() < numSamples)
                recordingStereoScratch.setSize(2, numSamples, false, false, true);

            recordingStereoScratch.clear();
            if (numOutputChannels > 0)
            {
                recordingStereoScratch.copyFrom(0, 0, outputBuffer, 0, 0, numSamples);
                if (numOutputChannels > 1)
                    recordingStereoScratch.copyFrom(1, 0, outputBuffer, 1, 0, numSamples);
                else
                    recordingStereoScratch.copyFrom(1, 0, outputBuffer, 0, 0, numSamples);
            }

            threadedWriter->write(recordingStereoScratch.getArrayOfReadPointers(), numSamples);
        }
        writerLock.exit();
    }
}

void HostEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (device == nullptr)
        return;

    const juce::ScopedLock scope(laneLock);
    sampleRate = device->getCurrentSampleRate();
    noteOnDedupeSamples = juce::jmax(16, static_cast<int> (sampleRate * 0.002)); // ~2ms duplicate suppression
    expectedBlockSize = juce::jmax(16, device->getCurrentBufferSizeSamples());
    recordingStereoScratch.setSize(2, expectedBlockSize, false, false, true);
    audioSampleCounter.store(0, std::memory_order_relaxed);
    std::fill(lastNoteOnSample.begin(), lastNoteOnSample.end(), 0);
    const auto activeOutputChannels = device->getActiveOutputChannels().countNumberOfSetBits();
    const auto namedOutputChannels = device->getOutputChannelNames().size();
    const auto outputChannels = juce::jmax(2, juce::jmax(activeOutputChannels, namedOutputChannels));
    const auto preallocSamples = juce::jmax(expectedBlockSize * 4, 4096);
    const auto hostProcessChannels = kHostProcessChannels;
    appendLog("Audio device start: sr=" + juce::String(sampleRate, 1)
              + " block=" + juce::String(expectedBlockSize)
              + " activeOut=" + juce::String(activeOutputChannels)
              + " namedOut=" + juce::String(namedOutputChannels)
              + " allocOut=" + juce::String(outputChannels));

    for (auto& lane : lanes)
    {
        lane.synth.setCurrentPlaybackSampleRate(sampleRate);
        lane.tempBuffer.setSize(hostProcessChannels, preallocSamples, false, false, true);
        if (lane.graph != nullptr && lane.plugin != nullptr)
        {
            lane.graph->releaseResources();
            lane.graph->setPlayConfigDetails(0, 2, sampleRate, expectedBlockSize);
            lane.graph->prepareToPlay(sampleRate, expectedBlockSize);
            lane.graph->reset();
            const auto busesOk = true;
            appendLog("Reinit lane instrument '" + lane.pluginName + "': "
                      + pluginIoSummary(*lane.plugin)
                      + (busesOk ? "" : " [bus fallback]"));
        }
        for (auto& fx : lane.fxPlugins)
            if (fx != nullptr)
            {
                const auto busesOk = preparePluginInstance(*fx, false, sampleRate, expectedBlockSize);
                appendLog("Reinit lane FX: " + pluginIoSummary(*fx)
                          + (busesOk ? "" : " [bus fallback]"));
                fx->suspendProcessing(! busesOk);
            }
    }

    masterTempBuffer.setSize(hostProcessChannels, preallocSamples, false, false, true);
    for (auto& fx : masterFxPlugins)
        if (fx != nullptr)
        {
            const auto busesOk = preparePluginInstance(*fx, false, sampleRate, expectedBlockSize);
            appendLog("Reinit master FX: " + pluginIoSummary(*fx)
                      + (busesOk ? "" : " [bus fallback]"));
            fx->suspendProcessing(! busesOk);
        }
}

void HostEngine::audioDeviceStopped() {}

HostEngine::Lane* HostEngine::getLane(int index)
{
    if (index < 0 || index >= static_cast<int> (lanes.size()))
        return nullptr;
    return &lanes[static_cast<size_t> (index)];
}

bool HostEngine::enqueueMidiMessage(int laneIndex, const juce::MidiMessage& message, uint64_t dueSample)
{
    auto* lane = getLane(laneIndex);
    if (lane == nullptr)
        return false;
    const juce::SpinLock::ScopedLockType scope(lane->midiQueueLock);
    const auto resolvedDueSample = (dueSample == 0)
                                       ? audioSampleCounter.load(std::memory_order_relaxed)
                                       : dueSample;

    if (message.isNoteOn())
    {
        const auto midiChannel = juce::jlimit(1, 16, message.getChannel());
        const auto noteNumber = juce::jlimit(0, 127, message.getNoteNumber());
        const auto activeIndex = ((midiChannel - 1) * 128) + noteNumber;
        // Prevent stacked note-ons on the same note/channel (can sound like AM/ring-mod beating).
        if (lane->activeNotes[static_cast<size_t> (activeIndex)] != 0)
            lane->midiQueue.push_back({ juce::MidiMessage::noteOff(midiChannel, noteNumber, 0.0f), resolvedDueSample });

        lane->midiQueue.push_back({ message, resolvedDueSample });
        lane->activeNotes[static_cast<size_t> (activeIndex)] = 1;
        return true;
    }

    if (message.isNoteOff())
    {
        const auto midiChannel = juce::jlimit(1, 16, message.getChannel());
        const auto noteNumber = juce::jlimit(0, 127, message.getNoteNumber());
        const auto activeIndex = ((midiChannel - 1) * 128) + noteNumber;
        lane->midiQueue.push_back({ message, resolvedDueSample });
        lane->activeNotes[static_cast<size_t> (activeIndex)] = 0;
        return true;
    }

    lane->midiQueue.push_back({ message, resolvedDueSample });
    return true;
}

bool HostEngine::pushRtEvent(const RtEvent& ev)
{
    if (! juce::isPositiveAndBelow(ev.lane, static_cast<int> (lanes.size())))
        return false;

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    rtEventFifo.prepareToWrite(1, start1, size1, start2, size2);
    if ((size1 + size2) <= 0)
        return false;

    if (size1 > 0)
        rtEventBuffer[static_cast<size_t> (start1)] = ev;
    else
        rtEventBuffer[static_cast<size_t> (start2)] = ev;

    rtEventFifo.finishedWrite(1);
    return true;
}

void HostEngine::drainRtEventsToMidi(uint64_t blockStartSample)
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    rtEventFifo.prepareToRead(rtEventFifoCapacity, start1, size1, start2, size2);

    auto consumeRange = [this, blockStartSample] (int start, int size)
    {
        for (int i = 0; i < size; ++i)
        {
            const auto& ev = rtEventBuffer[static_cast<size_t> (start + i)];
            const auto laneIndex = ev.lane;
            const auto channel = juce::jlimit(1, 16, ev.channel);
            const auto note = juce::jlimit(0, 127, ev.note);
            const auto velocity = juce::jlimit(0.0f, 1.0f, ev.velocity);

            if (ev.type == RtEventType::noteOn)
            {
                const auto noteStateIndex = (laneIndex * 16 * 128) + ((channel - 1) * 128) + note;
                auto& lastOn = lastNoteOnSample[static_cast<size_t> (noteStateIndex)];
                if (lastOn > 0 && blockStartSample > lastOn)
                {
                    const auto delta = blockStartSample - lastOn;
                    if (delta < static_cast<uint64_t> (noteOnDedupeSamples))
                        continue; // drop near-duplicate note-ons that create AM/ringing artifacts
                }
                lastOn = blockStartSample;

                enqueueMidiMessage(laneIndex, juce::MidiMessage::noteOn(channel, note, velocity), blockStartSample);
                if (ev.durationSec > 0.0f)
                {
                    const auto offsetSamples = static_cast<uint64_t> (juce::jmax(1.0, static_cast<double> (ev.durationSec) * sampleRate));
                    scheduleNoteOffAtSample(laneIndex, channel, note, 0.0f, blockStartSample + offsetSamples);
                }
            }
            else
            {
                enqueueMidiMessage(laneIndex, juce::MidiMessage::noteOff(channel, note, velocity), blockStartSample);
            }
        }
    };

    consumeRange(start1, size1);
    consumeRange(start2, size2);
    rtEventFifo.finishedRead(size1 + size2);
}

void HostEngine::popMidiEventsForLane(Lane& lane, juce::MidiBuffer& out, uint64_t blockStartSample, int numSamples)
{
    const auto blockEndSample = blockStartSample + static_cast<uint64_t> (juce::jmax(0, numSamples));
    const juce::SpinLock::ScopedLockType scope(lane.midiQueueLock);
    if (lane.midiQueue.empty())
        return;

    size_t writeIndex = 0;
    const auto size = lane.midiQueue.size();
    for (size_t readIndex = 0; readIndex < size; ++readIndex)
    {
        const auto& ev = lane.midiQueue[readIndex];
        if (ev.dueSample < blockEndSample)
        {
            const auto delta = (ev.dueSample <= blockStartSample)
                                   ? 0
                                   : static_cast<int> (ev.dueSample - blockStartSample);
            const auto sampleOffset = juce::jlimit(0, juce::jmax(0, numSamples - 1), delta);
            out.addEvent(ev.message, sampleOffset);
            continue;
        }

        if (writeIndex != readIndex)
            lane.midiQueue[writeIndex] = ev;
        ++writeIndex;
    }
    lane.midiQueue.resize(writeIndex);
}

int HostEngine::collectDueNoteOffs(std::array<DueNoteOff, maxDueOffsPerBlock>& out, uint64_t blockStartSample, int numSamples)
{
    if (scheduledNoteOffCount.load(std::memory_order_relaxed) <= 0)
        return 0;

    const auto blockEndSample = blockStartSample + static_cast<uint64_t> (juce::jmax(0, numSamples));
    const juce::SpinLock::ScopedLockType scope(scheduledLock);
    if (scheduledNoteOffs.empty())
    {
        scheduledNoteOffCount.store(0, std::memory_order_relaxed);
        return 0;
    }

    int count = 0;
    size_t writeIndex = 0;
    const auto size = scheduledNoteOffs.size();
    for (size_t readIndex = 0; readIndex < size; ++readIndex)
    {
        const auto& ev = scheduledNoteOffs[readIndex];
        if (ev.dueSample < blockEndSample)
        {
            if (count < maxDueOffsPerBlock)
            {
                out[static_cast<size_t> (count)].lane = ev.lane;
                out[static_cast<size_t> (count)].channel = ev.channel;
                out[static_cast<size_t> (count)].note = ev.note;
                out[static_cast<size_t> (count)].velocity = ev.velocity;
                const auto delta = (ev.dueSample <= blockStartSample)
                                       ? 0
                                       : static_cast<int> (ev.dueSample - blockStartSample);
                out[static_cast<size_t> (count)].sampleOffset = juce::jlimit(0, juce::jmax(0, numSamples - 1), delta);
                ++count;
            }
            continue; // consumed (or dropped if over maxDueOffsPerBlock)
        }

        if (writeIndex != readIndex)
            scheduledNoteOffs[writeIndex] = ev;
        ++writeIndex;
    }
    scheduledNoteOffs.resize(writeIndex);
    scheduledNoteOffCount.store(static_cast<int> (scheduledNoteOffs.size()), std::memory_order_relaxed);

    return count;
}

void HostEngine::scheduleNoteOffAtSample(int laneIndex, int midiChannel, int midiNote, float velocity, uint64_t dueSample)
{
    ScheduledNoteOff off;
    off.lane = laneIndex;
    off.channel = juce::jlimit(1, 16, midiChannel);
    off.note = midiNote;
    off.velocity = juce::jlimit(0.0f, 1.0f, velocity);
    off.dueSample = dueSample;

    const juce::SpinLock::ScopedLockType scope(scheduledLock);
    if (scheduledNoteOffs.size() > 16384)
        scheduledNoteOffs.erase(scheduledNoteOffs.begin(), scheduledNoteOffs.begin() + 2048);
    scheduledNoteOffs.push_back(off);
    scheduledNoteOffCount.store(static_cast<int> (scheduledNoteOffs.size()), std::memory_order_relaxed);
}

void HostEngine::scheduleNoteOff(int laneIndex, int midiChannel, int midiNote, float velocity, float durationSec)
{
    if (durationSec <= 0.0f)
        return;

    const auto nowSample = audioSampleCounter.load(std::memory_order_relaxed);
    const auto offsetSamples = static_cast<uint64_t> (juce::jmax(1.0, durationSec * sampleRate));
    juce::ignoreUnused(velocity);
    scheduleNoteOffAtSample(laneIndex, midiChannel, midiNote, 0.0f, nowSample + offsetSamples);
}

void HostEngine::appendLog(const juce::String& text)
{
    const juce::ScopedLock scope(logLock);
    log.insertTextAtCaret(text + "\n");
}

void HostEngine::saveHostConfigDialog()
{
    configSaveChooser = std::make_unique<juce::FileChooser>("Save Host Config",
                                                             juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("host-config.sconfig"),
                                                             "*.sconfig");
    configSaveChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                   | juce::FileBrowserComponent::canSelectFiles
                                   | juce::FileBrowserComponent::warnAboutOverwriting,
                                   [this] (const juce::FileChooser& chooser)
                                   {
                                       auto file = chooser.getResult();
                                       configSaveChooser.reset();
                                       if (file == juce::File())
                                           return;

                                       if (! file.hasFileExtension("sconfig"))
                                           file = file.withFileExtension("sconfig");

                                       if (saveHostConfigToFile(file))
                                       {
                                           uiStatus.setText("Saved config: " + file.getFileName(), juce::dontSendNotification);
                                           appendLog("Saved host config to " + file.getFullPathName());
                                       }
                                       else
                                       {
                                           uiStatus.setText("Failed to save config", juce::dontSendNotification);
                                           appendLog("Failed to save host config to " + file.getFullPathName());
                                       }
                                   });
}

void HostEngine::loadHostConfigDialog()
{
    auto initial = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    if (lastLoadedConfigFile.existsAsFile())
        initial = lastLoadedConfigFile;

    configLoadChooser = std::make_unique<juce::FileChooser>("Load Host Config",
                                                             initial,
                                                             "*.sconfig;*.xml");
    configLoadChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                   [this] (const juce::FileChooser& chooser)
                                   {
                                       const auto file = chooser.getResult();
                                       configLoadChooser.reset();
                                       if (! file.existsAsFile())
                                           return;

                                       if (loadHostConfigFromFile(file))
                                       {
                                           lastLoadedConfigFile = file;
                                           uiStatus.setText("Loaded config: " + file.getFileName(), juce::dontSendNotification);
                                           appendLog("Loaded host config from " + file.getFullPathName());
                                       }
                                       else
                                       {
                                           uiStatus.setText("Failed to load config", juce::dontSendNotification);
                                           appendLog("Failed to load host config from " + file.getFullPathName());
                                       }
                                   });
}

juce::ValueTree HostEngine::buildConfigTree()
{
    juce::ValueTree root("HostConfig");
    root.setProperty("version", 1, nullptr);
    root.setProperty("masterGain", masterGain.load(), nullptr);

    const juce::ScopedLock audioScope(deviceManager.getAudioCallbackLock());
    const juce::ScopedLock laneScope(laneLock);

    for (int i = 0; i < static_cast<int> (lanes.size()); ++i)
    {
        auto& lane = lanes[static_cast<size_t> (i)];
        juce::ValueTree laneNode("Lane");
        laneNode.setProperty("index", i, nullptr);
        laneNode.setProperty("gain", lane.gain.load(), nullptr);
        laneNode.setProperty("pan", lane.pan.load(), nullptr);
        laneNode.setProperty("muted", lane.muted.load(), nullptr);
        laneNode.setProperty("solo", lane.solo.load(), nullptr);

        if (lane.plugin != nullptr)
        {
            juce::PluginDescription desc;
            lane.plugin->fillInPluginDescription(desc);
            laneNode.setProperty("pluginId", desc.createIdentifierString(), nullptr);
            laneNode.setProperty("pluginName", desc.name, nullptr);

            juce::MemoryBlock pluginState;
            lane.plugin->getStateInformation(pluginState);
            laneNode.setProperty("pluginState", pluginState.toBase64Encoding(), nullptr);
        }

        for (int slot = 0; slot < kNumFxSlots; ++slot)
        {
            if (lane.fxPlugins[static_cast<size_t> (slot)] == nullptr)
                continue;

            juce::ValueTree fxNode("Fx");
            fxNode.setProperty("slot", slot, nullptr);

            juce::PluginDescription desc;
            lane.fxPlugins[static_cast<size_t> (slot)]->fillInPluginDescription(desc);
            fxNode.setProperty("pluginId", desc.createIdentifierString(), nullptr);
            fxNode.setProperty("pluginName", desc.name, nullptr);

            juce::MemoryBlock fxState;
            lane.fxPlugins[static_cast<size_t> (slot)]->getStateInformation(fxState);
            fxNode.setProperty("pluginState", fxState.toBase64Encoding(), nullptr);
            laneNode.addChild(fxNode, -1, nullptr);
        }

        root.addChild(laneNode, -1, nullptr);
    }

    for (int slot = 0; slot < kNumMasterFxSlots; ++slot)
    {
        if (masterFxPlugins[static_cast<size_t> (slot)] == nullptr)
            continue;

        juce::ValueTree fxNode("MasterFx");
        fxNode.setProperty("slot", slot, nullptr);

        juce::PluginDescription desc;
        masterFxPlugins[static_cast<size_t> (slot)]->fillInPluginDescription(desc);
        fxNode.setProperty("pluginId", desc.createIdentifierString(), nullptr);
        fxNode.setProperty("pluginName", desc.name, nullptr);

        juce::MemoryBlock fxState;
        masterFxPlugins[static_cast<size_t> (slot)]->getStateInformation(fxState);
        fxNode.setProperty("pluginState", fxState.toBase64Encoding(), nullptr);
        root.addChild(fxNode, -1, nullptr);
    }

    return root;
}

bool HostEngine::saveHostConfigToFile(const juce::File& file)
{
    auto xml = buildConfigTree().createXml();
    return xml != nullptr && xml->writeTo(file);
}

bool HostEngine::applyConfigTree(const juce::ValueTree& root)
{
    if (! root.hasType("HostConfig"))
        return false;

    const auto loadedMasterGain = static_cast<float> (root.getProperty("masterGain", 1.0f));
    masterGain.store(juce::jlimit(0.0f, 2.0f, loadedMasterGain));
    suppressUiCallbacks = true;
    masterGainSlider.setValue(masterGain.load(), juce::dontSendNotification);
    suppressUiCallbacks = false;

    refreshPluginCatalog();
    for (int slot = 0; slot < kNumMasterFxSlots; ++slot)
        unloadMasterEffectFromSlot(slot);

    for (int i = 0; i < root.getNumChildren(); ++i)
    {
        const auto laneNode = root.getChild(i);
        if (! laneNode.hasType("Lane"))
            continue;

        const auto laneIndex = static_cast<int> (laneNode.getProperty("index", -1));
        if (! juce::isPositiveAndBelow(laneIndex, static_cast<int> (lanes.size())))
            continue;

        closePluginEditorForLane(laneIndex);
        for (int slot = 0; slot < kNumFxSlots; ++slot)
            unloadEffectFromLaneSlot(laneIndex, slot);
        {
            const juce::ScopedLock laneScope(laneLock);
            if (auto* lane = getLane(laneIndex))
            {
                lane->gain.store(juce::jlimit(0.0f, 2.0f, static_cast<float> (laneNode.getProperty("gain", 1.0f))));
                lane->pan.store(juce::jlimit(-1.0f, 1.0f, static_cast<float> (laneNode.getProperty("pan", 0.0f))));
                lane->muted.store(static_cast<bool> (laneNode.getProperty("muted", false)));
                lane->solo.store(static_cast<bool> (laneNode.getProperty("solo", false)));
            }
        }

        const auto pluginId = laneNode.getProperty("pluginId").toString();
        const auto pluginState = laneNode.getProperty("pluginState").toString();

        if (pluginId.isNotEmpty())
        {
            bool restored = false;
            for (const auto& desc : knownPluginList.getTypes())
            {
                if (desc.createIdentifierString() == pluginId)
                {
                    restored = loadPluginDescriptionIntoLane(laneIndex, desc, pluginState);
                    break;
                }
            }

            if (! restored)
                unloadInstrumentFromLane(laneIndex);
        }
        else
        {
            unloadInstrumentFromLane(laneIndex);
        }

        for (int child = 0; child < laneNode.getNumChildren(); ++child)
        {
            const auto fxNode = laneNode.getChild(child);
            if (! fxNode.hasType("Fx"))
                continue;

            const auto slot = static_cast<int> (fxNode.getProperty("slot", -1));
            if (! juce::isPositiveAndBelow(slot, kNumFxSlots))
                continue;

            const auto fxPluginId = fxNode.getProperty("pluginId").toString();
            const auto fxPluginState = fxNode.getProperty("pluginState").toString();
            if (fxPluginId.isEmpty())
                continue;

            bool restoredFx = false;
            for (const auto& desc : knownPluginList.getTypes())
            {
                if (desc.createIdentifierString() == fxPluginId)
                {
                    restoredFx = loadEffectDescriptionIntoLaneSlot(laneIndex, slot, desc, fxPluginState);
                    break;
                }
            }

            if (! restoredFx)
                unloadEffectFromLaneSlot(laneIndex, slot);
        }
    }

    for (int i = 0; i < root.getNumChildren(); ++i)
    {
        const auto fxNode = root.getChild(i);
        if (! fxNode.hasType("MasterFx"))
            continue;

        const auto slot = static_cast<int> (fxNode.getProperty("slot", -1));
        if (! juce::isPositiveAndBelow(slot, kNumMasterFxSlots))
            continue;

        const auto fxPluginId = fxNode.getProperty("pluginId").toString();
        const auto fxPluginState = fxNode.getProperty("pluginState").toString();
        if (fxPluginId.isEmpty())
            continue;

        bool restoredFx = false;
        for (const auto& desc : knownPluginList.getTypes())
        {
            if (desc.createIdentifierString() == fxPluginId)
            {
                restoredFx = loadMasterEffectDescriptionIntoSlot(slot, desc, fxPluginState);
                break;
            }
        }

        if (! restoredFx)
            unloadMasterEffectFromSlot(slot);
    }

    syncAllLaneRows();
    return true;
}

bool HostEngine::loadHostConfigFromFile(const juce::File& file)
{
    std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(file));
    if (xml == nullptr)
        return false;

    const auto tree = juce::ValueTree::fromXml(*xml);
    if (! tree.isValid())
        return false;

    return applyConfigTree(tree);
}

void HostEngine::rebuildPluginMenus()
{
    suppressUiCallbacks = true;
    for (auto& picker : lanePluginPickers)
    {
        picker.clear(juce::dontSendNotification);
        picker.addItem("Built-in Sine", 1);
        for (int i = 0; i < static_cast<int> (instrumentCatalog.size()); ++i)
        {
            const auto& d = instrumentCatalog[static_cast<size_t> (i)];
            picker.addItem(d.name + " [" + d.pluginFormatName + "]", i + 2);
        }
    }

    for (auto& fxPicker : laneFxPickers)
    {
        fxPicker.clear(juce::dontSendNotification);
        fxPicker.addItem("None", 1);
        for (int i = 0; i < static_cast<int> (effectCatalog.size()); ++i)
        {
            const auto& d = effectCatalog[static_cast<size_t> (i)];
            fxPicker.addItem(d.name + " [" + d.pluginFormatName + "]", i + 2);
        }
    }
    for (auto& fxPicker : masterFxPickers)
    {
        fxPicker.clear(juce::dontSendNotification);
        fxPicker.addItem("None", 1);
        for (int i = 0; i < static_cast<int> (effectCatalog.size()); ++i)
        {
            const auto& d = effectCatalog[static_cast<size_t> (i)];
            fxPicker.addItem(d.name + " [" + d.pluginFormatName + "]", i + 2);
        }
    }
    suppressUiCallbacks = false;
}

juce::String HostEngine::computePluginScanSignature() const
{
    juce::StringArray entries;

    auto addSearchDirIfExists = [] (juce::StringArray& paths, const juce::File& dir)
    {
        if (dir.isDirectory())
            paths.addIfNotAlreadyThere(dir.getFullPathName());
    };

    for (auto* format : formatManager.getFormats())
    {
        auto discoveredPaths = format->searchPathsForPlugins(format->getDefaultLocationsToSearch(), true, false);

       #if JUCE_MAC
        if (dynamic_cast<juce::AudioUnitPluginFormat*> (format) != nullptr)
        {
            addSearchDirIfExists(discoveredPaths, juce::File("/System/Library/Components"));
            addSearchDirIfExists(discoveredPaths, juce::File("/Library/Audio/Plug-Ins/Components"));
            addSearchDirIfExists(discoveredPaths, juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                                                    .getChildFile("Library/Audio/Plug-Ins/Components"));
        }

        if (dynamic_cast<juce::VST3PluginFormat*> (format) != nullptr)
        {
            addSearchDirIfExists(discoveredPaths, juce::File("/Library/Audio/Plug-Ins/VST3"));
            addSearchDirIfExists(discoveredPaths, juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                                                    .getChildFile("Library/Audio/Plug-Ins/VST3"));
        }
       #endif

        discoveredPaths.sort(true);
        for (const auto& path : discoveredPaths)
        {
            juce::String line = format->getName() + "|" + path;
            const juce::File f(path);
            if (f.exists())
                line << "|" << juce::String(f.getLastModificationTime().toMilliseconds());
            entries.add(line);
        }
    }

    entries.sort(true);
    return entries.joinIntoString("\n");
}

bool HostEngine::loadPluginCatalogCache(const juce::String& signature)
{
    if (! pluginCacheFile.existsAsFile())
        return false;

    auto xml = std::unique_ptr<juce::XmlElement>(juce::XmlDocument::parse(pluginCacheFile));
    if (xml == nullptr || ! xml->hasTagName("PluginScanCache"))
        return false;

    const auto cachedSignature = xml->getStringAttribute("signature");
    if (cachedSignature != signature)
        return false;

    if (auto* knownPluginsXml = xml->getChildByName("KNOWNPLUGINS"))
    {
        knownPluginList.clear();
        knownPluginList.recreateFromXml(*knownPluginsXml);
        return knownPluginList.getNumTypes() > 0;
    }

    return false;
}

void HostEngine::savePluginCatalogCache(const juce::String& signature)
{
    auto knownXml = knownPluginList.createXml();
    if (knownXml == nullptr)
        return;

    juce::XmlElement root("PluginScanCache");
    root.setAttribute("version", 1);
    root.setAttribute("signature", signature);
    root.addChildElement(new juce::XmlElement(*knownXml));

    auto parent = pluginCacheFile.getParentDirectory();
    if (! parent.isDirectory())
        parent.createDirectory();

    root.writeTo(pluginCacheFile);
}

void HostEngine::rebuildCatalogFromKnownPlugins()
{
    instrumentCatalog.clear();
    effectCatalog.clear();

    for (const auto& desc : knownPluginList.getTypes())
    {
        if (desc.isInstrument)
            instrumentCatalog.push_back(desc);
        else
            effectCatalog.push_back(desc);
    }

    std::sort(instrumentCatalog.begin(), instrumentCatalog.end(), [] (const auto& a, const auto& b)
    {
        return a.name.compareIgnoreCase(b.name) < 0;
    });
    std::sort(effectCatalog.begin(), effectCatalog.end(), [] (const auto& a, const auto& b)
    {
        return a.name.compareIgnoreCase(b.name) < 0;
    });
}

void HostEngine::refreshPluginCatalog(bool forceRescan)
{
    const auto signature = computePluginScanSignature();
    if (! forceRescan && loadPluginCatalogCache(signature))
    {
        rebuildCatalogFromKnownPlugins();
        rebuildPluginMenus();
        syncAllLaneRows();
        uiStatus.setText("Plugins: " + juce::String(instrumentCatalog.size()) + " instr, "
                         + juce::String(effectCatalog.size()) + " fx", juce::dontSendNotification);
        appendLog("Loaded plugin cache: " + juce::String(knownPluginList.getNumTypes()) + " total, "
                  + juce::String(instrumentCatalog.size()) + " instruments, "
                  + juce::String(effectCatalog.size()) + " effects");
        return;
    }

    instrumentCatalog.clear();
    effectCatalog.clear();
    knownPluginList.clear();

    auto addSearchDirIfExists = [] (juce::StringArray& paths, const juce::File& dir)
    {
        if (dir.isDirectory())
            paths.addIfNotAlreadyThere(dir.getFullPathName());
    };

    int totalTypes = 0;
    bool sawAuFormat = false;

    for (auto* format : formatManager.getFormats())
    {
        auto discoveredPaths = format->searchPathsForPlugins(format->getDefaultLocationsToSearch(), true, false);

       #if JUCE_MAC
        if (dynamic_cast<juce::AudioUnitPluginFormat*> (format) != nullptr)
        {
            sawAuFormat = true;
            addSearchDirIfExists(discoveredPaths, juce::File("/System/Library/Components"));
            addSearchDirIfExists(discoveredPaths, juce::File("/Library/Audio/Plug-Ins/Components"));
            addSearchDirIfExists(discoveredPaths, juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                                                    .getChildFile("Library/Audio/Plug-Ins/Components"));
        }

        if (dynamic_cast<juce::VST3PluginFormat*> (format) != nullptr)
        {
            addSearchDirIfExists(discoveredPaths, juce::File("/Library/Audio/Plug-Ins/VST3"));
            addSearchDirIfExists(discoveredPaths, juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                                                    .getChildFile("Library/Audio/Plug-Ins/VST3"));
        }
       #endif

        juce::FileSearchPath searchPath;
        for (const auto& path : discoveredPaths)
            searchPath.add(juce::File(path), false);

        juce::PluginDirectoryScanner scanner(knownPluginList, *format, searchPath, true, deadMansPedalFile, false);
        juce::String pluginName;
        while (scanner.scanNextFile(true, pluginName)) {}

        totalTypes = knownPluginList.getNumTypes();
    }

    rebuildCatalogFromKnownPlugins();
    savePluginCatalogCache(signature);

    rebuildPluginMenus();
    syncAllLaneRows();

    uiStatus.setText("Plugins: " + juce::String(instrumentCatalog.size()) + " instr, "
                     + juce::String(effectCatalog.size()) + " fx", juce::dontSendNotification);
    appendLog("Plugin scan complete: " + juce::String(totalTypes) + " total, "
              + juce::String(instrumentCatalog.size()) + " instruments, "
              + juce::String(effectCatalog.size()) + " effects");
    if (sawAuFormat && instrumentCatalog.empty())
        appendLog("No AU instruments found. Ensure Logic components exist in /Library/Audio/Plug-Ins/Components.");
}

bool HostEngine::loadInstrumentIntoLane(int laneIndex, int instrumentIndex)
{
    if (! juce::isPositiveAndBelow(instrumentIndex, static_cast<int> (instrumentCatalog.size())))
        return false;

    const auto desc = instrumentCatalog[static_cast<size_t> (instrumentIndex)];
    if (! loadPluginDescriptionIntoLane(laneIndex, desc, {}))
        return false;

    uiStatus.setText("Loaded " + desc.name + " on lane " + juce::String(laneIndex), juce::dontSendNotification);
    appendLog("Loaded lane " + juce::String(laneIndex) + " instrument: " + desc.name);
    return true;
}

bool HostEngine::rebuildLaneGraphWithInstrument(int laneIndex, std::unique_ptr<juce::AudioPluginInstance> instrument)
{
    if (! juce::isPositiveAndBelow(laneIndex, static_cast<int> (lanes.size())))
        return false;
    if (instrument == nullptr)
        return false;

    auto graph = std::make_unique<juce::AudioProcessorGraph>();
    graph->setPlayConfigDetails(0, 2, sampleRate, expectedBlockSize);

    auto midiIn = graph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
        juce::AudioProcessorGraph::AudioGraphIOProcessor::midiInputNode));
    auto audioOut = graph->addNode(std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>(
        juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));
    auto instrumentNode = graph->addNode(std::move(instrument));

    if (midiIn == nullptr || audioOut == nullptr || instrumentNode == nullptr)
        return false;

    graph->addConnection({ { midiIn->nodeID, juce::AudioProcessorGraph::midiChannelIndex },
                           { instrumentNode->nodeID, juce::AudioProcessorGraph::midiChannelIndex } });
    graph->addConnection({ { instrumentNode->nodeID, 0 }, { audioOut->nodeID, 0 } });
    graph->addConnection({ { instrumentNode->nodeID, 1 }, { audioOut->nodeID, 1 } });

    graph->prepareToPlay(sampleRate, expectedBlockSize);
    graph->reset();

    auto* pluginRaw = dynamic_cast<juce::AudioPluginInstance*> (instrumentNode->getProcessor());
    if (pluginRaw == nullptr)
        return false;

    auto* lane = getLane(laneIndex);
    if (lane == nullptr)
        return false;

    if (lane->graph != nullptr)
        lane->graph->releaseResources();
    lane->graph = std::move(graph);
    lane->plugin = pluginRaw;
    return true;
}

bool HostEngine::loadPluginDescriptionIntoLane(int laneIndex, const juce::PluginDescription& desc, const juce::String& stateBase64)
{
    juce::String error;
    auto instance = formatManager.createPluginInstance(desc, sampleRate, expectedBlockSize, error);
    if (instance == nullptr)
    {
        uiStatus.setText("Failed to load plugin", juce::dontSendNotification);
        appendLog("Load failed: " + desc.name + " - " + error);
        return false;
    }

    if (stateBase64.isNotEmpty())
    {
        juce::MemoryBlock pluginState;
        if (pluginState.fromBase64Encoding(stateBase64) && pluginState.getSize() > 0)
            instance->setStateInformation(pluginState.getData(), static_cast<int> (pluginState.getSize()));
    }
    const auto busesOk = configurePluginBuses(*instance, true);
    const auto ioSummary = pluginIoSummary(*instance);

    {
        const juce::ScopedLock audioScope(deviceManager.getAudioCallbackLock());
        const juce::ScopedLock laneScope(laneLock);
        if (! rebuildLaneGraphWithInstrument(laneIndex, std::move(instance)))
            return false;

        if (auto* lane = getLane(laneIndex))
        {
            lane->pluginName = desc.name;
            lane->midiQueue.clear();
            lane->activeNotes.fill(0);
        }
    }

    closePluginEditorForLane(laneIndex);
    float laneGain = 1.0f;
    float lanePan = 0.0f;
    bool laneMuted = false;
    bool laneSolo = false;
    {
        const juce::ScopedLock scope(laneLock);
        if (auto* lane = getLane(laneIndex))
        {
            laneGain = lane->gain.load();
            lanePan = lane->pan.load();
            laneMuted = lane->muted.load();
            laneSolo = lane->solo.load();
        }
    }
    appendLog("Lane " + juce::String(laneIndex) + " IO: " + ioSummary
              + (busesOk ? "" : " [bus fallback]"));
    appendLog("Lane " + juce::String(laneIndex) + " state: gain=" + juce::String(laneGain, 3)
              + " pan=" + juce::String(lanePan, 3)
              + " mute=" + juce::String(laneMuted ? 1 : 0)
              + " solo=" + juce::String(laneSolo ? 1 : 0));

    // Quick audible sanity check for freshly loaded instruments.
    for (int ch = 1; ch <= 16; ++ch)
    {
        enqueueMidiMessage(laneIndex, juce::MidiMessage::noteOn(ch, 60, static_cast<juce::uint8> (127)));
        scheduleNoteOff(laneIndex, ch, 60, 0.0f, 0.4f);
    }
    {
        const juce::ScopedLock scope(laneLock);
        if (auto* lane = getLane(laneIndex))
            lane->auditionSamplesRemaining.store(static_cast<int> (sampleRate * 0.4), std::memory_order_relaxed);
    }
    appendLog("Lane " + juce::String(laneIndex) + " audition note sent (C4, 0.4s)");
    return true;
}

bool HostEngine::loadEffectIntoLaneSlot(int laneIndex, int slotIndex, int effectIndex)
{
    if (! juce::isPositiveAndBelow(effectIndex, static_cast<int> (effectCatalog.size())))
        return false;
    if (! juce::isPositiveAndBelow(slotIndex, kNumFxSlots))
        return false;

    const auto desc = effectCatalog[static_cast<size_t> (effectIndex)];
    if (! loadEffectDescriptionIntoLaneSlot(laneIndex, slotIndex, desc, {}))
        return false;

    uiStatus.setText("Loaded FX " + desc.name + " on lane " + juce::String(laneIndex)
                     + " slot " + juce::String(slotIndex + 1), juce::dontSendNotification);
    appendLog("Loaded FX lane " + juce::String(laneIndex) + " slot " + juce::String(slotIndex + 1)
              + ": " + desc.name);
    return true;
}

bool HostEngine::loadEffectDescriptionIntoLaneSlot(int laneIndex, int slotIndex, const juce::PluginDescription& desc, const juce::String& stateBase64)
{
    if (! juce::isPositiveAndBelow(slotIndex, kNumFxSlots))
        return false;

    juce::String error;
    auto instance = formatManager.createPluginInstance(desc, sampleRate, expectedBlockSize, error);
    if (instance == nullptr)
    {
        uiStatus.setText("Failed to load FX", juce::dontSendNotification);
        appendLog("FX load failed: " + desc.name + " - " + error);
        return false;
    }

    if (stateBase64.isNotEmpty())
    {
        juce::MemoryBlock pluginState;
        if (pluginState.fromBase64Encoding(stateBase64) && pluginState.getSize() > 0)
            instance->setStateInformation(pluginState.getData(), static_cast<int> (pluginState.getSize()));
    }
    const auto busesOk = preparePluginInstance(*instance, false, sampleRate, expectedBlockSize);
    if (! busesOk)
    {
        uiStatus.setText("FX bus layout unsupported", juce::dontSendNotification);
        appendLog("FX load rejected (unsupported bus layout): " + desc.name);
        return false;
    }
    const auto ioSummary = pluginIoSummary(*instance);

    {
        const juce::ScopedLock audioScope(deviceManager.getAudioCallbackLock());
        const juce::ScopedLock laneScope(laneLock);
        if (auto* lane = getLane(laneIndex))
        {
            if (lane->fxPlugins[static_cast<size_t> (slotIndex)] != nullptr)
                lane->fxPlugins[static_cast<size_t> (slotIndex)]->releaseResources();

            lane->fxPlugins[static_cast<size_t> (slotIndex)] = std::move(instance);
            lane->fxNames[static_cast<size_t> (slotIndex)] = desc.name;
        }
    }

    closeEffectEditorForLaneSlot(laneIndex, slotIndex);
    appendLog("Lane " + juce::String(laneIndex) + " FX" + juce::String(slotIndex + 1)
              + " IO: " + ioSummary
              + (busesOk ? "" : " [bus fallback]"));
    return true;
}

void HostEngine::unloadEffectFromLaneSlot(int laneIndex, int slotIndex)
{
    if (! juce::isPositiveAndBelow(slotIndex, kNumFxSlots))
        return;

    closeEffectEditorForLaneSlot(laneIndex, slotIndex);

    const juce::ScopedLock audioScope(deviceManager.getAudioCallbackLock());
    const juce::ScopedLock laneScope(laneLock);
    if (auto* lane = getLane(laneIndex))
    {
        if (lane->fxPlugins[static_cast<size_t> (slotIndex)] != nullptr)
            lane->fxPlugins[static_cast<size_t> (slotIndex)]->releaseResources();

        lane->fxPlugins[static_cast<size_t> (slotIndex)].reset();
        lane->fxNames[static_cast<size_t> (slotIndex)] = "None";
    }
}

bool HostEngine::loadMasterEffectIntoSlot(int slotIndex, int effectIndex)
{
    if (! juce::isPositiveAndBelow(slotIndex, kNumMasterFxSlots))
        return false;
    if (! juce::isPositiveAndBelow(effectIndex, static_cast<int> (effectCatalog.size())))
        return false;

    const auto desc = effectCatalog[static_cast<size_t> (effectIndex)];
    if (! loadMasterEffectDescriptionIntoSlot(slotIndex, desc, {}))
        return false;

    uiStatus.setText("Loaded Master FX " + desc.name + " in slot " + juce::String(slotIndex + 1), juce::dontSendNotification);
    appendLog("Loaded Master FX slot " + juce::String(slotIndex + 1) + ": " + desc.name);
    return true;
}

bool HostEngine::loadMasterEffectDescriptionIntoSlot(int slotIndex, const juce::PluginDescription& desc, const juce::String& stateBase64)
{
    if (! juce::isPositiveAndBelow(slotIndex, kNumMasterFxSlots))
        return false;

    juce::String error;
    auto instance = formatManager.createPluginInstance(desc, sampleRate, expectedBlockSize, error);
    if (instance == nullptr)
    {
        uiStatus.setText("Failed to load Master FX", juce::dontSendNotification);
        appendLog("Master FX load failed: " + desc.name + " - " + error);
        return false;
    }

    if (stateBase64.isNotEmpty())
    {
        juce::MemoryBlock pluginState;
        if (pluginState.fromBase64Encoding(stateBase64) && pluginState.getSize() > 0)
            instance->setStateInformation(pluginState.getData(), static_cast<int> (pluginState.getSize()));
    }
    const auto busesOk = preparePluginInstance(*instance, false, sampleRate, expectedBlockSize);
    if (! busesOk)
    {
        uiStatus.setText("Master FX bus layout unsupported", juce::dontSendNotification);
        appendLog("Master FX load rejected (unsupported bus layout): " + desc.name);
        return false;
    }
    const auto ioSummary = pluginIoSummary(*instance);

    const juce::ScopedLock audioScope(deviceManager.getAudioCallbackLock());
    if (masterFxPlugins[static_cast<size_t> (slotIndex)] != nullptr)
        masterFxPlugins[static_cast<size_t> (slotIndex)]->releaseResources();

    masterFxPlugins[static_cast<size_t> (slotIndex)] = std::move(instance);
    masterFxNames[static_cast<size_t> (slotIndex)] = desc.name;
    closeMasterEffectEditorForSlot(slotIndex);
    appendLog("Master FX" + juce::String(slotIndex + 1) + " IO: "
              + ioSummary
              + (busesOk ? "" : " [bus fallback]"));
    return true;
}

void HostEngine::unloadMasterEffectFromSlot(int slotIndex)
{
    if (! juce::isPositiveAndBelow(slotIndex, kNumMasterFxSlots))
        return;

    closeMasterEffectEditorForSlot(slotIndex);
    const juce::ScopedLock audioScope(deviceManager.getAudioCallbackLock());
    if (masterFxPlugins[static_cast<size_t> (slotIndex)] != nullptr)
        masterFxPlugins[static_cast<size_t> (slotIndex)]->releaseResources();

    masterFxPlugins[static_cast<size_t> (slotIndex)].reset();
    masterFxNames[static_cast<size_t> (slotIndex)] = "None";
}

void HostEngine::unloadInstrumentFromLane(int laneIndex)
{
    closePluginEditorForLane(laneIndex);

    const juce::ScopedLock audioScope(deviceManager.getAudioCallbackLock());
    const juce::ScopedLock laneScope(laneLock);
    if (auto* lane = getLane(laneIndex))
    {
        if (lane->graph != nullptr)
            lane->graph->releaseResources();
        lane->graph.reset();
        lane->plugin = nullptr;
        lane->pluginName = "Built-in Sine";
        lane->midiQueue.clear();
        lane->activeNotes.fill(0);
    }

    uiStatus.setText("Lane " + juce::String(laneIndex) + " uses built-in sine", juce::dontSendNotification);
    appendLog("Unloaded plugin on lane " + juce::String(laneIndex));
}

void HostEngine::syncLaneRow(int laneIndex)
{
    if (! juce::isPositiveAndBelow(laneIndex, static_cast<int> (lanes.size())))
        return;

    float gain = 1.0f;
    float pan = 0.0f;
    bool muted = false;
    bool solo = false;
    bool hasPlugin = false;
    juce::String pluginName;
    std::array<juce::String, kNumFxSlots> fxNames { "None", "None", "None" };
    std::array<bool, kNumFxSlots> hasFx { false, false, false };

    {
        const juce::ScopedLock scope(laneLock);
        if (auto* lane = getLane(laneIndex))
        {
            gain = lane->gain.load();
            pan = lane->pan.load();
            muted = lane->muted.load();
            solo = lane->solo.load();
            hasPlugin = (lane->plugin != nullptr);
            pluginName = lane->pluginName;
            for (int slot = 0; slot < kNumFxSlots; ++slot)
            {
                hasFx[static_cast<size_t> (slot)] = (lane->fxPlugins[static_cast<size_t> (slot)] != nullptr);
                fxNames[static_cast<size_t> (slot)] = lane->fxNames[static_cast<size_t> (slot)];
            }
        }
    }

    auto& muteButton = laneMuteButtons[static_cast<size_t> (laneIndex)];
    auto& soloButton = laneSoloButtons[static_cast<size_t> (laneIndex)];
    auto& gainSlider = laneGainSliders[static_cast<size_t> (laneIndex)];
    auto& panSlider = lanePanSliders[static_cast<size_t> (laneIndex)];
    auto& picker = lanePluginPickers[static_cast<size_t> (laneIndex)];
    auto& editButton = laneEditButtons[static_cast<size_t> (laneIndex)];
    auto& loadedLabel = lanePluginNameLabels[static_cast<size_t> (laneIndex)];

    int selectedId = 1;
    for (int i = 0; i < static_cast<int> (instrumentCatalog.size()); ++i)
    {
        if (instrumentCatalog[static_cast<size_t> (i)].name == pluginName)
        {
            selectedId = i + 2;
            break;
        }
    }

    suppressUiCallbacks = true;
    muteButton.setToggleState(muted, juce::dontSendNotification);
    soloButton.setToggleState(solo, juce::dontSendNotification);
    gainSlider.setValue(gain, juce::dontSendNotification);
    panSlider.setValue(pan, juce::dontSendNotification);
    picker.setSelectedId(selectedId, juce::dontSendNotification);
    for (int slot = 0; slot < kNumFxSlots; ++slot)
    {
        auto& fxPicker = laneFxPickers[static_cast<size_t> (fxUiIndex(laneIndex, slot))];
        int fxSelectedId = 1;
        for (int fx = 0; fx < static_cast<int> (effectCatalog.size()); ++fx)
        {
            if (effectCatalog[static_cast<size_t> (fx)].name == fxNames[static_cast<size_t> (slot)])
            {
                fxSelectedId = fx + 2;
                break;
            }
        }

        fxPicker.setSelectedId(fxSelectedId, juce::dontSendNotification);
        laneFxEditButtons[static_cast<size_t> (fxUiIndex(laneIndex, slot))].setEnabled(hasFx[static_cast<size_t> (slot)]);
    }
    suppressUiCallbacks = false;
    editButton.setEnabled(hasPlugin);
    loadedLabel.setText(pluginName.isNotEmpty() ? pluginName : "Built-in Sine", juce::dontSendNotification);

    const auto dimAlpha = muted ? 0.45f : 1.0f;
    laneLabels[static_cast<size_t> (laneIndex)].setAlpha(dimAlpha);
    laneSoloButtons[static_cast<size_t> (laneIndex)].setAlpha(dimAlpha);
    laneGainSliders[static_cast<size_t> (laneIndex)].setAlpha(dimAlpha);
    lanePanSliders[static_cast<size_t> (laneIndex)].setAlpha(dimAlpha);
    lanePluginPickers[static_cast<size_t> (laneIndex)].setAlpha(dimAlpha);
    laneEditButtons[static_cast<size_t> (laneIndex)].setAlpha(dimAlpha);
    laneFxExpandButtons[static_cast<size_t> (laneIndex)].setAlpha(dimAlpha);
    lanePluginNameLabels[static_cast<size_t> (laneIndex)].setAlpha(dimAlpha);
    for (int slot = 0; slot < kNumFxSlots; ++slot)
    {
        const auto slotAlpha = dimAlpha * (hasFx[static_cast<size_t> (slot)] ? 1.0f : 0.55f);
        laneFxPickers[static_cast<size_t> (fxUiIndex(laneIndex, slot))].setAlpha(slotAlpha);
        laneFxEditButtons[static_cast<size_t> (fxUiIndex(laneIndex, slot))].setAlpha(slotAlpha);
    }
}

void HostEngine::syncLaneFxUi(int laneIndex)
{
    syncLaneRow(laneIndex);
}

void HostEngine::syncMasterFxUi()
{
    suppressUiCallbacks = true;
    for (int slot = 0; slot < kNumMasterFxSlots; ++slot)
    {
        int selectedId = 1;
        for (int fx = 0; fx < static_cast<int> (effectCatalog.size()); ++fx)
        {
            if (effectCatalog[static_cast<size_t> (fx)].name == masterFxNames[static_cast<size_t> (slot)])
            {
                selectedId = fx + 2;
                break;
            }
        }

        const auto hasFx = (masterFxPlugins[static_cast<size_t> (slot)] != nullptr);
        masterFxPickers[static_cast<size_t> (slot)].setSelectedId(selectedId, juce::dontSendNotification);
        masterFxEditButtons[static_cast<size_t> (slot)].setEnabled(hasFx);
        const auto slotAlpha = hasFx ? 1.0f : 0.55f;
        masterFxPickers[static_cast<size_t> (slot)].setAlpha(slotAlpha);
        masterFxEditButtons[static_cast<size_t> (slot)].setAlpha(slotAlpha);
    }
    suppressUiCallbacks = false;
}

void HostEngine::syncAllLaneRows()
{
    for (int i = 0; i < static_cast<int> (lanes.size()); ++i)
        syncLaneRow(i);
    syncMasterFxUi();
}

void HostEngine::openPluginEditorForLane(int laneIndex)
{
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::String title = "Lane " + juce::String(laneIndex) + " Plugin";

    {
        const juce::ScopedLock scope(laneLock);
        auto* lane = getLane(laneIndex);
        if (lane == nullptr || lane->plugin == nullptr || ! lane->plugin->hasEditor())
            return;

        editor.reset(lane->plugin->createEditor());
        title = "Lane " + juce::String(laneIndex) + " - " + lane->pluginName;
    }

    if (editor == nullptr)
        return;

    closePluginEditorForLane(laneIndex);

    auto window = std::make_unique<PluginEditorWindow>(title, [this, laneIndex]
    {
        closePluginEditorForLane(laneIndex);
    });

    window->setContentOwned(editor.release(), true);
    window->centreWithSize(window->getWidth(), window->getHeight());
    window->setVisible(true);
    laneEditorWindows[static_cast<size_t> (laneIndex)] = std::move(window);
}

void HostEngine::closePluginEditorForLane(int laneIndex)
{
    if (! juce::isPositiveAndBelow(laneIndex, static_cast<int> (laneEditorWindows.size())))
        return;

    auto& window = laneEditorWindows[static_cast<size_t> (laneIndex)];
    if (window == nullptr)
        return;

    window->setVisible(false);
    window.reset();
}

void HostEngine::openEffectEditorForLaneSlot(int laneIndex, int slotIndex)
{
    if (! juce::isPositiveAndBelow(slotIndex, kNumFxSlots))
        return;

    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::String title = "Lane " + juce::String(laneIndex) + " FX " + juce::String(slotIndex + 1);

    {
        const juce::ScopedLock scope(laneLock);
        auto* lane = getLane(laneIndex);
        if (lane == nullptr)
            return;

        auto* fx = lane->fxPlugins[static_cast<size_t> (slotIndex)].get();
        if (fx == nullptr || ! fx->hasEditor())
            return;

        editor.reset(fx->createEditor());
        title = "Lane " + juce::String(laneIndex) + " FX" + juce::String(slotIndex + 1)
                + " - " + lane->fxNames[static_cast<size_t> (slotIndex)];
    }

    if (editor == nullptr)
        return;

    closeEffectEditorForLaneSlot(laneIndex, slotIndex);

    auto idx = static_cast<size_t> (fxUiIndex(laneIndex, slotIndex));
    auto window = std::make_unique<PluginEditorWindow>(title, [this, laneIndex, slotIndex]
    {
        closeEffectEditorForLaneSlot(laneIndex, slotIndex);
    });

    window->setContentOwned(editor.release(), true);
    window->centreWithSize(window->getWidth(), window->getHeight());
    window->setVisible(true);
    laneFxEditorWindows[idx] = std::move(window);
}

void HostEngine::closeEffectEditorForLaneSlot(int laneIndex, int slotIndex)
{
    if (! juce::isPositiveAndBelow(slotIndex, kNumFxSlots))
        return;

    const auto idx = static_cast<size_t> (fxUiIndex(laneIndex, slotIndex));
    if (! juce::isPositiveAndBelow(static_cast<int> (idx), static_cast<int> (laneFxEditorWindows.size())))
        return;

    auto& window = laneFxEditorWindows[idx];
    if (window == nullptr)
        return;

    window->setVisible(false);
    window.reset();
}

void HostEngine::openMasterEffectEditorForSlot(int slotIndex)
{
    if (! juce::isPositiveAndBelow(slotIndex, kNumMasterFxSlots))
        return;

    auto& fx = masterFxPlugins[static_cast<size_t> (slotIndex)];
    if (fx == nullptr || ! fx->hasEditor())
        return;

    auto editor = std::unique_ptr<juce::AudioProcessorEditor>(fx->createEditor());
    if (editor == nullptr)
        return;

    closeMasterEffectEditorForSlot(slotIndex);
    auto window = std::make_unique<PluginEditorWindow>("Master FX" + juce::String(slotIndex + 1) + " - " + masterFxNames[static_cast<size_t> (slotIndex)],
                                                       [this, slotIndex]
                                                       {
                                                           closeMasterEffectEditorForSlot(slotIndex);
                                                       });
    window->setContentOwned(editor.release(), true);
    window->centreWithSize(window->getWidth(), window->getHeight());
    window->setVisible(true);
    masterFxEditorWindows[static_cast<size_t> (slotIndex)] = std::move(window);
}

void HostEngine::closeMasterEffectEditorForSlot(int slotIndex)
{
    if (! juce::isPositiveAndBelow(slotIndex, kNumMasterFxSlots))
        return;

    auto& window = masterFxEditorWindows[static_cast<size_t> (slotIndex)];
    if (window == nullptr)
        return;
    window->setVisible(false);
    window.reset();
}

void HostEngine::handleTrigger(const juce::OSCMessage& m)
{
    const auto laneIndex = getIntArg(m, 0, 0);
    const auto eventId = getStringArg(m, 1).toLowerCase();
    const auto velocity = juce::jlimit(0.0f, 1.0f, getFloatArg(m, 2, 1.0f));

    int note = 60;
    if (eventId == "kick") note = 36;
    else if (eventId == "snare") note = 38;
    else if (eventId == "hat") note = 42;

    RtEvent ev;
    ev.type = RtEventType::noteOn;
    ev.lane = laneIndex;
    ev.channel = 1;
    ev.note = note;
    ev.velocity = velocity;
    ev.durationSec = 0.05f;
    const auto queued = pushRtEvent(ev);
    if (queued)
    {
        markLaneOscActivity(laneIndex);
    }
    else if (! juce::isPositiveAndBelow(laneIndex, static_cast<int> (lanes.size())))
    {
        appendLog("OSC trigger dropped: lane=" + juce::String(laneIndex)
                  + " out of range (valid: 0.." + juce::String(static_cast<int> (lanes.size()) - 1) + ")");
    }
}

void HostEngine::handleNote(const juce::OSCMessage& m)
{
    const auto laneIndex = getIntArg(m, 0, 0);
    const auto midiNote = juce::jlimit(0, 127, getIntArg(m, 1, 60));
    const auto velocity = juce::jlimit(0.0f, 1.0f, getFloatArg(m, 2, 0.8f));
    // If duration is omitted, use a short default gate so notes don't stack indefinitely.
    const bool hasDurationArg = m.size() > 3 && (m[3].isFloat32() || m[3].isInt32());
    const auto durationSec = hasDurationArg ? getFloatArg(m, 3, -1.0f) : 0.20f;

    RtEvent ev;
    ev.type = RtEventType::noteOn;
    ev.lane = laneIndex;
    ev.channel = 1;
    ev.note = midiNote;
    ev.velocity = velocity;
    ev.durationSec = durationSec;
    const auto queued = pushRtEvent(ev);
    if (queued)
    {
        markLaneOscActivity(laneIndex);
    }
    else if (! juce::isPositiveAndBelow(laneIndex, static_cast<int> (lanes.size())))
    {
        appendLog("OSC note_on dropped: lane=" + juce::String(laneIndex)
                  + " out of range (valid: 0.." + juce::String(static_cast<int> (lanes.size()) - 1) + ")");
    }
}

void HostEngine::handleNoteOff(const juce::OSCMessage& m)
{
    const auto laneIndex = getIntArg(m, 0, 0);
    const auto midiNote = juce::jlimit(0, 127, getIntArg(m, 1, 60));
    const auto velocity = juce::jlimit(0.0f, 1.0f, getFloatArg(m, 2, 0.0f));

    RtEvent ev;
    ev.type = RtEventType::noteOff;
    ev.lane = laneIndex;
    ev.channel = 1;
    ev.note = midiNote;
    ev.velocity = velocity;
    ev.durationSec = -1.0f;
    const auto queued = pushRtEvent(ev);
    if (queued)
    {
        markLaneOscActivity(laneIndex);
    }
    else if (! juce::isPositiveAndBelow(laneIndex, static_cast<int> (lanes.size())))
    {
        appendLog("OSC note_off dropped: lane=" + juce::String(laneIndex)
                  + " out of range (valid: 0.." + juce::String(static_cast<int> (lanes.size()) - 1) + ")");
    }
}

void HostEngine::handleMod(const juce::OSCMessage& m)
{
    const auto laneIndex = getIntArg(m, 0, 0);
    const auto target = getStringArg(m, 1);
    const auto value = getFloatArg(m, 2, 0.0f);

    if (target == "master.gain")
    {
        masterGain.store(juce::jlimit(0.0f, 2.0f, value));
        suppressUiCallbacks = true;
        masterGainSlider.setValue(masterGain.load(), juce::dontSendNotification);
        suppressUiCallbacks = false;
        return;
    }

    if (target == "lane.mute")
    {
        {
            const juce::ScopedLock scope(laneLock);
            auto* lane = getLane(laneIndex);
            if (lane == nullptr)
                return;

            lane->muted.store(value >= 0.5f);
        }
        markLaneOscActivity(laneIndex);
        syncLaneRow(laneIndex);
        return;
    }

    const juce::ScopedLock scope(laneLock);
    auto* lane = getLane(laneIndex);
    if (lane == nullptr)
        return;

    markLaneOscActivity(laneIndex);

    if (target == "lane.gain")
    {
        lane->gain.store(juce::jlimit(0.0f, 2.0f, value));
        return;
    }

    if (target == "lane.solo")
    {
        lane->solo.store(value >= 0.5f);
        return;
    }

    if (target == "lane.pan")
    {
        lane->pan.store(juce::jlimit(-1.0f, 1.0f, value));
        return;
    }

    if (target.startsWith("plugin[") && target.contains("].param[") && target.endsWithChar(']'))
    {
        const auto pluginOpen = target.indexOfChar('[');
        const auto pluginClose = target.indexOfChar(']');
        const auto paramOpen = target.lastIndexOfChar('[');
        const auto paramClose = target.lastIndexOfChar(']');
        if (pluginOpen < 0 || pluginClose <= pluginOpen || paramOpen < 0 || paramClose <= paramOpen)
            return;

        const auto pluginIndex = target.substring(pluginOpen + 1, pluginClose).getIntValue();
        const auto paramIndex = target.substring(paramOpen + 1, paramClose).getIntValue();

        juce::AudioPluginInstance* targetPlugin = nullptr;
        if (pluginIndex == 0)
        {
            targetPlugin = lane->plugin;
        }
        else if (juce::isPositiveAndBelow(pluginIndex - 1, kNumFxSlots))
        {
            targetPlugin = lane->fxPlugins[static_cast<size_t> (pluginIndex - 1)].get();
        }

        if (targetPlugin != nullptr && juce::isPositiveAndBelow(paramIndex, targetPlugin->getParameters().size()))
            targetPlugin->getParameters()[paramIndex]->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, value));
    }
}

void HostEngine::handleTransport(const juce::OSCMessage& m)
{
    if (m.size() < 1 || !m[0].isString())
        return;

    const auto command = m[0].getString();
    if (command == "recordStart")
    {
        const auto file = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                              .getNonexistentChildFile("split-audio-take", ".wav");
        if (startRecording(file))
        {
            appendLog("Recording to " + file.getFullPathName());
            recordingTargetFile = file;
            recordButton.setButtonText("Stop");
            recordButton.setToggleState(true, juce::dontSendNotification);
        }
        else
            appendLog("Recording start failed");
    }
    else if (command == "recordStop")
    {
        stopRecording();
        appendLog("Recording stopped");
        recordButton.setButtonText("Record");
        recordButton.setToggleState(false, juce::dontSendNotification);
    }
}

void HostEngine::handleTempo(const juce::OSCMessage& m)
{
    if (m.size() < 1)
        return;

    float bpm = 120.0f;
    if (m[0].isFloat32())
        bpm = m[0].getFloat32();
    else if (m[0].isInt32())
        bpm = static_cast<float> (m[0].getInt32());
    else
        return;

    bpm = juce::jlimit(20.0f, 300.0f, bpm);
    transportTempoBpm.store(bpm, std::memory_order_relaxed);
    pushTempoToLink(bpm);
    refreshTopRightStatusBox();

    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    const auto changedEnough = std::abs(bpm - lastTempoUiBpm) >= 0.1f;
    const auto longEnough = (nowMs - lastTempoUiMs) >= 250.0;
    if (changedEnough || longEnough)
    {
        uiStatus.setText("Tempo: " + juce::String(bpm, 2) + " BPM", juce::dontSendNotification);
        lastTempoUiBpm = bpm;
        lastTempoUiMs = nowMs;
    }
}

bool HostEngine::startRecording(const juce::File& destination)
{
    stopRecording();

    const auto targetRate = juce::jmax(8000.0, recordingSampleRate);

    if (auto stream = std::make_unique<juce::FileOutputStream>(destination))
    {
        juce::WavAudioFormat wav;
        auto* writer = wav.createWriterFor(stream.get(), targetRate, 2, recordingBitDepth, {}, 0);
        if (writer != nullptr)
        {
            stream.release();
            const juce::ScopedLock scope(writerLock);
            threadedWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(writer, writerThread, 32768);
            return true;
        }
    }

    appendLog("Failed to start recording");
    return false;
}

void HostEngine::stopRecording()
{
    const juce::ScopedLock scope(writerLock);
    threadedWriter.reset();
}

bool HostEngine::isRecordingActive()
{
    const juce::ScopedLock scope(writerLock);
    return threadedWriter != nullptr;
}

void HostEngine::toggleRecordingFromUi()
{
    if (isRecordingActive())
    {
        stopRecording();
        recordButton.setButtonText("Record");
        recordButton.setToggleState(false, juce::dontSendNotification);
        uiStatus.setText("Recording stopped", juce::dontSendNotification);
        appendLog("Recording stopped");
        return;
    }

    auto destination = recordingTargetFile;
    if (destination == juce::File())
        destination = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("split-audio-take.wav");

    auto parent = destination.getParentDirectory();
    if (! parent.exists())
        parent = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);

    const auto stem = destination.getFileNameWithoutExtension().isNotEmpty()
                          ? destination.getFileNameWithoutExtension()
                          : juce::String("split-audio-take");
    const auto file = parent.getNonexistentChildFile(stem, ".wav", false);

    if (startRecording(file))
    {
        recordingTargetFile = file;
        recordButton.setButtonText("Stop");
        recordButton.setToggleState(true, juce::dontSendNotification);
        uiStatus.setText("Recording: " + file.getFileName(), juce::dontSendNotification);
        appendLog("Recording to " + file.getFullPathName() + " (" + juce::String(recordingBitDepth) + "-bit)");
    }
    else
    {
        recordButton.setButtonText("Record");
        recordButton.setToggleState(false, juce::dontSendNotification);
        uiStatus.setText("Failed to start recording", juce::dontSendNotification);
    }
}

void HostEngine::openRecordingSettingsDialog()
{
    juce::PopupMenu menu;
    menu.addItem(1, "Set Recording File...");
    menu.addSeparator();
    menu.addItem(101, "16-bit WAV", true, recordingBitDepth == 16);
    menu.addItem(102, "24-bit WAV", true, recordingBitDepth == 24);
    menu.addItem(103, "32-bit WAV", true, recordingBitDepth == 32);
    menu.addSeparator();
    menu.addItem(201, "Sample Rate: 44100", true, std::abs(recordingSampleRate - 44100.0) < 1.0);
    menu.addItem(202, "Sample Rate: 48000", true, std::abs(recordingSampleRate - 48000.0) < 1.0);
    menu.addItem(203, "Sample Rate: 88200", true, std::abs(recordingSampleRate - 88200.0) < 1.0);
    menu.addItem(204, "Sample Rate: 96000", true, std::abs(recordingSampleRate - 96000.0) < 1.0);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&recordSettingsButton),
                       [this] (int selection)
                       {
                           if (selection == 0)
                               return;

                           if (selection == 1)
                           {
                               auto initial = recordingTargetFile;
                               if (initial == juce::File())
                                   initial = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("split-audio-take.wav");

                               recordSettingsChooser = std::make_unique<juce::FileChooser>("Set Recording File", initial, "*.wav");
                               recordSettingsChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                                                  | juce::FileBrowserComponent::canSelectFiles
                                                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                                                                  [this] (const juce::FileChooser& chooser)
                                                                  {
                                                                      auto file = chooser.getResult();
                                                                      recordSettingsChooser.reset();
                                                                      if (file == juce::File())
                                                                          return;
                                                                      if (! file.hasFileExtension("wav"))
                                                                          file = file.withFileExtension("wav");

                                                                      recordingTargetFile = file;
                                                                      appendLog("Recording file set: " + file.getFullPathName());
                                                                      uiStatus.setText("Rec file: " + file.getFileName(), juce::dontSendNotification);
                                                                  });
                               return;
                           }

                           if (selection == 101) recordingBitDepth = 16;
                           else if (selection == 102) recordingBitDepth = 24;
                           else if (selection == 103) recordingBitDepth = 32;
                           else if (selection >= 201 && selection <= 204)
                           {
                               double desiredRate = recordingSampleRate;
                               if (selection == 201) desiredRate = 44100.0;
                               else if (selection == 202) desiredRate = 48000.0;
                               else if (selection == 203) desiredRate = 88200.0;
                               else if (selection == 204) desiredRate = 96000.0;

                               juce::AudioDeviceManager::AudioDeviceSetup setup;
                               deviceManager.getAudioDeviceSetup(setup);
                               setup.sampleRate = desiredRate;
                               const auto err = deviceManager.setAudioDeviceSetup(setup, true);
                               if (err.isNotEmpty())
                               {
                                   appendLog("Failed to set sample rate to " + juce::String(desiredRate, 0) + ": " + err);
                                   uiStatus.setText("Sample rate change failed", juce::dontSendNotification);
                                   return;
                               }

                               if (auto* dev = deviceManager.getCurrentAudioDevice())
                               {
                                   recordingSampleRate = dev->getCurrentSampleRate();
                                   appendLog("Recording sample rate set to " + juce::String(recordingSampleRate, 0));
                                   uiStatus.setText("Rec SR: " + juce::String(recordingSampleRate, 0), juce::dontSendNotification);
                               }
                               else
                               {
                                   recordingSampleRate = desiredRate;
                                   appendLog("Recording sample rate set to " + juce::String(recordingSampleRate, 0));
                               }
                               return;
                           }

                           appendLog("Recording bit depth set to " + juce::String(recordingBitDepth));
                       });
}
