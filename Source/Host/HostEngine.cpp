#include "HostEngine.h"
#include "../Common/OscProtocol.h"

#include <algorithm>
#include <cmath>

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

constexpr int kNumFxSlots = 3;
constexpr int kNumMasterFxSlots = 6;

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
   #if JUCE_PLUGINHOST_AU
    formatManager.addFormat(std::make_unique<juce::AudioUnitPluginFormat>());
   #endif
   #if JUCE_PLUGINHOST_VST3
    formatManager.addFormat(std::make_unique<juce::VST3PluginFormat>());
   #endif

    deadMansPedalFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("split-audio-deadmanspedal.txt");

    for (auto& lane : lanes)
    {
        lane.synth.clearSounds();
        lane.synth.clearVoices();
        for (int v = 0; v < 8; ++v)
            lane.synth.addVoice(new SineVoice());
        lane.synth.addSound(new SineSound());
    }

    setOpaque(true);
    setBufferedToImage(true);

    auto styleTopButton = [] (juce::TextButton& b)
    {
        b.setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(45, 50, 58));
        b.setColour(juce::TextButton::buttonOnColourId, juce::Colour::fromRGB(62, 71, 84));
        b.setColour(juce::TextButton::textColourOffId, juce::Colour::fromRGB(225, 228, 234));
        b.setColour(juce::TextButton::textColourOnId, juce::Colour::fromRGB(239, 242, 248));
    };

    addAndMakeVisible(refreshPluginsButton);
    styleTopButton(refreshPluginsButton);
    refreshPluginsButton.onClick = [this]
    {
        refreshPluginCatalog();
    };

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
    uiStatus.setColour(juce::Label::textColourId, juce::Colour::fromRGB(216, 220, 228));
    uiStatus.setText("Ready. Click Refresh Plugins to scan AU/VST3.", juce::dontSendNotification);

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
    log.setText("Host started\n");
    logWindow = std::make_unique<LogWindow>(log, [this] (bool visible)
    {
        logDrawerOpen = visible;
        logDrawerButton.setButtonText(logDrawerOpen ? "Hide Logs" : "Logs");
    });

    rebuildPluginMenus();
    syncAllLaneRows();

    writerThread.startThread();

    deviceManager.initialiseWithDefaultDevices(0, 2);
    deviceManager.addAudioCallback(this);

    connect(oscproto::defaultPort);
    addListener(this);

    appendLog("Listening OSC on port " + juce::String(oscproto::defaultPort));
}

HostEngine::~HostEngine()
{
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

void HostEngine::paint(juce::Graphics& g)
{
    juce::ColourGradient bg(juce::Colour::fromRGB(20, 23, 30), 0.0f, 0.0f,
                            juce::Colour::fromRGB(13, 15, 19), 0.0f, static_cast<float> (getHeight()), false);
    g.setGradientFill(bg);
    g.fillAll();

    auto bounds = getLocalBounds().reduced(8);
    const auto topBar = bounds.removeFromTop(56);
    g.setColour(juce::Colour::fromRGB(34, 39, 48));
    g.fillRoundedRectangle(topBar.toFloat(), 6.0f);
    g.setColour(juce::Colour::fromRGB(52, 58, 69).withAlpha(0.65f));
    g.drawRoundedRectangle(topBar.toFloat().reduced(0.5f), 6.0f, 1.0f);

    const auto masterArea = masterHeaderLabel.getBounds()
                                .getUnion(masterFxExpandButton.getBounds())
                                .getUnion(masterGainSlider.getBounds())
                                .expanded(4, 4);
    g.setColour(juce::Colour::fromRGB(64, 58, 44).withAlpha(0.18f));
    g.fillRoundedRectangle(masterArea.toFloat(), 7.0f);
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

    const int refreshW = compactWidth ? 104 : 130;
    const int saveW = compactWidth ? 86 : 110;
    const int loadW = compactWidth ? 86 : 110;
    const int logsW = compactWidth ? 64 : 90;
    const int controlVMargin = compactHeight ? 6 : 8;
    refreshPluginsButton.setBounds(controls.removeFromLeft(refreshW).reduced(2, controlVMargin));
    saveConfigButton.setBounds(controls.removeFromLeft(saveW).reduced(2, controlVMargin));
    loadConfigButton.setBounds(controls.removeFromLeft(loadW).reduced(2, controlVMargin));
    logDrawerButton.setBounds(controls.removeFromLeft(logsW).reduced(2, controlVMargin));
    uiStatus.setBounds(controls.reduced(6, 8));

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

        laneLabels[static_cast<size_t> (i)].setBounds(col.removeFromTop(laneLabelH));
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

    masterGainSlider.setBounds(masterCol.reduced(6, 2));

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
    juce::AudioBuffer<float> outputBuffer(outputChannelData, numOutputChannels, numSamples);
    outputBuffer.clear();

    std::array<DueNoteOff, maxDueOffsPerBlock> dueOffs {};
    const auto dueCount = collectDueNoteOffs(dueOffs);

    bool anySoloed = false;
    {
        anySoloed = std::any_of(lanes.begin(), lanes.end(), [] (const auto& lane) { return lane.solo.load(); });
    }

    for (int laneIndex = 0; laneIndex < static_cast<int> (lanes.size()); ++laneIndex)
    {
        auto& lane = lanes[static_cast<size_t> (laneIndex)];
        if (lane.tempBuffer.getNumChannels() < numOutputChannels || lane.tempBuffer.getNumSamples() < numSamples)
            continue; // Avoid allocating in audio thread.

        lane.tempBuffer.clear();

        juce::MidiBuffer midi;
        popMidiEventsForLane(lane, midi);
        for (int i = 0; i < dueCount; ++i)
            if (dueOffs[static_cast<size_t> (i)].lane == laneIndex)
                midi.addEvent(juce::MidiMessage::noteOff(1, dueOffs[static_cast<size_t> (i)].note, dueOffs[static_cast<size_t> (i)].velocity), 0);

        const auto gain = lane.gain.load();
        const auto pan = lane.pan.load();
        const auto muted = lane.muted.load();
        const auto solo = lane.solo.load();
        auto* plugin = lane.plugin.get();

        if (muted || (anySoloed && ! solo))
            continue;

        if (plugin != nullptr)
            plugin->processBlock(lane.tempBuffer, midi);
        else
            lane.synth.renderNextBlock(lane.tempBuffer, midi, 0, numSamples);

        juce::MidiBuffer fxMidi;
        for (auto& fx : lane.fxPlugins)
            if (fx != nullptr)
                fx->processBlock(lane.tempBuffer, fxMidi);

        const auto clampedPan = juce::jlimit(-1.0f, 1.0f, pan);
        const auto leftGain = gain * juce::jlimit(0.0f, 1.0f, 1.0f - clampedPan);
        const auto rightGain = gain * juce::jlimit(0.0f, 1.0f, 1.0f + clampedPan);

        if (numOutputChannels > 0)
            outputBuffer.addFrom(0, 0, lane.tempBuffer, 0, 0, numSamples, leftGain);
        if (numOutputChannels > 1)
            outputBuffer.addFrom(1, 0, lane.tempBuffer, juce::jmin(1, lane.tempBuffer.getNumChannels() - 1), 0, numSamples, rightGain);
        for (int ch = 2; ch < numOutputChannels; ++ch)
            outputBuffer.addFrom(ch, 0, lane.tempBuffer, juce::jmin(ch, lane.tempBuffer.getNumChannels() - 1), 0, numSamples, gain);
    }

    if (masterTempBuffer.getNumChannels() >= numOutputChannels && masterTempBuffer.getNumSamples() >= numSamples)
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
            masterTempBuffer.copyFrom(ch, 0, outputBuffer, ch, 0, numSamples);

        juce::MidiBuffer masterFxMidi;
        for (auto& fx : masterFxPlugins)
            if (fx != nullptr)
                fx->processBlock(masterTempBuffer, masterFxMidi);

        for (int ch = 0; ch < numOutputChannels; ++ch)
            outputBuffer.copyFrom(ch, 0, masterTempBuffer, ch, 0, numSamples);
    }

    outputBuffer.applyGain(masterGain.load());

    if (writerLock.tryEnter())
    {
        if (threadedWriter != nullptr)
            threadedWriter->write(outputChannelData, numSamples);
        writerLock.exit();
    }
}

void HostEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (device == nullptr)
        return;

    const juce::ScopedLock scope(laneLock);
    sampleRate = device->getCurrentSampleRate();
    expectedBlockSize = juce::jmax(16, device->getCurrentBufferSizeSamples());
    const auto outputChannels = juce::jmax(2, device->getActiveOutputChannels().countNumberOfSetBits());
    const auto preallocSamples = expectedBlockSize * 2;

    for (auto& lane : lanes)
    {
        lane.synth.setCurrentPlaybackSampleRate(sampleRate);
        lane.tempBuffer.setSize(outputChannels, preallocSamples, false, false, true);
        if (lane.plugin != nullptr)
            lane.plugin->prepareToPlay(sampleRate, expectedBlockSize);
        for (auto& fx : lane.fxPlugins)
            if (fx != nullptr)
                fx->prepareToPlay(sampleRate, expectedBlockSize);
    }

    masterTempBuffer.setSize(outputChannels, preallocSamples, false, false, true);
    for (auto& fx : masterFxPlugins)
        if (fx != nullptr)
            fx->prepareToPlay(sampleRate, expectedBlockSize);
}

void HostEngine::audioDeviceStopped() {}

HostEngine::Lane* HostEngine::getLane(int index)
{
    if (index < 0 || index >= static_cast<int> (lanes.size()))
        return nullptr;
    return &lanes[static_cast<size_t> (index)];
}

bool HostEngine::enqueueMidiMessage(int laneIndex, const juce::MidiMessage& message)
{
    auto* lane = getLane(laneIndex);
    if (lane == nullptr)
        return false;

    const auto* raw = message.getRawData();
    const auto rawSize = message.getRawDataSize();
    if (raw == nullptr || rawSize <= 0)
        return false;

    RtMidiEvent ev;
    ev.status = static_cast<uint8_t> (raw[0]);
    ev.data1 = static_cast<uint8_t> (rawSize > 1 ? raw[1] : 0);
    ev.data2 = static_cast<uint8_t> (rawSize > 2 ? raw[2] : 0);

    const auto write = lane->midiWriteIndex.load(std::memory_order_relaxed);
    const auto read = lane->midiReadIndex.load(std::memory_order_acquire);
    const auto nextWrite = write + 1;
    if ((nextWrite - read) > static_cast<uint32_t> (midiQueueCapacity))
        return false;

    lane->midiQueue[write % static_cast<uint32_t> (midiQueueCapacity)] = ev;
    lane->midiWriteIndex.store(nextWrite, std::memory_order_release);
    return true;
}

void HostEngine::popMidiEventsForLane(Lane& lane, juce::MidiBuffer& out)
{
    auto read = lane.midiReadIndex.load(std::memory_order_relaxed);
    const auto write = lane.midiWriteIndex.load(std::memory_order_acquire);

    while (read < write)
    {
        const auto& ev = lane.midiQueue[read % static_cast<uint32_t> (midiQueueCapacity)];
        out.addEvent(juce::MidiMessage(ev.status, ev.data1, ev.data2), 0);
        ++read;
    }

    lane.midiReadIndex.store(read, std::memory_order_release);
}

int HostEngine::collectDueNoteOffs(std::array<DueNoteOff, maxDueOffsPerBlock>& out)
{
    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    const juce::SpinLock::ScopedLockType scope(scheduledLock);

    int count = 0;
    for (int i = static_cast<int> (scheduledNoteOffs.size()) - 1; i >= 0; --i)
    {
        const auto& ev = scheduledNoteOffs[static_cast<size_t> (i)];
        if (ev.dueMs <= nowMs)
        {
            if (count < maxDueOffsPerBlock)
            {
                out[static_cast<size_t> (count)].lane = ev.lane;
                out[static_cast<size_t> (count)].note = ev.note;
                out[static_cast<size_t> (count)].velocity = ev.velocity;
                ++count;
            }

            scheduledNoteOffs.erase(scheduledNoteOffs.begin() + i);
        }
    }

    return count;
}

void HostEngine::scheduleNoteOff(int laneIndex, int midiNote, float velocity, float durationSec)
{
    if (durationSec <= 0.0f)
        return;

    ScheduledNoteOff off;
    off.lane = laneIndex;
    off.note = midiNote;
    off.velocity = velocity;
    off.dueMs = juce::Time::getMillisecondCounterHiRes() + (durationSec * 1000.0);

    const juce::SpinLock::ScopedLockType scope(scheduledLock);
    scheduledNoteOffs.push_back(off);
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
    configLoadChooser = std::make_unique<juce::FileChooser>("Load Host Config",
                                                             juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
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

void HostEngine::refreshPluginCatalog()
{
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

    instance->prepareToPlay(sampleRate, expectedBlockSize);
    if (stateBase64.isNotEmpty())
    {
        juce::MemoryBlock pluginState;
        if (pluginState.fromBase64Encoding(stateBase64) && pluginState.getSize() > 0)
            instance->setStateInformation(pluginState.getData(), static_cast<int> (pluginState.getSize()));
    }

    {
        const juce::ScopedLock audioScope(deviceManager.getAudioCallbackLock());
        const juce::ScopedLock laneScope(laneLock);
        if (auto* lane = getLane(laneIndex))
        {
            if (lane->plugin != nullptr)
                lane->plugin->releaseResources();
            lane->plugin = std::move(instance);
            lane->pluginName = desc.name;
        }
    }

    closePluginEditorForLane(laneIndex);
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

    instance->prepareToPlay(sampleRate, expectedBlockSize);
    if (stateBase64.isNotEmpty())
    {
        juce::MemoryBlock pluginState;
        if (pluginState.fromBase64Encoding(stateBase64) && pluginState.getSize() > 0)
            instance->setStateInformation(pluginState.getData(), static_cast<int> (pluginState.getSize()));
    }

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

    instance->prepareToPlay(sampleRate, expectedBlockSize);
    if (stateBase64.isNotEmpty())
    {
        juce::MemoryBlock pluginState;
        if (pluginState.fromBase64Encoding(stateBase64) && pluginState.getSize() > 0)
            instance->setStateInformation(pluginState.getData(), static_cast<int> (pluginState.getSize()));
    }

    const juce::ScopedLock audioScope(deviceManager.getAudioCallbackLock());
    if (masterFxPlugins[static_cast<size_t> (slotIndex)] != nullptr)
        masterFxPlugins[static_cast<size_t> (slotIndex)]->releaseResources();

    masterFxPlugins[static_cast<size_t> (slotIndex)] = std::move(instance);
    masterFxNames[static_cast<size_t> (slotIndex)] = desc.name;
    closeMasterEffectEditorForSlot(slotIndex);
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
        if (lane->plugin != nullptr)
            lane->plugin->releaseResources();

        lane->plugin.reset();
        lane->pluginName = "Built-in Sine";
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

    enqueueMidiMessage(laneIndex, juce::MidiMessage::noteOn(1, note, velocity));
    scheduleNoteOff(laneIndex, note, velocity, 0.05f);
}

void HostEngine::handleNote(const juce::OSCMessage& m)
{
    const auto laneIndex = getIntArg(m, 0, 0);
    const auto midiNote = juce::jlimit(0, 127, getIntArg(m, 1, 60));
    const auto velocity = juce::jlimit(0.0f, 1.0f, getFloatArg(m, 2, 0.8f));
    const auto durationSec = getFloatArg(m, 3, -1.0f);

    enqueueMidiMessage(laneIndex, juce::MidiMessage::noteOn(1, midiNote, velocity));
    scheduleNoteOff(laneIndex, midiNote, velocity, durationSec);
}

void HostEngine::handleNoteOff(const juce::OSCMessage& m)
{
    const auto laneIndex = getIntArg(m, 0, 0);
    const auto midiNote = juce::jlimit(0, 127, getIntArg(m, 1, 60));
    const auto velocity = juce::jlimit(0.0f, 1.0f, getFloatArg(m, 2, 0.0f));

    enqueueMidiMessage(laneIndex, juce::MidiMessage::noteOff(1, midiNote, velocity));
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
        syncLaneRow(laneIndex);
        return;
    }

    const juce::ScopedLock scope(laneLock);
    auto* lane = getLane(laneIndex);
    if (lane == nullptr)
        return;

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
            targetPlugin = lane->plugin.get();
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
        startRecording(file);
        appendLog("Recording to " + file.getFullPathName());
    }
    else if (command == "recordStop")
    {
        stopRecording();
        appendLog("Recording stopped");
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

void HostEngine::startRecording(const juce::File& destination)
{
    stopRecording();

    if (auto stream = std::make_unique<juce::FileOutputStream>(destination))
    {
        juce::WavAudioFormat wav;
        auto* writer = wav.createWriterFor(stream.get(), sampleRate, 2, 24, {}, 0);
        if (writer != nullptr)
        {
            stream.release();
            const juce::ScopedLock scope(writerLock);
            threadedWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(writer, writerThread, 32768);
            return;
        }
    }

    appendLog("Failed to start recording");
}

void HostEngine::stopRecording()
{
    const juce::ScopedLock scope(writerLock);
    threadedWriter.reset();
}
