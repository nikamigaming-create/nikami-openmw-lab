#include "esm4questruntime.hpp"

#include "esmstore.hpp"
#include "cellstore.hpp"
#include "fnvplayerruntimestate.hpp"
#include "globals.hpp"
#include "ptr.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadcont.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loaddial.hpp>
#include <components/esm4/loaddoor.hpp>
#include <components/esm4/loadglob.hpp>
#include <components/esm4/loadinfo.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadmesg.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadscpt.hpp>
#include <components/esm4/loadterm.hpp>
#include <components/misc/strings/algorithm.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

namespace
{
    constexpr std::size_t CompiledStageRecursionLimit = 32;

    std::vector<std::string_view> tokenize(std::string_view line)
    {
        if (const std::size_t comment = line.find(';'); comment != std::string_view::npos)
            line = line.substr(0, comment);

        std::vector<std::string_view> result;
        while (!line.empty())
        {
            const std::size_t first = line.find_first_not_of(" \t\r");
            if (first == std::string_view::npos)
                break;
            line.remove_prefix(first);
            std::size_t end = std::string_view::npos;
            if (line.front() == '"' || line.front() == '\'')
            {
                const std::size_t closingQuote = line.find(line.front(), 1);
                end = closingQuote == std::string_view::npos ? line.size() : closingQuote + 1;
            }
            else if (line.size() > 2 && line[2] == '('
                && Misc::StringUtils::ciEqual(line.substr(0, 2), "if"))
                end = 2;
            else if (line.size() > 6 && line[6] == '('
                && Misc::StringUtils::ciEqual(line.substr(0, 6), "elseif"))
                end = 6;
            else
                end = line.find_first_of(" \t\r");
            result.push_back(line.substr(0, end));
            if (end == std::string_view::npos)
                break;
            line.remove_prefix(end);
        }
        return result;
    }

    bool parseInt(std::string_view value, std::int32_t& result)
    {
        const char* const begin = value.data();
        const char* const end = value.data() + value.size();
        const auto parsed = std::from_chars(begin, end, result, 10);
        return parsed.ec == std::errc{} && parsed.ptr == end;
    }

    bool parseFloat(std::string_view value, float& result)
    {
        const std::string text(value);
        char* end = nullptr;
        const float parsed = std::strtof(text.c_str(), &end);
        if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(parsed))
            return false;
        result = parsed;
        return true;
    }

    std::string removeQuotes(std::string_view value)
    {
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"')
                                  || (value.front() == '\'' && value.back() == '\'')))
            value = value.substr(1, value.size() - 2);
        return std::string(value);
    }

    std::string normaliseSourceToken(std::string_view value)
    {
        constexpr std::string_view TrimCharacters = "()[]{};,\"'";
        const auto isTrimCharacter = [&TrimCharacters](char character) {
            return character == '\0' || TrimCharacters.find(character) != std::string_view::npos;
        };
        while (!value.empty() && isTrimCharacter(value.front()))
            value.remove_prefix(1);
        while (!value.empty() && isTrimCharacter(value.back()))
            value.remove_suffix(1);
        return Misc::StringUtils::lowerCase(value);
    }

    std::string normaliseScriptVariable(std::string_view value)
    {
        std::string result = normaliseSourceToken(value);
        if (const std::size_t separator = result.rfind('.'); separator != std::string::npos)
            result.erase(0, separator + 1);
        return result;
    }

    std::vector<std::string> normaliseSourceTokens(const std::vector<std::string_view>& rawTokens)
    {
        std::vector<std::string> result;
        result.reserve(rawTokens.size());
        for (const std::string_view token : rawTokens)
        {
            std::string normalised = normaliseSourceToken(token);
            if (!normalised.empty())
                result.push_back(std::move(normalised));
        }
        return result;
    }

    float objectBoundsRadius(const std::array<std::uint8_t, 12>& bytes)
    {
        std::array<std::int16_t, 6> bounds{};
        static_assert(sizeof(bounds) == sizeof(bytes));
        std::memcpy(bounds.data(), bytes.data(), bytes.size());
        float result = 0.f;
        for (const std::int16_t bound : bounds)
            result = std::max(result, std::abs(static_cast<float>(bound)));
        return result;
    }

    bool hasSourceBlock(std::string_view source, std::string_view block)
    {
        const std::string wanted = normaliseSourceToken(block);
        std::istringstream stream{ std::string(source) };
        for (std::string line; std::getline(stream, line);)
        {
            const std::vector<std::string> tokens = normaliseSourceTokens(tokenize(line));
            if (tokens.size() >= 2 && tokens[0] == "begin" && tokens[1] == wanted)
                return true;
        }
        return false;
    }

    struct SourceConditionalFrame
    {
        std::vector<std::string> mCondition;
        bool mElseBranch = false;
    };

    std::optional<std::string> parseTimerDecrement(const std::vector<std::string>& tokens)
    {
        if (tokens.size() < 6 || tokens[0] != "set" || tokens[2] != "to" || tokens[4] != "-"
            || tokens[5] != "getsecondspassed")
            return std::nullopt;

        const std::string destination = normaliseScriptVariable(tokens[1]);
        const std::string source = normaliseScriptVariable(tokens[3]);
        if (destination.empty() || destination != source)
            return std::nullopt;
        return destination;
    }

    std::optional<std::string> parseTimerPositiveCondition(const std::vector<std::string>& condition)
    {
        if (condition.size() != 3 || condition[1] != ">" || condition[2] != "0")
            return std::nullopt;
        const std::string variable = normaliseScriptVariable(condition[0]);
        return variable.empty() ? std::nullopt : std::optional<std::string>(variable);
    }

    std::optional<std::string> parseRunFlagCondition(const std::vector<std::string>& condition)
    {
        if (condition.size() == 1)
        {
            const std::string variable = normaliseScriptVariable(condition[0]);
            return variable.empty() ? std::nullopt : std::optional<std::string>(variable);
        }
        if (condition.size() == 3 && condition[1] == "==" && condition[2] == "1")
        {
            const std::string variable = normaliseScriptVariable(condition[0]);
            return variable.empty() ? std::nullopt : std::optional<std::string>(variable);
        }
        return std::nullopt;
    }

    std::optional<std::uint8_t> parseStageCondition(
        const std::vector<std::string>& condition, std::string_view questEditorId)
    {
        if (condition.size() != 4 || condition[0] != "getstage" || condition[1] != questEditorId
            || condition[2] != "==")
            return std::nullopt;
        std::int32_t stage = 0;
        if (!parseInt(condition[3], stage) || stage < 0 || stage > 255)
            return std::nullopt;
        return static_cast<std::uint8_t>(stage);
    }

    struct AuthoredOpeningSource
    {
        std::string mMarkerEditorId;
        std::string mCinematicAsset;
        std::uint8_t mActivationStage = 0;
    };

    std::optional<AuthoredOpeningSource> findAuthoredOpeningSource(std::string_view source,
        std::string_view questEditorId)
    {
        std::optional<std::string> markerEditorId;
        std::optional<std::string> cinematicAsset;
        std::optional<std::uint8_t> activationStage;
        bool entersCharacterGeneration = false;
        std::istringstream stream{ std::string(source) };
        for (std::string line; std::getline(stream, line);)
        {
            const std::vector<std::string_view> tokens = tokenize(line);
            if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(tokens[0], "player.moveto"))
            {
                const std::string marker = removeQuotes(tokens[1]);
                if (marker.empty() || (markerEditorId && !Misc::StringUtils::ciEqual(*markerEditorId, marker)))
                    return std::nullopt;
                markerEditorId = marker;
            }
            else if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(tokens[0], "playbink"))
            {
                const std::string cinematic = removeQuotes(tokens[1]);
                if (cinematic.empty()
                    || (cinematicAsset && !Misc::StringUtils::ciEqual(*cinematicAsset, cinematic)))
                    return std::nullopt;
                cinematicAsset = cinematic;
            }
            else if (tokens.size() >= 3 && Misc::StringUtils::ciEqual(tokens[0], "setstage")
                && Misc::StringUtils::ciEqual(tokens[1], questEditorId))
            {
                std::int32_t stage = 0;
                if (!parseInt(tokens[2], stage) || stage <= 0 || stage > 255)
                    return std::nullopt;
                if (activationStage && *activationStage != stage)
                    return std::nullopt;
                activationStage = static_cast<std::uint8_t>(stage);
            }
            else if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(tokens[0], "setinchargen"))
            {
                std::int32_t value = 0;
                entersCharacterGeneration = entersCharacterGeneration || (parseInt(tokens[1], value) && value != 0);
            }
        }

        if (!markerEditorId || !activationStage || (!cinematicAsset && !entersCharacterGeneration))
            return std::nullopt;
        return AuthoredOpeningSource{ std::move(*markerEditorId), std::move(*cinematicAsset), *activationStage };
    }
}

namespace MWWorld
{
    void ESM4QuestRuntime::initialize(const ESMStore& store, const Globals* globals)
    {
        clear();
        mStore = &store;
        mGlobals = globals;
        for (const ESM4::Quest& quest : store.get<ESM4::Quest>())
        {
            ESM4QuestState state;
            state.mFlags = quest.mData.flags;
            for (const ESM4::QuestStage& stage : quest.mStages)
                state.mStageDone.emplace(stage.mIndex, false);
            for (const ESM4::QuestObjective& objective : quest.mObjectives)
                state.mObjectiveStatus.emplace(objective.mIndex, 0);
            if (const ESM4::Script* script = store.get<ESM4::Script>().search(ESM::RefId(quest.mQuestScript)))
            {
                for (const ESM4::ScriptLocalVariableData& variable : script->mScript.localVarData)
                    if (!variable.variableName.empty())
                        state.mVariables.emplace(Misc::StringUtils::lowerCase(variable.variableName), 0.f);
                if (std::vector<ESM4AuthoredGameModeTimer> timers
                    = compileAuthoredGameModeTimers(script->mScript.scriptSource, quest.mEditorId);
                    !timers.empty())
                {
                    mAuthoredGameModeTimers.insert_or_assign(quest.mId, std::move(timers));
                    mAuthoredGameModeSources.insert_or_assign(quest.mId, script->mScript.scriptSource);
                }

                const std::string loweredSource = Misc::StringUtils::lowerCase(script->mScript.scriptSource);
                const bool hasChoiceHandoff = loweredSource.find("begin gamemode") != std::string::npos
                    && loweredSource.find("getbuttonpressed") != std::string::npos
                    && loweredSource.find("showmessage") != std::string::npos;
                const bool hasMenuModeStageHandoff = loweredSource.find("begin menumode") != std::string::npos
                    && loweredSource.find("setstage") != std::string::npos;
                if (hasChoiceHandoff)
                    mAuthoredGameModeSources.insert_or_assign(quest.mId, script->mScript.scriptSource);
                if (hasMenuModeStageHandoff)
                    mAuthoredMenuModeSources.insert_or_assign(quest.mId, script->mScript.scriptSource);
            }
            mStates.insert_or_assign(quest.mId, std::move(state));
        }

        const auto registerActorReference = [this](const auto& actor) {
            if (actor.mId.isZeroOrUnset() || actor.mEditorId.empty())
                return;

            mActorReferenceStates.insert_or_assign(
                actor.mId, ActorReferenceState{ actor.mId, actor.mBaseObj, actor.mParent, actor.mEditorId });
            const std::string key = normaliseSourceToken(actor.mEditorId);
            const auto [existing, inserted] = mActorReferenceEditorIds.emplace(key, actor.mId);
            if (!inserted && existing->second != actor.mId)
                mAmbiguousActorReferenceEditorIds.insert_or_assign(key, true);
        };
        for (const ESM4::ActorCharacter& actor : store.get<ESM4::ActorCharacter>())
            registerActorReference(actor);
        for (const ESM4::ActorCreature& actor : store.get<ESM4::ActorCreature>())
            registerActorReference(actor);

        const auto findReferenceScript = [&store](ESM::FormId base)
            -> std::pair<const ESM4::Script*, float> {
            const auto resolve = [&store](const auto* record, float radius) -> std::pair<const ESM4::Script*, float> {
                if (record == nullptr || record->mScriptId.isZeroOrUnset())
                    return {};
                return { store.get<ESM4::Script>().search(ESM::RefId(record->mScriptId)), radius };
            };

            if (const ESM4::Activator* activator = store.get<ESM4::Activator>().search(ESM::RefId(base)))
                return resolve(activator, std::max(activator->mBoundRadius, objectBoundsRadius(activator->mObjectBounds)));
            if (const ESM4::Book* book = store.get<ESM4::Book>().search(ESM::RefId(base)))
                return resolve(book, std::max(0.f, book->mBoundRadius));
            if (const ESM4::Door* door = store.get<ESM4::Door>().search(ESM::RefId(base)))
                return resolve(door, std::max(0.f, door->mBoundRadius));
            if (const ESM4::Terminal* terminal = store.get<ESM4::Terminal>().search(ESM::RefId(base)))
                return resolve(terminal, 0.f);
            return {};
        };

        for (const ESM4::Reference& reference : store.get<ESM4::Reference>())
        {
            if (reference.mId.isZeroOrUnset())
                continue;
            const auto [script, radius] = findReferenceScript(reference.mBaseObj);
            if (script == nullptr || script->mScript.scriptSource.empty())
                continue;

            const bool onActivate = hasSourceBlock(script->mScript.scriptSource, "onactivate");
            const bool triggerEnter = hasSourceBlock(script->mScript.scriptSource, "ontriggerenter");
            if (!onActivate && !triggerEnter)
                continue;

            mReferenceScriptStates.insert_or_assign(reference.mId,
                ReferenceScriptState{ reference.mId, reference.mParent, reference.mPos, reference.mEditorId,
                    script->mScript.scriptSource, radius, reference.mScale, onActivate, triggerEnter, false });
        }
    }

    void ESM4QuestRuntime::clear()
    {
        mStore = nullptr;
        mGlobals = nullptr;
        mStates.clear();
        mActiveQuest.reset();
        mUnsupportedStageCommands.clear();
        mUnsupportedCompiledOpcodes.clear();
        mUnsupportedConditionFunctions.clear();
        mAuthoredGameModeSources.clear();
        mAuthoredMenuModeSources.clear();
        mAuthoredGameModeTimers.clear();
        mActorReferenceStates.clear();
        mActorReferenceEditorIds.clear();
        mAmbiguousActorReferenceEditorIds.clear();
        mSaidAuthoredDialogInfos.clear();
        mReferenceScriptStates.clear();
    }

    std::vector<ESM4AuthoredGameModeTimer> ESM4QuestRuntime::compileAuthoredGameModeTimers(
        std::string_view source, std::string_view questEditorId)
    {
        const std::string canonicalQuest = normaliseSourceToken(questEditorId);
        if (source.empty() || canonicalQuest.empty())
            return {};

        std::map<std::string, std::set<std::string>, std::less<>> runVariables;
        std::map<std::string, std::map<std::uint8_t, std::uint8_t>, std::less<>> transitions;
        std::set<std::string, std::less<>> ambiguousTransitions;
        bool inGameMode = false;
        std::vector<SourceConditionalFrame> conditionStack;

        std::istringstream stream{ std::string(source) };
        for (std::string line; std::getline(stream, line);)
        {
            const std::vector<std::string> tokens = normaliseSourceTokens(tokenize(line));
            if (tokens.empty())
                continue;

            if (tokens[0] == "begin")
            {
                inGameMode = tokens.size() >= 2 && tokens[1] == "gamemode";
                conditionStack.clear();
                continue;
            }
            if (tokens[0] == "end")
            {
                inGameMode = false;
                conditionStack.clear();
                continue;
            }
            if (!inGameMode)
                continue;

            if (tokens[0] == "if")
            {
                conditionStack.push_back({ std::vector<std::string>(tokens.begin() + 1, tokens.end()), false });
                continue;
            }
            if (tokens[0] == "elseif")
            {
                if (!conditionStack.empty())
                {
                    conditionStack.back().mCondition = std::vector<std::string>(tokens.begin() + 1, tokens.end());
                    conditionStack.back().mElseBranch = false;
                }
                continue;
            }
            if (tokens[0] == "else")
            {
                if (!conditionStack.empty())
                    conditionStack.back().mElseBranch = true;
                continue;
            }
            if (tokens[0] == "endif")
            {
                if (!conditionStack.empty())
                    conditionStack.pop_back();
                continue;
            }

            if (const std::optional<std::string> timer = parseTimerDecrement(tokens))
            {
                for (auto timerFrame = conditionStack.rbegin(); timerFrame != conditionStack.rend(); ++timerFrame)
                {
                    const std::optional<std::string> conditionalTimer = parseTimerPositiveCondition(timerFrame->mCondition);
                    if (!conditionalTimer || *conditionalTimer != *timer)
                        continue;
                    for (auto parent = std::next(timerFrame); parent != conditionStack.rend(); ++parent)
                    {
                        if (const std::optional<std::string> run = parseRunFlagCondition(parent->mCondition))
                        {
                            runVariables[*timer].insert(*run);
                            break;
                        }
                    }
                    break;
                }
                continue;
            }

            if (tokens.size() < 3 || tokens[0] != "setstage" || tokens[1] != canonicalQuest)
                continue;
            std::int32_t targetStage = 0;
            if (!parseInt(tokens[2], targetStage) || targetStage < 0 || targetStage > 255)
                continue;

            std::optional<std::string> timer;
            for (auto frame = conditionStack.rbegin(); frame != conditionStack.rend(); ++frame)
            {
                const std::optional<std::string> conditionalTimer = parseTimerPositiveCondition(frame->mCondition);
                if (frame->mElseBranch && conditionalTimer)
                {
                    timer = *conditionalTimer;
                    break;
                }
            }
            if (!timer)
                continue;

            std::optional<std::uint8_t> sourceStage;
            for (auto frame = conditionStack.rbegin(); frame != conditionStack.rend(); ++frame)
            {
                if (const std::optional<std::uint8_t> stage = parseStageCondition(frame->mCondition, canonicalQuest))
                {
                    sourceStage = *stage;
                    break;
                }
            }
            if (!sourceStage)
                continue;

            auto& timerTransitions = transitions[*timer];
            const auto [existing, inserted]
                = timerTransitions.emplace(*sourceStage, static_cast<std::uint8_t>(targetStage));
            if (!inserted && existing->second != static_cast<std::uint8_t>(targetStage))
                ambiguousTransitions.insert(*timer);
        }

        std::vector<ESM4AuthoredGameModeTimer> result;
        for (const auto& [timerVariable, candidates] : runVariables)
        {
            const auto transition = transitions.find(timerVariable);
            if (candidates.size() != 1 || transition == transitions.end() || transition->second.empty()
                || ambiguousTransitions.contains(timerVariable))
                continue;
            result.push_back({ *candidates.begin(), timerVariable, transition->second });
        }
        return result;
    }

    void ESM4QuestRuntime::update(float duration, bool paused)
    {
        if (duration <= 0.f || !std::isfinite(duration) || mStore == nullptr)
            return;

        // MenuMode is distinct from ordinary world time.  It is authored to
        // run while character-generation UI is open, so it must be evaluated
        // before the paused-world early return.
        for (const auto& [questId, source] : mAuthoredMenuModeSources)
        {
            const ESM4::Quest* const quest = mStore->get<ESM4::Quest>().search(ESM::RefId(questId));
            ESM4QuestState* const state = quest != nullptr ? findState(*quest) : nullptr;
            if (state == nullptr || (state->mFlags & ESM4QuestState::Flag_Running) == 0
                || (state->mFlags & (ESM4QuestState::Flag_Completed | ESM4QuestState::Flag_Failed)) != 0)
                continue;
            executeStageSource(source, questId, duration, "menumode");
        }

        if (paused)
            return;

        // Run the declared GameMode source against the quest's actual local
        // variables.  A compiled timer remains only as a fallback for a
        // source the narrow interpreter did not index; never apply both.
        for (const auto& [questId, source] : mAuthoredGameModeSources)
        {
            const ESM4::Quest* const quest = mStore->get<ESM4::Quest>().search(ESM::RefId(questId));
            ESM4QuestState* const state = quest != nullptr ? findState(*quest) : nullptr;
            if (state == nullptr || (state->mFlags & ESM4QuestState::Flag_Running) == 0
                || (state->mFlags & (ESM4QuestState::Flag_Completed | ESM4QuestState::Flag_Failed)) != 0)
                continue;
            executeStageSource(source, questId, duration, "gamemode");
        }

        for (const auto& [questId, timers] : mAuthoredGameModeTimers)
        {
            if (mAuthoredGameModeSources.contains(questId))
                continue;
            const ESM4::Quest* const quest = mStore != nullptr ? mStore->get<ESM4::Quest>().search(ESM::RefId(questId)) : nullptr;
            ESM4QuestState* const state = quest != nullptr ? findState(*quest) : nullptr;
            if (state == nullptr || (state->mFlags & ESM4QuestState::Flag_Running) == 0
                || (state->mFlags & (ESM4QuestState::Flag_Completed | ESM4QuestState::Flag_Failed)) != 0)
                continue;

            for (const ESM4AuthoredGameModeTimer& timer : timers)
            {
                const auto run = state->mVariables.find(timer.mRunVariable);
                const auto countdown = state->mVariables.find(timer.mTimerVariable);
                if (run == state->mVariables.end() || countdown == state->mVariables.end() || run->second == 0.f)
                    continue;

                if (countdown->second > 0.f)
                {
                    countdown->second = std::max(0.f, countdown->second - duration);
                    continue;
                }

                const auto transition = timer.mStageTransitions.find(state->mCurrentStage);
                if (transition == timer.mStageTransitions.end())
                    continue;
                if (!setStage(questId, transition->second))
                    Log(Debug::Warning) << "Fallout/ESM4 behavior: authored timer transition failed quest="
                                        << quest->mEditorId << " stage=" << static_cast<unsigned int>(state->mCurrentStage)
                                        << " target=" << static_cast<unsigned int>(transition->second);
                break;
            }
        }

        MWBase::World* const world = MWBase::Environment::tryGetWorld();
        if (world == nullptr)
            return;
        const MWWorld::Ptr player = world->getPlayerPtr();
        MWWorld::CellStore* const playerCell = player.isEmpty() ? nullptr : player.getCell();
        if (playerCell == nullptr || playerCell->getCell() == nullptr)
            return;
        const ESM::RefId playerCellId = playerCell->getCell()->getId();
        const osg::Vec3f playerPosition = player.getRefData().getPosition().asVec3();

        for (auto& [_, state] : mReferenceScriptStates)
        {
            if (!state.mHasTriggerEnter || state.mCell != playerCellId)
            {
                state.mPlayerWasInside = false;
                continue;
            }
            const float radius = state.mTriggerRadius * std::abs(state.mScale);
            if (!(radius > 0.f) || !std::isfinite(radius))
            {
                state.mPlayerWasInside = false;
                continue;
            }
            const bool inside = (playerPosition - state.mPosition.asVec3()).length2() <= radius * radius;
            const bool entered = inside && !state.mPlayerWasInside;
            state.mPlayerWasInside = inside;
            if (entered)
            {
                executeStageSource(state.mSource, {}, duration, "ontriggerenter", true);
                Log(Debug::Info) << "Fallout/ESM4 behavior: trigger-enter reference=" << state.mEditorId
                                 << " radius=" << radius;
            }
        }
    }

    const ESM4::Quest* ESM4QuestRuntime::resolveQuest(std::string_view id) const
    {
        if (mStore == nullptr || id.empty())
            return nullptr;

        if (id.starts_with("FormId:"))
        {
            try
            {
                return mStore->get<ESM4::Quest>().search(ESM::RefId::deserializeText(id));
            }
            catch (const std::exception&)
            {
                return nullptr;
            }
        }

        for (const ESM4::Quest& quest : mStore->get<ESM4::Quest>())
            if (Misc::StringUtils::ciEqual(quest.mEditorId, id))
                return &quest;
        return nullptr;
    }

    ESM4QuestState* ESM4QuestRuntime::findState(const ESM4::Quest& quest)
    {
        const auto found = mStates.find(quest.mId);
        return found != mStates.end() ? &found->second : nullptr;
    }

    const ESM4QuestState* ESM4QuestRuntime::findState(const ESM4::Quest& quest) const
    {
        const auto found = mStates.find(quest.mId);
        return found != mStates.end() ? &found->second : nullptr;
    }

    std::optional<ESM::FormId> ESM4QuestRuntime::resolveActorReference(std::string_view id) const
    {
        const std::string key = normaliseSourceToken(id);
        if (key.empty() || mAmbiguousActorReferenceEditorIds.contains(key))
            return std::nullopt;
        const auto found = mActorReferenceEditorIds.find(key);
        return found != mActorReferenceEditorIds.end() ? std::optional<ESM::FormId>(found->second) : std::nullopt;
    }

    bool ESM4QuestRuntime::executeAuthoredDialogue(ESM::FormId actorReference, std::string_view topicEditorId)
    {
        if (mStore == nullptr || actorReference.isZeroOrUnset())
            return false;

        const auto actorState = mActorReferenceStates.find(actorReference);
        if (actorState == mActorReferenceStates.end())
            return false;

        const ESM4::Dialogue* topic = nullptr;
        for (const ESM4::Dialogue& candidate : mStore->get<ESM4::Dialogue>())
        {
            if (Misc::StringUtils::ciEqual(candidate.mEditorId, topicEditorId))
            {
                topic = &candidate;
                break;
            }
        }
        if (topic == nullptr)
        {
            Log(Debug::Warning) << "Fallout/ESM4 behavior: Say could not resolve topic=" << topicEditorId;
            return false;
        }

        // INFO conditions are evaluated in the context of the actor selected
        // by the authored Say/SayTo command.  The generic quest evaluator is
        // deliberately actor-agnostic, so using it directly here rejected
        // valid Fallout dialogue such as GetIsID(DocMitchell).  Keep the
        // actor handling data-driven: the source supplies the actor
        // reference and the INFO supplies the condition.
        const auto compareDialogueValue = [](const ESM4::TargetCondition& condition, float actual) {
            if ((condition.condition & ESM4::CTF_UseGlobal) != 0)
                return false;

            switch (condition.condition & 0xe0)
            {
                case ESM4::CTF_EqualTo:
                    return actual == condition.comparison;
                case ESM4::CTF_NotEqualTo:
                    return actual != condition.comparison;
                case ESM4::CTF_GreaterThan:
                    return actual > condition.comparison;
                case ESM4::CTF_GrThOrEqTo:
                    return actual >= condition.comparison;
                case ESM4::CTF_LessThan:
                    return actual < condition.comparison;
                case ESM4::CTF_LeThOrEqTo:
                    return actual <= condition.comparison;
                default:
                    return false;
            }
        };
        const auto evaluateDialogueCondition = [this, &actorState, actorReference, &compareDialogueValue](
                                                     const ESM4::TargetCondition& condition) {
            switch (condition.functionIndex)
            {
                case ESM4::FUN_GetQuestRunning:
                case ESM4::FUN_GetStage:
                case ESM4::FUN_GetStageDone:
                case ESM4::FUN_GetGlobalValue:
                case ESM4::FUN_GetQuestVariable:
                case ESM4::FUN_GetQuestCompleted:
                case ESM4::FUN_GetObjectiveCompleted:
                case ESM4::FUN_GetObjectiveDisplayed:
                    return evaluateConditions({ condition });
                case ESM4::FUN_GetIsID:
                {
                    const ESM::FormId parameter = ESM::FormId::fromUint32(condition.param1);
                    return compareDialogueValue(
                        condition, (actorState->second.mBase == parameter || actorReference == parameter) ? 1.f : 0.f);
                }
                case ESM4::FUN_GetIsReference:
                    return compareDialogueValue(
                        condition, actorReference == ESM::FormId::fromUint32(condition.param1) ? 1.f : 0.f);
                case ESM4::FUN_GetInCell:
                    return compareDialogueValue(condition,
                        actorState->second.mCell == ESM::RefId(ESM::FormId::fromUint32(condition.param1)) ? 1.f : 0.f);
                default:
                    return false;
            }
        };
        const auto matchesDialogueConditions = [&evaluateDialogueCondition](
                                                const std::vector<ESM4::TargetCondition>& conditions) {
            for (std::size_t index = 0; index < conditions.size(); ++index)
            {
                bool groupResult = evaluateDialogueCondition(conditions[index]);
                while ((conditions[index].condition & ESM4::CTF_Combine) != 0 && index + 1 < conditions.size())
                {
                    ++index;
                    groupResult = groupResult || evaluateDialogueCondition(conditions[index]);
                }
                if (!groupResult)
                    return false;
            }
            return true;
        };

        const ESM4::DialogInfo* selected = nullptr;
        for (const ESM4::DialogInfo& info : mStore->get<ESM4::DialogInfo>())
        {
            if (info.mTopic != topic->mId)
                continue;
            if ((info.mInfoFlags & ESM4::INFO_SayOnce) != 0 && mSaidAuthoredDialogInfos.contains(info.mId))
                continue;
            if (!info.mSpeaker.isZeroOrUnset() && info.mSpeaker != actorState->second.mBase
                && info.mSpeaker != actorReference)
                continue;
            if (!info.mQuest.isZeroOrUnset())
            {
                const ESM4::Quest* const ownerQuest = mStore->get<ESM4::Quest>().search(ESM::RefId(info.mQuest));
                const ESM4QuestState* const ownerState = ownerQuest != nullptr ? findState(*ownerQuest) : nullptr;
                if (ownerQuest == nullptr || ownerState == nullptr
                    || (ownerState->mFlags & ESM4QuestState::Flag_Running) == 0
                    || !matchesDialogueConditions(ownerQuest->mTargetConditions))
                    continue;
            }
            if (!matchesDialogueConditions(info.mTargetConditions))
                continue;
            selected = &info;
            break;
        }
        if (selected == nullptr)
        {
            Log(Debug::Warning) << "Fallout/ESM4 behavior: Say had no eligible INFO actor="
                                << actorState->second.mEditorId << " topic=" << topic->mEditorId;
            return false;
        }

        if ((selected->mInfoFlags & ESM4::INFO_SayOnce) != 0)
            mSaidAuthoredDialogInfos.insert(selected->mId);

        if (!selected->mResponse.empty())
        {
            if (MWBase::WindowManager* const windowManager = MWBase::Environment::tryGetWindowManager())
                windowManager->scheduleMessageBox(selected->mResponse, MWGui::ShowInDialogueMode_Never);
        }
        if (!selected->mScript.scriptSource.empty())
            executeStageSource(selected->mScript.scriptSource);
        if (!selected->mEndScript.scriptSource.empty())
            executeStageSource(selected->mEndScript.scriptSource);

        Log(Debug::Info) << "Fallout/ESM4 behavior: Say actor=" << actorState->second.mEditorId
                         << " topic=" << topic->mEditorId << " info=" << ESM::RefId(selected->mId);
        return true;
    }

    bool ESM4QuestRuntime::startQuest(std::string_view id)
    {
        const ESM4::Quest* quest = resolveQuest(id);
        return quest != nullptr && startQuest(quest->mId);
    }

    bool ESM4QuestRuntime::startQuest(ESM::FormId id)
    {
        const ESM4::Quest* quest = mStore != nullptr ? mStore->get<ESM4::Quest>().search(ESM::RefId(id)) : nullptr;
        ESM4QuestState* state = quest != nullptr ? findState(*quest) : nullptr;
        if (state == nullptr)
            return false;
        state->mFlags |= ESM4QuestState::Flag_Running;
        return true;
    }

    bool ESM4QuestRuntime::stopQuest(std::string_view id)
    {
        const ESM4::Quest* quest = resolveQuest(id);
        return quest != nullptr && stopQuest(quest->mId);
    }

    bool ESM4QuestRuntime::stopQuest(ESM::FormId id)
    {
        const ESM4::Quest* quest = mStore != nullptr ? mStore->get<ESM4::Quest>().search(ESM::RefId(id)) : nullptr;
        ESM4QuestState* state = quest != nullptr ? findState(*quest) : nullptr;
        if (state == nullptr)
            return false;
        state->mFlags &= ~ESM4QuestState::Flag_Running;
        return true;
    }

    bool ESM4QuestRuntime::completeQuest(std::string_view id)
    {
        const ESM4::Quest* quest = resolveQuest(id);
        return quest != nullptr && completeQuest(quest->mId);
    }

    bool ESM4QuestRuntime::completeQuest(ESM::FormId id)
    {
        const ESM4::Quest* quest = mStore != nullptr ? mStore->get<ESM4::Quest>().search(ESM::RefId(id)) : nullptr;
        ESM4QuestState* state = quest != nullptr ? findState(*quest) : nullptr;
        if (state == nullptr)
            return false;
        state->mFlags |= ESM4QuestState::Flag_Completed;
        state->mFlags &= ~(ESM4QuestState::Flag_Running | ESM4QuestState::Flag_Failed);
        return true;
    }

    bool ESM4QuestRuntime::failQuest(std::string_view id)
    {
        const ESM4::Quest* quest = resolveQuest(id);
        ESM4QuestState* state = quest != nullptr ? findState(*quest) : nullptr;
        if (state == nullptr)
            return false;
        state->mFlags |= ESM4QuestState::Flag_Failed;
        state->mFlags &= ~(ESM4QuestState::Flag_Running | ESM4QuestState::Flag_Completed);
        return true;
    }

    bool ESM4QuestRuntime::setStage(std::string_view id, std::uint8_t stage)
    {
        const ESM4::Quest* quest = resolveQuest(id);
        return quest != nullptr && setStage(quest->mId, stage);
    }

    bool ESM4QuestRuntime::prepareStageScript(const ESM4::ScriptDefinition& script, CompiledStageScript& prepared) const
    {
        prepared = {};
        if (script.compiledData.empty())
        {
            prepared.mUseSourceFallback = true;
            return true;
        }

        std::vector<ESM4::ScriptBytecodeInstruction> instructions;
        if (!ESM4::decodeFalloutScriptBytecode(script.compiledData, instructions).succeeded())
            return false;

        for (const ESM4::ScriptBytecodeInstruction& instruction : instructions)
        {
            if (instruction.callingReferenceIndex
                && (*instruction.callingReferenceIndex == 0
                    || *instruction.callingReferenceIndex > script.references.size()))
                return false;

            if (instruction.opcode != 0x1036 && instruction.opcode != 0x1037
                && instruction.opcode != 0x1039 && instruction.opcode != 0x1071 && instruction.opcode != 0x11a2
                && instruction.opcode != 0x11a3 && instruction.opcode != 0x11dd)
            {
                prepared.mUseSourceFallback = true;
                prepared.mUnsupportedOpcodes.push_back(instruction.opcode);
                continue;
            }
            if (instruction.callingReferenceIndex)
                return false;

            std::vector<ESM4::ScriptBytecodeArgument> arguments;
            if (!ESM4::decodeFalloutScriptArguments(instruction.arguments, script.references, arguments).succeeded())
                return false;

            if (instruction.opcode == 0x11a2 || instruction.opcode == 0x11a3)
            {
                if (arguments.size() != 3)
                    return false;
                const ESM::FormId* questId = std::get_if<ESM::FormId>(&arguments[0]);
                const std::int32_t* objective = std::get_if<std::int32_t>(&arguments[1]);
                const std::int32_t* displayed = std::get_if<std::int32_t>(&arguments[2]);
                if (questId == nullptr || objective == nullptr || displayed == nullptr || mStore == nullptr)
                    return false;
                const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(*questId));
                const ESM4QuestState* state = quest != nullptr ? findState(*quest) : nullptr;
                if (state == nullptr || !state->mObjectiveStatus.contains(*objective))
                    return false;
                const CompiledQuestCommandType type = instruction.opcode == 0x11a2
                    ? CompiledQuestCommandType::SetObjectiveCompleted
                    : CompiledQuestCommandType::SetObjectiveDisplayed;
                prepared.mCommands.push_back({ type, *questId, *objective, *displayed != 0 });
            }
            else if (instruction.opcode == 0x1039) // SetStage Quest Stage
            {
                if (arguments.size() != 2)
                    return false;
                const ESM::FormId* questId = std::get_if<ESM::FormId>(&arguments[0]);
                const std::int32_t* stage = std::get_if<std::int32_t>(&arguments[1]);
                if (questId == nullptr || stage == nullptr || *stage < 0 || *stage > 255 || mStore == nullptr)
                    return false;
                const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(*questId));
                if (quest == nullptr || findState(*quest) == nullptr
                    || std::none_of(quest->mStages.begin(), quest->mStages.end(), [stage](const ESM4::QuestStage& value) {
                           return value.mIndex == *stage;
                       }))
                    return false;
                prepared.mCommands.push_back({ CompiledQuestCommandType::SetStage, *questId, 0, false,
                    static_cast<std::uint8_t>(*stage) });
            }
            else
            {
                if (arguments.size() != 1)
                    return false;
                const ESM::FormId* questId = std::get_if<ESM::FormId>(&arguments[0]);
                if (questId == nullptr || mStore == nullptr)
                    return false;
                const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(*questId));
                if (quest == nullptr || findState(*quest) == nullptr)
                    return false;
                CompiledQuestCommandType type = CompiledQuestCommandType::ForceActiveQuest;
                switch (instruction.opcode)
                {
                    case 0x1036:
                        type = CompiledQuestCommandType::StartQuest;
                        break;
                    case 0x1037:
                        type = CompiledQuestCommandType::StopQuest;
                        break;
                    case 0x1071:
                        type = CompiledQuestCommandType::CompleteQuest;
                        break;
                    case 0x11dd:
                        break;
                    default:
                        throw std::logic_error("unhandled preflighted Fallout quest opcode");
                }
                prepared.mCommands.push_back({ type, *questId, 0, false });
            }
        }

        // Never mix native prefix execution with an unsupported command. The already existing
        // source path is retained only as a whole-script compatibility fallback.
        if (prepared.mUseSourceFallback)
            prepared.mCommands.clear();
        return true;
    }

    bool ESM4QuestRuntime::stageContainsCompiledSetStage(const ESM4::QuestStage& stage) const
    {
        for (const ESM4::QuestStageEntry& entry : stage.mEntries)
        {
            if (entry.mScript.compiledData.empty())
                continue;
            std::vector<ESM4::ScriptBytecodeInstruction> instructions;
            if (!ESM4::decodeFalloutScriptBytecode(entry.mScript.compiledData, instructions).succeeded())
                continue;
            if (std::any_of(instructions.begin(), instructions.end(),
                    [](const ESM4::ScriptBytecodeInstruction& instruction) { return instruction.opcode == 0x1039; }))
                return true;
        }
        return false;
    }

    bool ESM4QuestRuntime::areCompiledStageConditionsPure(
        const std::vector<ESM4::TargetCondition>& conditions) const
    {
        for (const ESM4::TargetCondition& condition : conditions)
        {
            if (condition.runOn != 0)
                return false;
            switch (condition.functionIndex)
            {
                case ESM4::FUN_GetQuestRunning:
                case ESM4::FUN_GetStage:
                case ESM4::FUN_GetStageDone:
                case ESM4::FUN_GetGlobalValue:
                case ESM4::FUN_GetQuestCompleted:
                case ESM4::FUN_GetQuestVariable:
                case ESM4::FUN_GetObjectiveCompleted:
                case ESM4::FUN_GetObjectiveDisplayed:
                    break;
                default:
                    return false;
            }
            switch (condition.condition & 0xe0)
            {
                case ESM4::CTF_EqualTo:
                case ESM4::CTF_NotEqualTo:
                case ESM4::CTF_GreaterThan:
                case ESM4::CTF_GrThOrEqTo:
                case ESM4::CTF_LessThan:
                case ESM4::CTF_LeThOrEqTo:
                    break;
                default:
                    return false;
            }
        }
        return true;
    }

    bool ESM4QuestRuntime::preflightPureCompiledStage(
        ESM::FormId id, std::uint8_t stageIndex, std::vector<CompiledStageKey>& stack) const
    {
        if (mStore == nullptr)
            return false;
        const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(id));
        const auto stateIt = mStates.find(id);
        if (quest == nullptr || stateIt == mStates.end())
            return false;
        const auto stage = std::find_if(quest->mStages.begin(), quest->mStages.end(),
            [stageIndex](const ESM4::QuestStage& value) { return value.mIndex == stageIndex; });
        if (stage == quest->mStages.end())
            return false;

        const bool repeatedStages = (stateIt->second.mFlags & ESM4QuestState::Flag_AllowRepeatedStages) != 0;
        const auto done = stateIt->second.mStageDone.find(stage->mIndex);
        if (done != stateIt->second.mStageDone.end() && done->second && !repeatedStages)
            return true;

        const CompiledStageKey key{ id, stageIndex };
        if (stack.size() >= CompiledStageRecursionLimit
            || std::find(stack.begin(), stack.end(), key) != stack.end())
            return false;

        stack.push_back(key);
        bool valid = true;
        for (const ESM4::QuestStageEntry& entry : stage->mEntries)
        {
            if (!areCompiledStageConditionsPure(entry.mConditions))
            {
                valid = false;
                break;
            }
            CompiledStageScript prepared;
            if (!prepareStageScript(entry.mScript, prepared) || prepared.mUseSourceFallback)
            {
                valid = false;
                break;
            }
            for (const CompiledQuestCommand& command : prepared.mCommands)
            {
                if (command.mType == CompiledQuestCommandType::SetStage
                    && !preflightPureCompiledStage(command.mQuest, command.mStage, stack))
                {
                    valid = false;
                    break;
                }
            }
            if (!valid)
                break;
        }
        stack.pop_back();
        return valid;
    }

    bool ESM4QuestRuntime::executePureCompiledCommand(
        const CompiledQuestCommand& command, CompiledStageWorkingState& working)
    {
        if (command.mType == CompiledQuestCommandType::SetStage)
            return executePureCompiledStage(command.mQuest, command.mStage, working);

        const auto found = working.mStates.find(command.mQuest);
        if (found == working.mStates.end())
            return false;
        ESM4QuestState& state = found->second;
        switch (command.mType)
        {
            case CompiledQuestCommandType::StartQuest:
                state.mFlags |= ESM4QuestState::Flag_Running;
                return true;
            case CompiledQuestCommandType::StopQuest:
                state.mFlags &= ~ESM4QuestState::Flag_Running;
                return true;
            case CompiledQuestCommandType::CompleteQuest:
                state.mFlags |= ESM4QuestState::Flag_Completed;
                state.mFlags &= ~(ESM4QuestState::Flag_Running | ESM4QuestState::Flag_Failed);
                return true;
            case CompiledQuestCommandType::SetStage:
                return false;
            case CompiledQuestCommandType::SetObjectiveCompleted:
            {
                const auto objective = state.mObjectiveStatus.find(command.mObjective);
                if (objective == state.mObjectiveStatus.end())
                    return false;
                if (command.mValue)
                    objective->second |= ESM4QuestState::Objective_Completed;
                else
                    objective->second &= ~ESM4QuestState::Objective_Completed;
                return true;
            }
            case CompiledQuestCommandType::SetObjectiveDisplayed:
            {
                const auto objective = state.mObjectiveStatus.find(command.mObjective);
                if (objective == state.mObjectiveStatus.end())
                    return false;
                if (command.mValue)
                {
                    objective->second |= ESM4QuestState::Objective_Displayed;
                    state.mFlags |= ESM4QuestState::Flag_ShownInPipBoy;
                }
                else
                    objective->second &= ~ESM4QuestState::Objective_Displayed;
                return true;
            }
            case CompiledQuestCommandType::ForceActiveQuest:
                state.mFlags |= ESM4QuestState::Flag_ShownInPipBoy;
                working.mActiveQuest = command.mQuest;
                return true;
        }
        return false;
    }

    bool ESM4QuestRuntime::executePureCompiledStage(
        ESM::FormId id, std::uint8_t stageIndex, CompiledStageWorkingState& working)
    {
        if (mStore == nullptr)
            return false;
        const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(id));
        const auto stateIt = working.mStates.find(id);
        if (quest == nullptr || stateIt == working.mStates.end())
            return false;
        const auto stage = std::find_if(quest->mStages.begin(), quest->mStages.end(),
            [stageIndex](const ESM4::QuestStage& value) { return value.mIndex == stageIndex; });
        if (stage == quest->mStages.end())
            return false;

        ESM4QuestState& state = stateIt->second;
        const bool repeatedStages = (state.mFlags & ESM4QuestState::Flag_AllowRepeatedStages) != 0;
        const auto done = state.mStageDone.find(stage->mIndex);
        if (done != state.mStageDone.end() && done->second && !repeatedStages)
            return true;

        const CompiledStageKey key{ id, stageIndex };
        if (working.mStack.size() >= CompiledStageRecursionLimit
            || std::find(working.mStack.begin(), working.mStack.end(), key) != working.mStack.end())
            return false;

        working.mStack.push_back(key);
        const bool wasRunning = (state.mFlags & ESM4QuestState::Flag_Running) != 0;
        state.mFlags |= ESM4QuestState::Flag_Running;
        state.mCurrentStage = stageIndex;
        state.mStageDone[stage->mIndex] = true;

        bool success = true;
        bool executedEntry = false;
        for (const ESM4::QuestStageEntry& entry : stage->mEntries)
        {
            if (!evaluateConditions(entry.mConditions, working.mStates, false))
                continue;
            CompiledStageScript prepared;
            if (!prepareStageScript(entry.mScript, prepared) || prepared.mUseSourceFallback)
            {
                success = false;
                break;
            }
            executedEntry = true;
            for (const CompiledQuestCommand& command : prepared.mCommands)
            {
                if (!executePureCompiledCommand(command, working))
                {
                    success = false;
                    break;
                }
            }
            if (!success)
                break;
            if ((entry.mFlags & ESM4::QuestStageEntry::Flag_CompleteQuest) != 0)
            {
                state.mFlags |= ESM4QuestState::Flag_Completed;
                state.mFlags &= ~ESM4QuestState::Flag_Running;
            }
            if ((entry.mFlags & ESM4::QuestStageEntry::Flag_FailQuest) != 0)
            {
                state.mFlags |= ESM4QuestState::Flag_Failed;
                state.mFlags &= ~ESM4QuestState::Flag_Running;
            }
        }

        if (success)
        {
            const std::string& title = quest->mQuestName.empty() ? quest->mEditorId : quest->mQuestName;
            std::string notification = wasRunning ? "Quest Updated: " : "Quest Added: ";
            notification += title;
            for (const ESM4::QuestStageEntry& entry : stage->mEntries)
            {
                if (!entry.mLogEntry.empty() && evaluateConditions(entry.mConditions, working.mStates, false))
                {
                    notification += "\n";
                    notification += entry.mLogEntry;
                    break;
                }
            }
            working.mEffects.push_back({ id, stageIndex, wasRunning, executedEntry, std::move(notification) });
        }
        working.mStack.pop_back();
        return success;
    }

    bool ESM4QuestRuntime::executeCompiledStageTransaction(ESM::FormId id, std::uint8_t stageIndex)
    {
        std::vector<CompiledStageKey> preflightStack;
        if (!preflightPureCompiledStage(id, stageIndex, preflightStack))
            return false;

        CompiledStageWorkingState working{ mStates, mActiveQuest };
        if (!executePureCompiledStage(id, stageIndex, working))
            return false;

        mStates.swap(working.mStates);
        mActiveQuest.swap(working.mActiveQuest);
        flushCompiledStageEffects(working.mEffects);
        return true;
    }

    void ESM4QuestRuntime::flushCompiledStageEffects(const std::vector<PendingStageEffect>& effects)
    {
        MWBase::WindowManager* windowManager = MWBase::Environment::tryGetWindowManager();
        for (const PendingStageEffect& effect : effects)
        {
            const ESM4::Quest* quest
                = mStore != nullptr ? mStore->get<ESM4::Quest>().search(ESM::RefId(effect.mQuest)) : nullptr;
            const ESM4QuestState* state = quest != nullptr ? findState(*quest) : nullptr;
            if (quest == nullptr || state == nullptr)
                continue;
            const auto done = state->mStageDone.find(effect.mStage);
            Log(Debug::Info) << "FNV/ESM4 behavior: SetStage quest=" << quest->mEditorId
                             << " form=" << ESM::RefId(quest->mId).serializeText()
                             << " stage=" << static_cast<unsigned int>(effect.mStage)
                             << " flags=" << static_cast<unsigned int>(state->mFlags)
                             << " done=" << (done != state->mStageDone.end() && done->second)
                             << " entryExecuted=" << effect.mEntryExecuted;
            if (windowManager == nullptr)
                continue;
            try
            {
                windowManager->scheduleMessageBox(effect.mNotification, MWGui::ShowInDialogueMode_Never);
                Log(Debug::Info) << "FNV/ESM4 behavior: queued quest notification quest=" << quest->mEditorId
                                 << " stage=" << static_cast<unsigned int>(effect.mStage)
                                 << " mode=" << (effect.mWasRunning ? "updated" : "added");
            }
            catch (...)
            {
                Log(Debug::Warning) << "FNV/ESM4 behavior: quest notification failed after committed SetStage quest="
                                    << quest->mEditorId
                                    << " stage=" << static_cast<unsigned int>(effect.mStage);
            }
        }
    }

    bool ESM4QuestRuntime::setStage(ESM::FormId id, std::uint8_t stageIndex)
    {
        if (mStore == nullptr)
            return false;
        const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(id));
        ESM4QuestState* state = quest != nullptr ? findState(*quest) : nullptr;
        if (state == nullptr)
            return false;

        const auto stage = std::find_if(quest->mStages.begin(), quest->mStages.end(),
            [stageIndex](const ESM4::QuestStage& value) { return value.mIndex == stageIndex; });

        const bool repeatedStages = (state->mFlags & ESM4QuestState::Flag_AllowRepeatedStages) != 0;
        if (state->mStageDone[stageIndex] && !repeatedStages)
            return true;

        // Fallout source can use an undeclared stage as a state-only handoff
        // between GameMode branches.  It is still a valid SetStage operation:
        // record it and run no stage-entry source rather than rejecting the
        // authored transition outright.
        if (stage == quest->mStages.end())
        {
            state->mFlags |= ESM4QuestState::Flag_Running;
            state->mCurrentStage = stageIndex;
            state->mStageDone[stageIndex] = true;
            Log(Debug::Info) << "Fallout/ESM4 behavior: SetStage state-only quest=" << quest->mEditorId
                             << " stage=" << static_cast<unsigned int>(stageIndex);
            return true;
        }

        // Keep the authored source as the lossless boundary when a compiled
        // SetStage transaction cannot be completed.  New Vegas' VCG00 stage
        // zero contains a chain that advances the opening and starts its Bink;
        // rejecting that transaction outright strands the player in the cell
        // and makes OpenMW fall back to its unrelated Morrowind new-game movie.
        bool useWholeStageSourceFallback = false;
        if (stageContainsCompiledSetStage(*stage))
        {
            if (executeCompiledStageTransaction(id, stageIndex))
                return true;
            useWholeStageSourceFallback = true;
            Log(Debug::Info) << "Fallout/ESM4 behavior: compiled SetStage transaction deferred to authored source quest="
                             << quest->mEditorId << " stage=" << static_cast<unsigned int>(stageIndex);
        }

        struct PreparedEntry
        {
            const ESM4::QuestStageEntry* mEntry = nullptr;
            CompiledStageScript mScript;
        };
        std::vector<PreparedEntry> preparedEntries;
        for (const ESM4::QuestStageEntry& entry : stage->mEntries)
        {
            if (!evaluateConditions(entry.mConditions))
                continue;
            CompiledStageScript prepared;
            if (!prepareStageScript(entry.mScript, prepared))
            {
                Log(Debug::Warning) << "FNV/ESM4 behavior: malformed SCDA failed closed quest=" << quest->mEditorId
                                    << " stage=" << static_cast<unsigned int>(stageIndex);
                return false;
            }
            if (useWholeStageSourceFallback)
            {
                if (entry.mScript.scriptSource.empty())
                {
                    Log(Debug::Warning)
                        << "Fallout/ESM4 behavior: compiled SetStage transaction has no authored source fallback quest="
                        << quest->mEditorId << " stage=" << static_cast<unsigned int>(stageIndex);
                    return false;
                }
                prepared.mUseSourceFallback = true;
                prepared.mCommands.clear();
            }
            preparedEntries.push_back({ &entry, std::move(prepared) });
        }

        const bool wasRunning = (state->mFlags & ESM4QuestState::Flag_Running) != 0;
        state->mFlags |= ESM4QuestState::Flag_Running;
        state->mCurrentStage = stageIndex;
        state->mStageDone[stage->mIndex] = true;

        const bool executedEntry = !preparedEntries.empty();
        for (const PreparedEntry& preparedEntry : preparedEntries)
        {
            const ESM4::QuestStageEntry& entry = *preparedEntry.mEntry;
            for (const std::uint16_t opcode : preparedEntry.mScript.mUnsupportedOpcodes)
            {
                mUnsupportedCompiledOpcodes.push_back(opcode);
                Log(Debug::Warning) << "FNV/ESM4 behavior: unsupported SCDA opcode="
                                    << static_cast<unsigned int>(opcode)
                                    << " using temporary whole-SCTX fallback quest=" << quest->mEditorId
                                    << " stage=" << static_cast<unsigned int>(stageIndex);
            }
            if (preparedEntry.mScript.mUseSourceFallback)
                executeStageSource(entry.mScript.scriptSource, quest->mId);
            else
            {
                for (const CompiledQuestCommand& command : preparedEntry.mScript.mCommands)
                {
                    bool executed = false;
                    switch (command.mType)
                    {
                        case CompiledQuestCommandType::StartQuest:
                            executed = startQuest(command.mQuest);
                            break;
                        case CompiledQuestCommandType::StopQuest:
                            executed = stopQuest(command.mQuest);
                            break;
                        case CompiledQuestCommandType::CompleteQuest:
                            executed = completeQuest(command.mQuest);
                            break;
                        case CompiledQuestCommandType::SetStage:
                            throw std::logic_error("compiled SetStage escaped its transaction");
                        case CompiledQuestCommandType::SetObjectiveCompleted:
                            executed = setObjectiveCompleted(command.mQuest, command.mObjective, command.mValue);
                            break;
                        case CompiledQuestCommandType::SetObjectiveDisplayed:
                            executed = setObjectiveDisplayed(command.mQuest, command.mObjective, command.mValue);
                            break;
                        case CompiledQuestCommandType::ForceActiveQuest:
                            executed = forceActiveQuest(command.mQuest);
                            break;
                    }
                    if (!executed)
                        throw std::logic_error("preflighted Fallout quest command became invalid during execution");
                }
            }
            if ((entry.mFlags & ESM4::QuestStageEntry::Flag_CompleteQuest) != 0)
            {
                state->mFlags |= ESM4QuestState::Flag_Completed;
                state->mFlags &= ~ESM4QuestState::Flag_Running;
            }
            if ((entry.mFlags & ESM4::QuestStageEntry::Flag_FailQuest) != 0)
            {
                state->mFlags |= ESM4QuestState::Flag_Failed;
                state->mFlags &= ~ESM4QuestState::Flag_Running;
            }
        }

        Log(Debug::Info) << "FNV/ESM4 behavior: SetStage quest=" << quest->mEditorId
                         << " form=" << ESM::RefId(quest->mId).serializeText()
                         << " stage=" << static_cast<unsigned int>(stageIndex)
                         << " flags=" << static_cast<unsigned int>(state->mFlags)
                         << " done=" << state->mStageDone[stage->mIndex] << " entryExecuted=" << executedEntry;

        if (MWBase::WindowManager* windowManager = MWBase::Environment::tryGetWindowManager())
        {
            const std::string& title = quest->mQuestName.empty() ? quest->mEditorId : quest->mQuestName;
            std::string notification = wasRunning ? "Quest Updated: " : "Quest Added: ";
            notification += title;
            for (const ESM4::QuestStageEntry& entry : stage->mEntries)
            {
                if (!entry.mLogEntry.empty() && evaluateConditions(entry.mConditions))
                {
                    notification += "\n";
                    notification += entry.mLogEntry;
                    break;
                }
            }
            windowManager->scheduleMessageBox(std::move(notification), MWGui::ShowInDialogueMode_Never);
            Log(Debug::Info) << "FNV/ESM4 behavior: queued quest notification quest=" << quest->mEditorId
                             << " stage=" << static_cast<unsigned int>(stageIndex)
                             << " mode=" << (wasRunning ? "updated" : "added");
        }
        return true;
    }

    std::optional<float> ESM4QuestRuntime::evaluateConditionValue(const ESM4::TargetCondition& condition)
    {
        return evaluateConditionValue(condition, mStates, true);
    }

    std::optional<float> ESM4QuestRuntime::evaluateConditionValue(
        const ESM4::TargetCondition& condition, const QuestStateMap& states, bool recordUnsupported)
    {
        if (mStore == nullptr)
            return std::nullopt;

        const ESM::FormId parameter = ESM::FormId::fromUint32(condition.param1);
        const auto findQuestState = [this, &states, parameter]() -> const ESM4QuestState* {
            const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(parameter));
            if (quest == nullptr)
                return nullptr;
            const auto found = states.find(quest->mId);
            return found != states.end() ? &found->second : nullptr;
        };

        switch (condition.functionIndex)
        {
            case ESM4::FUN_GetQuestRunning:
                if (const ESM4QuestState* state = findQuestState())
                    return (state->mFlags & ESM4QuestState::Flag_Running) != 0 ? 1.f : 0.f;
                return 0.f;
            case ESM4::FUN_GetStage:
                if (const ESM4QuestState* state = findQuestState())
                    return static_cast<float>(state->mCurrentStage);
                return 0.f;
            case ESM4::FUN_GetStageDone:
                if (const ESM4QuestState* state = findQuestState())
                {
                    const auto found = state->mStageDone.find(static_cast<std::int16_t>(condition.param2));
                    return found != state->mStageDone.end() && found->second ? 1.f : 0.f;
                }
                return 0.f;
            case ESM4::FUN_GetGlobalValue:
                if (const ESM4::GlobalVariable* global
                    = mStore->get<ESM4::GlobalVariable>().search(ESM::RefId(parameter)))
                {
                    if (mGlobals != nullptr && !global->mEditorId.empty())
                    {
                        const GlobalVariableName name{ global->mEditorId };
                        if (mGlobals->getType(name) != ' ')
                            return (*mGlobals)[name].getFloat();
                    }
                    return global->mValue;
                }
                return 0.f;
            case ESM4::FUN_GetQuestCompleted:
                if (const ESM4QuestState* state = findQuestState())
                    return (state->mFlags & ESM4QuestState::Flag_Completed) != 0 ? 1.f : 0.f;
                return 0.f;
            case ESM4::FUN_GetQuestVariable:
                if (const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(parameter)))
                    if (const auto state = states.find(quest->mId); state != states.end())
                        if (const ESM4::Script* script
                            = mStore->get<ESM4::Script>().search(ESM::RefId(quest->mQuestScript)))
                            for (const ESM4::ScriptLocalVariableData& variable : script->mScript.localVarData)
                                if (variable.index == condition.param2)
                                {
                                    const auto found = state->second.mVariables.find(
                                        Misc::StringUtils::lowerCase(variable.variableName));
                                    return found != state->second.mVariables.end() ? found->second : 0.f;
                                }
                return 0.f;
            case ESM4::FUN_GetObjectiveCompleted:
                if (const ESM4QuestState* state = findQuestState())
                {
                    const auto found = state->mObjectiveStatus.find(static_cast<std::int32_t>(condition.param2));
                    return found != state->mObjectiveStatus.end()
                            && (found->second & ESM4QuestState::Objective_Completed) != 0
                        ? 1.f
                        : 0.f;
                }
                return 0.f;
            case ESM4::FUN_GetObjectiveDisplayed:
                if (const ESM4QuestState* state = findQuestState())
                {
                    const auto found = state->mObjectiveStatus.find(static_cast<std::int32_t>(condition.param2));
                    return found != state->mObjectiveStatus.end()
                            && (found->second & ESM4QuestState::Objective_Displayed) != 0
                        ? 1.f
                        : 0.f;
                }
                return 0.f;
            default:
                if (recordUnsupported
                    && std::find(mUnsupportedConditionFunctions.begin(), mUnsupportedConditionFunctions.end(),
                           condition.functionIndex)
                        == mUnsupportedConditionFunctions.end())
                    mUnsupportedConditionFunctions.push_back(condition.functionIndex);
                return std::nullopt;
        }
    }

    bool ESM4QuestRuntime::evaluateConditions(const std::vector<ESM4::TargetCondition>& conditions)
    {
        return evaluateConditions(conditions, mStates, true);
    }

    bool ESM4QuestRuntime::evaluateConditions(const std::vector<ESM4::TargetCondition>& conditions,
        const QuestStateMap& states, bool recordUnsupported)
    {
        for (std::size_t i = 0; i < conditions.size(); ++i)
        {
            const auto evaluate = [this, &states, recordUnsupported](const ESM4::TargetCondition& condition) {
                const std::optional<float> actual = evaluateConditionValue(condition, states, recordUnsupported);
                if (!actual)
                    return false;

                float expected = condition.comparison;
                if ((condition.condition & ESM4::CTF_UseGlobal) != 0)
                {
                    ESM4::TargetCondition globalCondition;
                    globalCondition.functionIndex = ESM4::FUN_GetGlobalValue;
                    globalCondition.param1 = std::bit_cast<std::uint32_t>(condition.comparison);
                    const std::optional<float> globalValue
                        = evaluateConditionValue(globalCondition, states, recordUnsupported);
                    if (!globalValue)
                        return false;
                    expected = *globalValue;
                }

                switch (condition.condition & 0xe0)
                {
                    case ESM4::CTF_EqualTo:
                        return *actual == expected;
                    case ESM4::CTF_NotEqualTo:
                        return *actual != expected;
                    case ESM4::CTF_GreaterThan:
                        return *actual > expected;
                    case ESM4::CTF_GrThOrEqTo:
                        return *actual >= expected;
                    case ESM4::CTF_LessThan:
                        return *actual < expected;
                    case ESM4::CTF_LeThOrEqTo:
                        return *actual <= expected;
                    default:
                        return false;
                }
            };

            bool groupResult = evaluate(conditions[i]);
            while ((conditions[i].condition & ESM4::CTF_Combine) != 0 && i + 1 < conditions.size())
            {
                ++i;
                if (!groupResult)
                    groupResult = evaluate(conditions[i]);
            }
            if (!groupResult)
                return false;
        }
        return true;
    }

    bool ESM4QuestRuntime::isStateDirty(ESM::FormId id, const ESM4QuestState& state) const
    {
        if (mActiveQuest == id || mStore == nullptr)
            return true;

        const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(id));
        if (quest == nullptr || state.mFlags != quest->mData.flags || state.mCurrentStage != 0)
            return true;
        if (std::any_of(
                state.mStageDone.begin(), state.mStageDone.end(), [](const auto& value) { return value.second; }))
            return true;
        return std::any_of(state.mObjectiveStatus.begin(), state.mObjectiveStatus.end(),
                   [](const auto& value) { return value.second != 0; })
            || std::any_of(state.mVariables.begin(), state.mVariables.end(),
                [](const auto& value) { return value.second != 0.f; });
    }

    int ESM4QuestRuntime::countSavedGameRecords() const
    {
        return static_cast<int>(std::count_if(mStates.begin(), mStates.end(),
            [this](const auto& value) { return isStateDirty(value.first, value.second); }));
    }

    void ESM4QuestRuntime::write(ESM::ESMWriter& writer) const
    {
        for (const auto& [id, state] : mStates)
        {
            if (!isStateDirty(id, state))
                continue;

            writer.startRecord(ESM::REC_FQST);
            writer.writeFormId(id, true, "FORM");
            writer.writeHNT("FLAG", state.mFlags);
            writer.writeHNT("STAG", state.mCurrentStage);
            writer.writeHNT("ACTV", static_cast<std::uint8_t>(mActiveQuest == id));

            const std::uint32_t doneCount = static_cast<std::uint32_t>(std::count_if(
                state.mStageDone.begin(), state.mStageDone.end(), [](const auto& value) { return value.second; }));
            writer.writeHNT("DNCT", doneCount);
            for (const auto& [stage, done] : state.mStageDone)
                if (done)
                    writer.writeHNT("DONE", stage);

            const std::uint32_t objectiveCount
                = static_cast<std::uint32_t>(std::count_if(state.mObjectiveStatus.begin(), state.mObjectiveStatus.end(),
                    [](const auto& value) { return value.second != 0; }));
            writer.writeHNT("OBCT", objectiveCount);
            for (const auto& [index, status] : state.mObjectiveStatus)
                if (status != 0)
                {
                    writer.writeHNT("OIDX", index);
                    writer.writeHNT("OFLG", status);
                }
            for (const auto& [name, value] : state.mVariables)
                if (value != 0.f)
                {
                    writer.writeHNString("VNAM", name);
                    writer.writeHNT("VVAL", value);
                }
            writer.endRecord(ESM::REC_FQST);
        }
    }

    void ESM4QuestRuntime::readRecord(ESM::ESMReader& reader)
    {
        ESM::FormId id = reader.getFormId(true, "FORM");
        const bool contentAvailable = reader.applyContentFileMapping(id);
        std::uint8_t flags = 0;
        std::uint8_t stage = 0;
        std::uint8_t active = 0;
        reader.getHNT(flags, "FLAG");
        reader.getHNT(stage, "STAG");
        reader.getHNT(active, "ACTV");

        std::uint32_t doneCount = 0;
        reader.getHNT(doneCount, "DNCT");
        if (doneCount > 65536)
            throw std::runtime_error("Fallout quest save has an invalid completed-stage count");
        std::vector<std::int16_t> doneStages(doneCount);
        for (std::int16_t& doneStage : doneStages)
            reader.getHNT(doneStage, "DONE");

        std::uint32_t objectiveCount = 0;
        reader.getHNT(objectiveCount, "OBCT");
        if (objectiveCount > 65536)
            throw std::runtime_error("Fallout quest save has an invalid objective count");
        std::vector<std::pair<std::int32_t, std::uint8_t>> objectives(objectiveCount);
        for (auto& [index, status] : objectives)
        {
            reader.getHNT(index, "OIDX");
            reader.getHNT(status, "OFLG");
        }

        const auto found = contentAvailable ? mStates.find(id) : mStates.end();
        ESM4QuestState* state = found != mStates.end() ? &found->second : nullptr;
        if (state == nullptr)
            return;
        state->mFlags = flags;
        state->mCurrentStage = stage;
        for (auto& [_, done] : state->mStageDone)
            done = false;
        for (const std::int16_t doneStage : doneStages)
            state->mStageDone[doneStage] = true;
        for (auto& [_, status] : state->mObjectiveStatus)
            status = 0;
        for (const auto& [index, status] : objectives)
            state->mObjectiveStatus[index] = status;
        for (auto& [_, value] : state->mVariables)
            value = 0.f;
        while (reader.isNextSub("VNAM"))
        {
            const std::string name = reader.getHString();
            float value = 0.f;
            reader.getHNT(value, "VVAL");
            if (const auto variable = state->mVariables.find(Misc::StringUtils::lowerCase(name));
                variable != state->mVariables.end())
                variable->second = value;
        }
        if (active != 0)
            mActiveQuest = id;

        const ESM4::Quest* quest = mStore != nullptr ? mStore->get<ESM4::Quest>().search(ESM::RefId(id)) : nullptr;
        Log(Debug::Info) << "FNV/ESM4 behavior: LoadedQuestState quest="
                         << (quest != nullptr ? quest->mEditorId : std::string("<missing>"))
                         << " form=" << ESM::RefId(id).serializeText()
                         << " stage=" << static_cast<unsigned int>(state->mCurrentStage)
                         << " flags=" << static_cast<unsigned int>(state->mFlags)
                         << " active=" << static_cast<unsigned int>(active) << " doneStages=" << doneStages.size()
                         << " objectives=" << objectives.size();
    }

    bool ESM4QuestRuntime::setObjectiveDisplayed(std::string_view id, std::int32_t objective, bool displayed)
    {
        const ESM4::Quest* quest = resolveQuest(id);
        return quest != nullptr && setObjectiveDisplayed(quest->mId, objective, displayed);
    }

    bool ESM4QuestRuntime::setObjectiveDisplayed(ESM::FormId id, std::int32_t objective, bool displayed)
    {
        const ESM4::Quest* quest = mStore != nullptr ? mStore->get<ESM4::Quest>().search(ESM::RefId(id)) : nullptr;
        ESM4QuestState* state = quest != nullptr ? findState(*quest) : nullptr;
        if (state == nullptr || !state->mObjectiveStatus.contains(objective))
            return false;
        if (displayed)
        {
            state->mObjectiveStatus[objective] |= ESM4QuestState::Objective_Displayed;
            state->mFlags |= ESM4QuestState::Flag_ShownInPipBoy;
        }
        else
            state->mObjectiveStatus[objective] &= ~ESM4QuestState::Objective_Displayed;
        return true;
    }

    bool ESM4QuestRuntime::setObjectiveCompleted(std::string_view id, std::int32_t objective, bool completed)
    {
        const ESM4::Quest* quest = resolveQuest(id);
        return quest != nullptr && setObjectiveCompleted(quest->mId, objective, completed);
    }

    bool ESM4QuestRuntime::setObjectiveCompleted(ESM::FormId id, std::int32_t objective, bool completed)
    {
        const ESM4::Quest* quest = mStore != nullptr ? mStore->get<ESM4::Quest>().search(ESM::RefId(id)) : nullptr;
        ESM4QuestState* state = quest != nullptr ? findState(*quest) : nullptr;
        if (state == nullptr || !state->mObjectiveStatus.contains(objective))
            return false;
        if (completed)
            state->mObjectiveStatus[objective] |= ESM4QuestState::Objective_Completed;
        else
            state->mObjectiveStatus[objective] &= ~ESM4QuestState::Objective_Completed;
        return true;
    }

    bool ESM4QuestRuntime::forceActiveQuest(std::string_view id)
    {
        const ESM4::Quest* quest = resolveQuest(id);
        return quest != nullptr && forceActiveQuest(quest->mId);
    }

    bool ESM4QuestRuntime::forceActiveQuest(ESM::FormId id)
    {
        const ESM4::Quest* quest = mStore != nullptr ? mStore->get<ESM4::Quest>().search(ESM::RefId(id)) : nullptr;
        ESM4QuestState* state = quest != nullptr ? findState(*quest) : nullptr;
        if (state == nullptr)
            return false;
        state->mFlags |= ESM4QuestState::Flag_ShownInPipBoy;
        mActiveQuest = quest->mId;
        return true;
    }

    bool ESM4QuestRuntime::setQuestVariable(std::string_view id, std::string_view variable, float value)
    {
        const ESM4::Quest* quest = resolveQuest(id);
        ESM4QuestState* state = quest != nullptr ? findState(*quest) : nullptr;
        if (state == nullptr)
            return false;
        const auto found = state->mVariables.find(Misc::StringUtils::lowerCase(variable));
        if (found == state->mVariables.end())
            return false;
        found->second = value;
        Log(Debug::Info) << "FNV/ESM4 behavior: SetQuestVariable quest=" << quest->mEditorId
                         << " variable=" << variable << " value=" << value;
        return true;
    }

    void ESM4QuestRuntime::executeStageSource(std::string_view source, std::optional<ESM::FormId> ownerQuest,
        float secondsPassed, std::string_view selectedBlock, bool actionReferenceIsPlayer)
    {
        struct ConditionalFrame
        {
            bool mParentActive = false;
            bool mBranchTaken = false;
            bool mActive = false;
        };

        const auto evaluateCondition = [this, ownerQuest, actionReferenceIsPlayer, secondsPassed](
                                           const std::vector<std::string>& tokens) -> bool {
            const auto variableValue = [this, ownerQuest](std::string_view variable) -> std::optional<float> {
                if (!ownerQuest)
                    return std::nullopt;
                const auto state = mStates.find(*ownerQuest);
                if (state == mStates.end())
                    return std::nullopt;
                const auto value = state->second.mVariables.find(normaliseScriptVariable(variable));
                return value != state->second.mVariables.end() ? std::optional<float>(value->second) : std::nullopt;
            };

            const auto comparison = [](float left, std::string_view operation, float right) {
                if (operation == "==")
                    return left == right;
                if (operation == "!=")
                    return left != right;
                if (operation == ">")
                    return left > right;
                if (operation == ">=")
                    return left >= right;
                if (operation == "<")
                    return left < right;
                if (operation == "<=")
                    return left <= right;
                return false;
            };

            const auto evaluateTerm = [this, &variableValue, &comparison, actionReferenceIsPlayer, secondsPassed](
                                          const std::vector<std::string>& term) -> bool {
                if (term.empty())
                    return false;

                std::optional<float> left;
                std::size_t next = 0;
                if (term[0] == "getstage" && term.size() >= 2)
                {
                    const ESM4QuestState* const state = search(term[1]);
                    left = state != nullptr ? static_cast<float>(state->mCurrentStage) : 0.f;
                    next = 2;
                }
                else if (term[0] == "getstagedone" && term.size() >= 3)
                {
                    std::int32_t stage = 0;
                    const ESM4QuestState* const state = search(term[1]);
                    if (!parseInt(term[2], stage))
                        return false;
                    bool done = false;
                    if (state != nullptr)
                    {
                        const auto found = state->mStageDone.find(static_cast<std::int16_t>(stage));
                        done = found != state->mStageDone.end() && found->second;
                    }
                    left = done ? 1.f : 0.f;
                    next = 3;
                }
                else if ((term[0] == "getobjectivedisplayed" || term[0] == "getobjectivecompleted")
                    && term.size() >= 3)
                {
                    std::int32_t objective = 0;
                    const ESM4QuestState* const state = search(term[1]);
                    if (!parseInt(term[2], objective))
                        return false;
                    const std::uint8_t flag = term[0] == "getobjectivedisplayed"
                        ? ESM4QuestState::Objective_Displayed
                        : ESM4QuestState::Objective_Completed;
                    bool hasFlag = false;
                    if (state != nullptr)
                    {
                        const auto found = state->mObjectiveStatus.find(objective);
                        hasFlag = found != state->mObjectiveStatus.end() && (found->second & flag) != 0;
                    }
                    left = hasFlag ? 1.f : 0.f;
                    next = 3;
                }
                else if (term[0] == "isactionref" && term.size() >= 2)
                {
                    left = actionReferenceIsPlayer && term[1] == "player" ? 1.f : 0.f;
                    next = 2;
                }
                else if (term[0] == "getsecondspassed")
                {
                    left = secondsPassed;
                    next = 1;
                }
                else if (term[0] == "getbuttonpressed")
                {
                    if (MWBase::WindowManager* const windowManager = MWBase::Environment::tryGetWindowManager())
                        left = static_cast<float>(windowManager->readPressedButton());
                    else
                        left = -1.f;
                    next = 1;
                }
                else if (term[0] == "getplayersex" || term[0] == "player.getsex")
                {
                    MWBase::World* const world = MWBase::Environment::tryGetWorld();
                    const MWWorld::Ptr player = world != nullptr ? world->getPlayerPtr() : MWWorld::Ptr{};
                    const ESM::NPC* const npc = player.isEmpty() ? nullptr : player.get<ESM::NPC>()->mBase;
                    left = npc != nullptr && !npc->isMale() ? 1.f : 0.f;
                    next = 1;
                }
                else
                {
                    left = variableValue(term[0]);
                    next = 1;
                }

                if (!left)
                    return false;
                if (next == term.size())
                    return *left != 0.f;
                if (next + 1 >= term.size())
                    return false;
                float right = 0.f;
                return parseFloat(term[next + 1], right) && comparison(*left, term[next], right);
            };

            std::vector<std::vector<std::string>> terms;
            std::vector<std::string> connectors;
            std::vector<std::string> current;
            for (const std::string& token : tokens)
            {
                if (token == "&&" || token == "||")
                {
                    if (current.empty())
                        return false;
                    terms.push_back(std::move(current));
                    current.clear();
                    connectors.push_back(token);
                }
                else
                    current.push_back(token);
            }
            if (current.empty())
                return false;
            terms.push_back(std::move(current));

            bool value = evaluateTerm(terms.front());
            for (std::size_t index = 1; index < terms.size(); ++index)
            {
                const bool next = evaluateTerm(terms[index]);
                value = connectors[index - 1] == "&&" ? value && next : value || next;
            }
            return value;
        };

        const auto setVariable = [this, ownerQuest](std::string_view target, float value) {
            const std::string normalised = normaliseSourceToken(target);
            if (const std::size_t separator = normalised.rfind('.'); separator != std::string::npos)
                return setQuestVariable(normalised.substr(0, separator), normalised.substr(separator + 1), value);
            if (!ownerQuest)
                return false;
            const auto state = mStates.find(*ownerQuest);
            if (state == mStates.end())
                return false;
            const auto variable = state->second.mVariables.find(normaliseScriptVariable(normalised));
            if (variable == state->second.mVariables.end())
                return false;
            variable->second = value;
            return true;
        };

        const auto movePlayerToMarker = [this](std::string_view markerEditorId) {
            MWBase::World* const world = MWBase::Environment::tryGetWorld();
            if (mStore == nullptr || world == nullptr)
                return false;
            const ESM4::Reference* marker = nullptr;
            for (const ESM4::Reference& reference : mStore->get<ESM4::Reference>())
            {
                if (!Misc::StringUtils::ciEqual(reference.mEditorId, markerEditorId))
                    continue;
                if (marker != nullptr)
                    return false;
                marker = &reference;
            }
            if (marker == nullptr || marker->mParent.empty())
                return false;
            world->changeToCell(marker->mParent, marker->mPos, true);
            const MWWorld::Ptr player = world->getPlayerPtr();
            const bool moved = player.isInCell();
            if (moved)
            {
                // An authored ESM4 marker is an anchor, not necessarily the
                // final collision surface.  The 0.51 cell handoff leaves a
                // player at that raw point; for Doc Mitchell's marker that is
                // visibly above the interior.  Reuse the engine's normal
                // collision grounding pass, as the established compatibility
                // runtime does for every player MoveTo.
                world->adjustPosition(player, true);
                const ESM::Position& position = player.getRefData().getPosition();
                Log(Debug::Info) << "FNV/ESM4 behavior: MoveTo actor=player marker=" << marker->mEditorId
                                 << " cell=" << marker->mParent << " position=(" << position.pos[0] << ","
                                 << position.pos[1] << "," << position.pos[2] << ") grounded=1";
            }
            return moved;
        };

        const auto applyDefaultSpecial = [](int total, std::string_view command) {
            MWBase::World* const world = MWBase::Environment::tryGetWorld();
            if (world == nullptr || total < 7 || total > 70)
                return false;

            std::array<float, 7> values{};
            int currentTotal = 0;
            const FalloutPlayerRuntimeState& playerState = world->getFalloutPlayerRuntimeState();
            for (std::size_t index = 0; index < values.size(); ++index)
            {
                const auto current = playerState.getCurrentActorValue(
                    FalloutPlayerRuntimeState::SpecialActorValueBegin + static_cast<std::uint32_t>(index));
                if (!current || !std::isfinite(current->mValue))
                    return false;
                const float rounded = std::round(current->mValue);
                if (rounded < 1.f || rounded > 10.f || std::abs(current->mValue - rounded) > 0.001f)
                    return false;
                values[index] = rounded;
                currentTotal += static_cast<int>(rounded);
            }
            while (currentTotal < total)
            {
                bool changed = false;
                for (float& value : values)
                {
                    if (value >= 10.f)
                        continue;
                    ++value;
                    ++currentTotal;
                    changed = true;
                    if (currentTotal == total)
                        break;
                }
                if (!changed)
                    break;
            }
            while (currentTotal > total)
            {
                bool changed = false;
                for (auto value = values.rbegin(); value != values.rend(); ++value)
                {
                    if (*value <= 1.f)
                        continue;
                    --*value;
                    --currentTotal;
                    changed = true;
                    if (currentTotal == total)
                        break;
                }
                if (!changed)
                    break;
            }
            if (currentTotal != total || !world->setFalloutPlayerSpecial(values))
                return false;

            if (MWBase::WindowManager* const windowManager = MWBase::Environment::tryGetWindowManager())
            {
                std::ostringstream message;
                message << "S.P.E.C.I.A.L.\n\nDefault allocation applied (" << total << " points):\nStrength "
                        << values[0] << "   Perception " << values[1] << "\nEndurance " << values[2]
                        << "   Charisma " << values[3] << "\nIntelligence " << values[4] << "   Agility "
                        << values[5] << "   Luck " << values[6];
                windowManager->scheduleMessageBox(message.str(), MWGui::ShowInDialogueMode_Never);
            }
            // Retain a canonical, capability-bearing event for the capture
            // contract. The allocation above is the real player SPECIAL
            // mutation; this line makes that state transition auditable
            // without treating a route stage alone as evidence.
            std::string commandName;
            if (command == "showlovetestermenuparams")
                commandName = "ShowLoveTesterMenuParams";
            else if (command == "ssbmp")
                commandName = "SSBMP";
            else
                commandName = std::string(command);
            Log(Debug::Info) << "FNV/ESM4 behavior: " << commandName << " default selection total=" << total
                             << " values=" << values[0] << ',' << values[1] << ',' << values[2] << ',' << values[3]
                             << ',' << values[4] << ',' << values[5] << ',' << values[6]
                             << " capability=character-special";
            return true;
        };

        const auto sourceValue = [this, ownerQuest, secondsPassed](const std::vector<std::string>& tokens,
                                     std::size_t& index) -> std::optional<float> {
            if (index >= tokens.size())
                return std::nullopt;
            const std::string& token = tokens[index++];
            if (token == "getsecondspassed")
                return secondsPassed;
            if (token == "getbuttonpressed")
            {
                if (MWBase::WindowManager* const windowManager = MWBase::Environment::tryGetWindowManager())
                    return static_cast<float>(windowManager->readPressedButton());
                return -1.f;
            }
            if (token == "getstage" && index < tokens.size())
            {
                const ESM4::Quest* const quest = resolveQuest(tokens[index++]);
                const ESM4QuestState* const state = quest != nullptr ? findState(*quest) : nullptr;
                return state != nullptr ? std::optional<float>(static_cast<float>(state->mCurrentStage)) : std::nullopt;
            }
            if (token == "getplayersex" || token == "player.getsex")
            {
                MWBase::World* const world = MWBase::Environment::tryGetWorld();
                const MWWorld::Ptr player = world != nullptr ? world->getPlayerPtr() : MWWorld::Ptr{};
                const ESM::NPC* const npc = player.isEmpty() ? nullptr : player.get<ESM::NPC>()->mBase;
                return npc != nullptr && !npc->isMale() ? 1.f : 0.f;
            }

            float literal = 0.f;
            if (parseFloat(token, literal))
                return literal;

            const std::string normalised = normaliseSourceToken(token);
            const auto valueFromQuest = [this](const ESM4::Quest& quest, std::string_view variable)
                -> std::optional<float> {
                const ESM4QuestState* const state = findState(quest);
                if (state == nullptr)
                    return std::nullopt;
                const auto found = state->mVariables.find(normaliseScriptVariable(variable));
                return found != state->mVariables.end() ? std::optional<float>(found->second) : std::nullopt;
            };
            if (const std::size_t separator = normalised.rfind('.'); separator != std::string::npos)
            {
                const ESM4::Quest* const quest = resolveQuest(normalised.substr(0, separator));
                return quest != nullptr ? valueFromQuest(*quest, normalised.substr(separator + 1)) : std::nullopt;
            }
            if (!ownerQuest || mStore == nullptr)
                return std::nullopt;
            const ESM4::Quest* const quest = mStore->get<ESM4::Quest>().search(ESM::RefId(*ownerQuest));
            return quest != nullptr ? valueFromQuest(*quest, normalised) : std::nullopt;
        };

        const auto sourceExpression = [&sourceValue](const std::vector<std::string>& tokens, std::size_t& index)
            -> std::optional<float> {
            std::optional<float> value = sourceValue(tokens, index);
            if (!value || index >= tokens.size())
                return value;
            const std::string& operation = tokens[index];
            if (operation != "+" && operation != "-" && operation != "*" && operation != "/")
                return value;
            ++index;
            const std::optional<float> operand = sourceValue(tokens, index);
            if (!operand)
                return std::nullopt;
            if (operation == "+")
                return *value + *operand;
            if (operation == "-")
                return *value - *operand;
            if (operation == "*")
                return *value * *operand;
            return *operand != 0.f ? std::optional<float>(*value / *operand) : std::nullopt;
        };

        std::vector<ConditionalFrame> conditionStack;
        bool sawBlocks = false;
        bool inSelectedBlock = selectedBlock.empty();
        bool active = inSelectedBlock;
        const std::string wantedBlock = normaliseSourceToken(selectedBlock);
        std::istringstream stream{ std::string(source) };
        for (std::string line; std::getline(stream, line);)
        {
            const std::vector<std::string_view> rawTokens = tokenize(line);
            const std::vector<std::string> tokens = normaliseSourceTokens(rawTokens);
            if (tokens.empty())
                continue;

            if (tokens[0] == "begin")
            {
                sawBlocks = true;
                inSelectedBlock = tokens.size() >= 2 && (wantedBlock.empty() || tokens[1] == wantedBlock);
                conditionStack.clear();
                active = inSelectedBlock;
                continue;
            }
            if (tokens[0] == "end")
            {
                if (sawBlocks && inSelectedBlock)
                    return;
                inSelectedBlock = false;
                active = false;
                conditionStack.clear();
                continue;
            }
            if (!inSelectedBlock)
                continue;

            if (tokens[0] == "if")
            {
                const bool branch = active && evaluateCondition(
                    std::vector<std::string>(tokens.begin() + 1, tokens.end()));
                conditionStack.push_back({ active, branch, branch });
                active = branch;
                continue;
            }
            if (tokens[0] == "elseif")
            {
                if (conditionStack.empty())
                {
                    mUnsupportedStageCommands.push_back(line);
                    continue;
                }
                ConditionalFrame& frame = conditionStack.back();
                const bool branch = frame.mParentActive && !frame.mBranchTaken
                    && evaluateCondition(std::vector<std::string>(tokens.begin() + 1, tokens.end()));
                frame.mBranchTaken = frame.mBranchTaken || branch;
                frame.mActive = branch;
                active = branch;
                continue;
            }
            if (tokens[0] == "else")
            {
                if (conditionStack.empty())
                {
                    mUnsupportedStageCommands.push_back(line);
                    continue;
                }
                ConditionalFrame& frame = conditionStack.back();
                frame.mActive = frame.mParentActive && !frame.mBranchTaken;
                frame.mBranchTaken = true;
                active = frame.mActive;
                continue;
            }
            if (tokens[0] == "endif")
            {
                if (conditionStack.empty())
                {
                    mUnsupportedStageCommands.push_back(line);
                    continue;
                }
                conditionStack.pop_back();
                active = conditionStack.empty() ? true : conditionStack.back().mActive;
                continue;
            }
            if (!active)
                continue;

            const std::string& command = tokens[0];
            if (command == "set" && tokens.size() >= 3)
            {
                std::size_t expression = tokens[2] == "to" ? 3 : 2;
                const std::optional<float> value = sourceExpression(tokens, expression);
                if (value && expression == tokens.size() && setVariable(tokens[1], *value))
                    continue;
            }
            else if (command == "setobjectivedisplayed" && tokens.size() >= 4)
            {
                std::int32_t objective = 0;
                std::int32_t displayed = 0;
                if (parseInt(tokens[2], objective) && parseInt(tokens[3], displayed)
                    && setObjectiveDisplayed(tokens[1], objective, displayed != 0))
                    continue;
            }
            else if (command == "setobjectivecompleted" && tokens.size() >= 4)
            {
                std::int32_t objective = 0;
                std::int32_t completed = 0;
                if (parseInt(tokens[2], objective) && parseInt(tokens[3], completed)
                    && setObjectiveCompleted(tokens[1], objective, completed != 0))
                    continue;
            }
            else if (command == "setstage" && tokens.size() >= 3)
            {
                std::int32_t stage = 0;
                if (parseInt(tokens[2], stage) && stage >= 0 && stage <= 255
                    && setStage(tokens[1], static_cast<std::uint8_t>(stage)))
                    continue;
            }
            else if (command == "startquest" && tokens.size() >= 2 && startQuest(tokens[1]))
                continue;
            else if (command == "stopquest" && tokens.size() >= 2 && stopQuest(tokens[1]))
                continue;
            else if (command == "completequest" && tokens.size() >= 2 && completeQuest(tokens[1]))
                continue;
            else if (command == "failquest" && tokens.size() >= 2 && failQuest(tokens[1]))
                continue;
            else if (command == "forceactivequest" && tokens.size() >= 2 && forceActiveQuest(tokens[1]))
                continue;
            else if (command == "setinchargen" && tokens.size() >= 2)
            {
                std::int32_t value = 0;
                if (parseInt(tokens[1], value))
                {
                    if (MWBase::World* const world = MWBase::Environment::tryGetWorld())
                        world->setGlobalInt(MWWorld::Globals::sCharGenState, value != 0 ? 1 : -1);
                    if (MWBase::WindowManager* const windowManager = MWBase::Environment::tryGetWindowManager())
                    {
                        const bool charGenActive = value != 0;
                        windowManager->setHudVisibility(!charGenActive);
                        windowManager->showCrosshair(!charGenActive);
                        Log(Debug::Info) << "OpenNV UI: gameplay overlay suppression=" << (charGenActive ? 1 : 0);
                    }
                    continue;
                }
            }
            else if (command == "getplayername")
            {
                if (MWBase::WindowManager* const windowManager = MWBase::Environment::tryGetWindowManager())
                {
                    windowManager->showAuthoredNameMenu();
                    continue;
                }
            }
            else if (command == "showracemenu")
            {
                if (MWBase::WindowManager* const windowManager = MWBase::Environment::tryGetWindowManager())
                {
                    windowManager->showAuthoredRaceMenu();
                    continue;
                }
            }
            else if (command == "showmessage" && tokens.size() >= 2 && mStore != nullptr)
            {
                const std::string messageEditorId = removeQuotes(rawTokens[1]);
                const ESM4::Message* message = nullptr;
                for (const ESM4::Message& candidate : mStore->get<ESM4::Message>())
                {
                    if (Misc::StringUtils::ciEqual(candidate.mEditorId, messageEditorId))
                    {
                        message = &candidate;
                        break;
                    }
                }
                MWBase::WindowManager* const windowManager = MWBase::Environment::tryGetWindowManager();
                if (message != nullptr && windowManager != nullptr)
                {
                    std::string text = message->mFullName;
                    if (!message->mDescription.empty())
                    {
                        if (!text.empty())
                            text += "\n\n";
                        text += message->mDescription;
                    }
                    if (text.empty())
                        text = message->mEditorId;
                    if (message->mButtons.empty())
                        windowManager->scheduleMessageBox(std::move(text), MWGui::ShowInDialogueMode_Never);
                    else if (!windowManager->isInteractiveMessageBoxActive())
                        windowManager->interactiveMessageBox(text, message->mButtons);
                    Log(Debug::Info) << "Fallout/ESM4 behavior: ShowMessage message=" << message->mEditorId
                                     << " buttons=" << message->mButtons.size();
                    continue;
                }
            }
            else if ((command == "showlovetestermenuparams" || command == "ssbmp") && tokens.size() >= 2)
            {
                std::int32_t total = 0;
                if (parseInt(tokens[1], total) && applyDefaultSpecial(total, command))
                    continue;
            }
            else if (command == "playbink" && tokens.size() >= 2)
            {
                const std::string asset = removeQuotes(rawTokens[1]);
                std::int32_t allowSkippingValue = 1;
                const bool allowSkipping = tokens.size() < 3
                    || (parseInt(tokens[2], allowSkippingValue) && allowSkippingValue != 0);
                if (!asset.empty())
                {
                    if (MWBase::WindowManager* const windowManager = MWBase::Environment::tryGetWindowManager())
                    {
                        try
                        {
                            windowManager->playVideo(asset, allowSkipping);
                            Log(Debug::Info) << "FNV/ESM4 behavior: PlayBink completed asset='" << asset << "'";
                            continue;
                        }
                        catch (const std::exception& e)
                        {
                            Log(Debug::Warning) << "FNV/ESM4 behavior: PlayBink failed asset='" << asset
                                                << "' error=" << e.what();
                        }
                    }
                }
            }
            else if (command == "player.moveto" && tokens.size() >= 2
                && movePlayerToMarker(removeQuotes(rawTokens[1])))
                continue;
            else if (command == "player.setscale" && tokens.size() >= 2)
            {
                float scale = 0.f;
                if (parseFloat(tokens[1], scale) && scale > 0.f)
                {
                    if (MWBase::World* const world = MWBase::Environment::tryGetWorld())
                        world->scaleObject(world->getPlayerPtr(), scale, true);
                    continue;
                }
            }
            else if (command == "enableplayercontrols" || command == "disableplayercontrols")
            {
                bool enabled = command == "enableplayercontrols";
                if (tokens.size() >= 2)
                {
                    std::int32_t movement = 0;
                    if (!parseInt(tokens[1], movement))
                        movement = enabled ? 1 : 0;
                    enabled = command == "enableplayercontrols" ? movement != 0 : movement == 0;
                }
                if (MWBase::InputManager* const inputManager = MWBase::Environment::get().getInputManager())
                {
                    inputManager->toggleControlSwitch("playercontrols", enabled);
                    continue;
                }
            }

            const std::size_t separator = command.rfind('.');
            if (separator != std::string::npos)
            {
                const std::string_view subject(command.data(), separator);
                const std::string_view action(command.data() + separator + 1, command.size() - separator - 1);
                if ((action == "say" && tokens.size() >= 2) || (action == "sayto" && tokens.size() >= 3))
                {
                    const std::size_t topicIndex = action == "sayto" ? 2 : 1;
                    const std::optional<ESM::FormId> actor = resolveActorReference(subject);
                    if (actor && executeAuthoredDialogue(*actor, removeQuotes(rawTokens[topicIndex])))
                        continue;
                }
                else if (subject == "player" && action == "sexchange" && tokens.size() >= 2)
                {
                    const bool male = tokens[1] == "male";
                    const bool female = tokens[1] == "female";
                    MWBase::World* const world = MWBase::Environment::tryGetWorld();
                    if ((male || female) && world != nullptr)
                    {
                        const MWWorld::Ptr player = world->getPlayerPtr();
                        const ESM::NPC* const npc = player.isEmpty() ? nullptr : player.get<ESM::NPC>()->mBase;
                        if (npc != nullptr)
                        {
                            MWBase::Environment::get().getMechanicsManager()->setPlayerRace(
                                npc->mRace, male, npc->mHead, npc->mHair);
                            Log(Debug::Info) << "Fallout/ESM4 behavior: SexChange player=" << tokens[1];
                            continue;
                        }
                    }
                }
            }

            mUnsupportedStageCommands.push_back(line);
        }
    }

    void ESM4QuestRuntime::executeResultSource(std::string_view source)
    {
        executeStageSource(source);
    }

    bool ESM4QuestRuntime::onReferenceActivated(const MWWorld::Ptr& reference, const MWWorld::Ptr& actor)
    {
        if (reference.isEmpty() || actor.isEmpty())
            return false;
        const auto found = mReferenceScriptStates.find(reference.getCellRef().getRefNum());
        if (found == mReferenceScriptStates.end() || !found->second.mHasOnActivate)
            return false;

        MWBase::World* const world = MWBase::Environment::tryGetWorld();
        const bool playerActivated = world != nullptr && actor == world->getPlayerPtr();
        executeStageSource(found->second.mSource, {}, 0.f, "onactivate", playerActivated);
        if (playerActivated)
        {
            Log(Debug::Info) << "FNV/ESM4 behavior: reference-script event reference="
                             << found->second.mEditorId << " event=onactivate argument=player";
        }
        Log(Debug::Info) << "Fallout/ESM4 behavior: reference activation reference=" << found->second.mEditorId
                         << " player=" << playerActivated;
        return true;
    }

    const ESM4QuestState* ESM4QuestRuntime::search(std::string_view id) const
    {
        const ESM4::Quest* quest = resolveQuest(id);
        return quest != nullptr ? findState(*quest) : nullptr;
    }

    const ESM4QuestState* ESM4QuestRuntime::search(ESM::FormId id) const
    {
        const auto found = mStates.find(id);
        return found != mStates.end() ? &found->second : nullptr;
    }

    std::vector<std::string> ESM4QuestRuntime::getStartGameEnabledQuestEditorIds() const
    {
        std::vector<std::string> result;
        if (mStore == nullptr)
            return result;

        for (const ESM4::Quest& quest : mStore->get<ESM4::Quest>())
        {
            if ((quest.mData.flags & ESM4::Quest::Flag_StartGameEnabled) != 0 && !quest.mEditorId.empty())
                result.push_back(quest.mEditorId);
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    std::optional<ESM4AuthoredStartPlacement> ESM4QuestRuntime::findAuthoredStartPlacement() const
    {
        if (mStore == nullptr)
            return std::nullopt;

        std::vector<ESM4AuthoredStartPlacement> candidates;
        for (const ESM4::Quest& quest : mStore->get<ESM4::Quest>())
        {
            for (const ESM4::QuestStage& stage : quest.mStages)
            {
                if (stage.mIndex != 0)
                    continue;
                for (const ESM4::QuestStageEntry& entry : stage.mEntries)
                {
                    // A conditional entry is not a reliable universal new-game
                    // contract. Do not turn a conditional branch into a spawn rule.
                    if (!entry.mConditions.empty())
                        continue;
                    const std::optional<AuthoredOpeningSource> opening
                        = findAuthoredOpeningSource(entry.mScript.scriptSource, quest.mEditorId);
                    if (!opening)
                        continue;

                    const ESM4::Reference* marker = nullptr;
                    unsigned markerCount = 0;
                    for (const ESM4::Reference& reference : mStore->get<ESM4::Reference>())
                    {
                        if (!Misc::StringUtils::ciEqual(reference.mEditorId, opening->mMarkerEditorId))
                            continue;
                        ++markerCount;
                        marker = &reference;
                    }
                    if (markerCount > 1)
                    {
                        Log(Debug::Warning) << "FNV/ESM4 behavior: authored opening marker is ambiguous quest="
                                            << quest.mEditorId << " marker=" << opening->mMarkerEditorId
                                            << " count=" << markerCount;
                        return std::nullopt;
                    }
                    if (marker == nullptr || marker->mParent.empty()
                        || mStore->get<ESM4::Cell>().search(marker->mParent) == nullptr)
                    {
                        Log(Debug::Verbose) << "FNV/ESM4 behavior: authored opening candidate rejected quest="
                                            << quest.mEditorId << " marker=" << opening->mMarkerEditorId
                                            << " markerCount=" << markerCount
                                            << " parent=" << (marker != nullptr ? marker->mParent.serializeText() : "")
                                            << " cellResolved="
                                            << (marker != nullptr
                                                    && mStore->get<ESM4::Cell>().search(marker->mParent) != nullptr);
                        continue;
                    }

                    candidates.push_back({ quest.mId, 0, opening->mActivationStage, marker->mId, marker->mParent,
                        marker->mPos, quest.mEditorId, marker->mEditorId, opening->mCinematicAsset });
                    if (candidates.size() > 1)
                    {
                        Log(Debug::Warning) << "FNV/ESM4 behavior: authored opening candidates are ambiguous first="
                                            << candidates.front().mQuestEditorId << " second=" << quest.mEditorId;
                        return std::nullopt;
                    }
                }
            }
        }
        return candidates.empty() ? std::nullopt : std::optional<ESM4AuthoredStartPlacement>(candidates.front());
    }

    std::optional<float> ESM4QuestRuntime::getQuestVariable(std::string_view id, std::string_view variable) const
    {
        const ESM4::Quest* quest = resolveQuest(id);
        const ESM4QuestState* state = quest != nullptr ? findState(*quest) : nullptr;
        if (state == nullptr)
            return std::nullopt;
        const auto found = state->mVariables.find(Misc::StringUtils::lowerCase(variable));
        return found != state->mVariables.end() ? std::optional<float>(found->second) : std::nullopt;
    }
}
