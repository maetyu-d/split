#include <JuceHeader.h>
#include "MainWindow.h"

class PatternIdeApp final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Pattern IDE"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }

    void initialise(const juce::String&) override
    {
        mainWindow = std::make_unique<MainWindow>(getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();
    }

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(PatternIdeApp)
