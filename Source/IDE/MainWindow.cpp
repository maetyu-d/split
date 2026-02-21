#include "MainWindow.h"
#include "../Common/OscProtocol.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
class IdeComponent final : public juce::Component,
                           private juce::Button::Listener,
                           private juce::HighResolutionTimer
{
public:
    IdeComponent()
    {
        addAndMakeVisible(runButton);
        runButton.setButtonText("Run");
        runButton.addListener(this);

        addAndMakeVisible(stopButton);
        stopButton.setButtonText("Stop");
        stopButton.addListener(this);

        addAndMakeVisible(loadButton);
        loadButton.setButtonText("Load");
        loadButton.addListener(this);

        addAndMakeVisible(saveButton);
        saveButton.setButtonText("Save");
        saveButton.addListener(this);

        addAndMakeVisible(status);
        status.setText("Ready. Sending OSC to 127.0.0.1:9001", juce::dontSendNotification);

        addAndMakeVisible(codeEditor);
        codeEditor.setMultiLine(true);
        codeEditor.setReturnKeyStartsNewLine(true);
        codeEditor.setTabKeyUsedAsCharacter(true);
        codeEditor.setFont(juce::Font(15.0f));
        codeEditor.setText(
            "# Split Language v0 (break-focused)\n"
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
            "# mod <step> <lane> <target> <value> [rampBeats] [chance] [every N] [notEvery N]\n"
            "mod 0 1 lane.pan -0.45 0.0 1.0 every 1\n"
            "mod 8 1 lane.pan 0.45 0.0 1.0 every 1\n",
            false);

        addAndMakeVisible(outputLog);
        outputLog.setMultiLine(true);
        outputLog.setReadOnly(true);
        outputLog.setText("Language:\n"
                          "  tempo <bpm>\n"
                          "  steps <count>\n"
                          "  swing <0..0.5>\n"
                          "  laneSteps <lane> <steps>\n"
                          "  trigger <step> <lane> <eventId> [velocity] [chance] [every N] [notEvery N]\n"
                          "  break <startStep> <lane> <pattern> [baseVelocity] [chance] [every N] [notEvery N]\n"
                          "  drill <step> <lane> <eventId> <hits> [velocity] [chance] [every N] [notEvery N]\n"
                          "  note <step> <lane> <midiNote> <velocity> [durationBeats] [eventId] [chance] [every N] [notEvery N]\n"
                          "  note <startStep> <lane> [m1 - m3 _ ...] <velocity> [durationBeats] [eventId] [chance] [every N] [notEvery N]\n"
                          "  mod <step> <lane> <target> <value> [rampBeats] [chance] [every N] [notEvery N]\n"
                          "  transport <step> <command> [chance] [every N] [notEvery N]\n"
                          "  scramble <lane> every <N> [chance]\n"
                          "  rotate <lane> <amount> every <N> [chance]\n"
                          "  stutter <lane> <eventId> <repeats> every <N> [chance]\n\n");

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
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(10);
        auto top = area.removeFromTop(34);
        runButton.setBounds(top.removeFromLeft(80).reduced(2));
        stopButton.setBounds(top.removeFromLeft(80).reduced(2));
        loadButton.setBounds(top.removeFromLeft(80).reduced(2));
        saveButton.setBounds(top.removeFromLeft(80).reduced(2));
        status.setBounds(top.reduced(6, 0));

        codeEditor.setBounds(area.removeFromTop(380));
        area.removeFromTop(8);
        outputLog.setBounds(area);
    }

private:
    enum class EventType
    {
        trigger,
        note,
        mod,
        transport,
        drill
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
        juce::String text;
        int midiNote = 60;
        float velocity = 1.0f;
        float durationBeats = -1.0f;
        float value = 0.0f;
        float rampBeats = 0.0f;
        float chance = 1.0f;
        int repeats = 1;
        int everyN = 0;
        int notEveryN = 0;
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
    };

    struct Program
    {
        double bpm = 120.0;
        int steps = 16;
        double swing = 0.0;
        juce::Array<StepEvent> events;
        std::unordered_map<int, int> laneSteps;
        std::vector<MutationRule> mutations;
    };

    void buttonClicked(juce::Button* b) override
    {
        if (b == &runButton)
        {
            Program parsed;
            juce::String parseError;
            if (! parseProgram(codeEditor.getText(), parsed, parseError))
            {
                running.store(false);
                stopTimer();
                status.setText("Parse failed", juce::dontSendNotification);
                outputLog.insertTextAtCaret("Parse error: " + parseError + "\n");
                return;
            }

            program = std::move(parsed);
            running.store(true);
            {
                const juce::ScopedLock lock(stateLock);
                startMs = juce::Time::getMillisecondCounterHiRes();
                nextStepToSchedule = 0;
                clearPendingEvents();
                laneStates.clear();
            }

            sender.send(oscproto::tempoAddress, static_cast<float>(program.bpm));
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
    }

    void loadScript()
    {
        loadChooser = std::make_unique<juce::FileChooser>("Load Script",
                                                          juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
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

                                     codeEditor.setText(contents, false);
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

                                     if (! file.replaceWithText(codeEditor.getText()))
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
            sender.send(oscproto::transportAddress, juce::String("stop"));

        running.store(false);
        {
            const juce::ScopedLock lock(stateLock);
            clearPendingEvents();
            laneStates.clear();
        }
        stopTimer();
        status.setText("Stopped", juce::dontSendNotification);
        outputLog.insertTextAtCaret("Stopped\n");
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

    bool cyclePassesConditions(long long cycleIndex, int everyN, int notEveryN) const
    {
        const auto oneBased = cycleIndex + 1;
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

    void queueEvent(const StepEvent& event, double dueMs)
    {
        pendingEvents.push(PendingEvent{ dueMs, event });
    }

    void emitNow(const StepEvent& event)
    {
        switch (event.type)
        {
            case EventType::trigger:
                sender.send(oscproto::triggerAddress, event.lane, event.text, event.velocity);
                break;
            case EventType::note:
            {
                if (event.durationBeats > 0.0f)
                {
                    const auto durationSec = event.durationBeats * static_cast<float> (60.0 / program.bpm);
                    sender.send(oscproto::noteOnAddress, event.lane, event.midiNote, event.velocity, durationSec, event.text);
                }
                else
                {
                    sender.send(oscproto::noteOnAddress, event.lane, event.midiNote, event.velocity);
                }
                break;
            }
            case EventType::mod:
            {
                const auto rampSec = event.rampBeats > 0.0f
                                         ? event.rampBeats * static_cast<float> (60.0 / program.bpm)
                                         : 0.0f;
                sender.send(oscproto::modAddress, event.lane, event.text, event.value, rampSec);
                break;
            }
            case EventType::transport:
                sender.send(oscproto::transportAddress, event.text);
                break;
            case EventType::drill:
                sender.send(oscproto::triggerAddress, event.lane, event.text, event.velocity);
                break;
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

            if (! cyclePassesConditions(laneCycle, rule.everyN, rule.notEveryN))
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
        for (const auto& event : program.events)
        {
            long long cycleIndex = 0;
            int eventStepNow = 0;
            double stepDurationMs = 0.0;
            double swingMs = 0.0;

            if (event.type == EventType::transport)
            {
                eventStepNow = getGlobalStep(absoluteStepCounter);
                if (eventStepNow != event.step)
                    continue;

                cycleIndex = getGlobalCycle(absoluteStepCounter);
                stepDurationMs = (4.0 / static_cast<double> (program.steps)) * (60.0 / program.bpm) * 1000.0;
                swingMs = (eventStepNow % 2 == 1) ? (program.swing * stepDurationMs) : 0.0;
            }
            else
            {
                const auto lane = event.lane;
                cycleIndex = getLaneCycle(absoluteStepCounter, lane);
                prepareLaneStateForCycle(lane, cycleIndex);

                const auto rawLaneStep = getLaneStep(absoluteStepCounter, lane);
                eventStepNow = mapLaneStepWithMutations(lane, rawLaneStep);
                if (eventStepNow != event.step)
                    continue;

                stepDurationMs = getStepDurationMs(lane);
                swingMs = (rawLaneStep % 2 == 1) ? (program.swing * stepDurationMs) : 0.0;
            }

            if (! cyclePassesConditions(cycleIndex, event.everyN, event.notEveryN))
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
        return lower == "every" || lower == "notevery";
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
                            juce::String& error)
    {
        for (int i = startIndex; i < t.size();)
        {
            const auto token = t[i].toLowerCase();

            if (token == "every" || token == "notevery")
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
                else notEveryN = value;

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

        return true;
    }

    bool addBreakPatternEvents(int startStep,
                               int lane,
                               const juce::String& pattern,
                               float baseVelocity,
                               float chance,
                               int everyN,
                               int notEveryN,
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
            e.chance = chance;
            e.everyN = everyN;
            e.notEveryN = notEveryN;

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
        out = {};
        juce::StringArray lines;
        lines.addLines(source);

        for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex)
        {
            auto line = lines[lineIndex].trim();
            if (line.isEmpty() || line.startsWithChar('#'))
                continue;

            juce::StringArray t;
            t.addTokens(line, " \t", "");
            t.trim();
            t.removeEmptyStrings();

            if (t.isEmpty())
                continue;

            const auto op = t[0].toLowerCase();

            if (op == "tempo")
            {
                if (t.size() != 2)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": tempo expects 1 argument";
                    return false;
                }

                out.bpm = juce::jlimit(20.0, 300.0, static_cast<double> (t[1].getDoubleValue()));
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
                    error = "line " + juce::String(lineIndex + 1) + ": trigger <step> <lane> <eventId> [velocity] [chance] [every N] [notEvery N]";
                    return false;
                }

                StepEvent e;
                e.type = EventType::trigger;
                if (! parseInt(t[1], e.step) || ! parseInt(t[2], e.lane))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid trigger step/lane";
                    return false;
                }

                e.text = t[3];
                int tail = 4;
                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    float maybeVelocity = 1.0f;
                    if (parseFloat(t[tail], maybeVelocity))
                    {
                        e.velocity = juce::jlimit(0.0f, 1.0f, maybeVelocity);
                        ++tail;
                    }
                }

                if (! parseConditionTail(t, tail, lineIndex + 1, e.chance, e.everyN, e.notEveryN, error))
                    return false;

                out.events.add(e);
                continue;
            }

            if (op == "break")
            {
                if (t.size() < 4)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": break <startStep> <lane> <pattern> [baseVelocity] [chance] [every N] [notEvery N]";
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
                float chance = 1.0f;
                int everyN = 0;
                int notEveryN = 0;
                int tail = 4;

                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    float maybeBase = 1.0f;
                    if (parseFloat(t[tail], maybeBase))
                    {
                        baseVelocity = juce::jlimit(0.0f, 1.0f, maybeBase);
                        ++tail;
                    }
                }

                if (! parseConditionTail(t, tail, lineIndex + 1, chance, everyN, notEveryN, error))
                    return false;

                if (! addBreakPatternEvents(startStep, lane, t[3], baseVelocity, chance, everyN, notEveryN, out, error, lineIndex + 1))
                    return false;

                continue;
            }

            if (op == "drill")
            {
                if (t.size() < 5)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": drill <step> <lane> <eventId> <hits> [velocity] [chance] [every N] [notEvery N]";
                    return false;
                }

                StepEvent e;
                e.type = EventType::drill;
                if (! parseInt(t[1], e.step) || ! parseInt(t[2], e.lane) || ! parseInt(t[4], e.repeats))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid drill args";
                    return false;
                }

                e.text = t[3];
                e.repeats = juce::jlimit(1, 32, e.repeats);

                int tail = 5;
                if (tail < t.size() && ! isConditionKeyword(t[tail]))
                {
                    float maybeVelocity = 1.0f;
                    if (parseFloat(t[tail], maybeVelocity))
                    {
                        e.velocity = juce::jlimit(0.0f, 1.0f, maybeVelocity);
                        ++tail;
                    }
                }

                if (! parseConditionTail(t, tail, lineIndex + 1, e.chance, e.everyN, e.notEveryN, error))
                    return false;

                out.events.add(e);
                continue;
            }

            if (op == "note")
            {
                if (t.size() < 5)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": note <step> <lane> <midiNote> <velocity> [durationBeats] [eventId] [chance] [every N] [notEvery N]";
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
                if (cursor >= t.size())
                {
                    error = "line " + juce::String(lineIndex + 1) + ": note missing velocity";
                    return false;
                }

                float velocity = 1.0f;
                if (! parseFloat(t[cursor], velocity))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid note velocity";
                    return false;
                }
                velocity = juce::jlimit(0.0f, 1.0f, velocity);

                float durationBeats = -1.0f;
                juce::String eventId = "note";
                float chance = 1.0f;
                int everyN = 0;
                int notEveryN = 0;

                int tail = cursor + 1;
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

                if (! parseConditionTail(t, tail, lineIndex + 1, chance, everyN, notEveryN, error))
                    return false;

                for (int i = 0; i < midiNotes.size(); ++i)
                {
                    if (midiNotes[i] < 0)
                        continue;

                    StepEvent e;
                    e.type = EventType::note;
                    e.step = startStep + i;
                    e.lane = lane;
                    e.midiNote = midiNotes[i];
                    e.velocity = velocity;
                    e.durationBeats = durationBeats;
                    e.text = eventId;
                    e.chance = chance;
                    e.everyN = everyN;
                    e.notEveryN = notEveryN;
                    out.events.add(e);
                }

                continue;
            }

            if (op == "mod")
            {
                if (t.size() < 5)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": mod <step> <lane> <target> <value> [rampBeats] [chance] [every N] [notEvery N]";
                    return false;
                }

                StepEvent e;
                e.type = EventType::mod;
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

                if (! parseConditionTail(t, tail, lineIndex + 1, e.chance, e.everyN, e.notEveryN, error))
                    return false;

                out.events.add(e);
                continue;
            }

            if (op == "transport")
            {
                if (t.size() < 3)
                {
                    error = "line " + juce::String(lineIndex + 1) + ": transport <step> <command> [chance] [every N] [notEvery N]";
                    return false;
                }

                StepEvent e;
                e.type = EventType::transport;
                if (! parseInt(t[1], e.step))
                {
                    error = "line " + juce::String(lineIndex + 1) + ": invalid transport step";
                    return false;
                }

                e.text = t[2];
                if (! parseConditionTail(t, 3, lineIndex + 1, e.chance, e.everyN, e.notEveryN, error))
                    return false;

                out.events.add(e);
                continue;
            }

            error = "line " + juce::String(lineIndex + 1) + ": unknown command '" + op + "'";
            return false;
        }

        for (const auto& e : out.events)
        {
            const auto stepCount = (e.type == EventType::transport)
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
        }

        return true;
    }

    juce::TextButton runButton;
    juce::TextButton stopButton;
    juce::TextButton loadButton;
    juce::TextButton saveButton;
    juce::Label status;
    juce::TextEditor codeEditor;
    juce::TextEditor outputLog;
    juce::OSCSender sender;
    std::unique_ptr<juce::FileChooser> loadChooser;
    std::unique_ptr<juce::FileChooser> saveChooser;

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
    juce::CriticalSection stateLock;

    std::mt19937 rng { std::random_device{}() };
    std::uniform_real_distribution<float> random01 { 0.0f, 1.0f };

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
