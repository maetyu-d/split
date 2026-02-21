#pragma once

#include <JuceHeader.h>
#include "HostEngine.h"

class HostMainWindow final : public juce::DocumentWindow
{
public:
    explicit HostMainWindow(const juce::String& name);
    void closeButtonPressed() override;

private:
    HostEngine engine;
};
