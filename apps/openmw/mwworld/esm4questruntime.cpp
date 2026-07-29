#include "esm4questruntime.hpp"

#include "cellstore.hpp"
#include "class.hpp"
#include "esmstore.hpp"
#include "fnvplayerruntimestate.hpp"
#include "globals.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm4/loadglob.hpp>
#include <components/esm4/loadimad.hpp>
#include <components/esm4/loadpack.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadcont.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loaddial.hpp>
#include <components/esm4/loaddoor.hpp>
#include <components/esm4/loadmesg.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadscpt.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/settings/settings.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/dialoguemanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwclass/fnvaipackage.hpp"
#include "../mwclass/esm4npc.hpp"
#include "player.hpp"

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
            {
                // Bethesda sources commonly omit the space in `if(condition)`.
                // Keep the control keyword separate so its matching endif
                // remains paired with this condition instead of an outer one.
                end = 2;
            }
            else if (line.size() > 6 && line[6] == '('
                && Misc::StringUtils::ciEqual(line.substr(0, 6), "elseif"))
            {
                end = 6;
            }
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
        const char* const begin = value.data();
        const char* const end = value.data() + value.size();
        const auto parsed = std::from_chars(begin, end, result);
        return parsed.ec == std::errc{} && parsed.ptr == end;
    }

    std::string removeQuotes(std::string_view value)
    {
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"')
                                  || (value.front() == '\'' && value.back() == '\'')))
            value = value.substr(1, value.size() - 2);
        return std::string(value);
    }

    using SourceTokens = std::vector<std::string>;

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

    std::string_view trimAsciiWhitespace(std::string_view value)
    {
        const std::size_t first = value.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
            return {};
        const std::size_t last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    }

    SourceTokens normaliseSourceTokens(const std::vector<std::string_view>& rawTokens)
    {
        SourceTokens result;
        result.reserve(rawTokens.size());
        for (const std::string_view token : rawTokens)
        {
            std::string normalised = normaliseSourceToken(token);
            if (!normalised.empty())
                result.push_back(std::move(normalised));
        }
        return result;
    }

    std::string normaliseScriptVariable(std::string_view value)
    {
        std::string result = normaliseSourceToken(value);
        if (const std::size_t separator = result.rfind('.'); separator != std::string::npos)
            result.erase(0, separator + 1);
        return result;
    }

    bool hasSourceBlock(std::string_view source, std::string_view block, std::string_view argument = {})
    {
        const std::string wanted = normaliseSourceToken(block);
        const std::string wantedArgument = normaliseSourceToken(argument);
        std::istringstream stream{ std::string(source) };
        for (std::string line; std::getline(stream, line);)
        {
            const std::string_view trimmed = trimAsciiWhitespace(line);
            // A leading semicolon comments out a complete Bethesda source
            // line.  Do not let a commented `begin OnPackageDone` make the AI
            // scheduler retain a one-shot package without an executable
            // handler.
            if (trimmed.empty() || trimmed.front() == ';')
                continue;
            const SourceTokens tokens = normaliseSourceTokens(tokenize(trimmed));
            if (tokens.size() >= 2 && tokens[0] == "begin" && tokens[1] == wanted
                && (wantedArgument.empty() || (tokens.size() >= 3 && tokens[2] == wantedArgument)))
                return true;
        }
        return false;
    }

    std::vector<std::string> sourceBlockArguments(std::string_view source, std::string_view block)
    {
        const std::string wanted = normaliseSourceToken(block);
        std::vector<std::string> result;
        std::istringstream stream{ std::string(source) };
        for (std::string line; std::getline(stream, line);)
        {
            const std::string_view trimmed = trimAsciiWhitespace(line);
            if (trimmed.empty() || trimmed.front() == ';')
                continue;
            const SourceTokens tokens = normaliseSourceTokens(tokenize(trimmed));
            if (tokens.size() < 2 || tokens[0] != "begin" || tokens[1] != wanted)
                continue;

            // A few older scripts omit the event argument.  Preserve the
            // previous player-trigger interpretation for that grammar by
            // retaining an empty argument; the dispatch site maps it to the
            // player while still selecting the unqualified source block.
            const std::string argument = tokens.size() >= 3 ? tokens[2] : std::string{};
            if (std::find(result.begin(), result.end(), argument) == result.end())
                result.push_back(argument);
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

    struct ScriptedReferenceBase
    {
        ESM::FormId mScript{};
        float mTriggerRadius = 0.f;
    };

    template <class T>
    std::optional<ScriptedReferenceBase> findScriptedReferenceBase(
        const MWWorld::ESMStore& store, ESM::FormId base)
    {
        const T* const record = store.get<T>().search(ESM::RefId(base));
        if (record == nullptr || record->mScriptId.isZeroOrUnset())
            return std::nullopt;
        return ScriptedReferenceBase{ record->mScriptId, std::max(0.f, record->mBoundRadius) };
    }

    std::optional<std::string> parseTimerDecrement(const SourceTokens& tokens)
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

    std::optional<std::string> parseTimerPositiveCondition(const SourceTokens& condition)
    {
        if (condition.size() != 3 || condition[1] != ">" || condition[2] != "0")
            return std::nullopt;
        const std::string variable = normaliseScriptVariable(condition[0]);
        return variable.empty() ? std::nullopt : std::optional<std::string>(variable);
    }

    std::optional<std::string> parseRunFlagCondition(const SourceTokens& condition)
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
        const SourceTokens& condition, std::string_view questEditorId)
    {
        if (condition.size() != 4 || condition[0] != "getstage" || condition[1] != questEditorId
            || condition[2] != "==")
            return std::nullopt;

        std::int32_t stage = 0;
        if (!parseInt(condition[3], stage) || stage < 0 || stage > 255)
            return std::nullopt;
        return static_cast<std::uint8_t>(stage);
    }

    struct SourceConditionalFrame
    {
        SourceTokens mCondition;
        bool mElseBranch = false;
    };

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
        }

        if (!markerEditorId || !cinematicAsset || !activationStage)
            return std::nullopt;
        return AuthoredOpeningSource{ std::move(*markerEditorId), std::move(*cinematicAsset), *activationStage };
    }

    void setAuthoredGameplayOverlayVisible(bool visible, std::string_view reason)
    {
        MWBase::WindowManager* const windowManager = MWBase::Environment::tryGetWindowManager();
        if (windowManager == nullptr)
            return;

        // Fallout-family scripts use this during cinematics and character
        // generation.  These are standard UI intents, so honor them here rather
        // than keying behaviour to a quest, cell, or named content record.
        windowManager->setGameplayOverlaySuppressed(!visible);
        windowManager->setHudVisibility(visible);
        windowManager->showCrosshair(visible);
        windowManager->setCursorVisible(visible);
        windowManager->setCursorActive(false);
        Log(Debug::Info) << "FNV/ESM4 behavior: authored gameplay overlay visible=" << visible
                         << " reason=" << reason;
    }

    bool setAuthoredCharGenState(std::int32_t value)
    {
        MWBase::World* const world = MWBase::Environment::tryGetWorld();
        if (world == nullptr)
            return false;

        const bool active = value != 0;
        world->setGlobalInt(MWWorld::Globals::sCharGenState, active ? 1 : -1);
        setAuthoredGameplayOverlayVisible(!active, active ? "SetInCharGen" : "SetInCharGen-complete");
        return true;
    }

    bool isAuthoredCharGenActive()
    {
        MWBase::World* const world = MWBase::Environment::tryGetWorld();
        return world != nullptr && world->getGlobalInt(MWWorld::Globals::sCharGenState) != -1;
    }
}

namespace MWWorld
{
    void ESM4QuestRuntime::initialize(const ESMStore& store, const Globals* globals)
    {
        clear();
        mStore = &store;
        mGlobals = globals;
        mAuthoredCompatibilityCommands = parseAuthoredCompatibilityCommandMappings(
            Settings::Manager::getString("script command mappings", "OpenNV Compatibility"));
        if (!mAuthoredCompatibilityCommands.empty())
        {
            Log(Debug::Info) << "OpenNV compatibility: loaded " << mAuthoredCompatibilityCommands.size()
                             << " declared authored script-command mapping(s)";
        }
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
                std::vector<ESM4AuthoredGameModeTimer> timers
                    = compileAuthoredGameModeTimers(script->mScript.scriptSource, quest.mEditorId);
                const std::string loweredSource = Misc::StringUtils::lowerCase(script->mScript.scriptSource);
                // A source is eligible only when its own grammar establishes
                // an unambiguous stage timer or a UI choice handoff.  This
                // keeps unrelated Start Game Enabled quest scripts from
                // becoming a per-frame interpreter workload while remaining
                // independent of campaign/editor IDs.
                const bool hasChoiceHandoff = loweredSource.find("begin gamemode") != std::string::npos
                    && loweredSource.find("getbuttonpressed") != std::string::npos
                    && loweredSource.find("showmessage") != std::string::npos;
                const bool hasMenuModeStageHandoff = loweredSource.find("begin menumode") != std::string::npos
                    && loweredSource.find("setstage") != std::string::npos;
                if (!timers.empty() || hasChoiceHandoff)
                {
                    mAuthoredGameModeSources.insert_or_assign(quest.mId, script->mScript.scriptSource);
                    if (!timers.empty())
                        mAuthoredGameModeTimers.insert_or_assign(quest.mId, std::move(timers));
                }
                if (hasMenuModeStageHandoff)
                    mAuthoredMenuModeSources.insert_or_assign(quest.mId, script->mScript.scriptSource);
            }
            mStates.insert_or_assign(quest.mId, std::move(state));
        }

        const auto registerActorScript = [this, &store](const auto& actor) {
            const ESM4::Npc* npc = store.get<ESM4::Npc>().search(ESM::RefId(actor.mBaseObj));
            const ESM4::Creature* creature = npc == nullptr
                ? store.get<ESM4::Creature>().search(ESM::RefId(actor.mBaseObj))
                : nullptr;
            const ESM::FormId scriptId = npc != nullptr ? npc->mScriptId : (creature != nullptr ? creature->mScriptId : ESM::FormId{});
            const ESM4::Script* script = scriptId.isZeroOrUnset()
                ? nullptr
                : store.get<ESM4::Script>().search(ESM::RefId(scriptId));
            if (script == nullptr || script->mScript.scriptSource.empty())
                return;

            const std::string loweredSource = Misc::StringUtils::lowerCase(script->mScript.scriptSource);
            // As with quest GameMode source, select a limited, data-shaped
            // compatibility subset.  The actor stays dormant until its placed
            // reference is live in the world, so this does not turn every NPC
            // record into an active simulation script.
            const bool relevant = loweredSource.find("begin gamemode") != std::string::npos
                && (loweredSource.find("getstage") != std::string::npos
                    || loweredSource.find("getsecondspassed") != std::string::npos
                    || loweredSource.find("say ") != std::string::npos
                    || loweredSource.find("sayto ") != std::string::npos);
            if (!relevant || actor.mId.isZeroOrUnset() || actor.mEditorId.empty())
                return;

            ActorScriptState state;
            state.mActor = actor.mId;
            state.mScript = scriptId;
            state.mCell = actor.mParent;
            state.mEditorId = actor.mEditorId;
            state.mSource = script->mScript.scriptSource;
            for (const ESM4::ScriptLocalVariableData& variable : script->mScript.localVarData)
                if (!variable.variableName.empty())
                    state.mVariables.emplace(Misc::StringUtils::lowerCase(variable.variableName), 0.f);
            mActorScriptStates.insert_or_assign(actor.mId, std::move(state));

            const std::string key = normaliseSourceToken(actor.mEditorId);
            const auto [existing, inserted] = mActorScriptEditorIds.emplace(key, actor.mId);
            if (!inserted && existing->second != actor.mId)
                mAmbiguousActorScriptEditorIds.insert_or_assign(key, true);
        };
        for (const ESM4::ActorCharacter& actor : store.get<ESM4::ActorCharacter>())
            registerActorScript(actor);
        for (const ESM4::ActorCreature& actor : store.get<ESM4::ActorCreature>())
            registerActorScript(actor);

        const auto resolveReferenceBase = [&store](const ESM4::Reference& reference)
            -> std::optional<ScriptedReferenceBase> {
            const ESM::RefId base(reference.mBaseObj);
            if (const ESM4::Activator* const activator = store.get<ESM4::Activator>().search(base))
            {
                if (activator->mScriptId.isZeroOrUnset())
                    return std::nullopt;
                return ScriptedReferenceBase{ activator->mScriptId,
                    std::max(std::max(0.f, activator->mBoundRadius), objectBoundsRadius(activator->mObjectBounds)) };
            }
            if (const auto scripted = findScriptedReferenceBase<ESM4::Door>(store, reference.mBaseObj))
                return scripted;
            if (const auto scripted = findScriptedReferenceBase<ESM4::Container>(store, reference.mBaseObj))
                return scripted;
            if (const auto scripted = findScriptedReferenceBase<ESM4::Book>(store, reference.mBaseObj))
                return scripted;
            return std::nullopt;
        };

        const auto registerReferenceScript = [this, &store, &resolveReferenceBase](const ESM4::Reference& reference) {
            const std::optional<ScriptedReferenceBase> base = resolveReferenceBase(reference);
            // A placed reference's FormID is the authoritative identity.  Many
            // legitimate trigger volumes and activators inherit an editor ID
            // only from their base record, leaving the placed record unnamed.
            // Do not make an optional authoring label a prerequisite for the
            // source event runtime: FormIDs remain unique and are what the
            // live world uses to find the placed object.
            if (!base || reference.mId.isZeroOrUnset())
                return;
            const ESM4::Script* const script = store.get<ESM4::Script>().search(ESM::RefId(base->mScript));
            if (script == nullptr || script->mScript.scriptSource.empty())
                return;

            const std::string_view source = script->mScript.scriptSource;
            const bool triggerEnter = hasSourceBlock(source, "ontriggerenter");
            const bool triggerLeave = hasSourceBlock(source, "ontriggerleave");
            const bool trigger = hasSourceBlock(source, "ontrigger");
            const bool activate = hasSourceBlock(source, "onactivate");
            const bool gameMode = hasSourceBlock(source, "gamemode");
            if (!triggerEnter && !triggerLeave && !trigger && !activate && !gameMode)
                return;

            ReferenceScriptState state;
            state.mReference = reference.mId;
            state.mScript = base->mScript;
            state.mCell = reference.mParent;
            state.mEditorId = reference.mEditorId.empty() ? reference.mId.toString("FormId:") : reference.mEditorId;
            state.mSource = script->mScript.scriptSource;
            state.mTriggerRadius = base->mTriggerRadius;
            state.mHasGameMode = gameMode;
            const auto registerTriggerParticipants = [&state, source](std::string_view block, auto member) {
                for (const std::string& argument : sourceBlockArguments(source, block))
                {
                    const auto found = std::find_if(state.mTriggerParticipants.begin(), state.mTriggerParticipants.end(),
                        [&argument](const ReferenceScriptTriggerParticipant& participant) {
                            return participant.mArgument == argument;
                        });
                    ReferenceScriptTriggerParticipant* const participant = found != state.mTriggerParticipants.end()
                        ? &*found
                        : &state.mTriggerParticipants.emplace_back();
                    if (participant->mArgument.empty() && !argument.empty())
                        participant->mArgument = argument;
                    participant->*member = true;
                }
            };
            registerTriggerParticipants("ontriggerenter", &ReferenceScriptTriggerParticipant::mHasEnter);
            registerTriggerParticipants("ontriggerleave", &ReferenceScriptTriggerParticipant::mHasLeave);
            registerTriggerParticipants("ontrigger", &ReferenceScriptTriggerParticipant::mHasTrigger);
            for (const ESM4::ScriptLocalVariableData& variable : script->mScript.localVarData)
                if (!variable.variableName.empty())
                    state.mVariables.emplace(Misc::StringUtils::lowerCase(variable.variableName), 0.f);

            const auto actorParticipant = std::find_if(state.mTriggerParticipants.begin(), state.mTriggerParticipants.end(),
                [](const ReferenceScriptTriggerParticipant& participant) {
                    return !participant.mArgument.empty()
                        && !Misc::StringUtils::ciEqual(participant.mArgument, "player");
                });
            if (std::getenv("OPENMW_COMPAT_ROUTE_PATH") != nullptr
                && actorParticipant != state.mTriggerParticipants.end())
            {
                Log(Debug::Info) << "FNV/ESM4 route trigger: registered actor volume reference=" << state.mEditorId
                                 << " form=" << ESM::RefId(state.mReference) << " cell=" << state.mCell
                                 << " radius=" << state.mTriggerRadius
                                 << " argument=" << actorParticipant->mArgument;
            }
            mReferenceScriptStates.insert_or_assign(reference.mId, std::move(state));

            // Name lookups remain a convenience for source expressions.  Do
            // not index an empty key, because unnamed placed references are
            // still valid event sources but cannot be addressed by a label.
            if (!reference.mEditorId.empty())
            {
                const std::string key = normaliseSourceToken(reference.mEditorId);
                const auto [existing, inserted] = mReferenceScriptEditorIds.emplace(key, reference.mId);
                if (!inserted && existing->second != reference.mId)
                    mAmbiguousReferenceScriptEditorIds.insert_or_assign(key, true);
            }
        };
        for (const ESM4::Reference& reference : store.get<ESM4::Reference>())
            registerReferenceScript(reference);
        if (!mReferenceScriptStates.empty())
        {
            Log(Debug::Info) << "FNV/ESM4 behavior: registered " << mReferenceScriptStates.size()
                             << " authored reference script event source(s)";
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
        mActiveImageSpaceModifiers.clear();
        mPlayedStageVideos.clear();
        mAuthoredGameModeSources.clear();
        mAuthoredGameModeTimers.clear();
        mAuthoredCompatibilityCommands.clear();
        mAuthoredMenuModeSources.clear();
        mActorScriptStates.clear();
        mActorScriptEditorIds.clear();
        mAmbiguousActorScriptEditorIds.clear();
        mPendingActorScriptEvents.clear();
        mPendingAuthoredSays.clear();
        mReferenceScriptStates.clear();
        mReferenceScriptEditorIds.clear();
        mAmbiguousReferenceScriptEditorIds.clear();
    }

    std::map<std::string, std::string, std::less<>> ESM4QuestRuntime::parseAuthoredCompatibilityCommandMappings(
        std::string_view mappings)
    {
        constexpr std::string_view CharacterAppearance = "character-appearance";
        constexpr std::string_view CharacterSpecial = "character-special";
        std::map<std::string, std::string, std::less<>> result;
        while (!mappings.empty())
        {
            const std::size_t separator = mappings.find(',');
            const std::string_view entry = trimAsciiWhitespace(mappings.substr(0, separator));
            if (separator == std::string_view::npos)
                mappings = {};
            else
                mappings.remove_prefix(separator + 1);

            const std::size_t capabilitySeparator = entry.find(':');
            if (capabilitySeparator == std::string_view::npos || entry.find(':', capabilitySeparator + 1) != std::string_view::npos)
                continue;
            const std::string command = normaliseSourceToken(trimAsciiWhitespace(entry.substr(0, capabilitySeparator)));
            const std::string capability
                = normaliseSourceToken(trimAsciiWhitespace(entry.substr(capabilitySeparator + 1)));
            if (command.empty() || (capability != CharacterAppearance && capability != CharacterSpecial)
                || result.contains(command))
                continue;
            result.emplace(command, capability);
        }
        return result;
    }

    const ESM4::ImageSpaceModifier* ESM4QuestRuntime::resolveImageSpaceModifier(std::string_view id) const
    {
        if (mStore == nullptr || id.empty())
            return nullptr;

        const ESM4::ImageSpaceModifier* result = nullptr;
        for (const ESM4::ImageSpaceModifier& candidate : mStore->get<ESM4::ImageSpaceModifier>())
        {
            if (!Misc::StringUtils::ciEqual(candidate.mEditorId, id))
                continue;
            if (result != nullptr)
            {
                Log(Debug::Warning) << "FNV/ESM4 behavior: image-space modifier editor ID is ambiguous id=" << id;
                return nullptr;
            }
            result = &candidate;
        }
        return result;
    }

    bool ESM4QuestRuntime::applyImageSpaceModifier(std::string_view id)
    {
        const ESM4::ImageSpaceModifier* const modifier = resolveImageSpaceModifier(id);
        if (modifier == nullptr)
        {
            Log(Debug::Warning) << "FNV/ESM4 behavior: ApplyImageSpaceModifier could not resolve id=" << id;
            return false;
        }

        const auto active = std::find_if(mActiveImageSpaceModifiers.begin(), mActiveImageSpaceModifiers.end(),
            [modifier](const ActiveImageSpaceModifier& entry) { return entry.mState.mId == modifier->mId; });
        if (active != mActiveImageSpaceModifiers.end())
        {
            // Repeating an already-live effect does not restart its authored
            // animation every simulation tick. A script that needs a restart
            // can explicitly remove and apply the record again.
            Log(Debug::Info) << "FNV/ESM4 behavior: ApplyImageSpaceModifier already active id="
                             << modifier->mEditorId;
            return true;
        }

        const bool animatable = (modifier->mAdapterFlags & 0x1u) != 0 && std::isfinite(modifier->mDuration)
            && modifier->mDuration > 0.f;
        mActiveImageSpaceModifiers.push_back(
            { { modifier->mId, 0.f, 1.f }, animatable ? modifier->mDuration : 0.f, animatable });
        Log(Debug::Info) << "FNV/ESM4 behavior: ApplyImageSpaceModifier id=" << modifier->mEditorId
                         << " form=" << ESM::RefId(modifier->mId) << " animatable=" << animatable
                         << " duration=" << modifier->mDuration;
        return true;
    }

    bool ESM4QuestRuntime::removeImageSpaceModifier(std::string_view id)
    {
        const ESM4::ImageSpaceModifier* const modifier = resolveImageSpaceModifier(id);
        if (modifier == nullptr)
        {
            Log(Debug::Warning) << "FNV/ESM4 behavior: RemoveImageSpaceModifier could not resolve id=" << id;
            return false;
        }

        const auto previousSize = mActiveImageSpaceModifiers.size();
        std::erase_if(mActiveImageSpaceModifiers,
            [modifier](const ActiveImageSpaceModifier& entry) { return entry.mState.mId == modifier->mId; });
        Log(Debug::Info) << "FNV/ESM4 behavior: RemoveImageSpaceModifier id=" << modifier->mEditorId
                         << " removed=" << (mActiveImageSpaceModifiers.size() != previousSize);
        return true;
    }

    void ESM4QuestRuntime::updateImageSpaceModifiers(float duration)
    {
        for (ActiveImageSpaceModifier& modifier : mActiveImageSpaceModifiers)
        {
            if (!modifier.mAnimatable || modifier.mDuration <= 0.f)
                continue;
            modifier.mState.mTime = std::clamp(modifier.mState.mTime + duration / modifier.mDuration, 0.f, 1.f);
        }

        // Timed IMADs are one-shot presentation effects.  Holding one at its
        // final keyframe makes a fade/black-screen overlay permanent; only
        // non-animatable modifiers stay active until the source script calls
        // `rimod` / RemoveImageSpaceModifier.
        std::erase_if(mActiveImageSpaceModifiers, [](const ActiveImageSpaceModifier& modifier) {
            return modifier.mAnimatable && modifier.mState.mTime >= 1.f;
        });
    }

    std::vector<ESM4::ImageSpaceModifierRuntimeState> ESM4QuestRuntime::getActiveImageSpaceModifiers() const
    {
        std::vector<ESM4::ImageSpaceModifierRuntimeState> result;
        result.reserve(mActiveImageSpaceModifiers.size());
        for (const ActiveImageSpaceModifier& entry : mActiveImageSpaceModifiers)
            result.push_back(entry.mState);
        return result;
    }

    std::vector<ESM4AuthoredGameModeTimer> ESM4QuestRuntime::compileAuthoredGameModeTimers(
        std::string_view source, std::string_view questEditorId)
    {
        const std::string canonicalQuest = normaliseSourceToken(questEditorId);
        if (source.empty() || canonicalQuest.empty())
            return {};

        // The source grammar intentionally remains narrow. We accept only a
        // timer decremented by GetSecondsPassed beneath a simple run flag, then
        // direct same-quest GetStage == N -> SetStage N branches in that
        // timer's else path. This is the retail GameMode countdown idiom used
        // by Bethesda openings, without assuming a quest, cell, or campaign.
        std::map<std::string, std::set<std::string>, std::less<>> runVariables;
        std::map<std::string, std::map<std::uint8_t, std::uint8_t>, std::less<>> transitions;
        std::set<std::string, std::less<>> ambiguousTransitions;
        bool inGameMode = false;
        std::vector<SourceConditionalFrame> conditionStack;

        std::istringstream stream{ std::string(source) };
        for (std::string line; std::getline(stream, line);)
        {
            const SourceTokens tokens = normaliseSourceTokens(tokenize(line));
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
                conditionStack.push_back({ SourceTokens(tokens.begin() + 1, tokens.end()), false });
                continue;
            }
            if (tokens[0] == "elseif")
            {
                if (!conditionStack.empty())
                {
                    conditionStack.back().mCondition = SourceTokens(tokens.begin() + 1, tokens.end());
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
                    const std::optional<std::string> conditionalTimer
                        = parseTimerPositiveCondition(timerFrame->mCondition);
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

        // IMAD playback is presentation state. It continues while an
        // authored MenuMode UI owns the rest of the simulation, so the VCG01
        // fade can complete behind the character-creation handoff.
        updateImageSpaceModifiers(duration);

        // MenuMode blocks remain live while the normal simulation is paused.
        // This is how authored character-creation menus hand a completed
        // choice back into their quest stage graph; it does not synthesize a
        // quest or campaign transition.
        if (MWBase::WindowManager* const windowManager = MWBase::Environment::tryGetWindowManager();
            windowManager != nullptr && windowManager->isGuiMode())
        {
            for (const auto& [questId, source] : mAuthoredMenuModeSources)
            {
                const ESM4::Quest* const quest = mStore->get<ESM4::Quest>().search(ESM::RefId(questId));
                ESM4QuestState* const state = quest != nullptr ? findState(*quest) : nullptr;
                if (state == nullptr || (state->mFlags & ESM4QuestState::Flag_Running) == 0
                    || (state->mFlags & (ESM4QuestState::Flag_Completed | ESM4QuestState::Flag_Failed)) != 0)
                    continue;
                executeStageSource(source, questId, duration, {}, "menumode");
            }
        }

        if (paused)
            return;

        // GameMode blocks are authored, data-owned progression. Execute the
        // source for every live quest with its own local-variable scope. This
        // replaces an opening-only clock with the same timer/branch code the
        // game data carries, including the UI choices that interrupt it.
        for (const auto& [questId, source] : mAuthoredGameModeSources)
        {
            const ESM4::Quest* const quest = mStore->get<ESM4::Quest>().search(ESM::RefId(questId));
            ESM4QuestState* const state = quest != nullptr ? findState(*quest) : nullptr;
            if (state == nullptr || (state->mFlags & ESM4QuestState::Flag_Running) == 0
                || (state->mFlags & (ESM4QuestState::Flag_Completed | ESM4QuestState::Flag_Failed)) != 0)
                continue;
            executeStageSource(source, questId, duration);
        }

        MWBase::World* const world = MWBase::Environment::tryGetWorld();
        if (world == nullptr)
            return;
        const MWWorld::Ptr player = world->getPlayerPtr();
        MWWorld::CellStore* const playerCell = player.isEmpty() ? nullptr : player.getCell();
        if (playerCell == nullptr || playerCell->getCell() == nullptr)
            return;
        const ESM::RefId playerCellId = playerCell->getCell()->getId();

        // GameMode blocks attached to placed actors have their own local
        // scope. Execute only the subset whose actor is live in the player's
        // current cell; that is the engine lifecycle for a local scripted
        // scene and prevents records in a different loaded cell from running
        // merely because their data is available. The record index remains
        // data-owned and no opening/campaign IDs are embedded here.
        for (const auto& [actorId, state] : mActorScriptStates)
        {
            MWWorld::Ptr actor = world->searchPtr(ESM::RefId(actorId), false, false);
            if (actor.isEmpty() || !actor.getClass().isActor() || state.mCell != playerCellId)
                continue;
            executeStageSource(state.mSource, {}, duration, actorId);
        }

        // Placed references can own GameMode blocks too.  Their local
        // variables belong to the placed object, exactly as they do for
        // trigger blocks, so run the source only while that object is live in
        // the player's current cell.  This intentionally has no quest, cell,
        // or editor-ID exception.
        for (const auto& [referenceId, state] : mReferenceScriptStates)
        {
            if (!state.mHasGameMode || state.mCell != playerCellId)
                continue;
            // ESM4 placed references can be indexed either by the FormID
            // RefId path or the legacy RefNum map depending on their base
            // type.  Trigger volumes commonly use the former, so accept both
            // just as the participant resolver does; otherwise an intact
            // source state can never observe its own volume.
            MWWorld::Ptr reference = world->searchPtr(ESM::RefId(referenceId), false, false);
            if (reference.isEmpty())
                reference = world->searchPtrByRefNum(referenceId);
            if (reference.isEmpty() || reference.getCell() == nullptr)
                continue;
            executeStageSource(state.mSource, {}, duration, {}, "gamemode", {}, referenceId);
        }

        // A scripted SayTo can legitimately start the next authored topic in
        // the INFO result of the previous one.  The dialogue manager rejects
        // overlapping voice streams, so retry only after the prior stream has
        // ended instead of dropping the source command.
        if (!mPendingAuthoredSays.empty())
        {
            std::vector<PendingAuthoredSay> pending;
            pending.swap(mPendingAuthoredSays);
            MWBase::SoundManager* const soundManager = MWBase::Environment::get().getSoundManager();
            for (PendingAuthoredSay& entry : pending)
            {
                const MWWorld::Ptr actor = world->searchPtr(ESM::RefId(entry.mActor), false, false);
                if (actor.isEmpty())
                    continue;
                if (soundManager != nullptr && soundManager->sayActive(actor))
                {
                    mPendingAuthoredSays.push_back(std::move(entry));
                    continue;
                }

                const ESM4::Dialogue* topic = nullptr;
                for (const ESM4::Dialogue& candidate : mStore->get<ESM4::Dialogue>())
                    if (Misc::StringUtils::ciEqual(candidate.mEditorId, entry.mTopic))
                    {
                        topic = &candidate;
                        break;
                    }
                if (topic == nullptr || !MWBase::Environment::get().getDialogueManager()->say(actor, ESM::RefId(topic->mId)))
                {
                    if (soundManager != nullptr && soundManager->sayActive(actor))
                    {
                        mPendingAuthoredSays.push_back(std::move(entry));
                        continue;
                    }
                    Log(Debug::Warning) << "FNV/ESM4 behavior: deferred Say could not start actor="
                                        << actor.getCellRef().getRefId() << " topic=" << entry.mTopic;
                    continue;
                }

                if (entry.mNotifyActorScript && entry.mActorScript)
                    mPendingActorScriptEvents.push_back(
                        { *entry.mActorScript, "saytodone", normaliseSourceToken(entry.mTopic) });
                Log(Debug::Info) << "FNV/ESM4 behavior: deferred Say started actor="
                                 << actor.getCellRef().getRefId() << " topic=" << entry.mTopic;
            }
        }

        // Trigger volumes are regular placed ESM4 references.  Their source
        // declares the named object which enters each event block, while its
        // authored bounds determine the volume.  This makes both player and
        // actor scene entry visible to the same source runtime, with no
        // quest/cell/editor-ID exception embedded in the engine.
        const auto resolveTriggerParticipant = [this, world, &player](std::string_view editorId) -> MWWorld::Ptr {
            if (editorId.empty() || Misc::StringUtils::ciEqual(editorId, "player"))
                return player;

            if (mStore != nullptr)
            {
                const auto findPlacedReference = [world, editorId](const auto& references) -> MWWorld::Ptr {
                    for (const auto& reference : references)
                    {
                        if (!Misc::StringUtils::ciEqual(reference.mEditorId, editorId))
                            continue;
                        // ESM4 placed actors are indexed through the normal
                        // world reference map; static trigger objects may be
                        // reached through the RefNum map.  A trigger's named
                        // participant is allowed to be either kind.
                        if (MWWorld::Ptr ptr = world->searchPtr(ESM::RefId(reference.mId), false, false);
                            !ptr.isEmpty())
                            return ptr;
                        if (MWWorld::Ptr ptr = world->searchPtrByRefNum(reference.mId); !ptr.isEmpty())
                            return ptr;
                    }
                    return {};
                };

                if (MWWorld::Ptr ptr = findPlacedReference(mStore->get<ESM4::Reference>()); !ptr.isEmpty())
                    return ptr;
                if (MWWorld::Ptr ptr = findPlacedReference(mStore->get<ESM4::ActorCharacter>()); !ptr.isEmpty())
                    return ptr;
                if (MWWorld::Ptr ptr = findPlacedReference(mStore->get<ESM4::ActorCreature>()); !ptr.isEmpty())
                    return ptr;
            }
            return world->searchPtr(ESM::RefId::stringRefId(std::string(editorId)), false, false);
        };
        const bool routeTrace = std::getenv("OPENMW_COMPAT_ROUTE_PATH") != nullptr;
        for (auto& [referenceId, state] : mReferenceScriptStates)
        {
            if (state.mTriggerParticipants.empty())
                continue;

            // Trigger volumes can be placed ESM4 references indexed through
            // the FormID RefId path rather than the legacy RefNum map.  Use
            // both world indexes, matching the named-participant resolver,
            // so a valid registered volume can dispatch its own events.
            MWWorld::Ptr reference = world->searchPtr(ESM::RefId(referenceId), false, false);
            if (reference.isEmpty())
                reference = world->searchPtrByRefNum(referenceId);
            if (reference.isEmpty() || reference.getCell() == nullptr)
            {
                if (routeTrace && state.mCell == playerCellId && !state.mLoggedUnresolvedForRoute)
                {
                    state.mLoggedUnresolvedForRoute = true;
                    Log(Debug::Info) << "FNV/ESM4 route trigger: unresolved volume reference=" << state.mEditorId
                                     << " form=" << ESM::RefId(referenceId) << " playerCell=" << playerCellId;
                }
                for (ReferenceScriptTriggerParticipant& participant : state.mTriggerParticipants)
                    participant.mWasInside = false;
                continue;
            }
            // A reference's serialized parent is useful for registration, but
            // it is not authoritative after streaming or a script-driven
            // cell move.  Event dispatch must follow the live placed object:
            // compare its current cell to the participant's current cell.
            if (reference.getCell() != player.getCell())
            {
                for (ReferenceScriptTriggerParticipant& participant : state.mTriggerParticipants)
                    participant.mWasInside = false;
                continue;
            }

            const float scale = std::abs(reference.getCellRef().getScale());
            const float radius = state.mTriggerRadius * scale;
            if (!(radius > 0.f) || !std::isfinite(radius))
            {
                if (routeTrace && !state.mLoggedInvalidRadiusForRoute)
                {
                    state.mLoggedInvalidRadiusForRoute = true;
                    Log(Debug::Info) << "FNV/ESM4 route trigger: invalid volume radius reference=" << state.mEditorId
                                     << " form=" << ESM::RefId(referenceId) << " radius=" << radius;
                }
                for (ReferenceScriptTriggerParticipant& participant : state.mTriggerParticipants)
                    participant.mWasInside = false;
                continue;
            }

            const osg::Vec3f referencePosition = reference.getRefData().getPosition().asVec3();
            for (ReferenceScriptTriggerParticipant& participant : state.mTriggerParticipants)
            {
                // An unqualified trigger block retains the previous player
                // semantics, while a qualified one is evaluated against its
                // authored placed actor/reference.
                const std::string_view actorArgument = participant.mArgument.empty()
                    ? std::string_view("player")
                    : std::string_view(participant.mArgument);
                const MWWorld::Ptr actor = resolveTriggerParticipant(actorArgument);
                if (actor.isEmpty() || actor.getCell() == nullptr || actor.getCell() != reference.getCell())
                {
                    if (routeTrace && !participant.mLoggedUnavailableForRoute)
                    {
                        participant.mLoggedUnavailableForRoute = true;
                        Log(Debug::Info) << "FNV/ESM4 route trigger: unavailable participant reference="
                                         << state.mEditorId << " argument=" << actorArgument
                                         << " resolved=" << !actor.isEmpty();
                    }
                    participant.mWasInside = false;
                    continue;
                }

                const osg::Vec3f actorPosition = actor.getRefData().getPosition().asVec3();
                const bool inside = (actorPosition - referencePosition).length2() <= radius * radius;
                const bool entered = inside && !participant.mWasInside;
                const bool left = !inside && participant.mWasInside;
                participant.mWasInside = inside;

                if (routeTrace && (entered || left))
                    Log(Debug::Info) << "FNV/ESM4 route trigger: volume=" << state.mEditorId
                                     << " argument=" << actorArgument << " transition=" << (entered ? "enter" : "leave")
                                     << " radius=" << radius;

                if (entered && participant.mHasEnter)
                {
                    executeStageSource(state.mSource, {}, duration, {}, "ontriggerenter", participant.mArgument,
                        referenceId, actorArgument);
                    Log(Debug::Info) << "FNV/ESM4 behavior: reference-script event reference=" << state.mEditorId
                                     << " event=ontriggerenter argument=" << actorArgument << " radius=" << radius;
                }
                if (inside && participant.mHasTrigger)
                    executeStageSource(state.mSource, {}, duration, {}, "ontrigger", participant.mArgument,
                        referenceId, actorArgument);
                if (left && participant.mHasLeave)
                {
                    executeStageSource(state.mSource, {}, duration, {}, "ontriggerleave", participant.mArgument,
                        referenceId, actorArgument);
                    Log(Debug::Info) << "FNV/ESM4 behavior: reference-script event reference=" << state.mEditorId
                                     << " event=ontriggerleave argument=" << actorArgument << " radius=" << radius;
                }
            }
        }

        if (mPendingActorScriptEvents.empty())
            return;

        std::vector<PendingActorScriptEvent> pending;
        pending.swap(mPendingActorScriptEvents);
        MWBase::SoundManager* const soundManager = MWBase::Environment::get().getSoundManager();
        for (const PendingActorScriptEvent& event : pending)
        {
            MWWorld::Ptr actor = world->searchPtr(ESM::RefId(event.mActor), false, false);
            if (actor.isEmpty())
                continue;
            // A voiced line completes only after the sound manager releases
            // the actor. Silent/missing voice assets complete on the following
            // simulation update, which is the same event boundary and avoids
            // relying on host input or wall-clock guesses.
            if (soundManager != nullptr && soundManager->sayActive(actor))
            {
                mPendingActorScriptEvents.push_back(event);
                continue;
            }
            const ActorScriptState* const state = findActorScriptState(event.mActor);
            if (state == nullptr)
                continue;
            executeStageSource(state->mSource, {}, duration, event.mActor, event.mEvent, event.mArgument);
            Log(Debug::Info) << "FNV/ESM4 behavior: actor-script event actor=" << state->mEditorId
                             << " event=" << event.mEvent << " argument=" << event.mArgument;
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

        // Fallout quests may use an otherwise undeclared stage as a state-only
        // transition from their GameMode script.  The Courier's VCG00 opening
        // quest, for example, advances 90 -> 95 before its timer advances it
        // to the declared stage 100.  Bethesda's SetStage records that state
        // even though there is no QUST stage entry to execute.
        if (stage == quest->mStages.end())
        {
            state->mFlags |= ESM4QuestState::Flag_Running;
            state->mCurrentStage = stageIndex;
            state->mStageDone[stageIndex] = true;
            Log(Debug::Info) << "FNV/ESM4 behavior: SetStage quest=" << quest->mEditorId
                             << " form=" << ESM::RefId(quest->mId).serializeText()
                             << " stage=" << static_cast<unsigned int>(stageIndex)
                             << " flags=" << static_cast<unsigned int>(state->mFlags)
                             << " done=true entryExecuted=false implicitStage=true";
            return true;
        }
        bool useWholeStageSourceFallback = false;
        if (stageContainsCompiledSetStage(*stage))
        {
            // A fully understood compiled SetStage chain is atomic.  If the
            // chain contains an unimplemented command, preserve the existing
            // whole-source fallback instead of discarding every authored
            // command in the stage.  The failed transaction has not committed
            // any state at this point.
            if (executeCompiledStageTransaction(id, stageIndex))
                return true;
            useWholeStageSourceFallback = true;
            Log(Debug::Info) << "FNV/ESM4 behavior: compiled SetStage transaction deferred to source fallback quest="
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
                // A failed transaction can only continue through the authored
                // source when the selected entry actually has a lossless
                // source representation.  Otherwise keep the existing
                // fail-closed behavior rather than executing a partial chain.
                if (entry.mScript.scriptSource.empty())
                {
                    Log(Debug::Warning) << "FNV/ESM4 behavior: compiled SetStage transaction has no source fallback quest="
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
            {
                // A stage source is the lossless fallback for compiled commands we
                // do not yet understand.  Keep capture diagnostics attached to
                // the data boundary rather than to a particular quest or scene:
                // this lets an unattended run prove the exact SCTX text that was
                // handed to the interpreter.
                if (std::getenv("OPENMW_AUTHORED_START_TELEMETRY") != nullptr)
                {
                    Log(Debug::Info) << "FNV/ESM4 telemetry: compiled source fallback quest=" << quest->mEditorId
                                     << " stage=" << static_cast<unsigned int>(stageIndex)
                                     << " bytes=" << entry.mScript.scriptSource.size()
                                     << " source='" << entry.mScript.scriptSource << "'";
                }
                executeStageSource(entry.mScript.scriptSource, quest->mId);
            }
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
            if (const auto found = state->mVariables.find(Misc::StringUtils::lowerCase(name));
                found != state->mVariables.end())
                found->second = value;
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
        float secondsPassed, std::optional<ESM::FormId> ownerActor, std::string_view selectedBlock,
        std::string_view selectedBlockArgument, std::optional<ESM::FormId> ownerReference,
        std::string_view actionReferenceArgument)
    {
        struct SourceExecutionFrame
        {
            bool mParentActive = false;
            bool mBranchTaken = false;
            bool mActive = false;
        };

        const auto sourceVariable = [this, ownerQuest, ownerActor, ownerReference](std::string_view token)
            -> std::optional<float> {
            const std::string normalised = normaliseSourceToken(token);
            if (normalised.empty())
                return std::nullopt;

            const auto valueFromQuest = [this](const ESM4::Quest& quest, std::string_view variable)
                -> std::optional<float> {
                const ESM4QuestState* const state = findState(quest);
                if (state == nullptr)
                    return std::nullopt;
                const auto found = state->mVariables.find(normaliseScriptVariable(variable));
                return found != state->mVariables.end() ? std::optional<float>(found->second) : std::nullopt;
            };

            const auto valueFromActor = [this](ESM::FormId actor, std::string_view variable) -> std::optional<float> {
                const ActorScriptState* const state = findActorScriptState(actor);
                if (state == nullptr)
                    return std::nullopt;
                const auto found = state->mVariables.find(normaliseScriptVariable(variable));
                return found != state->mVariables.end() ? std::optional<float>(found->second) : std::nullopt;
            };

            const auto valueFromReference = [this](ESM::FormId reference, std::string_view variable)
                -> std::optional<float> {
                const ReferenceScriptState* const state = findReferenceScriptState(reference);
                if (state == nullptr)
                    return std::nullopt;
                const auto found = state->mVariables.find(normaliseScriptVariable(variable));
                return found != state->mVariables.end() ? std::optional<float>(found->second) : std::nullopt;
            };

            if (const std::size_t separator = normalised.rfind('.'); separator != std::string::npos)
            {
                const ESM4::Quest* const quest = resolveQuest(normalised.substr(0, separator));
                if (quest != nullptr)
                    return valueFromQuest(*quest, normalised.substr(separator + 1));
                const std::optional<ESM::FormId> actor = resolveActorScript(normalised.substr(0, separator));
                if (actor)
                    return valueFromActor(*actor, normalised.substr(separator + 1));
                const std::optional<ESM::FormId> reference = resolveReferenceScript(normalised.substr(0, separator));
                return reference ? valueFromReference(*reference, normalised.substr(separator + 1)) : std::nullopt;
            }

            if (ownerQuest && mStore != nullptr)
            {
                const ESM4::Quest* const quest = mStore->get<ESM4::Quest>().search(ESM::RefId(*ownerQuest));
                return quest != nullptr ? valueFromQuest(*quest, normalised) : std::nullopt;
            }
            if (ownerActor)
                return valueFromActor(*ownerActor, normalised);
            return ownerReference ? valueFromReference(*ownerReference, normalised) : std::nullopt;
        };

        const std::string actionReference = normaliseSourceToken(actionReferenceArgument);
        const auto resolveAuthoredReference = [this](std::string_view editorId) -> MWWorld::Ptr {
            MWBase::World* const world = MWBase::Environment::tryGetWorld();
            if (world == nullptr)
                return {};
            if (Misc::StringUtils::ciEqual(editorId, "player"))
                return world->getPlayerPtr();

            if (mStore != nullptr)
            {
                const auto findPlacedReference = [&](const auto& references) -> MWWorld::Ptr {
                    for (const auto& reference : references)
                    {
                        if (!Misc::StringUtils::ciEqual(reference.mEditorId, editorId))
                            continue;
                        // Placed actor records live in the regular world
                        // lookup map, whereas many static references are
                        // additionally indexed by RefNum.  Check both
                        // engine-native identities so a source expression
                        // such as `SomeActorREF.IsCurrentFurnitureRef ...`
                        // does not silently lose its actor.
                        if (MWWorld::Ptr ptr = world->searchPtr(ESM::RefId(reference.mId), false, false);
                            !ptr.isEmpty())
                            return ptr;
                        if (MWWorld::Ptr ptr = world->searchPtrByRefNum(reference.mId); !ptr.isEmpty())
                            return ptr;
                    }
                    return {};
                };

                if (MWWorld::Ptr ptr = findPlacedReference(mStore->get<ESM4::Reference>()); !ptr.isEmpty())
                    return ptr;
                if (MWWorld::Ptr ptr = findPlacedReference(mStore->get<ESM4::ActorCharacter>()); !ptr.isEmpty())
                    return ptr;
                if (MWWorld::Ptr ptr = findPlacedReference(mStore->get<ESM4::ActorCreature>()); !ptr.isEmpty())
                    return ptr;
            }
            return world->searchPtr(ESM::RefId::stringRefId(std::string(editorId)), false, false);
        };

        const auto sourceValue = [this, &sourceVariable, &resolveAuthoredReference, secondsPassed,
                                     &actionReference](const SourceTokens& tokens,
                                     std::size_t& index) -> std::optional<float> {
            if (index >= tokens.size())
                return std::nullopt;
            const std::string& token = tokens[index++];
            if (token == "getsecondspassed")
                return secondsPassed;
            if (token == "getbuttonpressed")
            {
                if (MWBase::WindowManager* windowManager = MWBase::Environment::tryGetWindowManager())
                    return static_cast<float>(windowManager->readPressedButton());
                return -1.f;
            }
            if (token == "isactionref")
            {
                if (index >= tokens.size())
                    return std::nullopt;
                const std::string target = normaliseSourceToken(tokens[index++]);
                return !target.empty() && target == actionReference ? 1.f : 0.f;
            }
            if (const std::size_t separator = token.rfind('.'); separator != std::string::npos)
            {
                const std::string_view subject(token.data(), separator);
                const std::string_view command(token.data() + separator + 1, token.size() - separator - 1);
                if (command == "iscurrentfurnitureref")
                {
                    if (index >= tokens.size())
                        return std::nullopt;
                    const MWWorld::Ptr actor = resolveAuthoredReference(subject);
                    const MWWorld::Ptr furniture = resolveAuthoredReference(tokens[index++]);
                    if (actor.isEmpty() || furniture.isEmpty())
                        return 0.f;
                    MWBase::World* const world = MWBase::Environment::tryGetWorld();
                    const ESM::FormId furnitureRef = furniture.getCellRef().getRefNum();
                    if (world != nullptr && actor == world->getPlayerPtr())
                        return world->getPlayer().isOnFalloutFurniture(furnitureRef) ? 1.f : 0.f;
                    if (actor.getType() != ESM4::Npc::sRecordId)
                        return 0.f;
                    const MWClass::FalloutFurniturePlacement placement
                        = MWClass::ESM4Npc::getFurniturePlacement(actor);
                    return placement.mValid && placement.mFurnitureRef == furnitureRef ? 1.f : 0.f;
                }
            }
            if (token == "getstage")
            {
                if (index >= tokens.size())
                    return std::nullopt;
                const ESM4::Quest* const quest = resolveQuest(tokens[index++]);
                const ESM4QuestState* const state = quest != nullptr ? findState(*quest) : nullptr;
                return state != nullptr ? std::optional<float>(static_cast<float>(state->mCurrentStage)) : std::nullopt;
            }
            if (token == "getstagedone")
            {
                if (index + 1 >= tokens.size())
                    return std::nullopt;
                const ESM4::Quest* const quest = resolveQuest(tokens[index++]);
                std::int32_t stage = 0;
                if (quest == nullptr || !parseInt(tokens[index++], stage))
                    return std::nullopt;
                const ESM4QuestState* const state = findState(*quest);
                if (state == nullptr)
                    return std::nullopt;
                const auto found = state->mStageDone.find(static_cast<std::int16_t>(stage));
                return found != state->mStageDone.end() && found->second ? 1.f : 0.f;
            }
            if (token == "getobjectivedisplayed" || token == "getobjectivecompleted")
            {
                if (index + 1 >= tokens.size())
                    return std::nullopt;
                const ESM4::Quest* const quest = resolveQuest(tokens[index++]);
                std::int32_t objective = 0;
                if (quest == nullptr || !parseInt(tokens[index++], objective))
                    return std::nullopt;
                const ESM4QuestState* const state = findState(*quest);
                if (state == nullptr)
                    return std::nullopt;
                const auto found = state->mObjectiveStatus.find(objective);
                if (found == state->mObjectiveStatus.end())
                    return 0.f;
                const std::uint8_t flag = token == "getobjectivedisplayed"
                    ? ESM4QuestState::Objective_Displayed
                    : ESM4QuestState::Objective_Completed;
                return (found->second & flag) != 0 ? 1.f : 0.f;
            }
            float literal = 0.f;
            if (parseFloat(token, literal))
                return literal;
            return sourceVariable(token);
        };

        const auto sourceExpression = [&sourceValue](const SourceTokens& tokens, std::size_t& index)
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

        const auto evaluateSourceCondition = [&sourceExpression](const SourceTokens& condition) {
            const auto evaluateComparison = [&sourceExpression](const SourceTokens& tokens, std::size_t& index)
                -> std::optional<bool> {
                const std::optional<float> left = sourceExpression(tokens, index);
                if (!left)
                    return std::nullopt;
                if (index == tokens.size() || tokens[index] == "&&" || tokens[index] == "||")
                    return *left != 0.f;
                const std::string operation = tokens[index++];
                const std::optional<float> right = sourceExpression(tokens, index);
                if (!right)
                    return std::nullopt;
                if (operation == "==")
                    return *left == *right;
                if (operation == "!=")
                    return *left != *right;
                if (operation == ">")
                    return *left > *right;
                if (operation == ">=")
                    return *left >= *right;
                if (operation == "<")
                    return *left < *right;
                if (operation == "<=")
                    return *left <= *right;
                return std::nullopt;
            };

            std::size_t index = 0;
            std::optional<bool> value = evaluateComparison(condition, index);
            if (!value)
                return false;
            bool andValue = *value;
            bool combined = false;
            while (index < condition.size())
            {
                const std::string operation = condition[index++];
                std::optional<bool> next = evaluateComparison(condition, index);
                if (!next)
                    return false;
                if (operation == "&&")
                {
                    andValue = andValue && *next;
                    continue;
                }
                if (operation == "||")
                {
                    combined = combined || andValue;
                    andValue = *next;
                    continue;
                }
                return false;
            }
            return combined || andValue;
        };

        const bool traceRaceMenuSource = std::getenv("OPENMW_AUTHORED_START_TELEMETRY") != nullptr
            && source.find("ShowRaceMenu") != std::string_view::npos;
        if (traceRaceMenuSource)
            Log(Debug::Info) << "FNV/ESM4 telemetry: source contains ShowRaceMenu bytes=" << source.size()
                             << " selectedBlock=" << selectedBlock;

        std::vector<std::string> executableLines;
        std::vector<SourceExecutionFrame> conditionalStack;
        bool inExplicitBlock = false;
        bool inSelectedBlock = false;
        const std::string wantedBlock = normaliseSourceToken(selectedBlock);
        const std::string wantedBlockArgument = normaliseSourceToken(selectedBlockArgument);
        std::istringstream sourceStream{ std::string(source) };
        for (std::string line; std::getline(sourceStream, line);)
        {
            const SourceTokens tokens = normaliseSourceTokens(tokenize(line));
            if (tokens.empty())
                continue;
            if (tokens[0] == "begin")
            {
                inExplicitBlock = true;
                inSelectedBlock = tokens.size() >= 2 && tokens[1] == wantedBlock
                    && (wantedBlockArgument.empty()
                        || (tokens.size() >= 3 && tokens[2] == wantedBlockArgument));
                conditionalStack.clear();
                continue;
            }
            if (tokens[0] == "end")
            {
                inExplicitBlock = false;
                inSelectedBlock = false;
                conditionalStack.clear();
                continue;
            }
            if (inExplicitBlock && !inSelectedBlock)
                continue;

            const bool parentActive = conditionalStack.empty() || conditionalStack.back().mActive;
            if (tokens[0] == "if")
            {
                const bool active = parentActive && evaluateSourceCondition(
                    SourceTokens(tokens.begin() + 1, tokens.end()));
                conditionalStack.push_back({ parentActive, active, active });
                continue;
            }
            if (tokens[0] == "elseif")
            {
                if (conditionalStack.empty())
                    continue;
                SourceExecutionFrame& frame = conditionalStack.back();
                const bool active = frame.mParentActive && !frame.mBranchTaken
                    && evaluateSourceCondition(SourceTokens(tokens.begin() + 1, tokens.end()));
                frame.mActive = active;
                frame.mBranchTaken = frame.mBranchTaken || active;
                continue;
            }
            if (tokens[0] == "else")
            {
                if (!conditionalStack.empty())
                {
                    SourceExecutionFrame& frame = conditionalStack.back();
                    frame.mActive = frame.mParentActive && !frame.mBranchTaken;
                    frame.mBranchTaken = true;
                }
                continue;
            }
            if (tokens[0] == "endif")
            {
                if (!conditionalStack.empty())
                    conditionalStack.pop_back();
                continue;
            }
            if (parentActive)
                executableLines.push_back(std::move(line));
        }

        if (traceRaceMenuSource)
        {
            Log(Debug::Info) << "FNV/ESM4 telemetry: ShowRaceMenu source executableLineCount="
                             << executableLines.size();
            for (const std::string& executableLine : executableLines)
                Log(Debug::Info) << "FNV/ESM4 telemetry: ShowRaceMenu source line='" << executableLine << "'";
        }

        const auto setSourceVariable = [this, ownerQuest, ownerActor, ownerReference](std::string_view token,
                                           float value) {
            const std::string normalised = normaliseSourceToken(token);
            if (normalised.empty())
                return false;
            if (const std::size_t separator = normalised.rfind('.'); separator != std::string::npos)
            {
                const ESM4::Quest* const quest = resolveQuest(normalised.substr(0, separator));
                if (quest != nullptr)
                    return setQuestVariable(quest->mEditorId, normalised.substr(separator + 1), value);
                const std::optional<ESM::FormId> actor = resolveActorScript(normalised.substr(0, separator));
                if (actor)
                    return setActorScriptVariable(*actor, normalised.substr(separator + 1), value);
                const std::optional<ESM::FormId> reference = resolveReferenceScript(normalised.substr(0, separator));
                return reference && setReferenceScriptVariable(*reference, normalised.substr(separator + 1), value);
            }
            if (ownerQuest && mStore != nullptr)
            {
                const ESM4::Quest* const quest = mStore->get<ESM4::Quest>().search(ESM::RefId(*ownerQuest));
                return quest != nullptr && setQuestVariable(quest->mEditorId, normalised, value);
            }
            if (ownerActor)
                return setActorScriptVariable(*ownerActor, normalised, value);
            return ownerReference && setReferenceScriptVariable(*ownerReference, normalised, value);
        };

        const auto sayAuthoredTopic = [this](const MWWorld::Ptr& actor, std::string_view topicEditorId) {
            if (actor.isEmpty() || mStore == nullptr)
                return false;
            const ESM4::Dialogue* topic = nullptr;
            for (const ESM4::Dialogue& candidate : mStore->get<ESM4::Dialogue>())
                if (Misc::StringUtils::ciEqual(candidate.mEditorId, topicEditorId))
                {
                    topic = &candidate;
                    break;
                }
            if (topic == nullptr)
            {
                Log(Debug::Warning) << "FNV/ESM4 behavior: Say could not resolve topic=" << topicEditorId;
                return false;
            }
            const bool started = MWBase::Environment::get().getDialogueManager()->say(actor, ESM::RefId(topic->mId));
            Log(started ? Debug::Info : Debug::Warning) << "FNV/ESM4 behavior: Say actor="
                                                         << actor.getCellRef().getRefId() << " topic="
                                                         << topic->mEditorId << " started=" << started;
            return started;
        };

        const auto deferAuthoredSay = [this](const MWWorld::Ptr& actor, std::string_view topic,
                                          std::optional<ESM::FormId> actorScript, bool notifyActorScript) {
            MWBase::SoundManager* const soundManager = MWBase::Environment::get().getSoundManager();
            if (actor.isEmpty() || soundManager == nullptr || !soundManager->sayActive(actor))
                return false;
            const ESM::FormId actorId = actor.getCellRef().getRefNum();
            if (actorId.isZeroOrUnset())
                return false;
            const std::string normalisedTopic = normaliseSourceToken(topic);
            if (normalisedTopic.empty())
                return false;
            const auto duplicate = std::find_if(mPendingAuthoredSays.begin(), mPendingAuthoredSays.end(),
                [&actorId, &normalisedTopic](const PendingAuthoredSay& pending) {
                    return pending.mActor == actorId && pending.mTopic == normalisedTopic;
                });
            if (duplicate == mPendingAuthoredSays.end())
                mPendingAuthoredSays.push_back({ actorId, normalisedTopic, actorScript, notifyActorScript });
            Log(Debug::Info) << "FNV/ESM4 behavior: deferred Say actor=" << actor.getCellRef().getRefId()
                             << " topic=" << normalisedTopic << " queued="
                             << (duplicate == mPendingAuthoredSays.end());
            return true;
        };

        const auto invokeAuthoredCompatibilityCapability = [](std::string_view capability, std::string_view command,
                                                            std::optional<int> specialTotal = std::nullopt) {
            // Capabilities are intentionally semantic and engine-owned.  A
            // profile maps a source command to one of these names, but cannot
            // cause a source script to invoke arbitrary native behavior.
            if (capability == "character-appearance")
            {
                if (MWBase::WindowManager* const windowManager = MWBase::Environment::tryGetWindowManager())
                {
                    windowManager->showAuthoredRaceMenu();
                    Log(Debug::Info) << "OpenNV compatibility: authored command=" << command
                                     << " capability=" << capability << " handled=1";
                    return true;
                }
                Log(Debug::Warning) << "OpenNV compatibility: authored command=" << command
                                    << " capability=" << capability << " handled=0 window-manager-unavailable";
                return false;
            }

            if (capability != "character-special")
                return false;
            if (!specialTotal || *specialTotal < 7 || *specialTotal > 70)
            {
                Log(Debug::Warning) << "OpenNV compatibility: authored command=" << command
                                    << " capability=" << capability << " handled=0 invalid-special-total="
                                    << (specialTotal ? *specialTotal : -1);
                return false;
            }

            MWBase::World* const world = MWBase::Environment::tryGetWorld();
            if (world == nullptr)
            {
                Log(Debug::Warning) << "OpenNV compatibility: authored command=" << command
                                    << " capability=" << capability << " handled=0 world-unavailable";
                return false;
            }

            // ShowLoveTesterMenuParams declares the final SPECIAL total. Until
            // the interactive allocation panel is implemented, preserve the
            // current valid values and distribute any remaining points in the
            // canonical SPECIAL order. This produces a legal, reproducible
            // selection rather than bypassing the authored Vit-o-matic stage.
            constexpr std::size_t SpecialCount = 7;
            std::array<float, SpecialCount> values{};
            int currentTotal = 0;
            const MWWorld::FalloutPlayerRuntimeState& playerState = world->getFalloutPlayerRuntimeState();
            for (std::size_t index = 0; index < values.size(); ++index)
            {
                const auto current = playerState.getCurrentActorValue(
                    MWWorld::FalloutPlayerRuntimeState::SpecialActorValueBegin + static_cast<std::uint32_t>(index));
                if (!current)
                {
                    Log(Debug::Warning) << "OpenNV compatibility: authored command=" << command
                                        << " capability=" << capability
                                        << " handled=0 native-player-special-unavailable";
                    return false;
                }
                const float rounded = std::round(current->mValue);
                if (!std::isfinite(current->mValue) || std::abs(current->mValue - rounded) > 0.001f || rounded < 1.f
                    || rounded > 10.f)
                {
                    Log(Debug::Warning) << "OpenNV compatibility: authored command=" << command
                                        << " capability=" << capability
                                        << " handled=0 invalid-current-special value=" << current->mValue;
                    return false;
                }
                values[index] = rounded;
                currentTotal += static_cast<int>(rounded);
            }

            while (currentTotal < *specialTotal)
            {
                bool advanced = false;
                for (float& value : values)
                {
                    if (value >= 10.f)
                        continue;
                    ++value;
                    ++currentTotal;
                    advanced = true;
                    if (currentTotal == *specialTotal)
                        break;
                }
                if (!advanced)
                    break;
            }
            while (currentTotal > *specialTotal)
            {
                bool reduced = false;
                for (auto value = values.rbegin(); value != values.rend(); ++value)
                {
                    if (*value <= 1.f)
                        continue;
                    --*value;
                    --currentTotal;
                    reduced = true;
                    if (currentTotal == *specialTotal)
                        break;
                }
                if (!reduced)
                    break;
            }
            if (currentTotal != *specialTotal || !world->setFalloutPlayerSpecial(values))
            {
                Log(Debug::Warning) << "OpenNV compatibility: authored command=" << command
                                    << " capability=" << capability << " handled=0 special-allocation-failed total="
                                    << currentTotal;
                return false;
            }

            if (MWBase::WindowManager* const windowManager = MWBase::Environment::tryGetWindowManager())
            {
                std::ostringstream message;
                message << "Vit-o-matic Vigor Tester\n\nDefault S.P.E.C.I.A.L. allocation applied ("
                        << *specialTotal << " points):\nStrength " << values[0] << "   Perception " << values[1]
                        << "\nEndurance " << values[2] << "   Charisma " << values[3] << "\nIntelligence "
                        << values[4] << "   Agility " << values[5] << "   Luck " << values[6];
                windowManager->scheduleMessageBox(message.str(), MWGui::ShowInDialogueMode_Never);
            }
            Log(Debug::Info) << "FNV/ESM4 behavior: ShowLoveTesterMenuParams default selection total="
                             << *specialTotal << " values=" << values[0] << ',' << values[1] << ',' << values[2]
                             << ',' << values[3] << ',' << values[4] << ',' << values[5] << ',' << values[6]
                             << " capability=" << capability;
            return true;
        };

        for (const std::string& line : executableLines)
        {
            const std::vector<std::string_view> tokens = tokenize(line);
            if (tokens.empty())
                continue;
            const std::string commandToken = normaliseSourceToken(tokens[0]);

            if (std::getenv("OPENMW_AUTHORED_START_TELEMETRY") != nullptr
                && commandToken.find("racemenu") != std::string::npos)
            {
                Log(Debug::Info) << "FNV/ESM4 telemetry: authored source command token='" << tokens[0]
                                 << "' normalized='" << commandToken << "' bytes=" << tokens[0].size();
            }

            const std::size_t memberSeparator = tokens[0].find('.');
            if (memberSeparator != std::string_view::npos && memberSeparator != 0
                && memberSeparator + 1 < tokens[0].size())
            {
                const std::string_view subject = tokens[0].substr(0, memberSeparator);
                const std::string_view command = tokens[0].substr(memberSeparator + 1);
                MWBase::World* const world = MWBase::Environment::tryGetWorld();

                const auto resolveReference = [&](std::string_view editorId) -> MWWorld::Ptr {
                    if (world == nullptr)
                        return {};
                    if (Misc::StringUtils::ciEqual(editorId, "player"))
                        return world->getPlayerPtr();

                    if (mStore != nullptr)
                    {
                        const auto findPlacedReference = [&](const auto& references) -> MWWorld::Ptr {
                            for (const auto& reference : references)
                            {
                                if (!Misc::StringUtils::ciEqual(reference.mEditorId, editorId))
                                    continue;

                                // Actor references use the regular world
                                // map, while placed static references can be
                                // resolved through RefNum.  Script members
                                // such as StartConversation work for either.
                                if (MWWorld::Ptr ptr = world->searchPtr(ESM::RefId(reference.mId), false, false);
                                    !ptr.isEmpty())
                                    return ptr;
                                if (MWWorld::Ptr ptr = world->searchPtrByRefNum(reference.mId); !ptr.isEmpty())
                                    return ptr;
                            }
                            return {};
                        };

                        for (const ESM4::Reference& reference : mStore->get<ESM4::Reference>())
                            if (Misc::StringUtils::ciEqual(reference.mEditorId, editorId))
                                return world->searchPtrByRefNum(reference.mId);
                        if (MWWorld::Ptr ptr = findPlacedReference(mStore->get<ESM4::ActorCharacter>()); !ptr.isEmpty())
                            return ptr;
                        if (MWWorld::Ptr ptr = findPlacedReference(mStore->get<ESM4::ActorCreature>()); !ptr.isEmpty())
                            return ptr;
                    }
                    return world->searchPtr(ESM::RefId::stringRefId(std::string(editorId)), false, false);
                };

                const auto resolveActor = [&]() -> MWWorld::Ptr {
                    return resolveReference(subject);
                };

                if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(command, "MoveTo"))
                {
                    const std::string markerEditorId = removeQuotes(tokens[1]);
                    const MWWorld::Ptr actor = resolveActor();
                    const MWWorld::Ptr marker = resolveReference(markerEditorId);
                    if (world != nullptr && !actor.isEmpty() && !marker.isEmpty() && marker.getCell() != nullptr)
                    {
                        const MWWorld::Ptr moved = world->moveObject(actor, marker.getCell(),
                            marker.getRefData().getPosition().asVec3(), true, true);

                        // ESM4 MoveTo moves actors to an authored marker, but the marker itself is not necessarily
                        // on the final collision surface. Apply the engine's ordinary post-placement grounding pass
                        // for the player so every scripted player MoveTo receives the same collision recovery as a
                        // loaded player. This deliberately uses live cell collision rather than a quest, cell, or
                        // game-specific offset.
                        if (actor == world->getPlayerPtr())
                            world->adjustPosition(moved, true);

                        const ESM::Position& position = moved.getRefData().getPosition();
                        Log(Debug::Info) << "FNV/ESM4 behavior: MoveTo actor=" << subject
                                         << " marker=" << markerEditorId
                                         << " cell=" << marker.getCell()->getCell()->getId()
                                         << " position=(" << position.pos[0] << "," << position.pos[1] << ","
                                         << position.pos[2] << ")";
                        continue;
                    }

                    Log(Debug::Warning) << "FNV/ESM4 behavior: MoveTo could not resolve actor=" << subject
                                        << " marker=" << markerEditorId
                                        << " actorPresent=" << !actor.isEmpty()
                                        << " markerPresent=" << !marker.isEmpty();
                }
                else if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(command, "AddScriptPackage") && mStore != nullptr)
                {
                    const std::string packageEditorId = removeQuotes(tokens[1]);
                    const ESM4::AIPackage* package = nullptr;
                    for (const ESM4::AIPackage& candidate : mStore->get<ESM4::AIPackage>())
                        if (Misc::StringUtils::ciEqual(candidate.mEditorId, packageEditorId))
                        {
                            package = &candidate;
                            break;
                        }
                    const MWWorld::Ptr actor = resolveActor();
                    if (world != nullptr && package != nullptr && !actor.isEmpty()
                        && world->addESM4ScriptPackage(actor, package->mId))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: AddScriptPackage actor=" << subject
                                         << " package=" << package->mEditorId
                                         << " form=" << ESM::RefId(package->mId);
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "RemoveScriptPackage"))
                {
                    const MWWorld::Ptr actor = resolveActor();
                    if (world != nullptr && !actor.isEmpty() && world->removeESM4ScriptPackages(actor))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: RemoveScriptPackage actor=" << subject;
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "EVP")
                    || Misc::StringUtils::ciEqual(command, "EvaluatePackage"))
                {
                    const MWWorld::Ptr actor = resolveActor();
                    if (!actor.isEmpty() && MWClass::requestFnvAiPackageEvaluation(actor))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: EvaluatePackage actor=" << subject;
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "Enable")
                    || Misc::StringUtils::ciEqual(command, "Disable"))
                {
                    // Reference Enable/Disable is a world lifecycle operation,
                    // not merely a saved flag.  The World implementation adds
                    // or removes an active actor's regular mechanics state.
                    const MWWorld::Ptr reference = resolveActor();
                    if (world != nullptr && !reference.isEmpty())
                    {
                        const bool enable = Misc::StringUtils::ciEqual(command, "Enable");
                        const bool wasEnabled = reference.getRefData().isEnabled();
                        if (enable)
                            world->enable(reference);
                        else if (reference != world->getPlayerPtr())
                            world->disable(reference);
                        else
                        {
                            Log(Debug::Warning) << "FNV/ESM4 behavior: refused to disable player reference";
                            continue;
                        }
                        Log(Debug::Info) << "FNV/ESM4 behavior: reference " << (enable ? "enabled" : "disabled")
                                         << " reference=" << subject << " wasEnabled=" << wasEnabled;
                        continue;
                    }
                    Log(Debug::Warning) << "FNV/ESM4 behavior: " << command
                                        << " could not resolve reference=" << subject;
                }
                else if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(command, "SetOpenState"))
                {
                    // Fallout scripts use 0 for closed and 1 for open. Route
                    // that semantic request through the regular door state
                    // machine so physics, animation, and navigation receive
                    // the same change as an ordinary authored door action.
                    std::int32_t openState = 0;
                    const MWWorld::Ptr reference = resolveActor();
                    if (world != nullptr && parseInt(tokens[1], openState) && !reference.isEmpty()
                        && reference.getType() == ESM::REC_DOOR4 && (openState == 0 || openState == 1))
                    {
                        const MWWorld::DoorState state
                            = openState == 0 ? MWWorld::DoorState::Closing : MWWorld::DoorState::Opening;
                        world->activateDoor(reference, state);
                        Log(Debug::Info) << "FNV/ESM4 behavior: SetOpenState reference=" << subject
                                         << " state=" << openState;
                        continue;
                    }
                    Log(Debug::Warning) << "FNV/ESM4 behavior: SetOpenState unsupported reference=" << subject
                                        << " state=" << (tokens.size() >= 2 ? tokens[1] : std::string_view{})
                                        << " type=" << (reference.isEmpty() ? 0 : reference.getType());
                }
                else if ((Misc::StringUtils::ciEqual(command, "Say") && tokens.size() >= 2)
                    || (Misc::StringUtils::ciEqual(command, "SayTo") && tokens.size() >= 3))
                {
                    const bool sayTo = Misc::StringUtils::ciEqual(command, "SayTo");
                    const std::string topic = removeQuotes(tokens[sayTo ? 2 : 1]);
                    const MWWorld::Ptr actor = resolveActor();
                    const std::optional<ESM::FormId> actorScript = resolveActorScript(subject);
                    if (sayAuthoredTopic(actor, topic))
                    {
                        if (actorScript)
                            mPendingActorScriptEvents.push_back({ *actorScript, "saytodone", normaliseSourceToken(topic) });
                        continue;
                    }
                    if (deferAuthoredSay(actor, topic, actorScript, actorScript.has_value()))
                    {
                        continue;
                    }
                }
                else if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(command, "SexChange")
                    && Misc::StringUtils::ciEqual(subject, "player"))
                {
                    const std::string sex = normaliseSourceToken(tokens[1]);
                    const bool male = sex == "male";
                    const bool female = sex == "female";
                    if (world != nullptr && (male || female))
                    {
                        const MWWorld::Ptr player = world->getPlayerPtr();
                        const ESM::NPC* const npc = player.get<ESM::NPC>()->mBase;
                        MWBase::Environment::get().getMechanicsManager()->setPlayerRace(
                            npc->mRace, male, npc->mHead, npc->mHair);
                        Log(Debug::Info) << "FNV/ESM4 behavior: SexChange player=" << sex;
                        continue;
                    }
                }
            }

            if (((tokens.size() >= 2 && Misc::StringUtils::ciEqual(tokens[0], "Say"))
                    || (tokens.size() >= 3 && Misc::StringUtils::ciEqual(tokens[0], "SayTo")))
                && ownerActor)
            {
                MWBase::World* const world = MWBase::Environment::tryGetWorld();
                const MWWorld::Ptr actor = world != nullptr
                    ? world->searchPtr(ESM::RefId(*ownerActor), false, false)
                    : MWWorld::Ptr{};
                const bool sayTo = Misc::StringUtils::ciEqual(tokens[0], "SayTo");
                const std::string topic = removeQuotes(tokens[sayTo ? 2 : 1]);
                if (sayAuthoredTopic(actor, topic))
                {
                    mPendingActorScriptEvents.push_back({ *ownerActor, "saytodone", normaliseSourceToken(topic) });
                    continue;
                }
                if (deferAuthoredSay(actor, topic, ownerActor, true))
                {
                    continue;
                }
            }
            else if (tokens.size() >= 4 && Misc::StringUtils::ciEqual(tokens[0], "SetObjectiveDisplayed"))
            {
                std::int32_t objective = 0;
                std::int32_t displayed = 0;
                if (parseInt(tokens[2], objective) && parseInt(tokens[3], displayed)
                    && setObjectiveDisplayed(tokens[1], objective, displayed != 0))
                    continue;
            }
            else if ((Misc::StringUtils::ciEqual(tokens[0], "Enable")
                         || Misc::StringUtils::ciEqual(tokens[0], "Disable"))
                && (ownerReference || ownerActor))
            {
                MWBase::World* const world = MWBase::Environment::tryGetWorld();
                MWWorld::Ptr reference;
                if (world != nullptr)
                {
                    if (ownerReference)
                        reference = world->searchPtrByRefNum(*ownerReference);
                    else if (ownerActor)
                        reference = world->searchPtrByRefNum(*ownerActor);
                }
                if (!reference.isEmpty())
                {
                    const bool enable = Misc::StringUtils::ciEqual(tokens[0], "Enable");
                    const bool wasEnabled = reference.getRefData().isEnabled();
                    if (enable)
                        world->enable(reference);
                    else if (reference != world->getPlayerPtr())
                        world->disable(reference);
                    else
                    {
                        Log(Debug::Warning) << "FNV/ESM4 behavior: refused to disable player reference";
                        continue;
                    }
                    Log(Debug::Info) << "FNV/ESM4 behavior: reference " << (enable ? "enabled" : "disabled")
                                     << " reference=" << reference.getCellRef().getRefId()
                                     << " wasEnabled=" << wasEnabled;
                    continue;
                }
            }
            else if (tokens.size() >= 4 && Misc::StringUtils::ciEqual(tokens[0], "set")
                && Misc::StringUtils::ciEqual(tokens[2], "to"))
            {
                const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                std::size_t expression = 3;
                const std::optional<float> value = sourceExpression(sourceTokens, expression);
                if (value && expression == sourceTokens.size() && setSourceVariable(tokens[1], *value))
                    continue;
            }
            else if (tokens.size() >= 4 && Misc::StringUtils::ciEqual(tokens[0], "SetObjectiveCompleted"))
            {
                std::int32_t objective = 0;
                std::int32_t completed = 0;
                if (parseInt(tokens[2], objective) && parseInt(tokens[3], completed)
                    && setObjectiveCompleted(tokens[1], objective, completed != 0))
                    continue;
            }
            else if (tokens.size() >= 3 && Misc::StringUtils::ciEqual(tokens[0], "SetStage"))
            {
                std::int32_t stage = 0;
                if (std::getenv("OPENMW_AUTHORED_START_TELEMETRY") != nullptr)
                {
                    const ESM4QuestState* const before = search(tokens[1]);
                    Log(Debug::Info) << "FNV/ESM4 telemetry: source SetStage owner="
                                     << (ownerQuest ? ESM::RefId(*ownerQuest).serializeText() : std::string("<none>"))
                                     << " target=" << tokens[1] << " stage=" << tokens[2]
                                     << " currentStageBefore="
                                     << (before != nullptr ? static_cast<int>(before->mCurrentStage) : -1);
                }
                if (parseInt(tokens[2], stage) && stage >= 0 && stage <= 255
                    && setStage(tokens[1], static_cast<std::uint8_t>(stage)))
                    continue;
            }
            else if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(tokens[0], "StartQuest")
                && startQuest(tokens[1]))
                continue;
            else if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(tokens[0], "StopQuest")
                && stopQuest(tokens[1]))
                continue;
            else if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(tokens[0], "CompleteQuest")
                && completeQuest(tokens[1]))
                continue;
            else if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(tokens[0], "FailQuest")
                && failQuest(tokens[1]))
                continue;
            else if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(tokens[0], "ForceActiveQuest")
                && forceActiveQuest(tokens[1]))
                continue;
            // Fallout 3 / New Vegas source commonly uses the short-form
            // `imod` / `rimod` spellings.  They are the same engine commands
            // as ApplyImageSpaceModifier / RemoveImageSpaceModifier, so route
            // both spellings through the renderer-backed implementation.
            else if (tokens.size() >= 2
                && (Misc::StringUtils::ciEqual(tokens[0], "ApplyImageSpaceModifier")
                    || Misc::StringUtils::ciEqual(tokens[0], "imod"))
                && applyImageSpaceModifier(removeQuotes(tokens[1])))
                continue;
            else if (tokens.size() >= 2
                && (Misc::StringUtils::ciEqual(tokens[0], "RemoveImageSpaceModifier")
                    || Misc::StringUtils::ciEqual(tokens[0], "rimod"))
                && removeImageSpaceModifier(removeQuotes(tokens[1])))
                continue;
            else if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(tokens[0], "PlayBink"))
            {
                const std::string asset = removeQuotes(tokens[1]);
                std::int32_t allowSkippingValue = 1;
                const bool allowSkipping = tokens.size() < 3
                    || (parseInt(tokens[2], allowSkippingValue) && allowSkippingValue != 0);
                if (!asset.empty())
                {
                    if (MWBase::WindowManager* windowManager = MWBase::Environment::tryGetWindowManager())
                    {
                        try
                        {
                            // WindowManager owns the synchronous presentation
                            // loop, video audio stream, and its authored skip
                            // setting.  Returning here means the script may
                            // continue into its next authored stage.
                            windowManager->playVideo(asset, allowSkipping);
                            mPlayedStageVideos.push_back(asset);
                            Log(Debug::Info) << "FNV/ESM4 behavior: PlayBink completed asset='" << asset
                                             << "' allowSkipping=" << allowSkipping;
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
            else if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(tokens[0], "ShowMessage") && mStore != nullptr)
            {
                const std::string messageEditorId = removeQuotes(tokens[1]);
                const ESM4::Message* message = nullptr;
                for (const ESM4::Message& candidate : mStore->get<ESM4::Message>())
                    if (Misc::StringUtils::ciEqual(candidate.mEditorId, messageEditorId))
                    {
                        message = &candidate;
                        break;
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
                    Log(Debug::Info) << "FNV/ESM4 behavior: ShowMessage message=" << message->mEditorId
                                     << " buttons=" << message->mButtons.size();
                    continue;
                }
                Log(Debug::Warning) << "FNV/ESM4 behavior: ShowMessage could not resolve message=" << messageEditorId;
            }
            else if (commandToken == "showracemenu")
            {
                if (invokeAuthoredCompatibilityCapability("character-appearance", commandToken))
                    continue;
            }
            else if (const auto mapping = mAuthoredCompatibilityCommands.find(commandToken);
                mapping != mAuthoredCompatibilityCommands.end())
            {
                std::optional<int> specialTotal;
                if (mapping->second == "character-special" && tokens.size() >= 2)
                {
                    int parsed = 0;
                    if (parseInt(tokens[1], parsed))
                        specialTotal = parsed;
                }
                if (invokeAuthoredCompatibilityCapability(mapping->second, commandToken, specialTotal))
                    continue;
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "GetPlayerName"))
            {
                if (MWBase::WindowManager* const windowManager = MWBase::Environment::tryGetWindowManager())
                {
                    windowManager->showAuthoredNameMenu();
                    continue;
                }
            }
            else if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(tokens[0], "SetInCharGen"))
            {
                std::int32_t value = 0;
                if (parseInt(tokens[1], value) && setAuthoredCharGenState(value))
                    continue;
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "DisablePlayerControls"))
            {
                // The engine is already in character generation when a new game
                // begins.  A later stage can therefore carry only the control
                // lock while still relying on the original SetInCharGen intent to
                // keep the presentation free of gameplay HUD and cursor widgets.
                if (isAuthoredCharGenActive())
                    setAuthoredGameplayOverlayVisible(false, "DisablePlayerControls-charGen");
                continue;
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "EnablePlayerControls"))
            {
                if (!isAuthoredCharGenActive())
                    setAuthoredGameplayOverlayVisible(true, "EnablePlayerControls");
                continue;
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
        if (found == mReferenceScriptStates.end() || !hasSourceBlock(found->second.mSource, "onactivate"))
            return false;

        MWBase::World* const world = MWBase::Environment::tryGetWorld();
        const bool playerActivated = world != nullptr && actor == world->getPlayerPtr();
        const std::string_view argument = playerActivated ? "player" : std::string_view{};
        executeStageSource(found->second.mSource, {}, 0.f, {}, "onactivate", {}, found->first, argument);
        Log(Debug::Info) << "FNV/ESM4 behavior: reference-script event reference=" << found->second.mEditorId
                         << " event=onactivate argument=" << argument;
        return true;
    }

    void ESM4QuestRuntime::onActorScriptPackageDone(const MWWorld::Ptr& actor, ESM::FormId package)
    {
        if (actor.isEmpty() || mStore == nullptr || package.isZeroOrUnset())
            return;
        const ActorScriptState* const state = findActorScriptState(actor.getCellRef().getRefNum());
        const ESM4::AIPackage* const packageRecord = mStore->get<ESM4::AIPackage>().search(ESM::RefId(package));
        if (packageRecord == nullptr || packageRecord->mEditorId.empty())
            return;

        if (!packageRecord->mOnEndScript.scriptSource.empty())
        {
            executeResultSource(packageRecord->mOnEndScript.scriptSource);
            Log(Debug::Info) << "FNV/ESM4 behavior: package-script event package=" << packageRecord->mEditorId
                             << " event=onend sourceBytes=" << packageRecord->mOnEndScript.scriptSource.size();
        }

        if (state == nullptr || !hasSourceBlock(state->mSource, "onpackagedone", packageRecord->mEditorId))
            return;

        executeStageSource(state->mSource, {}, 0.f, state->mActor, "onpackagedone", packageRecord->mEditorId);
        Log(Debug::Info) << "FNV/ESM4 behavior: actor-script event actor=" << state->mEditorId
                         << " event=onpackagedone argument=" << packageRecord->mEditorId;
    }

    bool ESM4QuestRuntime::packageCompletionHasAuthoredHandler(const MWWorld::Ptr& actor, ESM::FormId package) const
    {
        if (actor.isEmpty() || mStore == nullptr || package.isZeroOrUnset())
            return false;
        const ESM4::AIPackage* const packageRecord = mStore->get<ESM4::AIPackage>().search(ESM::RefId(package));
        return packageRecord != nullptr
            && (!packageRecord->mOnEndScript.scriptSource.empty() || actorScriptHandlesPackageDone(actor, package));
    }

    bool ESM4QuestRuntime::actorScriptHandlesPackageDone(const MWWorld::Ptr& actor, ESM::FormId package) const
    {
        if (actor.isEmpty() || mStore == nullptr || package.isZeroOrUnset())
            return false;
        const ActorScriptState* const state = findActorScriptState(actor.getCellRef().getRefNum());
        const ESM4::AIPackage* const packageRecord = mStore->get<ESM4::AIPackage>().search(ESM::RefId(package));
        return state != nullptr && packageRecord != nullptr && !packageRecord->mEditorId.empty()
            && hasSourceBlock(state->mSource, "onpackagedone", packageRecord->mEditorId);
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

    ESM4QuestRuntime::ActorScriptState* ESM4QuestRuntime::findActorScriptState(ESM::FormId actor)
    {
        const auto found = mActorScriptStates.find(actor);
        return found != mActorScriptStates.end() ? &found->second : nullptr;
    }

    const ESM4QuestRuntime::ActorScriptState* ESM4QuestRuntime::findActorScriptState(ESM::FormId actor) const
    {
        const auto found = mActorScriptStates.find(actor);
        return found != mActorScriptStates.end() ? &found->second : nullptr;
    }

    std::optional<ESM::FormId> ESM4QuestRuntime::resolveActorScript(std::string_view id) const
    {
        const std::string key = normaliseSourceToken(id);
        if (key.empty() || mAmbiguousActorScriptEditorIds.contains(key))
            return std::nullopt;
        const auto found = mActorScriptEditorIds.find(key);
        return found != mActorScriptEditorIds.end() ? std::optional<ESM::FormId>(found->second) : std::nullopt;
    }

    bool ESM4QuestRuntime::setActorScriptVariable(ESM::FormId actor, std::string_view variable, float value)
    {
        ActorScriptState* const state = findActorScriptState(actor);
        if (state == nullptr)
            return false;
        const std::string key = normaliseScriptVariable(variable);
        const auto found = state->mVariables.find(key);
        if (found == state->mVariables.end())
            return false;
        found->second = value;
        Log(Debug::Info) << "FNV/ESM4 behavior: SetActorScriptVariable actor=" << state->mEditorId
                         << " variable=" << variable << " value=" << value;
        return true;
    }

    ESM4QuestRuntime::ReferenceScriptState* ESM4QuestRuntime::findReferenceScriptState(ESM::FormId reference)
    {
        const auto found = mReferenceScriptStates.find(reference);
        return found != mReferenceScriptStates.end() ? &found->second : nullptr;
    }

    const ESM4QuestRuntime::ReferenceScriptState* ESM4QuestRuntime::findReferenceScriptState(
        ESM::FormId reference) const
    {
        const auto found = mReferenceScriptStates.find(reference);
        return found != mReferenceScriptStates.end() ? &found->second : nullptr;
    }

    std::optional<ESM::FormId> ESM4QuestRuntime::resolveReferenceScript(std::string_view id) const
    {
        const std::string key = normaliseSourceToken(id);
        if (key.empty() || mAmbiguousReferenceScriptEditorIds.contains(key))
            return std::nullopt;
        const auto found = mReferenceScriptEditorIds.find(key);
        return found != mReferenceScriptEditorIds.end() ? std::optional<ESM::FormId>(found->second) : std::nullopt;
    }

    bool ESM4QuestRuntime::setReferenceScriptVariable(ESM::FormId reference, std::string_view variable, float value)
    {
        ReferenceScriptState* const state = findReferenceScriptState(reference);
        if (state == nullptr)
            return false;
        const std::string key = normaliseScriptVariable(variable);
        const auto found = state->mVariables.find(key);
        if (found == state->mVariables.end())
            return false;
        found->second = value;
        Log(Debug::Info) << "FNV/ESM4 behavior: SetReferenceScriptVariable reference=" << state->mEditorId
                         << " variable=" << variable << " value=" << value;
        return true;
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

                    candidates.push_back({ quest.mId, static_cast<std::uint8_t>(stage.mIndex), opening->mActivationStage, marker->mId, marker->mParent,
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
