#include "MainWindow.h"
#include "../Common/OscProtocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <queue>
#include <random>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
class SplitTokeniser final : public juce::CodeTokeniser
{
public:
    enum TokenType
    {
        tokenType_plain = 0,
        tokenType_comment,
        tokenType_keyword,
        tokenType_number,
        tokenType_symbol
    };

    int readNextToken(juce::CodeDocument::Iterator& source) override
    {
        const auto c = source.peekNextChar();
        if (c == 0)
            return tokenType_plain;

        if (juce::CharacterFunctions::isWhitespace(c))
        {
            source.skipWhitespace();
            return tokenType_plain;
        }

        if (c == '#')
        {
            source.skipToEndOfLine();
            return tokenType_comment;
        }

        if (juce::CharacterFunctions::isDigit(c)
            || ((c == '-' || c == '+') && juce::CharacterFunctions::isDigit(source.peekNextChar())))
        {
            source.skip();
            while (! source.isEOF())
            {
                const auto d = source.peekNextChar();
                if (juce::CharacterFunctions::isDigit(d) || d == '.')
                    source.skip();
                else
                    break;
            }
            return tokenType_number;
        }

        if (juce::CharacterFunctions::isLetter(c))
        {
            juce::String token;
            while (! source.isEOF())
            {
                const auto d = source.peekNextChar();
                if (! (juce::CharacterFunctions::isLetterOrDigit(d) || d == '_' || d == '.' || d == '[' || d == ']'))
                    break;
                token << juce::String::charToString(d);
                source.skip();
            }

            static const std::unordered_set<std::string> keywords {
                "tempo", "steps", "swing", "lanesteps", "barloop", "loopbars", "section", "language", "split", "teletype",
                "trigger", "break", "drill", "note", "randnote", "choicenote", "markovnote", "mod", "transport",
                "scramble", "rotate", "stutter", "every", "notevery", "from", "to",
                "velocitymode", "velocity", "lane", "midi", "normalized", "normalised",
                "script", "run", "set", "add", "sub", "mul", "div", "mod", "pow", "min", "max",
                "abs", "floor", "ceil", "round", "sin", "cos", "tan", "clamp", "lerp",
                "and", "or", "xor", "not", "rrand", "irand",
                "if", "then", "else", "repeat", "for", "while",
                "pset", "padd", "pget", "pswap", "prot", "pclr", "pfill", "prand",
                "metro", "clock", "x", "y", "z", "t"
            };
            return keywords.count(token.toLowerCase().toStdString()) > 0 ? tokenType_keyword : tokenType_plain;
        }

        source.skip();
        return tokenType_symbol;
    }

    juce::CodeEditorComponent::ColourScheme getDefaultColourScheme() override
    {
        juce::CodeEditorComponent::ColourScheme cs;
        cs.set("Plain", juce::Colour::fromRGB(215, 222, 236));
        cs.set("Comment", juce::Colour::fromRGB(124, 153, 131));
        cs.set("Keyword", juce::Colour::fromRGB(124, 185, 255));
        cs.set("Number", juce::Colour::fromRGB(221, 198, 138));
        cs.set("Symbol", juce::Colour::fromRGB(177, 188, 206));
        return cs;
    }
};

class ScriptCodeEditor final : public juce::CodeEditorComponent
{
public:
    ScriptCodeEditor(juce::CodeDocument& doc, juce::CodeTokeniser* tok)
        : juce::CodeEditorComponent(doc, tok), document(doc) {}

    void setActiveLine(int line1Based)
    {
        activeLine.store(line1Based);
        repaint();
    }

    void setErrorLine(int line1Based)
    {
        errorLine.store(line1Based);
        repaint();
    }

    void paintOverChildren(juce::Graphics& g) override
    {
        juce::CodeEditorComponent::paintOverChildren(g);
        drawLineOverlay(g, activeLine.load(), juce::Colour::fromRGB(92, 79, 48).withAlpha(0.35f), false);
        drawLineOverlay(g, errorLine.load(), juce::Colour::fromRGB(219, 92, 92).withAlpha(0.95f), true);
    }

private:
    void drawLineOverlay(juce::Graphics& g, int line1Based, juce::Colour colour, bool underlineOnly)
    {
        if (line1Based <= 0)
            return;

        juce::CodeDocument::Position p(document, 0);
        p.setLineAndIndex(line1Based - 1, 0);
        const auto ch = getCharacterBounds(p);
        const auto y = ch.getY();
        const auto h = getLineHeight();

        if (y + h < 0 || y > getHeight())
            return;

        if (! underlineOnly)
        {
            g.setColour(colour);
            g.fillRect(0, y, getWidth(), h);
        }
        else
        {
            g.setColour(colour);
            g.drawLine(0.0f, static_cast<float> (y + h - 2), static_cast<float> (getWidth()), static_cast<float> (y + h - 2), 2.0f);
        }
    }

    juce::CodeDocument& document;
    std::atomic<int> activeLine { -1 };
    std::atomic<int> errorLine { -1 };
};

class IdeComponent final : public juce::Component,
                           private juce::Button::Listener,
                           private juce::HighResolutionTimer,
                           private juce::AsyncUpdater
{
public:
    enum class DragTarget
    {
        none,
        timelineCode,
        codeLog
    };

    IdeComponent()
    {
        setOpaque(true);
        setBufferedToImage(true);

        auto styleButton = [] (juce::TextButton& b)
        {
            b.setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(43, 49, 58));
            b.setColour(juce::TextButton::buttonOnColourId, juce::Colour::fromRGB(59, 68, 82));
            b.setColour(juce::TextButton::textColourOffId, juce::Colour::fromRGB(225, 230, 238));
            b.setColour(juce::TextButton::textColourOnId, juce::Colour::fromRGB(243, 247, 252));
        };

        addAndMakeVisible(runButton);
        runButton.setButtonText("Run");
        runButton.setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(73, 63, 39));
        runButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour::fromRGB(92, 79, 48));
        runButton.setColour(juce::TextButton::textColourOnId, juce::Colour::fromRGB(221, 198, 138));
        runButton.setColour(juce::TextButton::textColourOffId, juce::Colour::fromRGB(210, 186, 122));
        runButton.addListener(this);

        addAndMakeVisible(stopButton);
        stopButton.setButtonText("Stop");
        styleButton(stopButton);
        stopButton.addListener(this);

        addAndMakeVisible(loadButton);
        loadButton.setButtonText("Load");
        styleButton(loadButton);
        loadButton.addListener(this);

        addAndMakeVisible(saveButton);
        saveButton.setButtonText("Save");
        styleButton(saveButton);
        saveButton.addListener(this);

        addAndMakeVisible(status);
        status.setJustificationType(juce::Justification::centredLeft);
        status.setColour(juce::Label::textColourId, juce::Colour::fromRGB(216, 223, 234));
        status.setColour(juce::Label::backgroundColourId, juce::Colour::fromRGB(27, 33, 42).withAlpha(0.72f));
        status.setColour(juce::Label::outlineColourId, juce::Colour::fromRGB(66, 78, 96).withAlpha(0.55f));
        status.setOpaque(true);
        status.setText("Ready. Sending OSC to 127.0.0.1:9001", juce::dontSendNotification);

        codeEditor = std::make_unique<ScriptCodeEditor>(codeDocument, &tokeniser);
        addAndMakeVisible(*codeEditor);
        codeEditor->setFont(juce::Font(juce::FontOptions("Menlo", 15.0f, juce::Font::plain)));
        codeEditor->setColour(juce::CodeEditorComponent::backgroundColourId, juce::Colour::fromRGB(16, 20, 28));
        codeEditor->setColour(juce::CodeEditorComponent::defaultTextColourId, juce::Colour::fromRGB(215, 222, 236));
        codeEditor->setColour(juce::CodeEditorComponent::highlightColourId, juce::Colour::fromRGB(74, 92, 132).withAlpha(0.55f));
        codeEditor->setColour(juce::CodeEditorComponent::lineNumberBackgroundId, juce::Colour::fromRGB(23, 28, 37));
        codeEditor->setColour(juce::CodeEditorComponent::lineNumberTextId, juce::Colour::fromRGB(119, 132, 153));
        codeEditor->setColourScheme(tokeniser.getDefaultColourScheme());
        codeEditor->setTabSize(4, true);
        codeEditor->setLineNumbersShown(true);
        codeEditor->loadContent(
            "# Split Language v0\n"
            "tempo 172\n"
            "steps 16\n"
            "swing 0.14\n"
            "laneSteps 0 16\n"
            "laneSteps 1 15\n"
            "\n"
            "# break <startStep> <lane> <pattern> [baseVelocity] [chance] [every N] [notEvery N]\n"
            "break 0 0 k-hs-k-hk-hs-k- 1.0 1.0 every 1\n"
            "\n"
            "# drill <step> <lane> <eventId> <hits> [velocity] [chance] [every N] [notEvery N]\n"
            "drill 7 0 snare 6 0.95 0.8 every 2\n"
            "drill 15 0 snare 8 0.90 0.6 every 4\n"
            "\n"
            "# note list with rests\n"
            "note 0 1 [60 - 64 - 67 - 72 -] 0.8 0.25 leadA 1.0 every 1\n"
            "\n"
            "# mutation rules\n"
            "scramble 0 every 4 0.8\n"
            "rotate 0 1 every 2 1.0\n"
            "stutter 0 snare 3 every 2 0.8\n"
            "\n"
            "# optional song form\n"
            "section a 8\n"
            "section b 8\n"
            "\n"
            "# mod <step> <lane> <target> <value> [rampBeats] [chance] [every N] [notEvery N] [from C] [to C]\n"
            "mod 0 1 lane.pan -0.45 0.0 1.0 every 1 section a\n"
            "mod 8 1 lane.pan 0.45 0.0 1.0 every 1 section b\n");

        addAndMakeVisible(outputLog);
        outputLog.setMultiLine(true);
        outputLog.setReadOnly(true);
        outputLog.setFont(juce::Font(juce::FontOptions("Menlo", 13.5f, juce::Font::plain)));
        outputLog.setColour(juce::TextEditor::backgroundColourId, juce::Colour::fromRGB(14, 18, 24));
        outputLog.setColour(juce::TextEditor::textColourId, juce::Colour::fromRGB(170, 180, 197));
        outputLog.setColour(juce::TextEditor::outlineColourId, juce::Colour::fromRGB(66, 78, 96));
        outputLog.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour::fromRGB(66, 78, 96));
        outputLog.setColour(juce::CaretComponent::caretColourId, juce::Colour::fromRGB(170, 180, 197));
        outputLog.setText("Language:\n"
                          "  tempo <bpm>\n"
                          "  tempo <step> <bpm> [chance] [every N] [notEvery N] [from C] [to C] [section S]\n"
                          "  language <split|teletype>\n"
                          "  steps <count>\n"
                          "  swing <0..0.5>\n"
                          "  laneSteps <lane> <steps>\n"
                          "  barLoop <startBar> <endBar>\n"
                          "  velocityMode <midi|normalized>\n"
                          "  velocity <value>\n"
                          "  velocity lane <lane> <value>\n"
                          "  section <name> <cycles>\n"
                          "  # teletype mode aliases: M/L/SC/TR/NT/RN/CN/MK/MD/TP/VS/VM\n"
                          "  # teletype core v2: SCRIPT <id> <command...> or block SCRIPT <id> ... END, RUN <step> <id>, vars X/Y/Z/T\n"
                          "  # script commands: math/logic (SET/ADD/SUB/MUL/DIV/...), IF/ELSE, REPEAT/FOR/WHILE,\n"
                          "  # pattern memory (PSET/PGET/...), and metro/clock (METRO/CLOCK)\n"
                          "  trigger <step> <lane> <eventId> [velocity] [chance] [every N] [notEvery N] [from C] [to C] [section S]\n"
                          "  break <startStep> <lane> <pattern> [baseVelocity] [chance] [every N] [notEvery N] [from C] [to C] [section S]\n"
                          "  drill <step> <lane> <eventId> <hits> [velocity] [chance] [every N] [notEvery N] [from C] [to C] [section S]\n"
                          "  note <step> <lane> <midiNote> [velocity] [durationBeats] [eventId] [chance] [every N] [notEvery N] [from C] [to C] [section S]\n"
                          "  note <startStep> <lane> [m1 - m3 _ ...] [velocity] [durationBeats] [eventId] [chance] [every N] [notEvery N] [from C] [to C] [section S]\n"
                          "  randnote <step> <lane> <minNote> <maxNote> [velocity] [durationBeats] [eventId] [chance] [every N] [notEvery N] [from C] [to C] [section S]\n"
                          "  choicenote <step> <lane> <n1:w1,n2:w2,...> [velocity] [durationBeats] [eventId] [chance] [every N] [notEvery N] [from C] [to C] [section S]\n"
                          "  markovnote <step> <lane> <minNote> <maxNote> <startNote> <pDown> <pStay> <pUp> [velocity] [durationBeats] [eventId] [chance] [every N] [notEvery N] [from C] [to C] [section S]\n"
                          "  mod <step> <lane> <target> <value> [rampBeats] [chance] [every N] [notEvery N] [from C] [to C] [section S]\n"
                          "  transport <step> <command> [chance] [every N] [notEvery N] [from C] [to C] [section S]\n"
                          "  scramble <lane> every <N> [chance]\n"
                          "  rotate <lane> <amount> every <N> [chance]\n"
                          "  stutter <lane> <eventId> <repeats> every <N> [chance]\n\n");

        addAndMakeVisible(oscMonitor);
        oscMonitor.setMultiLine(true);
        oscMonitor.setReadOnly(true);
        oscMonitor.setFont(juce::Font(juce::FontOptions("Menlo", 13.5f, juce::Font::plain)));
        oscMonitor.setColour(juce::TextEditor::backgroundColourId, juce::Colour::fromRGB(14, 18, 24));
        oscMonitor.setColour(juce::TextEditor::textColourId, juce::Colour::fromRGB(170, 180, 197));
        oscMonitor.setColour(juce::TextEditor::outlineColourId, juce::Colour::fromRGB(66, 78, 96));
        oscMonitor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour::fromRGB(66, 78, 96));
        oscMonitor.setColour(juce::CaretComponent::caretColourId, juce::Colour::fromRGB(170, 180, 197));
        oscMonitor.setText("OSC Monitor:\n");
        oscMonitor.setVisible(false);

        addAndMakeVisible(logModeButton);
        logModeButton.setButtonText("Mode: Log");
        styleButton(logModeButton);
        logModeButton.addListener(this);

        addAndMakeVisible(timelineToggleButton);
        timelineToggleButton.setButtonText("Tl:On");
        styleButton(timelineToggleButton);
        timelineToggleButton.addListener(this);

        addAndMakeVisible(transportLabel);
        transportLabel.setJustificationType(juce::Justification::centredRight);
        transportLabel.setColour(juce::Label::textColourId, juce::Colour::fromRGB(214, 188, 115));
        transportLabel.setText("Bar 1 Beat 1 Step 1", juce::dontSendNotification);

        addAndMakeVisible(transportMeter);
        transportMeter.setColour(juce::ProgressBar::foregroundColourId, juce::Colour::fromRGB(214, 188, 115));
        transportMeter.setColour(juce::ProgressBar::backgroundColourId, juce::Colour::fromRGB(60, 54, 41));

        if (! sender.connect("127.0.0.1", oscproto::defaultPort))
            status.setText("Failed to connect OSC socket", juce::dontSendNotification);
    }

    ~IdeComponent() override
    {
        stopTimer();
        runButton.removeListener(this);
        stopButton.removeListener(this);
        loadButton.removeListener(this);
        saveButton.removeListener(this);
        logModeButton.removeListener(this);
        timelineToggleButton.removeListener(this);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(10);
        auto top = area.removeFromTop(46);
        runButton.setBounds(top.removeFromLeft(92).reduced(3, 6));
        stopButton.setBounds(top.removeFromLeft(92).reduced(3, 6));
        loadButton.setBounds(top.removeFromLeft(92).reduced(3, 6));
        saveButton.setBounds(top.removeFromLeft(92).reduced(3, 6));
        logModeButton.setBounds(top.removeFromLeft(110).reduced(3, 6));
        timelineToggleButton.setBounds(top.removeFromLeft(126).reduced(3, 6));
        auto meterArea = top.removeFromRight(240).reduced(4, 8);
        transportLabel.setBounds(meterArea.removeFromTop(16));
        transportMeter.setBounds(meterArea.removeFromTop(12));
        status.setBounds(top.reduced(8, 8));

        const auto timelineSplitterThickness = 8;
        if (showTimeline)
        {
            const auto total = area.getHeight();
            const auto minTimelineH = 72;
            const auto minContentH = 120 + 120 + 8;
            auto timelineHeight = static_cast<int> (std::round(static_cast<double> (total) * timelinePaneRatio));
            timelineHeight = juce::jlimit(minTimelineH,
                                          juce::jmax(minTimelineH, total - minContentH - timelineSplitterThickness),
                                          timelineHeight);

            timelinePanelHeightPx = timelineHeight;
            timelineBounds = area.removeFromTop(timelineHeight).reduced(2);
            timelineSplitterBounds = area.removeFromTop(timelineSplitterThickness);
        }
        else
        {
            timelinePanelHeightPx = 0;
            timelineBounds = {};
            timelineSplitterBounds = {};
        }

        auto contentArea = area;
        const auto minPaneHeight = 120;
        const auto splitterThickness = 8;
        const auto total = contentArea.getHeight();
        auto codeHeight = static_cast<int> (std::round(static_cast<double> (total) * codePaneRatio));
        codeHeight = juce::jlimit(minPaneHeight, juce::jmax(minPaneHeight, total - minPaneHeight - splitterThickness), codeHeight);

        auto editorArea = contentArea.removeFromTop(codeHeight);
        if (codeEditor != nullptr)
            codeEditor->setBounds(editorArea.reduced(2));

        splitterBounds = contentArea.removeFromTop(splitterThickness);
        auto bottom = contentArea.reduced(2);
        outputLog.setBounds(bottom);
        oscMonitor.setBounds(bottom);
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        if (splitterBounds.contains(e.getPosition()) || (showTimeline && timelineSplitterBounds.contains(e.getPosition())))
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        else if (activeDragTarget == DragTarget::none)
            setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (showTimeline && timelineSplitterBounds.contains(e.getPosition()))
        {
            activeDragTarget = DragTarget::timelineCode;
            return;
        }
        if (splitterBounds.contains(e.getPosition()))
            activeDragTarget = DragTarget::codeLog;
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (activeDragTarget == DragTarget::none)
            return;

        auto area = getLocalBounds().reduced(10);
        area.removeFromTop(46);

        if (activeDragTarget == DragTarget::timelineCode && showTimeline)
        {
            const auto total = area.getHeight();
            const auto minTimelineH = 72;
            const auto minContentH = 120 + 120 + 8;
            const auto timelineSplitterThickness = 8;
            const auto top = area.getY();
            auto draggedY = e.getPosition().y - top;
            draggedY = juce::jlimit(minTimelineH,
                                    juce::jmax(minTimelineH, total - minContentH - timelineSplitterThickness),
                                    draggedY);
            timelinePaneRatio = juce::jlimit(0.12f, 0.6f, static_cast<float> (draggedY) / static_cast<float> (juce::jmax(1, total)));
        }
        else if (activeDragTarget == DragTarget::codeLog)
        {
            if (showTimeline)
                area.removeFromTop(timelinePanelHeightPx + 8);
            const auto minPaneHeight = 120;
            const auto splitterThickness = 8;
            const auto total = area.getHeight();
            const auto top = area.getY();
            auto draggedY = e.getPosition().y - top;
            draggedY = juce::jlimit(minPaneHeight, juce::jmax(minPaneHeight, total - minPaneHeight - splitterThickness), draggedY);
            codePaneRatio = juce::jlimit(0.2f, 0.8f, static_cast<float> (draggedY) / static_cast<float> (juce::jmax(1, total)));
        }

        resized();
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        activeDragTarget = DragTarget::none;
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    void paint(juce::Graphics& g) override
    {
        juce::ColourGradient bg(juce::Colour::fromRGB(20, 23, 30), 0.0f, 0.0f,
                                juce::Colour::fromRGB(13, 15, 19), 0.0f, static_cast<float> (getHeight()), false);
        g.setGradientFill(bg);
        g.fillAll();

        auto bounds = getLocalBounds().reduced(8);
        auto topBar = bounds.removeFromTop(50).toFloat();
        g.setColour(juce::Colour::fromRGB(33, 38, 47));
        g.fillRoundedRectangle(topBar, 6.0f);
        g.setColour(juce::Colour::fromRGB(56, 64, 77).withAlpha(0.72f));
        g.drawRoundedRectangle(topBar.reduced(0.5f), 6.0f, 1.0f);
        g.setColour(juce::Colour::fromRGB(95, 108, 128).withAlpha(0.18f));
        g.drawLine(topBar.getX() + 8.0f, topBar.getY() + 1.0f, topBar.getRight() - 8.0f, topBar.getY() + 1.0f, 1.0f);

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

        auto editorPanel = (codeEditor != nullptr ? codeEditor->getBounds() : juce::Rectangle<int>{}).expanded(2).toFloat();
        g.setColour(juce::Colour::fromRGB(30, 35, 43).withAlpha(0.85f));
        g.fillRoundedRectangle(editorPanel, 6.0f);
        g.setColour(juce::Colour::fromRGB(60, 70, 86).withAlpha(0.7f));
        g.drawRoundedRectangle(editorPanel, 6.0f, 1.0f);

        if (showTimeline && ! timelineBounds.isEmpty())
        {
            auto tl = timelineBounds.expanded(2).toFloat();
            g.setColour(juce::Colour::fromRGB(24, 30, 38).withAlpha(0.9f));
            g.fillRoundedRectangle(tl, 6.0f);
            g.setColour(juce::Colour::fromRGB(56, 70, 88).withAlpha(0.7f));
            g.drawRoundedRectangle(tl, 6.0f, 1.0f);
            drawTimelineGrid(g, timelineBounds.reduced(6));
        }

        g.setColour(juce::Colour::fromRGB(82, 94, 112).withAlpha(0.8f));
        if (showTimeline && ! timelineSplitterBounds.isEmpty())
            g.fillRoundedRectangle(timelineSplitterBounds.toFloat().reduced(14.0f, 2.0f), 3.0f);
        g.fillRoundedRectangle(splitterBounds.toFloat().reduced(14.0f, 2.0f), 3.0f);

        auto logPanel = (showingOscMonitor ? oscMonitor.getBounds() : outputLog.getBounds()).expanded(2).toFloat();
        g.setColour(juce::Colour::fromRGB(26, 31, 38).withAlpha(0.82f));
        g.fillRoundedRectangle(logPanel, 6.0f);
        g.setColour(juce::Colour::fromRGB(58, 68, 84).withAlpha(0.68f));
        g.drawRoundedRectangle(logPanel, 6.0f, 1.0f);
    }

private:
    enum class EventType
    {
        trigger,
        note,
        randNote,
        choiceNote,
        markovNote,
        runScript,
        mod,
        tempo,
        transport,
        drill
    };

    enum class NoteGenerationMode
    {
        fixed,
        randomRange,
        weightedChoice,
        markovWalk
    };

    enum class MutationType
    {
        scramble,
        rotate,
        stutter
    };

    struct StepEvent
    {
        EventType type = EventType::trigger;
        int step = 0;
        int lane = 0;
        int sourceLine = -1;
        juce::String text;
        int midiNote = 60;
        NoteGenerationMode noteMode = NoteGenerationMode::fixed;
        int noteMin = 0;
        int noteMax = 127;
        int markovStartNote = 60;
        float markovPDown = 0.33f;
        float markovPStay = 0.34f;
        float markovPUp = 0.33f;
        std::vector<int> weightedNotes;
        std::vector<float> weightedWeights;
        float velocity = 1.0f;
        float durationBeats = -1.0f;
        float value = 0.0f;
        float rampBeats = 0.0f;
        float chance = 1.0f;
        int repeats = 1;
        int scriptId = 0;
        int everyN = 0;
        int notEveryN = 0;
        int fromCycle = 0;
        int toCycle = 0;
        juce::String sectionName;
    };

    struct MutationRule
    {
        MutationType type = MutationType::scramble;
        int lane = 0;
        int amount = 1;
        int repeats = 2;
        juce::String eventId;
        float chance = 1.0f;
        int everyN = 1;
        int notEveryN = 0;
    };

    struct PendingEvent
    {
        double dueMs = 0.0;
        StepEvent event;
    };

    struct LaneRuntimeState
    {
        int lastCycle = -1;
        int rotateOffset = 0;
        std::vector<int> scrambleMap;
        std::unordered_map<std::string, int> stutterRepeats;
        std::unordered_map<std::string, int> markovCurrentNotes;
    };

    struct TeletypeMetro
    {
        bool running = false;
        int periodSteps = 4;
        int phase = 0;
        bool pulse = false;
        long long tickCount = 0;
    };

    struct Program
    {
        struct SectionDef
        {
            juce::String name;
            int cycles = 0;
            int startCycle = 0;
            int endCycle = 0;
        };

        double bpm = 120.0;
        int steps = 16;
        double swing = 0.0;
        bool velocityModeMidi = false;
        float defaultVelocityGlobal = 1.0f;
        std::unordered_map<int, float> defaultVelocityByLane;
        juce::Array<StepEvent> events;
        std::unordered_map<int, int> laneSteps;
        std::vector<MutationRule> mutations;
        std::vector<SectionDef> sections;
        std::unordered_map<std::string, std::pair<int, int>> sectionCycleRanges;
        std::unordered_map<int, juce::String> teletypeScripts;
        int barLoopStart = 0;
        int barLoopEnd = 0;
    };

    void drawTimelineGrid(juce::Graphics& g, juce::Rectangle<int> area)
    {
        if (area.isEmpty())
            return;

        Program snapshot;
        bool isRunning = false;
        double snapshotStartMs = 0.0;
        {
            const juce::ScopedLock lock(stateLock);
            snapshot = program;
            isRunning = running.load();
            snapshotStartMs = startMs;
        }

        auto getLaneStepsLocal = [&] (int lane) -> int
        {
            auto it = snapshot.laneSteps.find(lane);
            if (it != snapshot.laneSteps.end())
                return juce::jmax(1, it->second);
            return juce::jmax(1, snapshot.steps);
        };

        auto mapToPlaybackStepLocal = [&] (long long absoluteStepCounter) -> long long
        {
            if (snapshot.barLoopStart <= 0 || snapshot.barLoopEnd <= 0 || snapshot.barLoopEnd <= snapshot.barLoopStart)
                return absoluteStepCounter;

            const auto loopStartStep = static_cast<long long> (snapshot.barLoopStart - 1) * static_cast<long long> (snapshot.steps);
            const auto loopEndStepInclusive = static_cast<long long> (snapshot.barLoopEnd) * static_cast<long long> (snapshot.steps) - 1;
            const auto loopLen = loopEndStepInclusive - loopStartStep + 1;
            if (loopLen <= 0)
                return absoluteStepCounter;
            if (absoluteStepCounter <= loopEndStepInclusive)
                return absoluteStepCounter;

            const auto afterLoop = absoluteStepCounter - (loopEndStepInclusive + 1);
            return loopStartStep + (afterLoop % loopLen);
        };

        auto cyclePassesLocal = [&] (long long cycleIndex, const StepEvent& e) -> bool
        {
            const auto oneBased = cycleIndex + 1;
            if (e.sectionName.isNotEmpty())
            {
                auto it = snapshot.sectionCycleRanges.find(e.sectionName.toLowerCase().toStdString());
                if (it == snapshot.sectionCycleRanges.end())
                    return false;
                if (oneBased < it->second.first || oneBased > it->second.second)
                    return false;
            }
            if (e.fromCycle > 0 && oneBased < e.fromCycle)
                return false;
            if (e.toCycle > 0 && oneBased > e.toCycle)
                return false;
            if (e.everyN > 0 && (oneBased % e.everyN) != 0)
                return false;
            if (e.notEveryN > 0 && (oneBased % e.notEveryN) == 0)
                return false;
            return true;
        };

        auto eventGlyph = [] (EventType t) -> juce::String
        {
            if (t == EventType::note || t == EventType::randNote || t == EventType::choiceNote || t == EventType::markovNote)
                return "N";
            if (t == EventType::drill)
                return "D";
            if (t == EventType::mod)
                return "M";
            return "T";
        };

        enum ScriptLaneFlags
        {
            flagTrigger = 1,
            flagNote = 2,
            flagMod = 4
        };

        std::unordered_map<int, std::unordered_map<int, int>> scriptLaneFlagsById;
        std::function<void(int, std::unordered_map<int, int>&, std::unordered_set<int>&, int)> collectFromScriptId;
        std::function<void(const juce::String&, std::unordered_map<int, int>&, std::unordered_set<int>&, int)> collectFromCommand;

        collectFromCommand = [&] (const juce::String& command,
                                  std::unordered_map<int, int>& outFlags,
                                  std::unordered_set<int>& stack,
                                  int depth)
        {
            if (depth > 10)
                return;

            if (command.containsChar(';'))
            {
                juce::StringArray cmds;
                cmds.addTokens(command, ";", "");
                cmds.trim();
                cmds.removeEmptyStrings();
                for (const auto& c : cmds)
                    collectFromCommand(c.trim(), outFlags, stack, depth + 1);
                return;
            }

            juce::StringArray t;
            t.addTokens(command, " \t", "");
            t.trim();
            t.removeEmptyStrings();
            if (t.isEmpty())
                return;

            const auto op = t[0].toLowerCase();

            if ((op == "tr" || op == "nt" || op == "md") && t.size() >= 2)
            {
                int lane = 0;
                if (parseInt(t[1], lane))
                {
                    if (op == "tr") outFlags[lane] |= flagTrigger;
                    else if (op == "nt") outFlags[lane] |= flagNote;
                    else if (op == "md") outFlags[lane] |= flagMod;
                }
                return;
            }

            if (op == "run" && t.size() >= 2)
            {
                int id = 0;
                if (parseInt(t[1], id))
                    collectFromScriptId(id, outFlags, stack, depth + 1);
                return;
            }

            if (op == "if")
            {
                int thenIndex = -1;
                int elseIndex = -1;
                for (int i = 0; i < t.size(); ++i)
                {
                    const auto tk = t[i].toLowerCase();
                    if (tk == "then" && thenIndex < 0)
                        thenIndex = i;
                    else if (tk == "else")
                        elseIndex = i;
                }
                if (thenIndex > 0)
                {
                    juce::StringArray thenPart;
                    const auto end = (elseIndex > thenIndex ? elseIndex : t.size());
                    for (int i = thenIndex + 1; i < end; ++i)
                        thenPart.add(t[i]);
                    collectFromCommand(thenPart.joinIntoString(" "), outFlags, stack, depth + 1);
                }
                if (elseIndex > 0 && elseIndex + 1 < t.size())
                {
                    juce::StringArray elsePart;
                    for (int i = elseIndex + 1; i < t.size(); ++i)
                        elsePart.add(t[i]);
                    collectFromCommand(elsePart.joinIntoString(" "), outFlags, stack, depth + 1);
                }
                return;
            }

            if (op == "repeat")
            {
                int thenIndex = -1;
                for (int i = 0; i < t.size(); ++i)
                    if (t[i].toLowerCase() == "then") { thenIndex = i; break; }
                if (thenIndex > 0 && thenIndex + 1 < t.size())
                {
                    juce::StringArray body;
                    for (int i = thenIndex + 1; i < t.size(); ++i)
                        body.add(t[i]);
                    collectFromCommand(body.joinIntoString(" "), outFlags, stack, depth + 1);
                }
                return;
            }

            if (op == "for" || op == "while")
            {
                int thenIndex = -1;
                for (int i = 0; i < t.size(); ++i)
                    if (t[i].toLowerCase() == "then") { thenIndex = i; break; }
                if (thenIndex > 0 && thenIndex + 1 < t.size())
                {
                    juce::StringArray body;
                    for (int i = thenIndex + 1; i < t.size(); ++i)
                        body.add(t[i]);
                    collectFromCommand(body.joinIntoString(" "), outFlags, stack, depth + 1);
                }
                return;
            }
        };

        collectFromScriptId = [&] (int scriptId,
                                   std::unordered_map<int, int>& outFlags,
                                   std::unordered_set<int>& stack,
                                   int depth)
        {
            if (depth > 10)
                return;
            if (stack.count(scriptId) > 0)
                return;

            auto cacheIt = scriptLaneFlagsById.find(scriptId);
            if (cacheIt == scriptLaneFlagsById.end())
            {
                std::unordered_map<int, int> localFlags;
                auto it = snapshot.teletypeScripts.find(scriptId);
                if (it != snapshot.teletypeScripts.end())
                {
                    stack.insert(scriptId);
                    collectFromCommand(it->second, localFlags, stack, depth + 1);
                    stack.erase(scriptId);
                }
                cacheIt = scriptLaneFlagsById.emplace(scriptId, std::move(localFlags)).first;
            }

            for (const auto& kv : cacheIt->second)
                outFlags[kv.first] |= kv.second;
        };

        long long runningStep = 0;
        if (isRunning)
        {
            const auto stepDurMs = (4.0 / static_cast<double> (juce::jmax(1, snapshot.steps))) * (60.0 / juce::jmax(20.0, snapshot.bpm)) * 1000.0;
            const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - snapshotStartMs;
            runningStep = static_cast<long long> (std::floor(elapsedMs / stepDurMs));
        }
        const auto playbackStep = mapToPlaybackStepLocal(runningStep);
        const auto globalBar = static_cast<int> (playbackStep / static_cast<long long> (juce::jmax(1, snapshot.steps))) + 1;
        const auto globalStepCount = juce::jmax(1, snapshot.steps);
        const auto globalCycle = playbackStep / static_cast<long long> (globalStepCount);
        std::unordered_map<int, std::unordered_map<int, int>> ttLaneFlagsAtGlobalStep;
        for (const auto& e : snapshot.events)
        {
            if (e.type != EventType::runScript)
                continue;
            if (e.step < 0 || e.step >= globalStepCount)
                continue;
            if (! cyclePassesLocal(globalCycle, e))
                continue;

            std::unordered_set<int> stack;
            std::unordered_map<int, int> localFlags;
            collectFromScriptId(e.scriptId, localFlags, stack, 0);
            auto& stepMap = ttLaneFlagsAtGlobalStep[e.step];
            for (const auto& kv : localFlags)
                stepMap[kv.first] |= kv.second;
        }

        std::vector<int> lanes;
        std::unordered_set<int> laneSet;
        for (const auto& e : snapshot.events)
        {
            if (e.type == EventType::transport || e.type == EventType::runScript || e.type == EventType::tempo)
                continue;
            if (laneSet.insert(e.lane).second)
                lanes.push_back(e.lane);
        }
        for (const auto& stepKv : ttLaneFlagsAtGlobalStep)
            for (const auto& laneKv : stepKv.second)
                if (laneSet.insert(laneKv.first).second)
                    lanes.push_back(laneKv.first);
        if (lanes.empty())
            lanes = { 0, 1, 2, 3, 4 };
        std::sort(lanes.begin(), lanes.end());
        if (lanes.size() > 10)
            lanes.resize(10);

        auto header = area.removeFromTop(16);
        g.setColour(juce::Colour::fromRGB(176, 188, 208));
        g.setFont(juce::Font(juce::FontOptions("Menlo", 11.0f, juce::Font::bold)));
        g.drawText("Timeline (filtered, deterministic)  Bar " + juce::String(globalBar), header, juce::Justification::centredLeft);

        const auto laneCount = juce::jmax(1, static_cast<int> (lanes.size()));
        const auto rowGap = 2;
        const auto rowHeight = juce::jmax(11, (area.getHeight() - (laneCount - 1) * rowGap) / laneCount);
        const auto laneLabelWidth = 30;

        for (int row = 0; row < laneCount; ++row)
        {
            auto rowArea = area.removeFromTop(rowHeight);
            if (row < laneCount - 1)
                area.removeFromTop(rowGap);

            const auto lane = lanes[static_cast<size_t> (row)];
            auto labelArea = rowArea.removeFromLeft(laneLabelWidth);
            g.setColour(juce::Colour::fromRGB(152, 168, 192));
            g.setFont(juce::Font(juce::FontOptions("Menlo", 10.0f, juce::Font::bold)));
            g.drawText("L" + juce::String(lane), labelArea, juce::Justification::centredLeft);

            const auto laneSteps = juce::jmax(1, getLaneStepsLocal(lane));
            const auto cellW = juce::jmax(4, rowArea.getWidth() / laneSteps);
            const auto laneCycle = playbackStep / static_cast<long long> (laneSteps);
            auto currentLaneStep = static_cast<int> (playbackStep % static_cast<long long> (laneSteps));
            if (currentLaneStep < 0)
                currentLaneStep += laneSteps;

            for (int s = 0; s < laneSteps; ++s)
            {
                auto cell = juce::Rectangle<int>(rowArea.getX() + s * cellW, rowArea.getY(), cellW - 1, rowArea.getHeight());
                if (cell.getRight() > rowArea.getRight())
                    cell.setRight(rowArea.getRight());
                if (cell.getWidth() <= 1)
                    continue;

                bool willFire = false;
                bool probabilistic = false;
                bool hasTriggerLike = false;
                bool hasNoteLike = false;
                bool hasModLike = false;
                juce::String glyph = ".";

                for (const auto& e : snapshot.events)
                {
                    if (e.lane != lane)
                        continue;
                    if (e.type == EventType::transport || e.type == EventType::runScript || e.type == EventType::tempo)
                        continue;
                    if (e.step != s)
                        continue;
                    if (! cyclePassesLocal(laneCycle, e))
                        continue;

                    willFire = true;
                    probabilistic = probabilistic || (e.chance < 0.999f);
                    glyph = eventGlyph(e.type);
                    hasTriggerLike = hasTriggerLike || (e.type == EventType::trigger || e.type == EventType::drill);
                    hasNoteLike = hasNoteLike || (e.type == EventType::note || e.type == EventType::randNote || e.type == EventType::choiceNote || e.type == EventType::markovNote);
                    hasModLike = hasModLike || (e.type == EventType::mod);
                }

                const auto globalStepForCell = (s % globalStepCount);
                auto ttStepIt = ttLaneFlagsAtGlobalStep.find(globalStepForCell);
                if (ttStepIt != ttLaneFlagsAtGlobalStep.end())
                {
                    auto laneIt = ttStepIt->second.find(lane);
                    if (laneIt != ttStepIt->second.end())
                    {
                        willFire = true;
                        hasTriggerLike = hasTriggerLike || ((laneIt->second & flagTrigger) != 0);
                        hasNoteLike = hasNoteLike || ((laneIt->second & flagNote) != 0);
                        hasModLike = hasModLike || ((laneIt->second & flagMod) != 0);
                        glyph = hasNoteLike ? "N" : (hasModLike ? "M" : "T");
                    }
                }

                auto colour = juce::Colour::fromRGB(34, 40, 52);
                if (willFire)
                {
                    // Blue = trigger/drill, Green = note, Yellow = modulation, mixed -> desaturated cyan.
                    if (hasModLike && ! hasNoteLike && ! hasTriggerLike)
                        colour = juce::Colour::fromRGB(205, 173, 95);
                    else if (hasNoteLike && ! hasModLike && ! hasTriggerLike)
                        colour = juce::Colour::fromRGB(94, 204, 117);
                    else if (hasTriggerLike && ! hasNoteLike && ! hasModLike)
                        colour = juce::Colour::fromRGB(89, 153, 216);
                    else
                        colour = juce::Colour::fromRGB(125, 175, 178);
                }
                if (probabilistic)
                    colour = colour.withMultipliedAlpha(0.62f);

                g.setColour(colour);
                g.fillRoundedRectangle(cell.toFloat(), 1.5f);

                if (s == currentLaneStep)
                {
                    g.setColour(juce::Colour::fromRGB(223, 196, 126).withAlpha(0.9f));
                    g.drawRoundedRectangle(cell.toFloat().reduced(0.5f), 1.5f, 1.0f);
                }

                if (cell.getWidth() >= 10)
                {
                    g.setColour(willFire ? juce::Colour::fromRGB(13, 19, 28) : juce::Colour::fromRGB(104, 118, 140));
                    g.setFont(juce::Font(juce::FontOptions("Menlo", 9.0f, juce::Font::plain)));
                    g.drawText(glyph, cell, juce::Justification::centred);
                }
            }
        }
    }

    void buttonClicked(juce::Button* b) override
    {
        if (b == &runButton)
        {
            Program parsed;
            juce::String parseError;
            if (! parseProgram(codeDocument.getAllContent(), parsed, parseError))
            {
                running.store(false);
                stopTimer();
                status.setText("Parse failed", juce::dontSendNotification);
                outputLog.insertTextAtCaret("Parse error: " + parseError + "\n");
                if (codeEditor != nullptr)
                    codeEditor->setErrorLine(extractLineNumber(parseError));
                return;
            }

            if (codeEditor != nullptr)
                codeEditor->setErrorLine(-1);
            program = std::move(parsed);
            running.store(true);
            {
                const juce::ScopedLock lock(stateLock);
                startMs = juce::Time::getMillisecondCounterHiRes();
                nextStepToSchedule = 0;
                clearPendingEvents();
                laneStates.clear();
                teletypeVars.clear();
                teletypeVars["x"] = 0.0f;
                teletypeVars["y"] = 0.0f;
                teletypeVars["z"] = 0.0f;
                teletypeVars["t"] = 0.0f;
                teletypePattern.fill(0.0f);
                teletypeMetros.clear();
                lastMetroStepProcessed = std::numeric_limits<long long>::min();
            }

            sendTempo(static_cast<float>(program.bpm));
            startTimer(1);
            status.setText("Running", juce::dontSendNotification);
            outputLog.insertTextAtCaret("Running: bpm=" + juce::String(program.bpm, 2)
                                        + " steps=" + juce::String(program.steps)
                                        + " swing=" + juce::String(program.swing, 3)
                                        + " events=" + juce::String(program.events.size())
                                        + " mutations=" + juce::String(static_cast<int>(program.mutations.size()))
                                        + "\n");
            return;
        }

        if (b == &stopButton)
            stopExecution();

        if (b == &loadButton)
        {
            loadScript();
            return;
        }

        if (b == &saveButton)
        {
            saveScript();
            return;
        }

        if (b == &logModeButton)
        {
            showingOscMonitor = ! showingOscMonitor;
            outputLog.setVisible(! showingOscMonitor);
            oscMonitor.setVisible(showingOscMonitor);
            logModeButton.setButtonText(showingOscMonitor ? "Mode: OSC" : "Mode: Log");
            repaint();
            return;
        }

        if (b == &timelineToggleButton)
        {
            showTimeline = ! showTimeline;
            timelineToggleButton.setButtonText(showTimeline ? "Tl:On" : "Tl:Off");
            resized();
            repaint();
            return;
        }
    }

    void loadScript()
    {
        auto initial = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        if (lastLoadedScriptFile.existsAsFile())
            initial = lastLoadedScriptFile;

        loadChooser = std::make_unique<juce::FileChooser>("Load Script",
                                                          initial,
                                                          "*.split;*.txt");
        loadChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                 [this] (const juce::FileChooser& chooser)
                                 {
                                     const auto file = chooser.getResult();
                                     loadChooser.reset();
                                     if (! file.existsAsFile())
                                         return;

                                     const auto contents = file.loadFileAsString();
                                     if (contents.isEmpty())
                                     {
                                         status.setText("Load failed", juce::dontSendNotification);
                                         outputLog.insertTextAtCaret("Load failed: " + file.getFullPathName() + "\n");
                                         return;
                                     }

                                     codeDocument.replaceAllContent(contents);
                                     lastLoadedScriptFile = file;
                                     if (codeEditor != nullptr)
                                         codeEditor->setErrorLine(-1);
                                     status.setText("Loaded " + file.getFileName(), juce::dontSendNotification);
                                     outputLog.insertTextAtCaret("Loaded script: " + file.getFullPathName() + "\n");
                                 });
    }

    void saveScript()
    {
        saveChooser = std::make_unique<juce::FileChooser>("Save Script",
                                                          juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("pattern.split"),
                                                          "*.split;*.txt");
        saveChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                                 [this] (const juce::FileChooser& chooser)
                                 {
                                     auto file = chooser.getResult();
                                     saveChooser.reset();
                                     if (file == juce::File())
                                         return;

                                     if (! file.hasFileExtension("split") && ! file.hasFileExtension("txt"))
                                         file = file.withFileExtension("split");

                                     if (! file.replaceWithText(codeDocument.getAllContent()))
                                     {
                                         status.setText("Save failed", juce::dontSendNotification);
                                         outputLog.insertTextAtCaret("Save failed: " + file.getFullPathName() + "\n");
                                         return;
                                     }

                                     status.setText("Saved " + file.getFileName(), juce::dontSendNotification);
                                     outputLog.insertTextAtCaret("Saved script: " + file.getFullPathName() + "\n");
                                 });
    }

    void hiResTimerCallback() override
    {
        if (! running.load())
            return;

        const auto nowMs = juce::Time::getMillisecondCounterHiRes();
        const auto stepDurMs = (4.0 / static_cast<double> (program.steps)) * (60.0 / program.bpm) * 1000.0;
        const auto lookAheadMs = 35.0;
        const auto elapsedMs = nowMs - startMs;
        const auto targetStep = static_cast<long long> (std::floor((elapsedMs + lookAheadMs) / stepDurMs));
        const auto runningStep = static_cast<long long> (std::floor(elapsedMs / stepDurMs));
        const auto playbackStep = mapToPlaybackStep(runningStep);
        const auto stepNow = getGlobalStep(playbackStep);
        if ((nowMs - lastUiUpdateMs) >= 33.0)
        {
            uiStep.store(stepNow, std::memory_order_relaxed);
            uiBar.store(static_cast<int> (getGlobalCycle(playbackStep)) + 1, std::memory_order_relaxed);
            uiBeat.store(1 + ((stepNow * 4) / juce::jmax(1, program.steps)), std::memory_order_relaxed);
            uiMeter.store(juce::jlimit(0.0, 1.0, static_cast<double> (stepNow) / static_cast<double> (juce::jmax(1, program.steps))),
                          std::memory_order_relaxed);
            lastUiUpdateMs = nowMs;
            triggerAsyncUpdate();
        }

        {
            const juce::ScopedLock lock(stateLock);
            while (nextStepToSchedule <= targetStep)
            {
                const auto stepTimeMs = startMs + (static_cast<double> (nextStepToSchedule) * stepDurMs);
                dispatchStepCounter(nextStepToSchedule, stepTimeMs);
                ++nextStepToSchedule;
            }
        }

        flushPendingEvents(nowMs);
    }

    void stopExecution()
    {
        if (running.load())
            sendTransport("stop");

        running.store(false);
        {
            const juce::ScopedLock lock(stateLock);
            clearPendingEvents();
            laneStates.clear();
            teletypeMetros.clear();
        }
        stopTimer();
        status.setText("Stopped", juce::dontSendNotification);
        outputLog.insertTextAtCaret("Stopped\n");
        uiStep.store(0, std::memory_order_relaxed);
        uiBar.store(1, std::memory_order_relaxed);
        uiBeat.store(1, std::memory_order_relaxed);
        uiMeter.store(0.0, std::memory_order_relaxed);
        uiActiveLine.store(-1, std::memory_order_relaxed);
        triggerAsyncUpdate();
    }

    int getLaneSteps(int lane) const
    {
        auto it = program.laneSteps.find(lane);
        if (it != program.laneSteps.end())
            return it->second;
        return program.steps;
    }

    int getGlobalStep(long long absoluteStepCounter) const
    {
        int step = static_cast<int> (absoluteStepCounter % program.steps);
        if (step < 0)
            step += program.steps;
        return step;
    }

    long long getGlobalCycle(long long absoluteStepCounter) const
    {
        return absoluteStepCounter / static_cast<long long> (program.steps);
    }

    long long mapToPlaybackStep(long long absoluteStepCounter) const
    {
        if (program.barLoopStart <= 0 || program.barLoopEnd <= 0 || program.barLoopEnd <= program.barLoopStart)
            return absoluteStepCounter;

        const auto loopStartStep = static_cast<long long> (program.barLoopStart - 1) * static_cast<long long> (program.steps);
        const auto loopEndStepInclusive = static_cast<long long> (program.barLoopEnd) * static_cast<long long> (program.steps) - 1;
        const auto loopLen = loopEndStepInclusive - loopStartStep + 1;
        if (loopLen <= 0)
            return absoluteStepCounter;

        if (absoluteStepCounter <= loopEndStepInclusive)
            return absoluteStepCounter;

        const auto afterLoop = absoluteStepCounter - (loopEndStepInclusive + 1);
        return loopStartStep + (afterLoop % loopLen);
    }

    int getLaneStep(long long absoluteStepCounter, int lane) const
    {
        const auto laneStepCount = getLaneSteps(lane);
        int step = static_cast<int> (absoluteStepCounter % laneStepCount);
        if (step < 0)
            step += laneStepCount;
        return step;
    }

    long long getLaneCycle(long long absoluteStepCounter, int lane) const
    {
        const auto laneStepCount = getLaneSteps(lane);
        return absoluteStepCounter / static_cast<long long> (laneStepCount);
    }

    double getStepDurationMs(int lane) const
    {
        const auto laneStepCount = getLaneSteps(lane);
        return (4.0 / static_cast<double> (laneStepCount)) * (60.0 / program.bpm) * 1000.0;
    }

    bool cyclePassesConditions(long long cycleIndex,
                               int everyN,
                               int notEveryN,
                               int fromCycle,
                               int toCycle,
                               const juce::String& sectionName) const
    {
        const auto oneBased = cycleIndex + 1;
        if (sectionName.isNotEmpty())
        {
            const auto key = sectionName.toLowerCase().toStdString();
            auto it = program.sectionCycleRanges.find(key);
            if (it == program.sectionCycleRanges.end())
                return false;
            if (oneBased < it->second.first || oneBased > it->second.second)
                return false;
        }
        if (fromCycle > 0 && oneBased < fromCycle)
            return false;
        if (toCycle > 0 && oneBased > toCycle)
            return false;
        if (everyN > 0 && (oneBased % everyN) != 0)
            return false;
        if (notEveryN > 0 && (oneBased % notEveryN) == 0)
            return false;
        return true;
    }

    bool shouldFire(float chance)
    {
        if (chance >= 1.0f)
            return true;
        if (chance <= 0.0f)
            return false;
        return random01(rng) <= chance;
    }

    float convertVelocityInput(float value) const
    {
        if (program.velocityModeMidi)
            return juce::jlimit(0.0f, 127.0f, value) / 127.0f;
        return juce::jlimit(0.0f, 1.0f, value);
    }

    float defaultVelocityForLaneRuntime(int lane) const
    {
        auto it = program.defaultVelocityByLane.find(lane);
        if (it != program.defaultVelocityByLane.end())
            return it->second;
        return program.defaultVelocityGlobal;
    }

    int parsePatternIndexToken(const juce::String& token) const
    {
        auto s = token.trim().toLowerCase();
        if (s.startsWith("p[") && s.endsWithChar(']'))
            s = s.substring(2, s.length() - 1);
        else if (s.startsWithChar('p') && s.length() > 1)
            s = s.substring(1);
        else
            return -1;

        int idx = -1;
        if (! parseInt(s, idx))
            return -1;
        return juce::isPositiveAndBelow(idx, static_cast<int> (teletypePattern.size())) ? idx : -1;
    }

    TeletypeMetro& getMetro(int id)
    {
        return teletypeMetros[id];
    }

    bool evalComparison(float lhs, const juce::String& cmp, float rhs) const
    {
        if (cmp == "==" || cmp == "=") return std::abs(lhs - rhs) <= 0.0001f;
        if (cmp == "!=") return std::abs(lhs - rhs) > 0.0001f;
        if (cmp == ">") return lhs > rhs;
        if (cmp == "<") return lhs < rhs;
        if (cmp == ">=") return lhs >= rhs;
        if (cmp == "<=") return lhs <= rhs;
        return false;
    }

    void advanceTeletypeMetros(long long absoluteStepCounter)
    {
        if (absoluteStepCounter == lastMetroStepProcessed)
            return;

        lastMetroStepProcessed = absoluteStepCounter;
        for (auto& kv : teletypeMetros)
        {
            auto& m = kv.second;
            m.pulse = false;
            if (! m.running)
                continue;

            const auto period = juce::jmax(1, m.periodSteps);
            const auto rel = absoluteStepCounter - static_cast<long long> (m.phase);
            if (rel >= 0 && (rel % period) == 0)
            {
                m.pulse = true;
                ++m.tickCount;
            }
        }
    }

    float resolveScalarToken(const juce::String& token) const
    {
        const auto key = token.trim().toLowerCase();
        if (key == "x" || key == "y" || key == "z" || key == "t")
        {
            auto it = teletypeVars.find(key.toStdString());
            if (it != teletypeVars.end())
                return it->second;
            return 0.0f;
        }

        const auto pIdx = parsePatternIndexToken(key);
        if (pIdx >= 0)
            return teletypePattern[static_cast<size_t> (pIdx)];

        if (key.startsWithChar('m') && key.length() > 1)
        {
            const auto idxToken = key.substring(1);
            int mId = 0;
            if (parseInt(idxToken, mId))
            {
                auto it = teletypeMetros.find(mId);
                if (it != teletypeMetros.end())
                    return it->second.pulse ? 1.0f : 0.0f;
            }
        }

        if (key.startsWith("mc") && key.length() > 2)
        {
            int mId = 0;
            if (parseInt(key.substring(2), mId))
            {
                auto it = teletypeMetros.find(mId);
                if (it != teletypeMetros.end())
                    return static_cast<float> (it->second.tickCount);
            }
        }

        return token.getFloatValue();
    }

    bool assignTeletypeVar(const juce::String& token, float value)
    {
        const auto key = token.trim().toLowerCase();
        if (key != "x" && key != "y" && key != "z" && key != "t")
            return false;
        teletypeVars[key.toStdString()] = value;
        return true;
    }

    bool executeTeletypeCommand(const juce::String& command, int depth = 0)
    {
        if (depth > 10)
            return false;

        if (command.containsChar(';'))
        {
            juce::StringArray cmds;
            cmds.addTokens(command, ";", "");
            cmds.trim();
            cmds.removeEmptyStrings();
            bool ok = true;
            for (const auto& c : cmds)
                ok = executeTeletypeCommand(c.trim(), depth + 1) && ok;
            return ok;
        }

        juce::StringArray t;
        t.addTokens(command, " \t", "");
        t.trim();
        t.removeEmptyStrings();
        if (t.isEmpty())
            return true;

        const auto op = t[0].toLowerCase();

        if (op == "set" && t.size() >= 3)
            return assignTeletypeVar(t[1], resolveScalarToken(t[2]));

        if (op == "add" && t.size() >= 3)
        {
            const auto cur = resolveScalarToken(t[1]);
            return assignTeletypeVar(t[1], cur + resolveScalarToken(t[2]));
        }

        if (op == "sub" && t.size() >= 3)
        {
            const auto cur = resolveScalarToken(t[1]);
            return assignTeletypeVar(t[1], cur - resolveScalarToken(t[2]));
        }

        if (op == "mul" && t.size() >= 3)
        {
            const auto cur = resolveScalarToken(t[1]);
            return assignTeletypeVar(t[1], cur * resolveScalarToken(t[2]));
        }

        if (op == "div" && t.size() >= 3)
        {
            const auto cur = resolveScalarToken(t[1]);
            const auto rhs = resolveScalarToken(t[2]);
            if (std::abs(rhs) <= 0.000001f)
                return false;
            return assignTeletypeVar(t[1], cur / rhs);
        }

        if (op == "mod" && t.size() >= 3)
        {
            const auto cur = resolveScalarToken(t[1]);
            const auto rhs = resolveScalarToken(t[2]);
            if (std::abs(rhs) <= 0.000001f)
                return false;
            return assignTeletypeVar(t[1], std::fmod(cur, rhs));
        }

        if (op == "pow" && t.size() >= 3)
        {
            const auto cur = resolveScalarToken(t[1]);
            return assignTeletypeVar(t[1], std::pow(cur, resolveScalarToken(t[2])));
        }

        if (op == "min" && t.size() >= 3)
        {
            const auto cur = resolveScalarToken(t[1]);
            return assignTeletypeVar(t[1], juce::jmin(cur, resolveScalarToken(t[2])));
        }

        if (op == "max" && t.size() >= 3)
        {
            const auto cur = resolveScalarToken(t[1]);
            return assignTeletypeVar(t[1], juce::jmax(cur, resolveScalarToken(t[2])));
        }

        if (op == "abs" && t.size() >= 2)
            return assignTeletypeVar(t[1], std::abs(resolveScalarToken(t[1])));

        if (op == "floor" && t.size() >= 2)
            return assignTeletypeVar(t[1], std::floor(resolveScalarToken(t[1])));

        if (op == "ceil" && t.size() >= 2)
            return assignTeletypeVar(t[1], std::ceil(resolveScalarToken(t[1])));

        if (op == "round" && t.size() >= 2)
            return assignTeletypeVar(t[1], std::round(resolveScalarToken(t[1])));

        if (op == "sin" && t.size() >= 2)
            return assignTeletypeVar(t[1], std::sin(resolveScalarToken(t[1])));

        if (op == "cos" && t.size() >= 2)
            return assignTeletypeVar(t[1], std::cos(resolveScalarToken(t[1])));

        if (op == "tan" && t.size() >= 2)
            return assignTeletypeVar(t[1], std::tan(resolveScalarToken(t[1])));

        if (op == "clamp" && t.size() >= 4)
        {
            const auto lo = resolveScalarToken(t[2]);
            const auto hi = resolveScalarToken(t[3]);
            return assignTeletypeVar(t[1], juce::jlimit(juce::jmin(lo, hi), juce::jmax(lo, hi), resolveScalarToken(t[1])));
        }

        if (op == "lerp" && t.size() >= 5)
        {
            const auto a = resolveScalarToken(t[2]);
            const auto b = resolveScalarToken(t[3]);
            const auto u = resolveScalarToken(t[4]);
            return assignTeletypeVar(t[1], a + (b - a) * u);
        }

        if (op == "rrand" && t.size() >= 4)
        {
            const auto lo = resolveScalarToken(t[2]);
            const auto hi = resolveScalarToken(t[3]);
            const auto minV = juce::jmin(lo, hi);
            const auto maxV = juce::jmax(lo, hi);
            std::uniform_real_distribution<float> dist(minV, maxV);
            return assignTeletypeVar(t[1], dist(rng));
        }

        if (op == "irand" && t.size() >= 4)
        {
            const auto lo = static_cast<int> (std::round(resolveScalarToken(t[2])));
            const auto hi = static_cast<int> (std::round(resolveScalarToken(t[3])));
            const auto minV = juce::jmin(lo, hi);
            const auto maxV = juce::jmax(lo, hi);
            std::uniform_int_distribution<int> dist(minV, maxV);
            return assignTeletypeVar(t[1], static_cast<float> (dist(rng)));
        }

        if (op == "and" && t.size() >= 3)
        {
            const auto lhs = resolveScalarToken(t[1]) != 0.0f;
            const auto rhs = resolveScalarToken(t[2]) != 0.0f;
            return assignTeletypeVar(t[1], (lhs && rhs) ? 1.0f : 0.0f);
        }

        if (op == "or" && t.size() >= 3)
        {
            const auto lhs = resolveScalarToken(t[1]) != 0.0f;
            const auto rhs = resolveScalarToken(t[2]) != 0.0f;
            return assignTeletypeVar(t[1], (lhs || rhs) ? 1.0f : 0.0f);
        }

        if (op == "xor" && t.size() >= 3)
        {
            const auto lhs = resolveScalarToken(t[1]) != 0.0f;
            const auto rhs = resolveScalarToken(t[2]) != 0.0f;
            return assignTeletypeVar(t[1], (lhs != rhs) ? 1.0f : 0.0f);
        }

        if (op == "not" && t.size() >= 2)
        {
            const auto cur = resolveScalarToken(t[1]) != 0.0f;
            return assignTeletypeVar(t[1], cur ? 0.0f : 1.0f);
        }

        if (op == "cmp" && t.size() >= 5)
            return assignTeletypeVar(t[1], evalComparison(resolveScalarToken(t[2]), t[3], resolveScalarToken(t[4])) ? 1.0f : 0.0f);

        if (op == "if" && t.size() >= 6)
        {
            const auto lhs = resolveScalarToken(t[1]);
            const auto cmp = t[2];
            const auto rhs = resolveScalarToken(t[3]);
            const auto thenTok = t[4].toLowerCase();
            if (thenTok != "then")
                return false;

            int elseIndex = -1;
            for (int i = 5; i < t.size(); ++i)
            {
                if (t[i].toLowerCase() == "else")
                {
                    elseIndex = i;
                    break;
                }
            }

            const auto ok = evalComparison(lhs, cmp, rhs);
            if (ok)
            {
                juce::StringArray nested;
                const auto end = (elseIndex >= 0 ? elseIndex : t.size());
                for (int i = 5; i < end; ++i)
                    nested.add(t[i]);
                return executeTeletypeCommand(nested.joinIntoString(" "), depth + 1);
            }

            if (elseIndex >= 0 && elseIndex + 1 < t.size())
            {
                juce::StringArray nested;
                for (int i = elseIndex + 1; i < t.size(); ++i)
                    nested.add(t[i]);
                return executeTeletypeCommand(nested.joinIntoString(" "), depth + 1);
            }

            return true;
        }

        if (op == "repeat" && t.size() >= 4)
        {
            if (t[2].toLowerCase() != "then")
                return false;
            const auto count = juce::jlimit(0, 256, static_cast<int> (std::round(resolveScalarToken(t[1]))));
            juce::StringArray nested;
            for (int i = 3; i < t.size(); ++i)
                nested.add(t[i]);
            const auto cmd = nested.joinIntoString(" ");
            bool ok = true;
            for (int i = 0; i < count; ++i)
                ok = executeTeletypeCommand(cmd, depth + 1) && ok;
            return ok;
        }

        if (op == "for" && t.size() >= 7)
        {
            if (t[5].toLowerCase() != "then")
                return false;
            const auto var = t[1];
            const auto start = static_cast<int> (std::round(resolveScalarToken(t[2])));
            const auto end = static_cast<int> (std::round(resolveScalarToken(t[3])));
            auto step = static_cast<int> (std::round(resolveScalarToken(t[4])));
            if (step == 0)
                step = (start <= end ? 1 : -1);

            juce::StringArray nested;
            for (int i = 6; i < t.size(); ++i)
                nested.add(t[i]);
            const auto cmd = nested.joinIntoString(" ");

            bool ok = true;
            int guard = 0;
            if (step > 0)
            {
                for (int i = start; i <= end && guard < 2048; i += step, ++guard)
                {
                    ok = assignTeletypeVar(var, static_cast<float> (i)) && ok;
                    ok = executeTeletypeCommand(cmd, depth + 1) && ok;
                }
            }
            else
            {
                for (int i = start; i >= end && guard < 2048; i += step, ++guard)
                {
                    ok = assignTeletypeVar(var, static_cast<float> (i)) && ok;
                    ok = executeTeletypeCommand(cmd, depth + 1) && ok;
                }
            }
            return ok;
        }

        if (op == "while" && t.size() >= 6)
        {
            if (t[4].toLowerCase() != "then")
                return false;

            juce::StringArray nested;
            for (int i = 5; i < t.size(); ++i)
                nested.add(t[i]);
            const auto cmd = nested.joinIntoString(" ");

            bool ok = true;
            int guard = 0;
            while (guard < 512)
            {
                const auto lhs = resolveScalarToken(t[1]);
                const auto rhs = resolveScalarToken(t[3]);
                if (! evalComparison(lhs, t[2], rhs))
                    break;
                ok = executeTeletypeCommand(cmd, depth + 1) && ok;
                ++guard;
            }
            return ok;
        }

        if (op == "pset" && t.size() >= 3)
        {
            const auto idx = parsePatternIndexToken(t[1].startsWithChar('p') ? t[1] : ("p" + t[1]));
            if (idx < 0)
                return false;
            teletypePattern[static_cast<size_t> (idx)] = resolveScalarToken(t[2]);
            return true;
        }

        if (op == "padd" && t.size() >= 3)
        {
            const auto idx = parsePatternIndexToken(t[1].startsWithChar('p') ? t[1] : ("p" + t[1]));
            if (idx < 0)
                return false;
            teletypePattern[static_cast<size_t> (idx)] += resolveScalarToken(t[2]);
            return true;
        }

        if (op == "pget" && t.size() >= 3)
        {
            const auto idx = parsePatternIndexToken(t[2].startsWithChar('p') ? t[2] : ("p" + t[2]));
            if (idx < 0)
                return false;
            return assignTeletypeVar(t[1], teletypePattern[static_cast<size_t> (idx)]);
        }

        if (op == "pswap" && t.size() >= 3)
        {
            const auto a = parsePatternIndexToken(t[1].startsWithChar('p') ? t[1] : ("p" + t[1]));
            const auto b = parsePatternIndexToken(t[2].startsWithChar('p') ? t[2] : ("p" + t[2]));
            if (a < 0 || b < 0)
                return false;
            std::swap(teletypePattern[static_cast<size_t> (a)], teletypePattern[static_cast<size_t> (b)]);
            return true;
        }

        if (op == "prot" && t.size() >= 2)
        {
            const auto n = static_cast<int> (std::round(resolveScalarToken(t[1])));
            if (! teletypePattern.empty())
            {
                const auto sz = static_cast<int> (teletypePattern.size());
                auto r = n % sz;
                if (r < 0)
                    r += sz;
                if (r != 0)
                    std::rotate(teletypePattern.rbegin(), teletypePattern.rbegin() + r, teletypePattern.rend());
            }
            return true;
        }

        if (op == "pclr")
        {
            if (t.size() == 1)
            {
                teletypePattern.fill(0.0f);
                return true;
            }
            const auto idx = parsePatternIndexToken(t[1].startsWithChar('p') ? t[1] : ("p" + t[1]));
            if (idx < 0)
                return false;
            teletypePattern[static_cast<size_t> (idx)] = 0.0f;
            return true;
        }

        if (op == "pfill" && t.size() >= 2)
        {
            teletypePattern.fill(resolveScalarToken(t[1]));
            return true;
        }

        if (op == "prand" && t.size() >= 4)
        {
            const auto idx = parsePatternIndexToken(t[1].startsWithChar('p') ? t[1] : ("p" + t[1]));
            if (idx < 0)
                return false;
            const auto lo = resolveScalarToken(t[2]);
            const auto hi = resolveScalarToken(t[3]);
            std::uniform_real_distribution<float> dist(juce::jmin(lo, hi), juce::jmax(lo, hi));
            teletypePattern[static_cast<size_t> (idx)] = dist(rng);
            return true;
        }

        if ((op == "clock" || op == "clk") && t.size() >= 2)
        {
            program.bpm = juce::jlimit(20.0, 300.0, static_cast<double> (resolveScalarToken(t[1])));
            sendTempo(static_cast<float> (program.bpm));
            return true;
        }

        if ((op == "metro" || op == "mtr") && t.size() >= 3)
        {
            const auto id = static_cast<int> (std::round(resolveScalarToken(t[1])));
            auto& m = getMetro(id);
            m.periodSteps = juce::jmax(1, static_cast<int> (std::round(resolveScalarToken(t[2]))));
            m.running = true;
            m.pulse = false;
            return true;
        }

        if (op == "metro.start" && t.size() >= 2)
        {
            auto& m = getMetro(static_cast<int> (std::round(resolveScalarToken(t[1]))));
            m.running = true;
            return true;
        }

        if (op == "metro.stop" && t.size() >= 2)
        {
            auto& m = getMetro(static_cast<int> (std::round(resolveScalarToken(t[1]))));
            m.running = false;
            m.pulse = false;
            return true;
        }

        if (op == "metro.reset" && t.size() >= 2)
        {
            auto& m = getMetro(static_cast<int> (std::round(resolveScalarToken(t[1]))));
            m.tickCount = 0;
            m.pulse = false;
            return true;
        }

        if (op == "metro.period" && t.size() >= 3)
        {
            auto& m = getMetro(static_cast<int> (std::round(resolveScalarToken(t[1]))));
            m.periodSteps = juce::jmax(1, static_cast<int> (std::round(resolveScalarToken(t[2]))));
            return true;
        }

        if (op == "metro.phase" && t.size() >= 3)
        {
            auto& m = getMetro(static_cast<int> (std::round(resolveScalarToken(t[1]))));
            m.phase = static_cast<int> (std::round(resolveScalarToken(t[2])));
            return true;
        }

        if (op == "run" && t.size() >= 2)
        {
            const auto id = static_cast<int> (resolveScalarToken(t[1]));
            auto it = program.teletypeScripts.find(id);
            if (it == program.teletypeScripts.end())
                return false;
            return executeTeletypeCommand(it->second, depth + 1);
        }

        if (op == "tr" && t.size() >= 3)
        {
            const auto lane = static_cast<int> (resolveScalarToken(t[1]));
            auto velocity = defaultVelocityForLaneRuntime(lane);
            if (t.size() >= 4)
                velocity = convertVelocityInput(resolveScalarToken(t[3]));
            sendTrigger(lane, t[2], velocity);
            return true;
        }

        if (op == "nt" && t.size() >= 3)
        {
            const auto lane = static_cast<int> (resolveScalarToken(t[1]));
            const auto midi = juce::jlimit(0, 127, static_cast<int> (resolveScalarToken(t[2])));
            auto velocity = defaultVelocityForLaneRuntime(lane);
            if (t.size() >= 4)
                velocity = convertVelocityInput(resolveScalarToken(t[3]));
            float durationSec = -1.0f;
            if (t.size() >= 5)
                durationSec = resolveScalarToken(t[4]) * static_cast<float> (60.0 / program.bpm);
            juce::String eventId = "tt";
            if (t.size() >= 6)
                eventId = t[5];
            sendNote(lane, midi, velocity, durationSec, eventId);
            return true;
        }

        if (op == "md" && t.size() >= 4)
        {
            const auto lane = static_cast<int> (resolveScalarToken(t[1]));
            const auto target = t[2];
            const auto value = resolveScalarToken(t[3]);
            float rampSec = 0.0f;
            if (t.size() >= 5)
                rampSec = resolveScalarToken(t[4]) * static_cast<float> (60.0 / program.bpm);
            sendMod(lane, target, value, rampSec);
            return true;
        }

        if (op == "tp" && t.size() >= 2)
        {
            sendTransport(t[1]);
            return true;
        }

        return false;
    }

    int resolveGeneratedNote(const StepEvent& event)
    {
        switch (event.noteMode)
        {
            case NoteGenerationMode::fixed:
                return juce::jlimit(0, 127, event.midiNote);

            case NoteGenerationMode::randomRange:
            {
                const auto lo = juce::jmin(event.noteMin, event.noteMax);
                const auto hi = juce::jmax(event.noteMin, event.noteMax);
                if (hi <= lo)
                    return juce::jlimit(0, 127, lo);
                std::uniform_int_distribution<int> dist(lo, hi);
                return juce::jlimit(0, 127, dist(rng));
            }

            case NoteGenerationMode::weightedChoice:
            {
                if (event.weightedNotes.empty() || event.weightedWeights.empty())
                    return juce::jlimit(0, 127, event.midiNote);

                float sum = 0.0f;
                for (auto w : event.weightedWeights)
                    sum += juce::jmax(0.0f, w);

                if (sum <= 0.0f)
                    return juce::jlimit(0, 127, event.weightedNotes.front());

                std::uniform_real_distribution<float> dist(0.0f, sum);
                const auto r = dist(rng);
                float acc = 0.0f;
                for (size_t i = 0; i < event.weightedNotes.size() && i < event.weightedWeights.size(); ++i)
                {
                    acc += juce::jmax(0.0f, event.weightedWeights[i]);
                    if (r <= acc)
                        return juce::jlimit(0, 127, event.weightedNotes[i]);
                }
                return juce::jlimit(0, 127, event.weightedNotes.back());
            }

            case NoteGenerationMode::markovWalk:
            {
                auto& laneState = getLaneState(event.lane);
                const auto key = juce::String(event.lane) + ":" + juce::String(event.sourceLine) + ":" + event.text.toLowerCase();
                auto it = laneState.markovCurrentNotes.find(key.toStdString());
                if (it == laneState.markovCurrentNotes.end())
                    it = laneState.markovCurrentNotes.emplace(key.toStdString(), juce::jlimit(0, 127, event.markovStartNote)).first;

                const auto current = juce::jlimit(juce::jmin(event.noteMin, event.noteMax),
                                                  juce::jmax(event.noteMin, event.noteMax),
                                                  it->second);

                const auto pDown = juce::jmax(0.0f, event.markovPDown);
                const auto pStay = juce::jmax(0.0f, event.markovPStay);
                const auto pUp = juce::jmax(0.0f, event.markovPUp);
                const auto pSum = pDown + pStay + pUp;
                if (pSum > 0.0f)
                {
                    std::uniform_real_distribution<float> dist(0.0f, pSum);
                    const auto r = dist(rng);
                    int next = current;
                    if (r < pDown) next = current - 1;
                    else if (r > (pDown + pStay)) next = current + 1;
                    it->second = juce::jlimit(juce::jmin(event.noteMin, event.noteMax),
                                              juce::jmax(event.noteMin, event.noteMax),
                                              next);
                }

                return juce::jlimit(0, 127, current);
            }
        }

        return juce::jlimit(0, 127, event.midiNote);
    }

    void queueEvent(const StepEvent& event, double dueMs)
    {
        pendingEvents.push(PendingEvent{ dueMs, event });
    }

    void emitNow(const StepEvent& event)
    {
        switch (event.type)
        {
            case EventType::trigger:
                sendTrigger(event.lane, event.text, event.velocity);
                break;
            case EventType::note:
            case EventType::randNote:
            case EventType::choiceNote:
            case EventType::markovNote:
            {
                const auto midi = resolveGeneratedNote(event);
                if (event.durationBeats > 0.0f)
                {
                    const auto durationSec = event.durationBeats * static_cast<float> (60.0 / program.bpm);
                    sendNote(event.lane, midi, event.velocity, durationSec, event.text);
                }
                else
                {
                    sendNote(event.lane, midi, event.velocity, -1.0f, event.text);
                }
                break;
            }
            case EventType::mod:
            {
                const auto rampSec = event.rampBeats > 0.0f
                                         ? event.rampBeats * static_cast<float> (60.0 / program.bpm)
                                         : 0.0f;
                sendMod(event.lane, event.text, event.value, rampSec);
                break;
            }
            case EventType::tempo:
                program.bpm = juce::jlimit(20.0, 300.0, static_cast<double> (event.value));
                sendTempo(static_cast<float> (program.bpm));
                break;
            case EventType::transport:
                sendTransport(event.text);
                break;
            case EventType::drill:
                sendTrigger(event.lane, event.text, event.velocity);
                break;
            case EventType::runScript:
            {
                auto it = program.teletypeScripts.find(event.scriptId);
                if (it != program.teletypeScripts.end())
                    executeTeletypeCommand(it->second, 0);
                break;
            }
        }

        if (event.sourceLine > 0)
        {
            uiActiveLine.store(event.sourceLine, std::memory_order_relaxed);
            triggerAsyncUpdate();
        }
    }

    void flushPendingEvents(double nowMs)
    {
        const juce::ScopedLock lock(stateLock);
        while (! pendingEvents.empty())
        {
            const auto& next = pendingEvents.top();
            if (next.dueMs > nowMs)
                break;

            emitNow(next.event);
            pendingEvents.pop();
        }
    }

    LaneRuntimeState& getLaneState(int lane)
    {
        return laneStates[lane];
    }

    void prepareLaneStateForCycle(int lane, long long laneCycle)
    {
        auto& state = getLaneState(lane);
        const auto laneStepCount = getLaneSteps(lane);

        if (state.lastCycle == laneCycle)
            return;

        state.lastCycle = static_cast<int> (laneCycle);

        state.scrambleMap.resize(static_cast<size_t> (laneStepCount));
        for (int i = 0; i < laneStepCount; ++i)
            state.scrambleMap[static_cast<size_t> (i)] = i;

        state.stutterRepeats.clear();

        for (const auto& rule : program.mutations)
        {
            if (rule.lane != lane)
                continue;

            if (! cyclePassesConditions(laneCycle, rule.everyN, rule.notEveryN, 0, 0, {}))
                continue;

            if (! shouldFire(rule.chance))
                continue;

            if (rule.type == MutationType::scramble)
            {
                std::shuffle(state.scrambleMap.begin(), state.scrambleMap.end(), rng);
            }
            else if (rule.type == MutationType::rotate)
            {
                const auto steps = juce::jmax(1, laneStepCount);
                state.rotateOffset = (state.rotateOffset + rule.amount) % steps;
                if (state.rotateOffset < 0)
                    state.rotateOffset += steps;
            }
            else if (rule.type == MutationType::stutter)
            {
                const auto key = rule.eventId.toStdString();
                auto it = state.stutterRepeats.find(key);
                if (it == state.stutterRepeats.end())
                    state.stutterRepeats[key] = juce::jlimit(1, 16, rule.repeats);
                else
                    it->second = juce::jmax(it->second, juce::jlimit(1, 16, rule.repeats));
            }
        }
    }

    int mapLaneStepWithMutations(int lane, int rawLaneStep)
    {
        auto& state = getLaneState(lane);
        const auto laneStepCount = getLaneSteps(lane);

        int s = rawLaneStep;
        if (laneStepCount > 0)
        {
            s = (s + state.rotateOffset) % laneStepCount;
            if (s < 0)
                s += laneStepCount;
        }

        if (! state.scrambleMap.empty() && juce::isPositiveAndBelow(s, static_cast<int> (state.scrambleMap.size())))
            s = state.scrambleMap[static_cast<size_t> (s)];

        return s;
    }

    void maybeApplyStutter(const StepEvent& event, double baseMs, double laneStepDurationMs)
    {
        if (event.type != EventType::trigger)
            return;

        auto& state = getLaneState(event.lane);
        auto it = state.stutterRepeats.find(event.text.toStdString());
        if (it == state.stutterRepeats.end())
            return;

        const auto repeats = juce::jlimit(1, 16, it->second);
        if (repeats <= 1)
            return;

        const auto spacingMs = laneStepDurationMs / static_cast<double> (repeats);
        for (int i = 1; i < repeats; ++i)
            queueEvent(event, baseMs + spacingMs * static_cast<double> (i));
    }

    void dispatchStepCounter(long long absoluteStepCounter, double nowMs)
    {
        const auto playbackStepCounter = mapToPlaybackStep(absoluteStepCounter);
        advanceTeletypeMetros(playbackStepCounter);

        for (const auto& event : program.events)
        {
            long long cycleIndex = 0;
            int eventStepNow = 0;
            double stepDurationMs = 0.0;
            double swingMs = 0.0;

            if (event.type == EventType::transport || event.type == EventType::runScript || event.type == EventType::tempo)
            {
                eventStepNow = getGlobalStep(playbackStepCounter);
                if (eventStepNow != event.step)
                    continue;

                cycleIndex = getGlobalCycle(playbackStepCounter);
                stepDurationMs = (4.0 / static_cast<double> (program.steps)) * (60.0 / program.bpm) * 1000.0;
                if (event.type == EventType::tempo)
                    swingMs = 0.0;
                else
                    swingMs = (eventStepNow % 2 == 1) ? (program.swing * stepDurationMs) : 0.0;
            }
            else
            {
                const auto lane = event.lane;
                cycleIndex = getLaneCycle(playbackStepCounter, lane);
                prepareLaneStateForCycle(lane, cycleIndex);

                const auto rawLaneStep = getLaneStep(playbackStepCounter, lane);
                eventStepNow = mapLaneStepWithMutations(lane, rawLaneStep);
                if (eventStepNow != event.step)
                    continue;

                stepDurationMs = getStepDurationMs(lane);
                swingMs = (rawLaneStep % 2 == 1) ? (program.swing * stepDurationMs) : 0.0;
            }

            if (! cyclePassesConditions(cycleIndex,
                                        event.everyN,
                                        event.notEveryN,
                                        event.fromCycle,
                                        event.toCycle,
                                        event.sectionName))
                continue;

            if (! shouldFire(event.chance))
                continue;

            const auto dueMs = nowMs + swingMs;

            if (event.type == EventType::drill)
            {
                const auto hits = juce::jlimit(1, 32, event.repeats);
                const auto spacingMs = stepDurationMs / static_cast<double> (hits);
                for (int i = 0; i < hits; ++i)
                {
                    auto e = event;
                    e.type = EventType::trigger;
                    queueEvent(e, dueMs + spacingMs * static_cast<double> (i));
                }
                continue;
            }

            queueEvent(event, dueMs);
            maybeApplyStutter(event, dueMs, stepDurationMs);
        }
    }

    void handleAsyncUpdate() override
    {
        transportMeterValue = uiMeter.load(std::memory_order_relaxed);
        transportLabel.setText("Bar " + juce::String(uiBar.load(std::memory_order_relaxed))
                               + " Beat " + juce::String(uiBeat.load(std::memory_order_relaxed))
                               + " Step " + juce::String(uiStep.load(std::memory_order_relaxed) + 1),
                               juce::dontSendNotification);

        if (codeEditor != nullptr)
        {
            const auto active = uiActiveLine.exchange(-1, std::memory_order_relaxed);
            if (active > 0)
                codeEditor->setActiveLine(active);
        }

        juce::StringArray drained;
        {
            const juce::ScopedLock lock(oscLock);
            drained.swapWith(queuedOscLines);
        }

        for (const auto& line : drained)
            oscMonitor.insertTextAtCaret(line + "\n");

        if (showTimeline && ! timelineBounds.isEmpty())
            repaint(timelineBounds);
    }

    int extractLineNumber(const juce::String& errorText) const
    {
        const auto lower = errorText.toLowerCase();
        const auto index = lower.indexOf("line ");
        if (index < 0)
            return -1;

        auto digits = lower.substring(index + 5).retainCharacters("0123456789");
        return digits.isNotEmpty() ? digits.getIntValue() : -1;
    }

    void queueOscLine(const juce::String& line)
    {
        const juce::ScopedLock lock(oscLock);
        queuedOscLines.add(line);
        triggerAsyncUpdate();
    }

    void sendTempo(float bpm)
    {
        sender.send(oscproto::tempoAddress, bpm);
        queueOscLine(oscproto::tempoAddress + juce::String(" bpm=") + juce::String(bpm, 2));
    }

    void sendTrigger(int lane, const juce::String& eventId, float velocity)
    {
        sender.send(oscproto::triggerAddress, lane, eventId, velocity);
        queueOscLine(oscproto::triggerAddress + juce::String(" lane=") + juce::String(lane)
                     + " id=" + eventId + " vel=" + juce::String(velocity, 3));
    }

    void sendNote(int lane, int midiNote, float velocity, float durationSec, const juce::String& eventId)
    {
        if (durationSec > 0.0f)
            sender.send(oscproto::noteOnAddress, lane, midiNote, velocity, durationSec, eventId);
        else
            sender.send(oscproto::noteOnAddress, lane, midiNote, velocity);

        queueOscLine(oscproto::noteOnAddress + juce::String(" lane=") + juce::String(lane)
                     + " note=" + juce::String(midiNote)
                     + " vel=" + juce::String(velocity, 3)
                     + (durationSec > 0.0f ? " dur=" + juce::String(durationSec, 3) : juce::String()));
    }

    void sendMod(int lane, const juce::String& target, float value, float rampSec)
    {
        sender.send(oscproto::modAddress, lane, target, value, rampSec);
        queueOscLine(oscproto::modAddress + juce::String(" lane=") + juce::String(lane)
                     + " target=" + target + " val=" + juce::String(value, 3)
                     + " ramp=" + juce::String(rampSec, 3));
    }

    void sendTransport(const juce::String& command)
    {
        sender.send(oscproto::transportAddress, command);
        queueOscLine(oscproto::transportAddress + juce::String(" cmd=") + command);
    }

    static bool parseInt(const juce::String& token, int& value)
    {
        if (! token.containsOnly("-0123456789"))
            return false;

        value = token.getIntValue();
        return true;
    }

    static bool parseFloat(const juce::String& token, float& value)
    {
        const auto parsed = token.getFloatValue();
        if (! std::isfinite(parsed))
            return false;

        value = parsed;
        return true;
    }

    static bool isConditionKeyword(const juce::String& token)
    {
        const auto lower = token.toLowerCase();
        return lower == "every" || lower == "notevery" || lower == "from" || lower == "to" || lower == "section";
    }

    bool parseChanceToken(const juce::String& token, float& outChance)
    {
        float parsed = 1.0f;
        if (! parseFloat(token, parsed))
            return false;
        outChance = juce::jlimit(0.0f, 1.0f, parsed);
        return true;
    }

    bool parseConditionTail(const juce::StringArray& t,
                            int startIndex,
                            int lineNumber,
                            float& chance,
                            int& everyN,
                            int& notEveryN,
                            int& fromCycle,
                            int& toCycle,
                            juce::String& sectionName,
                            juce::String& error)
    {
        for (int i = startIndex; i < t.size();)
        {
            const auto token = t[i].toLowerCase();

            if (token == "section")
            {
                if (i + 1 >= t.size())
                {
                    error = "line " + juce::String(lineNumber) + ": missing section name";
                    return false;
                }

                if (sectionName.isNotEmpty())
                {
                    error = "line " + juce::String(lineNumber) + ": section already set";
                    return false;
                }

                sectionName = t[i + 1].trim().toLowerCase();
                if (sectionName.isEmpty())
                {
                    error = "line " + juce::String(lineNumber) + ": invalid section name";
                    return false;
                }

                i += 2;
                continue;
            }

            if (token == "every" || token == "notevery" || token == "from" || token == "to")
            {
                if (i + 1 >= t.size())
                {
                    error = "line " + juce::String(lineNumber) + ": missing count after " + token;
                    return false;
                }

                int value = 0;
                if (! parseInt(t[i + 1], value) || value <= 0)
                {
                    error = "line " + juce::String(lineNumber) + ": invalid " + token + " count";
                    return false;
                }

                if (token == "every") everyN = value;
                else if (token == "notevery") notEveryN = value;
                else if (token == "from") fromCycle = value;
                else toCycle = value;

                i += 2;
                continue;
            }

            float maybeChance = 1.0f;
            if (parseChanceToken(t[i], maybeChance))
            {
                chance = maybeChance;
                ++i;
                continue;
            }

            error = "line " + juce::String(lineNumber) + ": unexpected token '" + t[i] + "'";
            return false;
        }

        if (fromCycle > 0 && toCycle > 0 && fromCycle > toCycle)
        {
            error = "line " + juce::String(lineNumber) + ": from cycle must be <= to cycle";
            return false;
        }

        return true;
    }

    bool parseWeightedNoteChoices(const juce::String& token,
                                  std::vector<int>& outNotes,
                                  std::vector<float>& outWeights,
                                  juce::String& error,
                                  int lineNumber)
    {
        juce::StringArray pairs;
        pairs.addTokens(token, ",", "");
        pairs.trim();
        pairs.removeEmptyStrings();
        if (pairs.isEmpty())
        {
            error = "line " + juce::String(lineNumber) + ": empty choice note list";
            return false;
        }

        float weightSum = 0.0f;
        for (const auto& p : pairs)
        {
            const auto colon = p.indexOfChar(':');
            if (colon <= 0 || colon >= p.length() - 1)
            {
                error = "line " + juce::String(lineNumber) + ": choice notes must be note:weight pairs";
                return false;
            }

            int note = 0;
            float weight = 0.0f;
            if (! parseInt(p.substring(0, colon).trim(), note) || ! parseFloat(p.substring(colon + 1).trim(), weight))
            {
                error = "line " + juce::String(lineNumber) + ": invalid choice note pair '" + p + "'";
                return false;
            }

            note = juce::jlimit(0, 127, note);
            weight = juce::jmax(0.0f, weight);
            outNotes.push_back(note);
            outWeights.push_back(weight);
            weightSum += weight;
        }

        if (outNotes.empty() || weightSum <= 0.0f)
        {
            error = "line " + juce::String(lineNumber) + ": choice notes need positive total weight";
            return false;
        }

        return true;
    }

    bool addBreakPatternEvents(int startStep,
                               int lane,
                               const juce::String& pattern,
                               float baseVelocity,
                               float chance,
                               int everyN,
                               int notEveryN,
                               int fromCycle,
                               int toCycle,
                               const juce::String& sectionName,
                               Program& out,
                               juce::String& error,
                               int lineNumber)
    {
        int offset = 0;
        for (auto c : pattern)
        {
            if (c == ' ' || c == '\t')
                continue;

            StepEvent e;
            e.type = EventType::trigger;
            e.step = startStep + offset;
            e.lane = lane;
            e.sourceLine = lineNumber;
            e.chance = chance;
            e.everyN = everyN;
            e.notEveryN = notEveryN;
            e.fromCycle = fromCycle;
            e.toCycle = toCycle;
            e.sectionName = sectionName;

            switch (c)
            {
                case '-':
                case '_':
                case '.':
                    ++offset;
                    continue;
                case 'k': e.text = "kick";  e.velocity = 0.90f * baseVelocity; break;
                case 'K': e.text = "kick";  e.velocity = 1.00f * baseVelocity; break;
                case 's': e.text = "snare"; e.velocity = 0.85f * baseVelocity; break;
                case 'S': e.text = "snare"; e.velocity = 1.00f * baseVelocity; break;
                case 'h': e.text = "hat";   e.velocity = 0.55f * baseVelocity; break;
                case 'H': e.text = "hat";   e.velocity = 0.80f * baseVelocity; break;
                case 'g': e.text = "snare"; e.velocity = 0.35f * baseVelocity; break;
                case 'G': e.text = "snare"; e.velocity = 0.50f * baseVelocity; break;
                default:
                    error = "line " + juce::String(lineNumber) + ": invalid break pattern char '" + juce::String::charToString(c) + "'";
                    return false;
            }

            e.velocity = juce::jlimit(0.0f, 1.0f, e.velocity);
            out.events.add(e);
            ++offset;
        }

        return true;
    }

    bool parseProgram(const juce::String& source, Program& out, juce::String& error)
    {
        enum class LanguageMode
        {
            split,
            teletype
        };

        out = {};
        LanguageMode languageMode = LanguageMode::split;
        bool velocityModeMidi = false;
        float defaultVelocityGlobal = 1.0f;
        std::unordered_map<int, float> defaultVelocityByLane;
        int scriptBlockId = -1;
        juce::StringArray scriptBlockLines;

        auto parseVelocityValue = [&] (const juce::String& token, float& outVelocity) -> bool
        {
            float parsed = 0.0f;
            if (! parseFloat(token, parsed))
                return false;

            if (velocityModeMidi)
                outVelocity = juce::jlimit(0.0f, 127.0f, parsed) / 127.0f;
            else
                outVelocity = juce::jlimit(0.0f, 1.0f, parsed);

            return true;
        };

        auto defaultVelocityForLane = [&] (int lane) -> float
        {
            auto it = defaultVelocityByLane.find(lane);
            if (it != defaultVelocityByLane.end())
                return it->second;
            return defaultVelocityGlobal;
        };

        juce::StringArray lines;
        lines.addLines(source);

        for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex)
        {
            auto line = lines[lineIndex].trim();
            if (scriptBlockId >= 0)
            {
                if (line.isEmpty() || line.startsWithChar('#'))
                    continue;

                if (line.toLowerCase() == "end")
                {
                    out.teletypeScripts[scriptBlockId] = scriptBlockLines.joinIntoString(" ; ");
                    scriptBlockId = -1;
                    scriptBlockLines.clear();
                    continue;
                }

                scriptBlockLines.add(line);
                continue;
            }

            if (line.isEmpty() || line.startsWithChar('#'))
                continue;

            juce::StringArray t;
            t.addTokens(line, " \t", "");
            t.trim();
            t.removeEmptyStrings();

            if (t.isEmpty())
                continue;

            auto op = t[0].toLowerCase();

            const auto rawOp = op;

            if (op == "language")
            {
                if (t.size() != 2)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": language <split|teletype>";
                    return false;
                }

                const auto mode = t[1].toLowerCase();
                if (mode == "split")
                    languageMode = LanguageMode::split;
                else if (mode == "teletype")
                    languageMode = LanguageMode::teletype;
                else
                {
                    error = "line " + juce::String(lineIndex + 1) + ": unknown language '" + t[1] + "'";
                    return false;
                }
                continue;
            }

            if (languageMode == LanguageMode::teletype)
            {
                // Teletype-style alias layer mapped onto the OSC event model.
                if (op == "m") op = "tempo";
                else if (op == "l") op = "steps";
                else if (op == "sc") op = "section";
                else if (op == "tr") op = "trigger";
                else if (op == "nt") op = "note";
                else if (op == "rn") op = "randnote";
                else if (op == "cn") op = "choicenote";
                else if (op == "mk") op = "markovnote";
                else if (op == "md") op = "mod";
                else if (op == "tp") op = "transport";
                else if (op == "vs") op = "velocity";
                else if (op == "vm") op = "velocitymode";
                else if (op == "script") op = "script";
                else if (op == "run") op = "run";
            }

            if (op == "tempo")
            {
                if (t.size() == 2)
                {
                    out.bpm = juce::jlimit(20.0, 300.0, static_cast<double> (t[1].getDoubleValue()));
                    continue;
                }

                if (t.size() < 3)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": tempo <bpm> OR tempo <step> <bpm> [chance] [every N] [notEvery N] [from C] [to C] [section S]";
                    return false;
                }

                StepEvent e;
                e.type = EventType::tempo;
                e.sourceLine = lineIndex + 1;
                if (! parseInt(t[1], e.step) || ! parseFloat(t[2], e.value))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid tempo event args";
                    return false;
                }

                e.value = juce::jlimit(20.0f, 300.0f, e.value);
                if (! parseConditionTail(t, 3, lineIndex + 1, e.chance, e.everyN, e.notEveryN, e.fromCycle, e.toCycle, e.sectionName, error))
                    return false;

                out.events.add(e);
                continue;
            }

            if (op == "steps")
            {
                if (t.size() != 2)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": steps expects 1 argument";
                    return false;
                }

                int parsedSteps = t[1].getIntValue();
                if (parsedSteps <= 0)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": steps must be > 0";
                    return false;
                }

                out.steps = juce::jlimit(1, 128, parsedSteps);
                continue;
            }

            if (op == "swing")
            {
                if (t.size() != 2)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": swing expects 1 argument";
                    return false;
                }

                out.swing = juce::jlimit(0.0, 0.5, static_cast<double> (t[1].getDoubleValue()));
                continue;
            }

            if (op == "lanesteps")
            {
                if (t.size() != 3)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": laneSteps <lane> <steps>";
                    return false;
                }

                int lane = 0;
                int laneSteps = 0;
                if (! parseInt(t[1], lane) || ! parseInt(t[2], laneSteps) || laneSteps <= 0)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid laneSteps args";
                    return false;
                }

                out.laneSteps[lane] = juce::jlimit(1, 128, laneSteps);
                continue;
            }

            if (op == "barloop" || op == "loopbars")
            {
                if (t.size() != 3)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": barLoop <startBar> <endBar>";
                    return false;
                }

                int startBar = 0;
                int endBar = 0;
                if (! parseInt(t[1], startBar) || ! parseInt(t[2], endBar))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid barLoop args";
                    return false;
                }

                if (startBar <= 0 || endBar <= 0 || startBar >= endBar)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": barLoop requires startBar >= 1 and endBar > startBar";
                    return false;
                }

                out.barLoopStart = startBar;
                out.barLoopEnd = endBar;
                continue;
            }

            if (op == "velocitymode")
            {
                if (t.size() != 2)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": velocityMode <midi|normalized>";
                    return false;
                }

                const auto mode = t[1].toLowerCase();
                if (mode == "midi")
                    velocityModeMidi = true;
                else if (mode == "normalized" || mode == "normalised")
                    velocityModeMidi = false;
                else
                {
                    error = "line " + juce::String(lineIndex + 1) + ": velocityMode must be midi or normalized";
                    return false;
                }
                continue;
            }

            if (op == "velocity")
            {
                if (t.size() == 2)
                {
                    if (! parseVelocityValue(t[1], defaultVelocityGlobal))
                    {
                        error = "line " + juce::String(lineIndex + 1) + ": invalid velocity value";
                        return false;
                    }
                    continue;
                }

                if (t.size() == 4 && t[1].toLowerCase() == "lane")
                {
                    int lane = 0;
                    if (! parseInt(t[2], lane))
                    {
                        error = "line " + juce::String(lineIndex + 1) + ": invalid velocity lane";
                        return false;
                    }
                    float laneVelocity = 1.0f;
                    if (! parseVelocityValue(t[3], laneVelocity))
                    {
                        error = "line " + juce::String(lineIndex + 1) + ": invalid lane velocity value";
                        return false;
                    }
                    defaultVelocityByLane[lane] = laneVelocity;
                    continue;
                }

                error = "line " + juce::String(lineIndex + 1) + ": velocity <value> OR velocity lane <lane> <value>";
                return false;
            }

            if (languageMode == LanguageMode::teletype && rawOp == "script")
            {
                if (t.size() == 2)
                {
                    int id = 0;
                    if (! parseInt(t[1], id))
                    {
                        error = "line " + juce::String(lineIndex + 1) + ": invalid SCRIPT id";
                        return false;
                    }

                    scriptBlockId = id;
                    scriptBlockLines.clear();
                    continue;
                }

                if (t.size() < 3)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": SCRIPT <id> <command...>";
                    return false;
                }

                int id = 0;
                if (! parseInt(t[1], id))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid SCRIPT id";
                    return false;
                }

                juce::StringArray cmdTokens;
                for (int i = 2; i < t.size(); ++i)
                    cmdTokens.add(t[i]);
                out.teletypeScripts[id] = cmdTokens.joinIntoString(" ");
                continue;
            }

            if (languageMode == LanguageMode::teletype && rawOp == "run")
            {
                if (t.size() < 3)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": RUN <step> <scriptId> [chance] [every N] [notEvery N] [from C] [to C] [section S]";
                    return false;
                }

                StepEvent e;
                e.type = EventType::runScript;
                e.sourceLine = lineIndex + 1;
                if (! parseInt(t[1], e.step) || ! parseInt(t[2], e.scriptId))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid RUN args";
                    return false;
                }

                if (! parseConditionTail(t, 3, lineIndex + 1, e.chance, e.everyN, e.notEveryN, e.fromCycle, e.toCycle, e.sectionName, error))
                    return false;

                out.events.add(e);
                continue;
            }

            if (op == "section")
            {
                if (t.size() != 3)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": section <name> <cycles>";
                    return false;
                }

                int cycles = 0;
                if (! parseInt(t[2], cycles) || cycles <= 0)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid section cycles";
                    return false;
                }

                Program::SectionDef s;
                s.name = t[1].trim().toLowerCase();
                if (s.name.isEmpty())
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid section name";
                    return false;
                }

                if (out.sectionCycleRanges.find(s.name.toStdString()) != out.sectionCycleRanges.end())
                {
                    error = "line " + juce::String(lineIndex + 1) + ": duplicate section '" + s.name + "'";
                    return false;
                }

                s.cycles = cycles;
                out.sections.push_back(s);
                out.sectionCycleRanges[s.name.toStdString()] = { 0, 0 }; // reserve key for duplicate detection
                continue;
            }

            if (op == "scramble")
            {
                if (t.size() < 4)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": scramble <lane> every <N> [chance]";
                    return false;
                }

                MutationRule m;
                m.type = MutationType::scramble;
                if (! parseInt(t[1], m.lane))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid scramble lane";
                    return false;
                }

                if (t[2].toLowerCase() != "every")
                {
                    error = "line " + juce::String(lineIndex + 1) + ": scramble expects 'every <N>'";
                    return false;
                }

                if (! parseInt(t[3], m.everyN) || m.everyN <= 0)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid scramble every";
                    return false;
                }

                if (t.size() >= 5 && ! parseChanceToken(t[4], m.chance))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid scramble chance";
                    return false;
                }

                out.mutations.push_back(m);
                continue;
            }

            if (op == "rotate")
            {
                if (t.size() < 5)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": rotate <lane> <amount> every <N> [chance]";
                    return false;
                }

                MutationRule m;
                m.type = MutationType::rotate;
                if (! parseInt(t[1], m.lane) || ! parseInt(t[2], m.amount))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid rotate lane/amount";
                    return false;
                }

                if (t[3].toLowerCase() != "every")
                {
                    error = "line " + juce::String(lineIndex + 1) + ": rotate expects 'every <N>'";
                    return false;
                }

                if (! parseInt(t[4], m.everyN) || m.everyN <= 0)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid rotate every";
                    return false;
                }

                if (t.size() >= 6 && ! parseChanceToken(t[5], m.chance))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid rotate chance";
                    return false;
                }

                out.mutations.push_back(m);
                continue;
            }

            if (op == "stutter")
            {
                if (t.size() < 6)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": stutter <lane> <eventId> <repeats> every <N> [chance]";
                    return false;
                }

                MutationRule m;
                m.type = MutationType::stutter;
                if (! parseInt(t[1], m.lane) || ! parseInt(t[3], m.repeats))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid stutter lane/repeats";
                    return false;
                }

                m.eventId = t[2];
                m.repeats = juce::jlimit(1, 16, m.repeats);

                if (t[4].toLowerCase() != "every")
                {
                    error = "line " + juce::String(lineIndex + 1) + ": stutter expects 'every <N>'";
                    return false;
                }

                if (! parseInt(t[5], m.everyN) || m.everyN <= 0)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid stutter every";
                    return false;
                }

                if (t.size() >= 7 && ! parseChanceToken(t[6], m.chance))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid stutter chance";
                    return false;
                }

                out.mutations.push_back(m);
                continue;
            }

            if (op == "trigger")
            {
                if (t.size() < 4)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": trigger <step> <lane> <eventId> [velocity] [chance] [every N] [notEvery N] [from C] [to C] [section S]";
                    return false;
                }

                StepEvent e;
                e.type = EventType::trigger;
                e.sourceLine = lineIndex + 1;
                if (! parseInt(t[1], e.step) || ! parseInt(t[2], e.lane))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid trigger step/lane";
                    return false;
                }

                e.text = t[3];
                e.velocity = defaultVelocityForLane(e.lane);
                int tail = 4;
                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    float maybeVelocity = 1.0f;
                    if (parseVelocityValue(t[tail], maybeVelocity))
                    {
                        e.velocity = maybeVelocity;
                        ++tail;
                    }
                }

                if (! parseConditionTail(t, tail, lineIndex + 1, e.chance, e.everyN, e.notEveryN, e.fromCycle, e.toCycle, e.sectionName, error))
                    return false;

                out.events.add(e);
                continue;
            }

            if (op == "break")
            {
                if (t.size() < 4)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": break <startStep> <lane> <pattern> [baseVelocity] [chance] [every N] [notEvery N] [from C] [to C] [section S]";
                    return false;
                }

                int startStep = 0;
                int lane = 0;
                if (! parseInt(t[1], startStep) || ! parseInt(t[2], lane))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid break step/lane";
                    return false;
                }

                float baseVelocity = 1.0f;
                baseVelocity = defaultVelocityForLane(lane);
                float chance = 1.0f;
                int everyN = 0;
                int notEveryN = 0;
                int fromCycle = 0;
                int toCycle = 0;
                juce::String sectionName;
                int tail = 4;

                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    float maybeBase = 1.0f;
                    if (parseVelocityValue(t[tail], maybeBase))
                    {
                        baseVelocity = maybeBase;
                        ++tail;
                    }
                }

                if (! parseConditionTail(t, tail, lineIndex + 1, chance, everyN, notEveryN, fromCycle, toCycle, sectionName, error))
                    return false;

                if (! addBreakPatternEvents(startStep, lane, t[3], baseVelocity, chance, everyN, notEveryN, fromCycle, toCycle, sectionName, out, error, lineIndex + 1))
                    return false;

                continue;
            }

            if (op == "drill")
            {
                if (t.size() < 5)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": drill <step> <lane> <eventId> <hits> [velocity] [chance] [every N] [notEvery N] [from C] [to C] [section S]";
                    return false;
                }

                StepEvent e;
                e.type = EventType::drill;
                e.sourceLine = lineIndex + 1;
                if (! parseInt(t[1], e.step) || ! parseInt(t[2], e.lane) || ! parseInt(t[4], e.repeats))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid drill args";
                    return false;
                }

                e.text = t[3];
                e.repeats = juce::jlimit(1, 32, e.repeats);
                e.velocity = defaultVelocityForLane(e.lane);

                int tail = 5;
                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    float maybeVelocity = 1.0f;
                    if (parseVelocityValue(t[tail], maybeVelocity))
                    {
                        e.velocity = maybeVelocity;
                        ++tail;
                    }
                }

                if (! parseConditionTail(t, tail, lineIndex + 1, e.chance, e.everyN, e.notEveryN, e.fromCycle, e.toCycle, e.sectionName, error))
                    return false;

                out.events.add(e);
                continue;
            }

            if (op == "randnote")
            {
                if (t.size() < 5)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": randnote <step> <lane> <minNote> <maxNote> [velocity] [durationBeats] [eventId] [chance] [every N] [notEvery N] [from C] [to C] [section S]";
                    return false;
                }

                StepEvent e;
                e.type = EventType::randNote;
                e.noteMode = NoteGenerationMode::randomRange;
                e.sourceLine = lineIndex + 1;
                if (! parseInt(t[1], e.step) || ! parseInt(t[2], e.lane) || ! parseInt(t[3], e.noteMin) || ! parseInt(t[4], e.noteMax))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid randnote args";
                    return false;
                }
                e.velocity = defaultVelocityForLane(e.lane);
                e.text = "randnote";

                int tail = 5;
                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    float maybeVelocity = 0.0f;
                    if (parseVelocityValue(t[tail], maybeVelocity))
                    {
                        e.velocity = maybeVelocity;
                        ++tail;
                    }
                }

                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    float maybeDuration = 0.0f;
                    if (parseFloat(t[tail], maybeDuration))
                    {
                        e.durationBeats = maybeDuration;
                        ++tail;
                    }
                }

                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    e.text = t[tail];
                    ++tail;
                }

                if (! parseConditionTail(t, tail, lineIndex + 1, e.chance, e.everyN, e.notEveryN, e.fromCycle, e.toCycle, e.sectionName, error))
                    return false;

                out.events.add(e);
                continue;
            }

            if (op == "choicenote")
            {
                if (t.size() < 4)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": choicenote <step> <lane> <n:w,...> [velocity] [durationBeats] [eventId] [chance] [every N] [notEvery N] [from C] [to C] [section S]";
                    return false;
                }

                StepEvent e;
                e.type = EventType::choiceNote;
                e.noteMode = NoteGenerationMode::weightedChoice;
                e.sourceLine = lineIndex + 1;
                if (! parseInt(t[1], e.step) || ! parseInt(t[2], e.lane))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid choicenote step/lane";
                    return false;
                }

                if (! parseWeightedNoteChoices(t[3], e.weightedNotes, e.weightedWeights, error, lineIndex + 1))
                    return false;

                e.velocity = defaultVelocityForLane(e.lane);
                e.text = "choicenote";

                int tail = 4;
                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    float maybeVelocity = 0.0f;
                    if (parseVelocityValue(t[tail], maybeVelocity))
                    {
                        e.velocity = maybeVelocity;
                        ++tail;
                    }
                }

                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    float maybeDuration = 0.0f;
                    if (parseFloat(t[tail], maybeDuration))
                    {
                        e.durationBeats = maybeDuration;
                        ++tail;
                    }
                }

                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    e.text = t[tail];
                    ++tail;
                }

                if (! parseConditionTail(t, tail, lineIndex + 1, e.chance, e.everyN, e.notEveryN, e.fromCycle, e.toCycle, e.sectionName, error))
                    return false;

                out.events.add(e);
                continue;
            }

            if (op == "markovnote")
            {
                if (t.size() < 10)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": markovnote <step> <lane> <minNote> <maxNote> <startNote> <pDown> <pStay> <pUp> [velocity] [durationBeats] [eventId] [chance] [every N] [notEvery N] [from C] [to C] [section S]";
                    return false;
                }

                StepEvent e;
                e.type = EventType::markovNote;
                e.noteMode = NoteGenerationMode::markovWalk;
                e.sourceLine = lineIndex + 1;
                if (! parseInt(t[1], e.step) || ! parseInt(t[2], e.lane) || ! parseInt(t[3], e.noteMin) || ! parseInt(t[4], e.noteMax) || ! parseInt(t[5], e.markovStartNote))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid markovnote note args";
                    return false;
                }

                if (! parseFloat(t[6], e.markovPDown) || ! parseFloat(t[7], e.markovPStay) || ! parseFloat(t[8], e.markovPUp))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid markovnote probabilities";
                    return false;
                }
                e.velocity = defaultVelocityForLane(e.lane);
                e.text = "markovnote";

                int tail = 9;
                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    float maybeVelocity = 0.0f;
                    if (parseVelocityValue(t[tail], maybeVelocity))
                    {
                        e.velocity = maybeVelocity;
                        ++tail;
                    }
                }

                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    float maybeDuration = 0.0f;
                    if (parseFloat(t[tail], maybeDuration))
                    {
                        e.durationBeats = maybeDuration;
                        ++tail;
                    }
                }

                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    e.text = t[tail];
                    ++tail;
                }

                if (! parseConditionTail(t, tail, lineIndex + 1, e.chance, e.everyN, e.notEveryN, e.fromCycle, e.toCycle, e.sectionName, error))
                    return false;

                out.events.add(e);
                continue;
            }

            if (op == "note")
            {
                if (t.size() < 4)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": note <step> <lane> <midiNote|[...]> [velocity] [durationBeats] [eventId] [chance] [every N] [notEvery N] [from C] [to C] [section S]";
                    return false;
                }

                int startStep = 0;
                int lane = 0;
                if (! parseInt(t[1], startStep) || ! parseInt(t[2], lane))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid note args";
                    return false;
                }

                juce::Array<int> midiNotes;
                int cursor = 3;
                if (t[cursor].startsWithChar('['))
                {
                    juce::String listToken = t[cursor];
                    while (! listToken.endsWithChar(']'))
                    {
                        ++cursor;
                        if (cursor >= t.size())
                        {
                            error = "line " + juce::String(lineIndex + 1) + ": unterminated note list";
                            return false;
                        }

                        listToken += " " + t[cursor];
                    }

                    auto body = listToken.fromFirstOccurrenceOf("[", false, false)
                                        .upToLastOccurrenceOf("]", false, false)
                                        .trim();
                    juce::StringArray noteTokens;
                    noteTokens.addTokens(body, " \t,", "");
                    noteTokens.trim();
                    noteTokens.removeEmptyStrings();

                    if (noteTokens.isEmpty())
                    {
                        error = "line " + juce::String(lineIndex + 1) + ": empty note list";
                        return false;
                    }

                    for (const auto& noteToken : noteTokens)
                    {
                        if (noteToken == "-" || noteToken == "_")
                        {
                            midiNotes.add(-1);
                            continue;
                        }

                        int parsedNote = 0;
                        if (! parseInt(noteToken, parsedNote))
                        {
                            error = "line " + juce::String(lineIndex + 1) + ": invalid note '" + noteToken + "' in list";
                            return false;
                        }

                        midiNotes.add(juce::jlimit(0, 127, parsedNote));
                    }
                }
                else
                {
                    int parsedNote = 0;
                    if (! parseInt(t[cursor], parsedNote))
                    {
                        error = "line " + juce::String(lineIndex + 1) + ": invalid note args";
                        return false;
                    }

                    midiNotes.add(juce::jlimit(0, 127, parsedNote));
                }

                ++cursor;
                float velocity = defaultVelocityForLane(lane);
                if (cursor < t.size() && ! isConditionKeyword(t[cursor]))
                {
                    float maybeVelocity = 0.0f;
                    if (parseVelocityValue(t[cursor], maybeVelocity))
                    {
                        velocity = maybeVelocity;
                        ++cursor;
                    }
                }

                float durationBeats = -1.0f;
                juce::String eventId = "note";
                float chance = 1.0f;
                int everyN = 0;
                int notEveryN = 0;
                int fromCycle = 0;
                int toCycle = 0;
                juce::String sectionName;

                int tail = cursor;
                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    float maybeDuration = 0.0f;
                    if (parseFloat(t[tail], maybeDuration))
                    {
                        durationBeats = maybeDuration;
                        ++tail;
                    }
                }

                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    float maybeChance = 0.0f;
                    if (parseChanceToken(t[tail], maybeChance)
                        && (tail == t.size() - 1 || isConditionKeyword(t[tail + 1])))
                    {
                        chance = maybeChance;
                        ++tail;
                    }
                    else
                    {
                        eventId = t[tail];
                        ++tail;
                    }
                }

                if (! parseConditionTail(t, tail, lineIndex + 1, chance, everyN, notEveryN, fromCycle, toCycle, sectionName, error))
                    return false;

                for (int i = 0; i < midiNotes.size(); ++i)
                {
                    if (midiNotes[i] < 0)
                        continue;

                    StepEvent e;
                    e.type = EventType::note;
                    e.sourceLine = lineIndex + 1;
                    e.step = startStep + i;
                    e.lane = lane;
                    e.midiNote = midiNotes[i];
                    e.velocity = velocity;
                    e.durationBeats = durationBeats;
                    e.text = eventId;
                    e.chance = chance;
                    e.everyN = everyN;
                    e.notEveryN = notEveryN;
                    e.fromCycle = fromCycle;
                    e.toCycle = toCycle;
                    e.sectionName = sectionName;
                    out.events.add(e);
                }

                continue;
            }

            if (op == "mod")
            {
                if (t.size() < 5)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": mod <step> <lane> <target> <value> [rampBeats] [chance] [every N] [notEvery N] [from C] [to C] [section S]";
                    return false;
                }

                StepEvent e;
                e.type = EventType::mod;
                e.sourceLine = lineIndex + 1;
                if (! parseInt(t[1], e.step) || ! parseInt(t[2], e.lane) || ! parseFloat(t[4], e.value))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid mod args";
                    return false;
                }

                e.text = t[3];
                int tail = 5;
                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    float maybeRamp = 0.0f;
                    if (parseFloat(t[tail], maybeRamp))
                    {
                        e.rampBeats = maybeRamp;
                        ++tail;
                    }
                }

                if (! parseConditionTail(t, tail, lineIndex + 1, e.chance, e.everyN, e.notEveryN, e.fromCycle, e.toCycle, e.sectionName, error))
                    return false;

                out.events.add(e);
                continue;
            }

            if (op == "transport")
            {
                if (t.size() < 3)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": transport <step> <command> [chance] [every N] [notEvery N] [from C] [to C] [section S]";
                    return false;
                }

                StepEvent e;
                e.type = EventType::transport;
                e.sourceLine = lineIndex + 1;
                if (! parseInt(t[1], e.step))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid transport step";
                    return false;
                }

                e.text = t[2];
                if (! parseConditionTail(t, 3, lineIndex + 1, e.chance, e.everyN, e.notEveryN, e.fromCycle, e.toCycle, e.sectionName, error))
                    return false;

                out.events.add(e);
                continue;
            }

            error = "line " + juce::String(lineIndex + 1) + ": unknown command '" + op + "'";
            return false;
        }

        out.velocityModeMidi = velocityModeMidi;
        out.defaultVelocityGlobal = defaultVelocityGlobal;
        out.defaultVelocityByLane = defaultVelocityByLane;

        if (scriptBlockId >= 0)
        {
            error = "unterminated SCRIPT block " + juce::String(scriptBlockId) + " (missing END)";
            return false;
        }

        out.sectionCycleRanges.clear();
        int sectionStart = 1;
        for (auto& s : out.sections)
        {
            s.startCycle = sectionStart;
            s.endCycle = sectionStart + s.cycles - 1;
            out.sectionCycleRanges[s.name.toStdString()] = { s.startCycle, s.endCycle };
            sectionStart = s.endCycle + 1;
        }

        for (const auto& e : out.events)
        {
            const auto stepCount = (e.type == EventType::transport || e.type == EventType::runScript || e.type == EventType::tempo)
                                       ? out.steps
                                       : ([&]() {
                                             auto it = out.laneSteps.find(e.lane);
                                             return (it != out.laneSteps.end()) ? it->second : out.steps;
                                         })();

            if (e.step < 0 || e.step >= stepCount)
            {
                error = "event step out of range on lane " + juce::String(e.lane)
                        + " (step=" + juce::String(e.step)
                        + ", laneSteps=" + juce::String(stepCount) + ")";
                return false;
            }

            if (e.sectionName.isNotEmpty()
                && out.sectionCycleRanges.find(e.sectionName.toStdString()) == out.sectionCycleRanges.end())
            {
                error = "unknown section '" + e.sectionName + "' for lane " + juce::String(e.lane);
                return false;
            }
        }

        if (out.barLoopStart > 0 && out.barLoopEnd > 0 && out.barLoopEnd <= out.barLoopStart)
        {
            error = "barLoop requires endBar > startBar";
            return false;
        }

        return true;
    }

    juce::TextButton runButton;
    juce::TextButton stopButton;
    juce::TextButton loadButton;
    juce::TextButton saveButton;
    juce::TextButton logModeButton;
    juce::TextButton timelineToggleButton;
    juce::Label status;
    juce::Label transportLabel;
    double transportMeterValue = 0.0;
    juce::ProgressBar transportMeter { transportMeterValue };
    juce::CodeDocument codeDocument;
    SplitTokeniser tokeniser;
    std::unique_ptr<ScriptCodeEditor> codeEditor;
    juce::TextEditor outputLog;
    juce::TextEditor oscMonitor;
    juce::OSCSender sender;
    std::unique_ptr<juce::FileChooser> loadChooser;
    std::unique_ptr<juce::FileChooser> saveChooser;
    juce::File lastLoadedScriptFile;

    Program program;
    std::atomic<bool> running { false };
    double startMs = 0.0;
    long long nextStepToSchedule = 0;
    struct PendingEventCompare
    {
        bool operator()(const PendingEvent& a, const PendingEvent& b) const noexcept
        {
            return a.dueMs > b.dueMs;
        }
    };
    std::priority_queue<PendingEvent, std::vector<PendingEvent>, PendingEventCompare> pendingEvents;
    std::unordered_map<int, LaneRuntimeState> laneStates;
    std::unordered_map<std::string, float> teletypeVars;
    std::array<float, 64> teletypePattern {};
    std::unordered_map<int, TeletypeMetro> teletypeMetros;
    long long lastMetroStepProcessed = std::numeric_limits<long long>::min();
    juce::CriticalSection stateLock;

    std::mt19937 rng { std::random_device{}() };
    std::uniform_real_distribution<float> random01 { 0.0f, 1.0f };
    juce::Rectangle<int> timelineBounds;
    juce::Rectangle<int> timelineSplitterBounds;
    juce::Rectangle<int> splitterBounds;
    int timelinePanelHeightPx = 126;
    float timelinePaneRatio = 0.22f;
    float codePaneRatio = 0.66f;
    DragTarget activeDragTarget = DragTarget::none;
    bool showingOscMonitor = false;
    bool showTimeline = true;
    std::atomic<int> uiBar { 1 };
    std::atomic<int> uiBeat { 1 };
    std::atomic<int> uiStep { 0 };
    std::atomic<double> uiMeter { 0.0 };
    std::atomic<int> uiActiveLine { -1 };
    double lastUiUpdateMs = 0.0;
    juce::CriticalSection oscLock;
    juce::StringArray queuedOscLines;

    void clearPendingEvents()
    {
        while (! pendingEvents.empty())
            pendingEvents.pop();
    }
};
} // namespace

MainWindow::MainWindow(const juce::String& name)
    : DocumentWindow(name,
                     juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
                     DocumentWindow::allButtons)
{
    setUsingNativeTitleBar(true);
    setContentOwned(new IdeComponent(), true);
    setResizable(true, true);
    centreWithSize(980, 720);
    setVisible(true);
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}
