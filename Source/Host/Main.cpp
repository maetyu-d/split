#include <JuceHeader.h>
#include "HostMainWindow.h"

class PluginHostApp final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Plugin Host"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }

    void initialise(const juce::String&) override
    {
        mainWindow = std::make_unique<HostMainWindow>(getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();
    }

private:
    std::unique_ptr<HostMainWindow> mainWindow;
};

START_JUCE_APPLICATION(PluginHostApp)
