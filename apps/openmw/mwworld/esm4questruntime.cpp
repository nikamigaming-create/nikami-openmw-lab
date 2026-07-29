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
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loaddial.hpp>
#include <components/esm4/loadfact.hpp>
#include <components/esm4/loadflst.hpp>
#include <components/esm4/loadglob.hpp>
#include <components/esm4/loadimad.hpp>
#include <components/esm4/loadidle.hpp>
#include <components/esm4/loadpack.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadcont.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loaddoor.hpp>
#include <components/esm4/loadmesg.hpp>
#include <components/esm4/loadnote.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadperk.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadrepu.hpp>
#include <components/esm4/loadscpt.hpp>
#include <components/esm4/loadsndr.hpp>
#include <components/esm4/loadsoun.hpp>
#include <components/esm4/loadspel.hpp>
#include <components/esm4/loadwrld.hpp>
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

    std::optional<MWWorld::ESM4QuestActorFlag> actorFlagFromSourceCommand(std::string_view command)
    {
        if (Misc::StringUtils::ciEqual(command, "SetUnconscious")
            || Misc::StringUtils::ciEqual(command, "GetUnconscious"))
            return MWWorld::ESM4QuestActorFlag::Unconscious;
        if (Misc::StringUtils::ciEqual(command, "SetRestrained")
            || Misc::StringUtils::ciEqual(command, "GetRestrained"))
            return MWWorld::ESM4QuestActorFlag::Restrained;
        if (Misc::StringUtils::ciEqual(command, "SetPlayerTeammate")
            || Misc::StringUtils::ciEqual(command, "GetPlayerTeammate"))
            return MWWorld::ESM4QuestActorFlag::PlayerTeammate;
        if (Misc::StringUtils::ciEqual(command, "IgnoreCrime")
            || Misc::StringUtils::ciEqual(command, "GetIgnoreCrime"))
            return MWWorld::ESM4QuestActorFlag::IgnoreCrime;
        if (Misc::StringUtils::ciEqual(command, "SetGhost")
            || Misc::StringUtils::ciEqual(command, "GetIsGhost"))
            return MWWorld::ESM4QuestActorFlag::Ghost;
        if (Misc::StringUtils::ciEqual(command, "SetIgnoreFriendlyHits")
            || Misc::StringUtils::ciEqual(command, "GetIgnoreFriendlyHits"))
            return MWWorld::ESM4QuestActorFlag::IgnoreFriendlyHits;
        if (Misc::StringUtils::ciEqual(command, "SetAlert")
            || Misc::StringUtils::ciEqual(command, "GetIsAlerted"))
            return MWWorld::ESM4QuestActorFlag::Alert;
        return std::nullopt;
    }

    std::string removeQuotes(std::string_view value)
    {
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"')
                                  || (value.front() == '\'' && value.back() == '\'')))
            value = value.substr(1, value.size() - 2);
        return std::string(value);
    }

    std::string_view trim(std::string_view value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.remove_prefix(1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.remove_suffix(1);
        return value;
    }

    void recordAllyPair(MWWorld::ESM4QuestState& state, ESM::FormId first, ESM::FormId second)
    {
        if (second.toUint32() < first.toUint32())
            std::swap(first, second);
        const std::pair pair{ first, second };
        std::erase_if(state.mEnemies, [first, second](const MWWorld::ESM4QuestState::EnemyRelation& value) {
            return value.mFirst == first && value.mSecond == second;
        });
        if (std::find(state.mAllies.begin(), state.mAllies.end(), pair) == state.mAllies.end())
            state.mAllies.push_back(pair);
    }

    void recordEnemyRelation(MWWorld::ESM4QuestState& state, ESM::FormId first, ESM::FormId second,
        bool firstTreatsSecondAsNeutral, bool secondTreatsFirstAsNeutral)
    {
        if (second.toUint32() < first.toUint32())
        {
            std::swap(first, second);
            std::swap(firstTreatsSecondAsNeutral, secondTreatsFirstAsNeutral);
        }
        const auto found = std::find_if(state.mEnemies.begin(), state.mEnemies.end(),
            [first, second](const MWWorld::ESM4QuestState::EnemyRelation& value) {
                return value.mFirst == first && value.mSecond == second;
            });
        const MWWorld::ESM4QuestState::EnemyRelation relation{
            first, second, firstTreatsSecondAsNeutral, secondTreatsFirstAsNeutral
        };
        std::erase(state.mAllies, std::pair{ first, second });
        if (found == state.mEnemies.end())
            state.mEnemies.push_back(relation);
        else
            *found = relation;
    }

    bool isControlKeyword(std::string_view line, std::string_view keyword, bool allowOpeningParenthesis = false)
    {
        if (line.size() < keyword.size()
            || !Misc::StringUtils::ciEqual(line.substr(0, keyword.size()), keyword))
            return false;
        if (line.size() == keyword.size())
            return true;
        const char next = line[keyword.size()];
        return std::isspace(static_cast<unsigned char>(next)) || (allowOpeningParenthesis && next == '(');
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
        mReferenceIds.clear();
        mFactionIds.clear();
        mInventoryItemIds.clear();
        mFormListIds.clear();
        mReputationIds.clear();
        mNoteIds.clear();
        mPerkIds.clear();
        mActorBaseIds.clear();
        mSpellIds.clear();
        mMessageIds.clear();
        mIdleAnimationIds.clear();
        mSoundIds.clear();
        mCellIds.clear();
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

    bool ESM4QuestRuntime::playImageSpaceModifier(ESM::FormId modifierId, float strength)
    {
        if (mStore == nullptr || modifierId.isZeroOrUnset() || !std::isfinite(strength)
            || strength <= 0.f)
            return false;

        const ESM4::ImageSpaceModifier* modifier
            = mStore->get<ESM4::ImageSpaceModifier>().search(ESM::RefId(modifierId));
        if (modifier == nullptr || !std::isfinite(modifier->mDuration) || modifier->mDuration <= 0.f)
            return false;

        const bool animatable = (modifier->mAdapterFlags & 0x1u) != 0;
        mActiveImageSpaceModifiers.push_back(
            { { modifierId, 0.f, strength }, modifier->mDuration, animatable });
        return true;
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
            prepared.mUseSourceFallback = !script.scriptSource.empty();
            return true;
        }

        std::vector<ESM4::ScriptBytecodeInstruction> instructions;
        if (!ESM4::decodeFalloutScriptBytecode(script.compiledData, instructions).succeeded())
            return false;

        const auto readUint16 = [](std::span<const std::uint8_t> bytes, std::size_t offset) {
            return static_cast<std::uint16_t>(
                bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
        };
        std::vector<bool> conditionalElseSeen;
        for (const ESM4::ScriptBytecodeInstruction& instruction : instructions)
        {
            if (instruction.callingReferenceIndex
                && (*instruction.callingReferenceIndex == 0
                    || *instruction.callingReferenceIndex > script.references.size()))
                return false;

            if (instruction.opcode != 0x0015
                && instruction.opcode != 0x0016 && instruction.opcode != 0x0017
                && instruction.opcode != 0x0018 && instruction.opcode != 0x0019
                && instruction.opcode != 0x1002
                && instruction.opcode != 0x101d
                && instruction.opcode != 0x1021 && instruction.opcode != 0x1022
                && instruction.opcode != 0x1034 && instruction.opcode != 0x1036
                && instruction.opcode != 0x1037
                && instruction.opcode != 0x1039 && instruction.opcode != 0x1052
                && instruction.opcode != 0x1055 && instruction.opcode != 0x1059 && instruction.opcode != 0x105e
                && instruction.opcode != 0x1071 && instruction.opcode != 0x1073
                && instruction.opcode != 0x1078 && instruction.opcode != 0x1079
                && instruction.opcode != 0x108b
                && instruction.opcode != 0x1097 && instruction.opcode != 0x1098
                && instruction.opcode != 0x109e
                && instruction.opcode != 0x10cc
                && instruction.opcode != 0x1111
                && instruction.opcode != 0x114a
                && instruction.opcode != 0x1177 && instruction.opcode != 0x117c
                && instruction.opcode != 0x117d
                && instruction.opcode != 0x1239
                && instruction.opcode != 0x11a2
                && instruction.opcode != 0x11a3 && instruction.opcode != 0x11ad && instruction.opcode != 0x11dd
                && instruction.opcode != 0x11fa)
            {
                prepared.mUseSourceFallback = true;
                prepared.mUnsupportedOpcodes.push_back(instruction.opcode);
                continue;
            }
            std::span<const std::uint8_t> argumentPayload = instruction.arguments;
            if (instruction.opcode == 0x0016 || instruction.opcode == 0x0018) // If / ElseIf
            {
                if (instruction.callingReferenceIndex || argumentPayload.size() < 4)
                    return false;
                if (instruction.opcode == 0x0018
                    && (conditionalElseSeen.empty() || conditionalElseSeen.back()))
                    return false;
                const std::uint16_t expressionSize = readUint16(argumentPayload, 2);
                if (expressionSize != argumentPayload.size() - 4)
                    return false;
                const std::span<const std::uint8_t> expression = argumentPayload.subspan(4);

                enum class ExpressionDecodeResult
                {
                    Supported,
                    Unsupported,
                    Malformed,
                };
                CompiledQuestCondition condition;
                const auto decodeValue = [&](std::uint16_t functionOpcode,
                                             const std::vector<ESM4::ScriptBytecodeArgument>& functionArguments,
                                             CompiledConditionToken& token) {
                    token.mType = CompiledConditionTokenType::Value;
                    if (functionOpcode == 0x103a && functionArguments.size() == 1)
                    {
                        const ESM::FormId* quest = std::get_if<ESM::FormId>(&functionArguments[0]);
                        if (quest == nullptr)
                            return ExpressionDecodeResult::Malformed;
                        token.mValueType = CompiledConditionValueType::GetStage;
                        token.mQuest = *quest;
                    }
                    else if (functionOpcode == 0x103b && functionArguments.size() == 2)
                    {
                        const ESM::FormId* quest = std::get_if<ESM::FormId>(&functionArguments[0]);
                        const std::int32_t* stage = std::get_if<std::int32_t>(&functionArguments[1]);
                        if (quest == nullptr || stage == nullptr)
                            return ExpressionDecodeResult::Malformed;
                        token.mValueType = CompiledConditionValueType::GetStageDone;
                        token.mQuest = *quest;
                        token.mStage = *stage;
                    }
                    else if ((functionOpcode == 0x11a4 || functionOpcode == 0x11a5)
                        && functionArguments.size() == 2)
                    {
                        const ESM::FormId* quest = std::get_if<ESM::FormId>(&functionArguments[0]);
                        const std::int32_t* objective = std::get_if<std::int32_t>(&functionArguments[1]);
                        if (quest == nullptr || objective == nullptr)
                            return ExpressionDecodeResult::Malformed;
                        token.mValueType = functionOpcode == 0x11a4
                            ? CompiledConditionValueType::GetObjectiveCompleted
                            : CompiledConditionValueType::GetObjectiveDisplayed;
                        token.mQuest = *quest;
                        token.mStage = *objective;
                    }
                    else if ((functionOpcode == 0x1038 || functionOpcode == 0x1222)
                        && functionArguments.size() == 1)
                    {
                        const ESM::FormId* quest = std::get_if<ESM::FormId>(&functionArguments[0]);
                        if (quest == nullptr)
                            return ExpressionDecodeResult::Malformed;
                        token.mValueType = functionOpcode == 0x1038
                            ? CompiledConditionValueType::GetQuestRunning
                            : CompiledConditionValueType::GetQuestCompleted;
                        token.mQuest = *quest;
                    }
                    else
                        return ExpressionDecodeResult::Unsupported;
                    if (mStore == nullptr
                        || mStore->get<ESM4::Quest>().search(ESM::RefId(token.mQuest)) == nullptr
                        || !mStates.contains(token.mQuest))
                        return ExpressionDecodeResult::Malformed;
                    return ExpressionDecodeResult::Supported;
                };

                ExpressionDecodeResult decodeResult = ExpressionDecodeResult::Supported;
                std::size_t expressionOffset = 0;
                std::size_t stackDepth = 0;
                while (expressionOffset < expression.size())
                {
                    if (expression[expressionOffset++] != 0x20 || expressionOffset == expression.size())
                    {
                        decodeResult = ExpressionDecodeResult::Malformed;
                        break;
                    }

                    CompiledConditionToken token;
                    if (expression[expressionOffset] == 0x72)
                    {
                        if (expression.size() - expressionOffset < 4)
                        {
                            decodeResult = ExpressionDecodeResult::Malformed;
                            break;
                        }
                        const std::uint16_t referenceIndex = readUint16(expression, expressionOffset + 1);
                        if (referenceIndex == 0 || referenceIndex > script.references.size() || mStore == nullptr)
                        {
                            decodeResult = ExpressionDecodeResult::Malformed;
                            break;
                        }
                        token.mQuest = script.references[referenceIndex - 1];
                        if (expression[expressionOffset + 3] == 0x73)
                        {
                            if (expression.size() - expressionOffset < 6)
                            {
                                decodeResult = ExpressionDecodeResult::Malformed;
                                break;
                            }
                            const std::uint16_t variableIndex = readUint16(expression, expressionOffset + 4);
                            const ESM4::Quest* quest
                                = mStore->get<ESM4::Quest>().search(ESM::RefId(token.mQuest));
                            const ESM4::Script* questScript = quest == nullptr
                                ? nullptr
                                : mStore->get<ESM4::Script>().search(ESM::RefId(quest->mQuestScript));
                            if (quest == nullptr || findState(*quest) == nullptr || questScript == nullptr)
                            {
                                decodeResult = ExpressionDecodeResult::Malformed;
                                break;
                            }
                            const auto variable = std::ranges::find(questScript->mScript.localVarData, variableIndex,
                                &ESM4::ScriptLocalVariableData::index);
                            if (variable == questScript->mScript.localVarData.end()
                                || variable->variableName.empty())
                            {
                                decodeResult = ExpressionDecodeResult::Malformed;
                                break;
                            }
                            token.mType = CompiledConditionTokenType::Value;
                            token.mValueType = CompiledConditionValueType::QuestVariable;
                            token.mVariable = Misc::StringUtils::lowerCase(variable->variableName);
                            expressionOffset += 6;
                        }
                        else if (expression[expressionOffset + 3] == 0x58)
                        {
                            if (expression.size() - expressionOffset < 8)
                            {
                                decodeResult = ExpressionDecodeResult::Malformed;
                                break;
                            }
                            const std::uint16_t functionOpcode = readUint16(expression, expressionOffset + 4);
                            const std::uint16_t functionArgumentSize = readUint16(expression, expressionOffset + 6);
                            if (functionArgumentSize > expression.size() - expressionOffset - 8)
                            {
                                decodeResult = ExpressionDecodeResult::Malformed;
                                break;
                            }
                            if (functionOpcode != 0x102e)
                            {
                                decodeResult = ExpressionDecodeResult::Unsupported;
                                break;
                            }
                            if (functionArgumentSize != 0)
                            {
                                decodeResult = ExpressionDecodeResult::Malformed;
                                break;
                            }
                            const bool actorExists
                                = mStore->get<ESM4::ActorCharacter>().search(token.mQuest) != nullptr
                                || mStore->get<ESM4::ActorCreature>().search(token.mQuest) != nullptr;
                            if (!actorExists)
                            {
                                decodeResult = ExpressionDecodeResult::Malformed;
                                break;
                            }
                            if (!mActorDeadHandler)
                            {
                                decodeResult = ExpressionDecodeResult::Unsupported;
                                break;
                            }
                            token.mType = CompiledConditionTokenType::Value;
                            token.mValueType = CompiledConditionValueType::GetDead;
                            prepared.mHasLiveCondition = true;
                            expressionOffset += 8;
                        }
                        else
                        {
                            decodeResult = ExpressionDecodeResult::Unsupported;
                            break;
                        }
                        ++stackDepth;
                    }
                    else if (expression[expressionOffset] == 0x58)
                    {
                        if (expression.size() - expressionOffset < 5)
                        {
                            decodeResult = ExpressionDecodeResult::Malformed;
                            break;
                        }
                        const std::uint16_t functionOpcode = readUint16(expression, expressionOffset + 1);
                        const std::uint16_t functionArgumentSize = readUint16(expression, expressionOffset + 3);
                        if (functionArgumentSize > expression.size() - expressionOffset - 5)
                        {
                            decodeResult = ExpressionDecodeResult::Malformed;
                            break;
                        }
                        std::vector<ESM4::ScriptBytecodeArgument> functionArguments;
                        if (!ESM4::decodeFalloutScriptArguments(expression.subspan(
                                expressionOffset + 5, functionArgumentSize), script.references, functionArguments)
                                 .succeeded())
                        {
                            decodeResult = ExpressionDecodeResult::Malformed;
                            break;
                        }
                        decodeResult = decodeValue(functionOpcode, functionArguments, token);
                        if (decodeResult != ExpressionDecodeResult::Supported)
                            break;
                        expressionOffset += 5 + functionArgumentSize;
                        ++stackDepth;
                    }
                    else
                    {
                        const auto nextSeparator
                            = std::find(expression.begin() + static_cast<std::ptrdiff_t>(expressionOffset),
                                expression.end(), 0x20);
                        const std::size_t tokenEnd = static_cast<std::size_t>(nextSeparator - expression.begin());
                        const std::string_view text(
                            reinterpret_cast<const char*>(expression.data() + expressionOffset),
                            tokenEnd - expressionOffset);
                        if (parseFloat(text, token.mNumber))
                        {
                            token.mType = CompiledConditionTokenType::Number;
                            ++stackDepth;
                        }
                        else
                        {
                            if (stackDepth < 2)
                            {
                                decodeResult = ExpressionDecodeResult::Malformed;
                                break;
                            }
                            if (text == "==")
                                token.mType = CompiledConditionTokenType::Equal;
                            else if (text == "!=")
                                token.mType = CompiledConditionTokenType::NotEqual;
                            else if (text == "<")
                                token.mType = CompiledConditionTokenType::Less;
                            else if (text == "<=")
                                token.mType = CompiledConditionTokenType::LessEqual;
                            else if (text == ">")
                                token.mType = CompiledConditionTokenType::Greater;
                            else if (text == ">=")
                                token.mType = CompiledConditionTokenType::GreaterEqual;
                            else if (text == "&&")
                                token.mType = CompiledConditionTokenType::LogicalAnd;
                            else if (text == "||")
                                token.mType = CompiledConditionTokenType::LogicalOr;
                            else
                            {
                                decodeResult = ExpressionDecodeResult::Unsupported;
                                break;
                            }
                            --stackDepth;
                        }
                        expressionOffset = tokenEnd;
                    }
                    condition.mPostfix.push_back(std::move(token));
                }
                if (decodeResult == ExpressionDecodeResult::Supported && stackDepth != 1)
                    decodeResult = ExpressionDecodeResult::Malformed;
                if (decodeResult == ExpressionDecodeResult::Malformed)
                    return false;
                const bool supported = decodeResult == ExpressionDecodeResult::Supported;

                if (!supported)
                {
                    prepared.mUseSourceFallback = true;
                    prepared.mUnsupportedOpcodes.push_back(instruction.opcode);
                }
                else
                {
                    CompiledQuestCommand command;
                    command.mType = instruction.opcode == 0x0016
                        ? CompiledQuestCommandType::If
                        : CompiledQuestCommandType::ElseIf;
                    command.mCondition = std::move(condition);
                    prepared.mCommands.push_back(std::move(command));
                }
                if (instruction.opcode == 0x0016)
                    conditionalElseSeen.push_back(false);
                continue;
            }
            if (instruction.opcode == 0x0017) // Else
            {
                if (instruction.callingReferenceIndex || argumentPayload.size() != 2
                    || conditionalElseSeen.empty() || conditionalElseSeen.back())
                    return false;
                conditionalElseSeen.back() = true;
                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::Else;
                prepared.mCommands.push_back(std::move(command));
                continue;
            }
            if (instruction.opcode == 0x0019) // EndIf
            {
                if (instruction.callingReferenceIndex || !argumentPayload.empty() || conditionalElseSeen.empty())
                    return false;
                conditionalElseSeen.pop_back();
                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::EndIf;
                prepared.mCommands.push_back(std::move(command));
                continue;
            }
            if (instruction.opcode == 0x0015) // set Quest.local to literal or compiled expression
            {
                if (instruction.callingReferenceIndex || argumentPayload.size() < 8 || argumentPayload[0] != 0x72
                    || (argumentPayload[3] != 0x66 && argumentPayload[3] != 0x73) || mStore == nullptr)
                    return false;
                const std::uint16_t referenceIndex = readUint16(argumentPayload, 1);
                const std::uint16_t variableIndex = readUint16(argumentPayload, 4);
                const std::uint16_t expressionSize = readUint16(argumentPayload, 6);
                if (referenceIndex == 0 || referenceIndex > script.references.size()
                    || expressionSize != argumentPayload.size() - 8)
                    return false;

                const ESM::FormId questId = script.references[referenceIndex - 1];
                const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(questId));
                const ESM4::Script* questScript = quest == nullptr
                    ? nullptr
                    : mStore->get<ESM4::Script>().search(ESM::RefId(quest->mQuestScript));
                if (quest == nullptr || findState(*quest) == nullptr || questScript == nullptr)
                    return false;
                const auto variable = std::ranges::find(
                    questScript->mScript.localVarData, variableIndex, &ESM4::ScriptLocalVariableData::index);
                if (variable == questScript->mScript.localVarData.end() || variable->variableName.empty())
                    return false;

                const std::string_view expression(
                    reinterpret_cast<const char*>(argumentPayload.data() + 8), expressionSize);
                float value = 0.f;
                CompiledQuestCommand command;
                command.mQuest = questId;
                command.mVariable = Misc::StringUtils::lowerCase(variable->variableName);
                if (parseFloat(trim(expression), value))
                {
                    command.mType = CompiledQuestCommandType::SetVariable;
                    command.mNumber = value;
                    prepared.mCommands.push_back(std::move(command));
                    continue;
                }

                // Fallout's compiled "Player.GetItemCount Item" expression is:
                // 20 72 <owner-ref> 58 2f10 0500 0100 72 <item-ref>.
                // Keep this deliberately narrow so other expression bytecode cannot be
                // mistaken for a supported value and silently produce bad quest state.
                if (expressionSize != 14 || argumentPayload[8] != 0x20 || argumentPayload[9] != 0x72
                    || argumentPayload[12] != 0x58 || readUint16(argumentPayload, 13) != 0x102f
                    || readUint16(argumentPayload, 15) != 5 || readUint16(argumentPayload, 17) != 1
                    || argumentPayload[19] != 0x72)
                {
                    prepared.mUseSourceFallback = true;
                    prepared.mUnsupportedOpcodes.push_back(instruction.opcode);
                    continue;
                }
                if (!mItemCountHandler)
                    return false;
                const std::uint16_t ownerIndex = readUint16(argumentPayload, 10);
                const std::uint16_t itemIndex = readUint16(argumentPayload, 20);
                if (ownerIndex == 0 || ownerIndex > script.references.size() || itemIndex == 0
                    || itemIndex > script.references.size())
                    return false;
                const ESM::FormId owner = script.references[ownerIndex - 1];
                const ESM::FormId item = script.references[itemIndex - 1];
                if ((owner.mIndex != 0x7 && owner.mIndex != 0x14) || item.isZeroOrUnset())
                    return false;
                command.mType = CompiledQuestCommandType::SetVariableFromItemCount;
                command.mTarget = owner;
                command.mTopic = item;
                prepared.mCommands.push_back(std::move(command));
                continue;
            }
            if (instruction.opcode == 0x1059) // ShowMessage
            {
                // Every ShowMessage instruction in FalloutNV.esm (24/24) stores its one
                // encoded argument followed by six zero bytes inside the instruction frame.
                constexpr std::size_t encodedArgumentBytes = 5;
                constexpr std::size_t trailingBytes = 6;
                if (argumentPayload.size() != encodedArgumentBytes + trailingBytes
                    || !std::all_of(argumentPayload.end() - trailingBytes, argumentPayload.end(),
                        [](std::uint8_t value) { return value == 0; }))
                    return false;
                argumentPayload = argumentPayload.first(encodedArgumentBytes);
            }
            std::vector<ESM4::ScriptBytecodeArgument> arguments;
            // Zero-argument reference functions such as EVP and ResetAI have an empty frame rather
            // than the two-byte argument count used by ordinary command functions.
            if (!((instruction.opcode == 0x105e || instruction.opcode == 0x1098
                       || instruction.opcode == 0x11fa)
                    && argumentPayload.empty())
                && !ESM4::decodeFalloutScriptArguments(argumentPayload, script.references, arguments).succeeded())
                return false;

            if (instruction.opcode == 0x1002) // reference.AddItem item count
            {
                if (!instruction.callingReferenceIndex || arguments.size() != 2 || !mAddItemHandler
                    || mStore == nullptr)
                    return false;
                const ESM::FormId owner = script.references[*instruction.callingReferenceIndex - 1];
                const ESM::FormId* item = std::get_if<ESM::FormId>(&arguments[0]);
                const std::int32_t* count = std::get_if<std::int32_t>(&arguments[1]);
                const bool ownerIsPlayer = owner.mIndex == 0x7 || owner.mIndex == 0x14;
                const bool ownerExists = ownerIsPlayer || mStore->get<ESM4::Reference>().search(owner) != nullptr
                    || mStore->get<ESM4::ActorCharacter>().search(owner) != nullptr
                    || mStore->get<ESM4::ActorCreature>().search(owner) != nullptr;
                if (!ownerExists || item == nullptr || item->isZeroOrUnset() || count == nullptr || *count <= 0)
                    return false;
                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::AddItem;
                command.mQuest = owner;
                command.mTarget = *item;
                command.mObjective = *count;
                prepared.mCommands.push_back(std::move(command));
            }
            else if (instruction.opcode == 0x1052) // reference.RemoveItem item count
            {
                // FalloutNV.esm uses a reference-called two-argument form in the quest-stage
                // frames covered here. Do not guess at the optional third argument used by a
                // small number of other scripts until its retail semantics are proven.
                if (!instruction.callingReferenceIndex || arguments.size() != 2 || !mRemoveItemHandler
                    || mStore == nullptr)
                    return false;
                const ESM::FormId owner = script.references[*instruction.callingReferenceIndex - 1];
                const ESM::FormId* item = std::get_if<ESM::FormId>(&arguments[0]);
                const std::int32_t* count = std::get_if<std::int32_t>(&arguments[1]);
                const bool ownerIsPlayer = owner.mIndex == 0x7 || owner.mIndex == 0x14;
                const bool ownerExists = ownerIsPlayer || mStore->get<ESM4::Reference>().search(owner) != nullptr
                    || mStore->get<ESM4::ActorCharacter>().search(owner) != nullptr
                    || mStore->get<ESM4::ActorCreature>().search(owner) != nullptr;
                if (!ownerExists || item == nullptr || item->isZeroOrUnset() || count == nullptr || *count <= 0)
                    return false;
                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::RemoveItem;
                command.mQuest = owner;
                command.mTarget = *item;
                command.mObjective = *count;
                prepared.mCommands.push_back(std::move(command));
            }
            else if (instruction.opcode == 0x1177) // [player.]RewardXP amount
            {
                if (arguments.size() != 1 || !mRewardXpHandler)
                    return false;
                if (instruction.callingReferenceIndex)
                {
                    const ESM::FormId owner = script.references[*instruction.callingReferenceIndex - 1];
                    if (owner.mIndex != 0x7 && owner.mIndex != 0x14)
                        return false;
                }
                const std::int32_t* amount = std::get_if<std::int32_t>(&arguments[0]);
                if (amount == nullptr || *amount <= 0)
                    return false;
                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::RewardXp;
                command.mObjective = *amount;
                prepared.mCommands.push_back(std::move(command));
            }
            else if (instruction.opcode == 0x101d) // [actor.]RemoveSpell spell
            {
                // The combined base-master corpus has 39 one-SPEL frames:
                // 26 quest-stage implicit-player calls and 13 explicit
                // player/actor calls. Both forms use the same actor-effect
                // capability as source scripts.
                if (arguments.size() != 1 || !mActorEffectCommandHandler || mStore == nullptr)
                    return false;
                ESM::FormId actor{ .mIndex = 0x14, .mContentFile = 0 };
                if (instruction.callingReferenceIndex)
                    actor = script.references[*instruction.callingReferenceIndex - 1];
                const bool actorExists = actor.mIndex == 0x7 || actor.mIndex == 0x14
                    || mStore->get<ESM4::ActorCharacter>().search(actor) != nullptr
                    || mStore->get<ESM4::ActorCreature>().search(actor) != nullptr;
                const ESM::FormId* spell = std::get_if<ESM::FormId>(&arguments[0]);
                if (!actorExists || spell == nullptr
                    || mStore->get<ESM4::Spell>().search(ESM::RefId(*spell)) == nullptr)
                    return false;

                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::SetActorEffect;
                command.mQuest = actor;
                command.mTarget = *spell;
                command.mValue = false;
                prepared.mCommands.push_back(std::move(command));
            }
            else if (instruction.opcode == 0x117c || instruction.opcode == 0x117d) // AddNote / RemoveNote
            {
                // The combined Fallout 3 and New Vegas base-master quest
                // corpus has 144 one-note frames. Calls are either global or
                // explicitly player-qualified; no other receiver occurs.
                if (arguments.size() != 1 || !mNoteHandler || mStore == nullptr)
                    return false;
                if (instruction.callingReferenceIndex)
                {
                    const ESM::FormId owner = script.references[*instruction.callingReferenceIndex - 1];
                    if (owner.mIndex != 0x7 && owner.mIndex != 0x14)
                        return false;
                }
                const ESM::FormId* note = std::get_if<ESM::FormId>(&arguments[0]);
                if (note == nullptr || mStore->get<ESM4::Note>().search(*note) == nullptr)
                    return false;
                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::SetNote;
                command.mQuest = *note;
                command.mValue = instruction.opcode == 0x117c;
                prepared.mCommands.push_back(std::move(command));
            }
            else if (instruction.opcode == 0x114a) // AddAchievement id
            {
                // All 63 base-master quest frames are global calls with one
                // positive literal integer (the observed IDs are 1..50).
                if (instruction.callingReferenceIndex || arguments.size() != 1 || !mAchievementHandler)
                    return false;
                const std::int32_t* achievement = std::get_if<std::int32_t>(&arguments[0]);
                if (achievement == nullptr || *achievement <= 0)
                    return false;
                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::AddAchievement;
                command.mObjective = *achievement;
                prepared.mCommands.push_back(std::move(command));
            }
            else if (instruction.opcode == 0x1239) // AddReputation reputation infamy/fame bump
            {
                if (instruction.callingReferenceIndex || arguments.size() != 3 || !mAddReputationHandler
                    || mStore == nullptr)
                    return false;
                const ESM::FormId* reputation = std::get_if<ESM::FormId>(&arguments[0]);
                const std::int32_t* fame = std::get_if<std::int32_t>(&arguments[1]);
                const std::int32_t* bump = std::get_if<std::int32_t>(&arguments[2]);
                // The frozen FalloutNV.esm corpus contains 77 AddReputation frames: 11 use 0, 65 use 1,
                // and VMS20 stage 100 uses 2 for RepNVGreatKhans. Preserve the command's boolean contract
                // (zero is infamy, the two observed nonzero values are fame) without accepting values that
                // do not occur in the retail corpus.
                if (reputation == nullptr || fame == nullptr || bump == nullptr
                    || *fame < 0 || *fame > 2 || *bump < 1 || *bump > 5
                    || mStore->get<ESM4::Reputation>().search(ESM::RefId(*reputation)) == nullptr)
                    return false;
                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::AddReputation;
                command.mQuest = *reputation;
                command.mValue = *fame != 0;
                command.mObjective = *bump;
                prepared.mCommands.push_back(std::move(command));
            }
            else if (instruction.opcode == 0x1021 || instruction.opcode == 0x1022
                || instruction.opcode == 0x1073 || instruction.opcode == 0x108b) // Enable / Disable / Unlock / Kill
            {
                if (!instruction.callingReferenceIndex || !arguments.empty() || !mReferenceCommandHandler
                    || mStore == nullptr)
                    return false;
                const ESM::FormId target = script.references[*instruction.callingReferenceIndex - 1];
                const bool referenceExists = mStore->get<ESM4::Reference>().search(target) != nullptr;
                const bool actorExists = mStore->get<ESM4::ActorCharacter>().search(target) != nullptr
                    || mStore->get<ESM4::ActorCreature>().search(target) != nullptr;
                const bool targetExists = referenceExists || actorExists;
                if (!targetExists || (instruction.opcode == 0x1073 && !referenceExists)
                    || (instruction.opcode == 0x108b && !actorExists))
                    return false;
                CompiledQuestCommandType type = CompiledQuestCommandType::Kill;
                if (instruction.opcode == 0x1021)
                    type = CompiledQuestCommandType::Enable;
                else if (instruction.opcode == 0x1022)
                    type = CompiledQuestCommandType::Disable;
                else if (instruction.opcode == 0x1073)
                    type = CompiledQuestCommandType::Unlock;
                prepared.mCommands.push_back({ type, target });
            }
            else if (instruction.opcode == 0x10cc) // reference.SetDestroyed bool
            {
                if (!instruction.callingReferenceIndex || arguments.size() != 1 || !mSetDestroyedHandler
                    || mStore == nullptr)
                    return false;
                const ESM::FormId target = script.references[*instruction.callingReferenceIndex - 1];
                const std::int32_t* destroyed = std::get_if<std::int32_t>(&arguments[0]);
                const bool targetExists = mStore->get<ESM4::Reference>().search(target) != nullptr
                    || mStore->get<ESM4::ActorCharacter>().search(target) != nullptr
                    || mStore->get<ESM4::ActorCreature>().search(target) != nullptr;
                // Frozen FalloutNV.esm corpus: all 43 frames have one literal boolean argument; 32 set and 11 clear.
                if (!targetExists || destroyed == nullptr || (*destroyed != 0 && *destroyed != 1))
                    return false;
                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::SetDestroyed;
                command.mQuest = target;
                command.mValue = *destroyed != 0;
                prepared.mCommands.push_back(std::move(command));
            }
            else if (instruction.opcode == 0x1055) // ShowMap marker
            {
                // Frozen FalloutNV.esm quest corpus: all 32 frames are non-reference-called and carry
                // exactly one SCRO map-marker reference. None supplies the optional fast-travel flag.
                if (instruction.callingReferenceIndex || arguments.size() != 1 || !mShowMapHandler
                    || mStore == nullptr)
                    return false;
                const ESM::FormId* markerId = std::get_if<ESM::FormId>(&arguments[0]);
                const ESM4::Reference* marker = markerId == nullptr
                    ? nullptr
                    : mStore->get<ESM4::Reference>().search(*markerId);
                if (marker == nullptr || !marker->mIsMapMarker || marker->mFullName.empty())
                    return false;
                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::ShowMap;
                command.mQuest = *markerId;
                command.mValue = false;
                prepared.mCommands.push_back(std::move(command));
            }
            else if (instruction.opcode == 0x1111) // EnableFastTravel canFastTravel [canWait] [keepOnCellChange]
            {
                // Frozen FalloutNV.esm quest corpus: 14 frames, all global calls with one to three literal
                // boolean arguments. FNV added canWait and keepOnCellChange after Fallout 3.
                if (instruction.callingReferenceIndex || arguments.empty() || arguments.size() > 3
                    || !mEnableFastTravelHandler)
                    return false;
                std::array<bool, 3> values{ false, true, false };
                for (std::size_t index = 0; index < arguments.size(); ++index)
                {
                    const std::int32_t* value = std::get_if<std::int32_t>(&arguments[index]);
                    if (value == nullptr || (*value != 0 && *value != 1))
                        return false;
                    values[index] = *value != 0;
                }
                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::EnableFastTravel;
                command.mValue = values[0];
                command.mSecondaryValue = values[1];
                command.mObjective = values[2] ? 1 : 0;
                prepared.mCommands.push_back(std::move(command));
            }
            else if (instruction.opcode == 0x1034) // reference.SayTo listener topic
            {
                if (!instruction.callingReferenceIndex || arguments.size() != 2 || !mSayToHandler
                    || mStore == nullptr)
                    return false;
                const ESM::FormId speaker = script.references[*instruction.callingReferenceIndex - 1];
                const ESM::FormId* listener = std::get_if<ESM::FormId>(&arguments[0]);
                const ESM::FormId* topic = std::get_if<ESM::FormId>(&arguments[1]);
                const bool speakerExists = mStore->get<ESM4::ActorCharacter>().search(speaker) != nullptr
                    || mStore->get<ESM4::ActorCreature>().search(speaker) != nullptr;
                const bool listenerIsPlayer
                    = listener != nullptr && (listener->mIndex == 0x7 || listener->mIndex == 0x14);
                const bool listenerExists = listener != nullptr
                    && (listenerIsPlayer || mStore->get<ESM4::ActorCharacter>().search(*listener) != nullptr
                        || mStore->get<ESM4::ActorCreature>().search(*listener) != nullptr);
                if (!speakerExists || !listenerExists || topic == nullptr
                    || mStore->get<ESM4::Dialogue>().search(ESM::RefId(*topic)) == nullptr)
                    return false;
                prepared.mCommands.push_back(
                    { CompiledQuestCommandType::SayTo, speaker, 0, false, 0, *listener, *topic });
            }
            else if (instruction.opcode == 0x1079) // SetAlly Faction Faction
            {
                if (instruction.callingReferenceIndex || arguments.size() != 2 || !mSetAllyHandler
                    || mStore == nullptr)
                    return false;
                const ESM::FormId* first = std::get_if<ESM::FormId>(&arguments[0]);
                const ESM::FormId* second = std::get_if<ESM::FormId>(&arguments[1]);
                if (first == nullptr || second == nullptr || first->isZeroOrUnset() || second->isZeroOrUnset()
                    || *first == *second || mStore->get<ESM4::Faction>().search(ESM::RefId(*first)) == nullptr
                    || mStore->get<ESM4::Faction>().search(ESM::RefId(*second)) == nullptr)
                    return false;
                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::SetAlly;
                command.mQuest = *first;
                command.mTarget = *second;
                prepared.mCommands.push_back(std::move(command));
            }
            else if (instruction.opcode == 0x1078) // SetEnemy Faction Faction [firstNeutral] [secondNeutral]
            {
                if (instruction.callingReferenceIndex || (arguments.size() != 2 && arguments.size() != 4)
                    || !mSetEnemyHandler || mStore == nullptr)
                    return false;
                const ESM::FormId* first = std::get_if<ESM::FormId>(&arguments[0]);
                const ESM::FormId* second = std::get_if<ESM::FormId>(&arguments[1]);
                std::int32_t firstNeutral = 0;
                std::int32_t secondNeutral = 0;
                if (arguments.size() == 4)
                {
                    const std::int32_t* firstFlag = std::get_if<std::int32_t>(&arguments[2]);
                    const std::int32_t* secondFlag = std::get_if<std::int32_t>(&arguments[3]);
                    if (firstFlag == nullptr || secondFlag == nullptr
                        || (*firstFlag != 0 && *firstFlag != 1) || (*secondFlag != 0 && *secondFlag != 1))
                        return false;
                    firstNeutral = *firstFlag;
                    secondNeutral = *secondFlag;
                }
                if (first == nullptr || second == nullptr || first->isZeroOrUnset() || second->isZeroOrUnset()
                    || *first == *second || mStore->get<ESM4::Faction>().search(ESM::RefId(*first)) == nullptr
                    || mStore->get<ESM4::Faction>().search(ESM::RefId(*second)) == nullptr)
                    return false;
                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::SetEnemy;
                command.mQuest = *first;
                command.mTarget = *second;
                command.mValue = firstNeutral != 0;
                command.mSecondaryValue = secondNeutral != 0;
                prepared.mCommands.push_back(std::move(command));
            }
            else if (instruction.opcode == 0x105e) // EvaluatePackage / evp
            {
                if (!instruction.callingReferenceIndex || !arguments.empty() || !mReferenceCommandHandler
                    || mStore == nullptr)
                    return false;
                const ESM::FormId target = script.references[*instruction.callingReferenceIndex - 1];
                if (mStore->get<ESM4::ActorCharacter>().search(target) == nullptr
                    && mStore->get<ESM4::ActorCreature>().search(target) == nullptr)
                    return false;
                prepared.mCommands.push_back({ CompiledQuestCommandType::EvaluatePackage, target });
            }
            else if (instruction.opcode == 0x11fa) // reference.ResetAI
            {
                // FalloutNV.esm has 26/26 reference-called, zero-argument frames:
                // 22 ACHR targets and 4 ACRE targets.
                if (!instruction.callingReferenceIndex || !arguments.empty() || !mReferenceCommandHandler
                    || mStore == nullptr)
                    return false;
                const ESM::FormId target = script.references[*instruction.callingReferenceIndex - 1];
                if (mStore->get<ESM4::ActorCharacter>().search(target) == nullptr
                    && mStore->get<ESM4::ActorCreature>().search(target) == nullptr)
                    return false;
                prepared.mCommands.push_back({ CompiledQuestCommandType::ResetAi, target });
            }
            else if (instruction.opcode == 0x1097 || instruction.opcode == 0x1098)
            {
                // Across the Fallout 3 and New Vegas base-master quest
                // corpora, all 50 AddScriptPackage and 12
                // RemoveScriptPackage frames are actor-qualified. Add carries
                // exactly one PACK reference; remove has an empty frame.
                const bool add = instruction.opcode == 0x1097;
                if (!instruction.callingReferenceIndex || !mScriptPackageHandler || mStore == nullptr
                    || arguments.size() != (add ? 1u : 0u))
                    return false;
                const ESM::FormId actor = script.references[*instruction.callingReferenceIndex - 1];
                const bool actorExists = actor.mIndex == 0x7 || actor.mIndex == 0x14
                    || mStore->get<ESM4::ActorCharacter>().search(actor) != nullptr
                    || mStore->get<ESM4::ActorCreature>().search(actor) != nullptr;
                if (!actorExists)
                    return false;

                ESM::FormId package;
                if (add)
                {
                    const ESM::FormId* value = std::get_if<ESM::FormId>(&arguments[0]);
                    if (value == nullptr || mStore->get<ESM4::AIPackage>().search(*value) == nullptr)
                        return false;
                    package = *value;
                }

                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::SetScriptPackage;
                command.mQuest = actor;
                command.mTarget = package;
                command.mValue = add;
                prepared.mCommands.push_back(std::move(command));
            }
            else if (instruction.opcode == 0x109e) // reference.MoveTo marker
            {
                // Across the Fallout 3 and Fallout: New Vegas base-master
                // quest corpora, all 195 MoveTo frames are reference-called
                // and carry exactly one object-reference argument.
                if (!instruction.callingReferenceIndex || arguments.size() != 1 || !mMoveToHandler
                    || mStore == nullptr)
                    return false;
                const ESM::FormId reference = script.references[*instruction.callingReferenceIndex - 1];
                const ESM::FormId* marker = std::get_if<ESM::FormId>(&arguments[0]);
                const auto exists = [this](ESM::FormId id) {
                    const bool isPlayer = id.mIndex == 0x7 || id.mIndex == 0x14;
                    return isPlayer || mStore->get<ESM4::Reference>().search(id) != nullptr
                        || mStore->get<ESM4::ActorCharacter>().search(id) != nullptr
                        || mStore->get<ESM4::ActorCreature>().search(id) != nullptr;
                };
                if (!exists(reference) || marker == nullptr || !exists(*marker))
                    return false;
                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::MoveTo;
                command.mQuest = reference;
                command.mTarget = *marker;
                prepared.mCommands.push_back(std::move(command));
            }
            else if (instruction.opcode == 0x1059) // ShowMessage
            {
                if (instruction.callingReferenceIndex || arguments.size() != 1 || !mMessageHandler
                    || mStore == nullptr)
                    return false;
                const ESM::FormId* messageId = std::get_if<ESM::FormId>(&arguments[0]);
                if (messageId == nullptr
                    || mStore->get<ESM4::Message>().search(*messageId) == nullptr)
                    return false;
                prepared.mCommands.push_back({ CompiledQuestCommandType::ShowMessage, *messageId });
            }
            else if (instruction.callingReferenceIndex)
                return false;
            else if (instruction.opcode == 0x11a2 || instruction.opcode == 0x11a3)
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
            else if (instruction.opcode == 0x11ad) // CompleteAllObjectives Quest
            {
                if (instruction.callingReferenceIndex || arguments.size() != 1 || mStore == nullptr)
                    return false;
                const ESM::FormId* questId = std::get_if<ESM::FormId>(&arguments[0]);
                const ESM4::Quest* quest = questId == nullptr
                    ? nullptr
                    : mStore->get<ESM4::Quest>().search(ESM::RefId(*questId));
                if (questId == nullptr || quest == nullptr || findState(*quest) == nullptr)
                    return false;
                prepared.mCommands.push_back({ CompiledQuestCommandType::CompleteAllObjectives, *questId });
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

        if (!conditionalElseSeen.empty())
            return false;
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

    bool ESM4QuestRuntime::stageContainsCompiledLiveCondition(const ESM4::QuestStage& stage) const
    {
        for (const ESM4::QuestStageEntry& entry : stage.mEntries)
        {
            CompiledStageScript prepared;
            if (prepareStageScript(entry.mScript, prepared) && !prepared.mUseSourceFallback
                && prepared.mHasLiveCondition)
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

    std::optional<bool> ESM4QuestRuntime::evaluateCompiledCondition(
        const CompiledQuestCondition& condition, const QuestStateMap& states) const
    {
        std::vector<float> stack;
        stack.reserve(condition.mPostfix.size());
        for (const CompiledConditionToken& token : condition.mPostfix)
        {
            if (token.mType == CompiledConditionTokenType::Number)
            {
                stack.push_back(token.mNumber);
                continue;
            }
            if (token.mType == CompiledConditionTokenType::Value)
            {
                if (token.mValueType == CompiledConditionValueType::GetDead)
                {
                    const std::optional<bool> dead
                        = mActorDeadHandler ? mActorDeadHandler(token.mQuest) : std::nullopt;
                    if (!dead)
                        return std::nullopt;
                    stack.push_back(*dead ? 1.f : 0.f);
                    continue;
                }
                const auto found = states.find(token.mQuest);
                if (found == states.end())
                    return std::nullopt;
                float value = 0.f;
                switch (token.mValueType)
                {
                    case CompiledConditionValueType::QuestVariable:
                    {
                        const auto variable = found->second.mVariables.find(token.mVariable);
                        if (variable == found->second.mVariables.end())
                            return std::nullopt;
                        value = variable->second;
                        break;
                    }
                    case CompiledConditionValueType::GetStage:
                        value = found->second.mCurrentStage;
                        break;
                    case CompiledConditionValueType::GetStageDone:
                    {
                        const auto stage = found->second.mStageDone.find(static_cast<std::int16_t>(token.mStage));
                        value = stage != found->second.mStageDone.end() && stage->second ? 1.f : 0.f;
                        break;
                    }
                    case CompiledConditionValueType::GetObjectiveCompleted:
                    case CompiledConditionValueType::GetObjectiveDisplayed:
                    {
                        const auto objective = found->second.mObjectiveStatus.find(token.mStage);
                        if (objective == found->second.mObjectiveStatus.end())
                            return std::nullopt;
                        const std::uint8_t flag
                            = token.mValueType == CompiledConditionValueType::GetObjectiveCompleted
                            ? ESM4QuestState::Objective_Completed
                            : ESM4QuestState::Objective_Displayed;
                        value = (objective->second & flag) != 0 ? 1.f : 0.f;
                        break;
                    }
                    case CompiledConditionValueType::GetQuestRunning:
                        value = (found->second.mFlags & ESM4QuestState::Flag_Running) != 0 ? 1.f : 0.f;
                        break;
                    case CompiledConditionValueType::GetQuestCompleted:
                        value = (found->second.mFlags & ESM4QuestState::Flag_Completed) != 0 ? 1.f : 0.f;
                        break;
                    case CompiledConditionValueType::GetDead:
                        return std::nullopt;
                }
                stack.push_back(value);
                continue;
            }

            if (stack.size() < 2)
                return std::nullopt;
            const float right = stack.back();
            stack.pop_back();
            const float left = stack.back();
            stack.pop_back();
            bool result = false;
            switch (token.mType)
            {
                case CompiledConditionTokenType::Equal:
                    result = left == right;
                    break;
                case CompiledConditionTokenType::NotEqual:
                    result = left != right;
                    break;
                case CompiledConditionTokenType::Less:
                    result = left < right;
                    break;
                case CompiledConditionTokenType::LessEqual:
                    result = left <= right;
                    break;
                case CompiledConditionTokenType::Greater:
                    result = left > right;
                    break;
                case CompiledConditionTokenType::GreaterEqual:
                    result = left >= right;
                    break;
                case CompiledConditionTokenType::LogicalAnd:
                    result = left != 0.f && right != 0.f;
                    break;
                case CompiledConditionTokenType::LogicalOr:
                    result = left != 0.f || right != 0.f;
                    break;
                case CompiledConditionTokenType::Value:
                case CompiledConditionTokenType::Number:
                    return std::nullopt;
            }
            stack.push_back(result ? 1.f : 0.f);
        }
        if (stack.size() != 1)
            return std::nullopt;
        return stack.back() != 0.f;
    }

    bool ESM4QuestRuntime::updateCompiledConditionalState(const CompiledQuestCommand& command,
        const QuestStateMap& states, std::vector<CompiledConditionalFrame>& stack, bool& execute) const
    {
        execute = stack.empty() || stack.back().mActive;
        if (command.mType == CompiledQuestCommandType::If)
        {
            if (!command.mCondition)
                return false;
            const bool parentActive = execute;
            const std::optional<bool> condition
                = parentActive ? evaluateCompiledCondition(*command.mCondition, states) : false;
            if (!condition)
                return false;
            const bool active = parentActive && *condition;
            stack.push_back({ parentActive, active, active });
            execute = false;
            return true;
        }
        if (command.mType == CompiledQuestCommandType::ElseIf)
        {
            if (stack.empty() || !command.mCondition)
                return false;
            CompiledConditionalFrame& frame = stack.back();
            const std::optional<bool> condition = frame.mParentActive && !frame.mBranchTaken
                ? evaluateCompiledCondition(*command.mCondition, states)
                : false;
            if (!condition)
                return false;
            frame.mActive = frame.mParentActive && !frame.mBranchTaken && *condition;
            frame.mBranchTaken = frame.mBranchTaken || frame.mActive;
            execute = false;
            return true;
        }
        if (command.mType == CompiledQuestCommandType::Else)
        {
            if (stack.empty())
                return false;
            CompiledConditionalFrame& frame = stack.back();
            frame.mActive = frame.mParentActive && !frame.mBranchTaken;
            frame.mBranchTaken = true;
            execute = false;
            return true;
        }
        if (command.mType == CompiledQuestCommandType::EndIf)
        {
            if (stack.empty())
                return false;
            stack.pop_back();
            execute = false;
            return true;
        }
        return true;
    }

    bool ESM4QuestRuntime::executePureCompiledCommand(
        const CompiledQuestCommand& command, CompiledStageWorkingState& working)
    {
        if (command.mType == CompiledQuestCommandType::SetStage)
            return executePureCompiledStage(command.mQuest, command.mStage, working);
        if (command.mType == CompiledQuestCommandType::EvaluatePackage
            || command.mType == CompiledQuestCommandType::ResetAi
            || command.mType == CompiledQuestCommandType::MoveTo
            || command.mType == CompiledQuestCommandType::SetScriptPackage
            || command.mType == CompiledQuestCommandType::SetActorEffect
            || command.mType == CompiledQuestCommandType::ShowMessage
            || command.mType == CompiledQuestCommandType::SetNote
            || command.mType == CompiledQuestCommandType::AddAchievement
            || command.mType == CompiledQuestCommandType::SayTo
            || command.mType == CompiledQuestCommandType::Enable
            || command.mType == CompiledQuestCommandType::Disable
            || command.mType == CompiledQuestCommandType::Unlock
            || command.mType == CompiledQuestCommandType::Kill
            || command.mType == CompiledQuestCommandType::AddItem
            || command.mType == CompiledQuestCommandType::RemoveItem
            || command.mType == CompiledQuestCommandType::RewardXp
            || command.mType == CompiledQuestCommandType::AddReputation
            || command.mType == CompiledQuestCommandType::SetDestroyed
            || command.mType == CompiledQuestCommandType::ShowMap
            || command.mType == CompiledQuestCommandType::EnableFastTravel)
        {
            working.mExternalEffects.push_back(
                { command.mType, command.mQuest, command.mTarget, command.mTopic,
                    command.mValue, command.mSecondaryValue, command.mObjective });
            return true;
        }

        const auto found = working.mStates.find(command.mQuest);
        if (found == working.mStates.end())
            return false;
        ESM4QuestState& state = found->second;
        switch (command.mType)
        {
            case CompiledQuestCommandType::If:
            case CompiledQuestCommandType::ElseIf:
            case CompiledQuestCommandType::Else:
            case CompiledQuestCommandType::EndIf:
                return false;
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
            case CompiledQuestCommandType::CompleteAllObjectives:
                for (auto& objective : state.mObjectiveStatus)
                    objective.second |= ESM4QuestState::Objective_Completed;
                return true;
            case CompiledQuestCommandType::ForceActiveQuest:
                state.mFlags |= ESM4QuestState::Flag_ShownInPipBoy;
                working.mActiveQuest = command.mQuest;
                return true;
            case CompiledQuestCommandType::SetVariable:
            {
                const auto variable = state.mVariables.find(command.mVariable);
                if (variable == state.mVariables.end())
                    return false;
                variable->second = command.mNumber;
                return true;
            }
            case CompiledQuestCommandType::SetVariableFromItemCount:
            {
                const auto variable = state.mVariables.find(command.mVariable);
                const std::optional<int> count = mItemCountHandler
                    ? mItemCountHandler(command.mTarget, command.mTopic)
                    : std::nullopt;
                if (variable == state.mVariables.end() || !count || *count < 0)
                    return false;
                variable->second = static_cast<float>(*count);
                return true;
            }
            case CompiledQuestCommandType::SetAlly:
            case CompiledQuestCommandType::SetEnemy:
            case CompiledQuestCommandType::Enable:
            case CompiledQuestCommandType::Disable:
            case CompiledQuestCommandType::Unlock:
            case CompiledQuestCommandType::Kill:
            case CompiledQuestCommandType::ResetAi:
            case CompiledQuestCommandType::MoveTo:
            case CompiledQuestCommandType::SetScriptPackage:
            case CompiledQuestCommandType::SetActorEffect:
            case CompiledQuestCommandType::AddItem:
            case CompiledQuestCommandType::RemoveItem:
            case CompiledQuestCommandType::EvaluatePackage:
            case CompiledQuestCommandType::ShowMessage:
            case CompiledQuestCommandType::SetNote:
            case CompiledQuestCommandType::AddAchievement:
            case CompiledQuestCommandType::SayTo:
            case CompiledQuestCommandType::RewardXp:
            case CompiledQuestCommandType::AddReputation:
            case CompiledQuestCommandType::SetDestroyed:
            case CompiledQuestCommandType::ShowMap:
            case CompiledQuestCommandType::EnableFastTravel:
                return false;
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
            std::vector<CompiledConditionalFrame> conditionalStack;
            for (const CompiledQuestCommand& command : prepared.mCommands)
            {
                bool execute = true;
                if (!updateCompiledConditionalState(command, working.mStates, conditionalStack, execute))
                {
                    success = false;
                    break;
                }
                if (!execute)
                    continue;
                if (command.mType == CompiledQuestCommandType::SetAlly
                    || command.mType == CompiledQuestCommandType::SetEnemy)
                {
                    if (command.mType == CompiledQuestCommandType::SetAlly)
                        recordAllyPair(state, command.mQuest, command.mTarget);
                    else
                        recordEnemyRelation(
                            state, command.mQuest, command.mTarget, command.mValue, command.mSecondaryValue);
                    working.mExternalEffects.push_back({ command.mType, command.mQuest, command.mTarget,
                        command.mTopic, command.mValue, command.mSecondaryValue });
                    continue;
                }
                if (!executePureCompiledCommand(command, working))
                {
                    success = false;
                    break;
                }
            }
            if (!conditionalStack.empty())
                success = false;
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
        flushCompiledExternalEffects(working.mExternalEffects);
        flushCompiledStageEffects(working.mEffects);
        return true;
    }

    void ESM4QuestRuntime::flushCompiledExternalEffects(const std::vector<PendingExternalEffect>& effects)
    {
        for (const PendingExternalEffect& effect : effects)
        {
            bool executed = false;
            std::string command;
            switch (effect.mType)
            {
                case CompiledQuestCommandType::EvaluatePackage:
                    command = "EvaluatePackage ";
                    executed = mReferenceCommandHandler
                        && mReferenceCommandHandler(ESM4QuestReferenceCommand::EvaluatePackage, effect.mTarget);
                    break;
                case CompiledQuestCommandType::ResetAi:
                    command = "ResetAI ";
                    executed = mReferenceCommandHandler
                        && mReferenceCommandHandler(ESM4QuestReferenceCommand::ResetAi, effect.mTarget);
                    break;
                case CompiledQuestCommandType::MoveTo:
                    command = "MoveTo ";
                    executed = mMoveToHandler && mMoveToHandler(effect.mTarget, effect.mListener);
                    break;
                case CompiledQuestCommandType::SetScriptPackage:
                    command = effect.mValue ? "AddScriptPackage " : "RemoveScriptPackage ";
                    executed = mScriptPackageHandler
                        && mScriptPackageHandler(effect.mTarget,
                            effect.mValue ? std::optional<ESM::FormId>{ effect.mListener } : std::nullopt);
                    break;
                case CompiledQuestCommandType::SetActorEffect:
                    command = effect.mValue ? "AddSpell " : "RemoveSpell ";
                    executed = mActorEffectCommandHandler
                        && mActorEffectCommandHandler(effect.mTarget, effect.mListener, effect.mValue);
                    break;
                case CompiledQuestCommandType::ShowMessage:
                    command = "ShowMessage ";
                    executed = mMessageHandler && mMessageHandler(effect.mTarget);
                    break;
                case CompiledQuestCommandType::SetNote:
                    command = effect.mValue ? "AddNote " : "RemoveNote ";
                    executed = mNoteHandler && mNoteHandler(effect.mTarget, effect.mValue);
                    break;
                case CompiledQuestCommandType::AddAchievement:
                    command = "AddAchievement ";
                    executed = mAchievementHandler
                        && mAchievementHandler(static_cast<std::uint32_t>(effect.mCount));
                    break;
                case CompiledQuestCommandType::SayTo:
                    command = "SayTo ";
                    executed = mSayToHandler && mSayToHandler(effect.mTarget, effect.mListener, effect.mTopic);
                    break;
                case CompiledQuestCommandType::SetAlly:
                    command = "SetAlly ";
                    executed = mSetAllyHandler && mSetAllyHandler(effect.mTarget, effect.mListener);
                    break;
                case CompiledQuestCommandType::SetEnemy:
                    command = "SetEnemy ";
                    executed = mSetEnemyHandler
                        && mSetEnemyHandler(
                            effect.mTarget, effect.mListener, effect.mValue, effect.mSecondaryValue);
                    break;
                case CompiledQuestCommandType::Enable:
                    command = "Enable ";
                    executed = mReferenceCommandHandler
                        && mReferenceCommandHandler(ESM4QuestReferenceCommand::Enable, effect.mTarget);
                    break;
                case CompiledQuestCommandType::Disable:
                    command = "Disable ";
                    executed = mReferenceCommandHandler
                        && mReferenceCommandHandler(ESM4QuestReferenceCommand::Disable, effect.mTarget);
                    break;
                case CompiledQuestCommandType::Unlock:
                    command = "Unlock ";
                    executed = mReferenceCommandHandler
                        && mReferenceCommandHandler(ESM4QuestReferenceCommand::Unlock, effect.mTarget);
                    break;
                case CompiledQuestCommandType::Kill:
                    command = "Kill ";
                    executed = mReferenceCommandHandler
                        && mReferenceCommandHandler(ESM4QuestReferenceCommand::Kill, effect.mTarget);
                    break;
                case CompiledQuestCommandType::AddItem:
                    command = "AddItem ";
                    executed = mAddItemHandler
                        && mAddItemHandler(effect.mTarget, effect.mListener, effect.mCount);
                    break;
                case CompiledQuestCommandType::RemoveItem:
                    command = "RemoveItem ";
                    executed = mRemoveItemHandler
                        && mRemoveItemHandler(effect.mTarget, effect.mListener, effect.mCount);
                    break;
                case CompiledQuestCommandType::RewardXp:
                    command = "RewardXP ";
                    executed = mRewardXpHandler && mRewardXpHandler(effect.mCount);
                    break;
                case CompiledQuestCommandType::AddReputation:
                    command = "AddReputation ";
                    executed = mAddReputationHandler
                        && mAddReputationHandler(effect.mTarget, effect.mValue, effect.mCount);
                    break;
                case CompiledQuestCommandType::SetDestroyed:
                    command = "SetDestroyed ";
                    executed = mSetDestroyedHandler && mSetDestroyedHandler(effect.mTarget, effect.mValue);
                    break;
                case CompiledQuestCommandType::ShowMap:
                    command = "ShowMap ";
                    executed = mShowMapHandler && mShowMapHandler(effect.mTarget, effect.mValue);
                    break;
                case CompiledQuestCommandType::EnableFastTravel:
                    command = "EnableFastTravel ";
                    executed = mEnableFastTravelHandler
                        && mEnableFastTravelHandler(
                            effect.mValue, effect.mSecondaryValue, effect.mCount != 0);
                    break;
                default:
                    throw std::logic_error("non-external command queued as a Fallout quest external effect");
            }
            if (effect.mType == CompiledQuestCommandType::EnableFastTravel)
                command += std::to_string(static_cast<int>(effect.mValue)) + " "
                    + std::to_string(static_cast<int>(effect.mSecondaryValue)) + " "
                    + std::to_string(effect.mCount);
            else if (effect.mType == CompiledQuestCommandType::RewardXp)
                command += std::to_string(effect.mCount);
            else if (effect.mType == CompiledQuestCommandType::AddAchievement)
                command += std::to_string(effect.mCount);
            else
                command += ESM::RefId(effect.mTarget).serializeText();
            if (effect.mType == CompiledQuestCommandType::AddReputation)
                command += " " + std::to_string(static_cast<int>(effect.mValue)) + " "
                    + std::to_string(effect.mCount);
            if (effect.mType == CompiledQuestCommandType::SetDestroyed)
                command += " " + std::to_string(static_cast<int>(effect.mValue));
            if (effect.mType == CompiledQuestCommandType::ShowMap && effect.mValue)
                command += " 1";
            if (effect.mType == CompiledQuestCommandType::AddItem
                || effect.mType == CompiledQuestCommandType::RemoveItem)
                command += " " + ESM::RefId(effect.mListener).serializeText() + " "
                    + std::to_string(effect.mCount);
            if (effect.mType == CompiledQuestCommandType::MoveTo)
                command += " " + ESM::RefId(effect.mListener).serializeText();
            if (effect.mType == CompiledQuestCommandType::SetScriptPackage && effect.mValue)
                command += " " + ESM::RefId(effect.mListener).serializeText();
            if (effect.mType == CompiledQuestCommandType::SetActorEffect)
                command += " " + ESM::RefId(effect.mListener).serializeText();
            if (executed)
                Log(Debug::Info) << "FNV/ESM4 behavior: executed committed quest stage effect " << command;
            else
            {
                mUnsupportedStageCommands.push_back(command);
                Log(Debug::Warning) << "FNV/ESM4 behavior: committed quest stage effect failed " << command;
            }
        }
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
        else if (stageContainsCompiledLiveCondition(*stage))
            return executeCompiledStageTransaction(id, stageIndex);

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
                std::vector<CompiledConditionalFrame> conditionalStack;
                for (const CompiledQuestCommand& command : preparedEntry.mScript.mCommands)
                {
                    bool execute = true;
                    if (!updateCompiledConditionalState(command, mStates, conditionalStack, execute))
                        throw std::logic_error("preflighted Fallout quest conditional became invalid");
                    if (!execute)
                        continue;
                    bool executed = false;
                    switch (command.mType)
                    {
                        case CompiledQuestCommandType::If:
                        case CompiledQuestCommandType::ElseIf:
                        case CompiledQuestCommandType::Else:
                        case CompiledQuestCommandType::EndIf:
                            throw std::logic_error("compiled conditional escaped control-flow handling");
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
                        case CompiledQuestCommandType::CompleteAllObjectives:
                        {
                            ESM4QuestState* targetState = nullptr;
                            if (const ESM4::Quest* targetQuest
                                = mStore->get<ESM4::Quest>().search(ESM::RefId(command.mQuest)))
                                targetState = findState(*targetQuest);
                            if (targetState != nullptr)
                            {
                                for (auto& objective : targetState->mObjectiveStatus)
                                    objective.second |= ESM4QuestState::Objective_Completed;
                                executed = true;
                            }
                            break;
                        }
                        case CompiledQuestCommandType::ForceActiveQuest:
                            executed = forceActiveQuest(command.mQuest);
                            break;
                        case CompiledQuestCommandType::SetVariable:
                        {
                            const ESM4::Quest* targetQuest
                                = mStore->get<ESM4::Quest>().search(ESM::RefId(command.mQuest));
                            executed = targetQuest != nullptr
                                && setQuestVariable(targetQuest->mEditorId, command.mVariable, command.mNumber);
                            break;
                        }
                        case CompiledQuestCommandType::SetVariableFromItemCount:
                        {
                            const ESM4::Quest* targetQuest
                                = mStore->get<ESM4::Quest>().search(ESM::RefId(command.mQuest));
                            const std::optional<int> count = mItemCountHandler
                                ? mItemCountHandler(command.mTarget, command.mTopic)
                                : std::nullopt;
                            executed = targetQuest != nullptr && count && *count >= 0
                                && setQuestVariable(
                                    targetQuest->mEditorId, command.mVariable, static_cast<float>(*count));
                            break;
                        }
                        case CompiledQuestCommandType::SetAlly:
                            executed = mSetAllyHandler && mSetAllyHandler(command.mQuest, command.mTarget);
                            if (executed)
                                recordAllyPair(*state, command.mQuest, command.mTarget);
                            break;
                        case CompiledQuestCommandType::SetEnemy:
                            executed = mSetEnemyHandler
                                && mSetEnemyHandler(command.mQuest, command.mTarget, command.mValue,
                                    command.mSecondaryValue);
                            if (executed)
                                recordEnemyRelation(
                                    *state, command.mQuest, command.mTarget, command.mValue, command.mSecondaryValue);
                            break;
                        case CompiledQuestCommandType::Enable:
                            executed = mReferenceCommandHandler
                                && mReferenceCommandHandler(ESM4QuestReferenceCommand::Enable, command.mQuest);
                            break;
                        case CompiledQuestCommandType::Disable:
                            executed = mReferenceCommandHandler
                                && mReferenceCommandHandler(ESM4QuestReferenceCommand::Disable, command.mQuest);
                            break;
                        case CompiledQuestCommandType::Unlock:
                            executed = mReferenceCommandHandler
                                && mReferenceCommandHandler(ESM4QuestReferenceCommand::Unlock, command.mQuest);
                            break;
                        case CompiledQuestCommandType::Kill:
                            executed = mReferenceCommandHandler
                                && mReferenceCommandHandler(ESM4QuestReferenceCommand::Kill, command.mQuest);
                            break;
                        case CompiledQuestCommandType::ResetAi:
                            executed = mReferenceCommandHandler
                                && mReferenceCommandHandler(ESM4QuestReferenceCommand::ResetAi, command.mQuest);
                            break;
                        case CompiledQuestCommandType::MoveTo:
                            executed = mMoveToHandler && mMoveToHandler(command.mQuest, command.mTarget);
                            break;
                        case CompiledQuestCommandType::SetScriptPackage:
                            executed = mScriptPackageHandler
                                && mScriptPackageHandler(command.mQuest,
                                    command.mValue ? std::optional<ESM::FormId>{ command.mTarget } : std::nullopt);
                            break;
                        case CompiledQuestCommandType::SetActorEffect:
                            executed = mActorEffectCommandHandler
                                && mActorEffectCommandHandler(command.mQuest, command.mTarget, command.mValue);
                            break;
                        case CompiledQuestCommandType::AddItem:
                            executed = mAddItemHandler
                                && mAddItemHandler(command.mQuest, command.mTarget, command.mObjective);
                            break;
                        case CompiledQuestCommandType::RemoveItem:
                            executed = mRemoveItemHandler
                                && mRemoveItemHandler(command.mQuest, command.mTarget, command.mObjective);
                            break;
                        case CompiledQuestCommandType::EvaluatePackage:
                            executed = mReferenceCommandHandler
                                && mReferenceCommandHandler(ESM4QuestReferenceCommand::EvaluatePackage, command.mQuest);
                            break;
                        case CompiledQuestCommandType::ShowMessage:
                            executed = mMessageHandler && mMessageHandler(command.mQuest);
                            break;
                        case CompiledQuestCommandType::SetNote:
                            executed = mNoteHandler && mNoteHandler(command.mQuest, command.mValue);
                            break;
                        case CompiledQuestCommandType::AddAchievement:
                            executed = mAchievementHandler
                                && mAchievementHandler(static_cast<std::uint32_t>(command.mObjective));
                            break;
                        case CompiledQuestCommandType::SayTo:
                            executed = mSayToHandler
                                && mSayToHandler(command.mQuest, command.mTarget, command.mTopic);
                            break;
                        case CompiledQuestCommandType::RewardXp:
                            executed = mRewardXpHandler && mRewardXpHandler(command.mObjective);
                            break;
                        case CompiledQuestCommandType::AddReputation:
                            executed = mAddReputationHandler
                                && mAddReputationHandler(command.mQuest, command.mValue, command.mObjective);
                            break;
                        case CompiledQuestCommandType::SetDestroyed:
                            executed = mSetDestroyedHandler && mSetDestroyedHandler(command.mQuest, command.mValue);
                            break;
                        case CompiledQuestCommandType::ShowMap:
                            executed = mShowMapHandler && mShowMapHandler(command.mQuest, command.mValue);
                            break;
                        case CompiledQuestCommandType::EnableFastTravel:
                            executed = mEnableFastTravelHandler
                                && mEnableFastTravelHandler(
                                    command.mValue, command.mSecondaryValue, command.mObjective != 0);
                            break;
                    }
                    if (!executed)
                    {
                        if (command.mType == CompiledQuestCommandType::EvaluatePackage
                            || command.mType == CompiledQuestCommandType::ResetAi
                            || command.mType == CompiledQuestCommandType::MoveTo
                            || command.mType == CompiledQuestCommandType::SetScriptPackage
                            || command.mType == CompiledQuestCommandType::SetActorEffect
                            || command.mType == CompiledQuestCommandType::ShowMessage
                            || command.mType == CompiledQuestCommandType::SetNote
                            || command.mType == CompiledQuestCommandType::AddAchievement
                            || command.mType == CompiledQuestCommandType::SayTo
                            || command.mType == CompiledQuestCommandType::SetAlly
                            || command.mType == CompiledQuestCommandType::SetEnemy
                            || command.mType == CompiledQuestCommandType::Enable
                            || command.mType == CompiledQuestCommandType::Disable
                            || command.mType == CompiledQuestCommandType::Unlock
                            || command.mType == CompiledQuestCommandType::Kill
                            || command.mType == CompiledQuestCommandType::AddItem
                            || command.mType == CompiledQuestCommandType::RemoveItem
                            || command.mType == CompiledQuestCommandType::RewardXp
                            || command.mType == CompiledQuestCommandType::AddReputation
                            || command.mType == CompiledQuestCommandType::SetDestroyed
                            || command.mType == CompiledQuestCommandType::ShowMap
                            || command.mType == CompiledQuestCommandType::EnableFastTravel)
                        {
                            std::string failure;
                            if (command.mType == CompiledQuestCommandType::EvaluatePackage)
                                failure = "EvaluatePackage " + ESM::RefId(command.mQuest).serializeText();
                            else if (command.mType == CompiledQuestCommandType::ResetAi)
                                failure = "ResetAI " + ESM::RefId(command.mQuest).serializeText();
                            else if (command.mType == CompiledQuestCommandType::MoveTo)
                                failure = "MoveTo " + ESM::RefId(command.mQuest).serializeText() + " "
                                    + ESM::RefId(command.mTarget).serializeText();
                            else if (command.mType == CompiledQuestCommandType::SetScriptPackage)
                                failure = std::string(command.mValue ? "AddScriptPackage " : "RemoveScriptPackage ")
                                    + ESM::RefId(command.mQuest).serializeText()
                                    + (command.mValue ? " " + ESM::RefId(command.mTarget).serializeText() : "");
                            else if (command.mType == CompiledQuestCommandType::SetActorEffect)
                                failure = std::string(command.mValue ? "AddSpell " : "RemoveSpell ")
                                    + ESM::RefId(command.mQuest).serializeText() + " "
                                    + ESM::RefId(command.mTarget).serializeText();
                            else if (command.mType == CompiledQuestCommandType::ShowMessage)
                                failure = "ShowMessage " + ESM::RefId(command.mQuest).serializeText();
                            else if (command.mType == CompiledQuestCommandType::SetNote)
                                failure = std::string(command.mValue ? "AddNote " : "RemoveNote ")
                                    + ESM::RefId(command.mQuest).serializeText();
                            else if (command.mType == CompiledQuestCommandType::AddAchievement)
                                failure = "AddAchievement " + std::to_string(command.mObjective);
                            else if (command.mType == CompiledQuestCommandType::SetAlly)
                                failure = "SetAlly " + ESM::RefId(command.mQuest).serializeText() + " "
                                    + ESM::RefId(command.mTarget).serializeText();
                            else if (command.mType == CompiledQuestCommandType::SetEnemy)
                                failure = "SetEnemy " + ESM::RefId(command.mQuest).serializeText() + " "
                                    + ESM::RefId(command.mTarget).serializeText();
                            else if (command.mType == CompiledQuestCommandType::Enable)
                                failure = "Enable " + ESM::RefId(command.mQuest).serializeText();
                            else if (command.mType == CompiledQuestCommandType::Disable)
                                failure = "Disable " + ESM::RefId(command.mQuest).serializeText();
                            else if (command.mType == CompiledQuestCommandType::Unlock)
                                failure = "Unlock " + ESM::RefId(command.mQuest).serializeText();
                            else if (command.mType == CompiledQuestCommandType::Kill)
                                failure = "Kill " + ESM::RefId(command.mQuest).serializeText();
                            else if (command.mType == CompiledQuestCommandType::AddItem)
                                failure = "AddItem " + ESM::RefId(command.mQuest).serializeText() + " "
                                    + ESM::RefId(command.mTarget).serializeText() + " "
                                    + std::to_string(command.mObjective);
                            else if (command.mType == CompiledQuestCommandType::RemoveItem)
                                failure = "RemoveItem " + ESM::RefId(command.mQuest).serializeText() + " "
                                    + ESM::RefId(command.mTarget).serializeText() + " "
                                    + std::to_string(command.mObjective);
                            else if (command.mType == CompiledQuestCommandType::RewardXp)
                                failure = "RewardXP " + std::to_string(command.mObjective);
                            else if (command.mType == CompiledQuestCommandType::AddReputation)
                                failure = "AddReputation " + ESM::RefId(command.mQuest).serializeText() + " "
                                    + std::to_string(static_cast<int>(command.mValue)) + " "
                                    + std::to_string(command.mObjective);
                            else if (command.mType == CompiledQuestCommandType::SetDestroyed)
                                failure = "SetDestroyed " + ESM::RefId(command.mQuest).serializeText() + " "
                                    + std::to_string(static_cast<int>(command.mValue));
                            else if (command.mType == CompiledQuestCommandType::ShowMap)
                                failure = "ShowMap " + ESM::RefId(command.mQuest).serializeText()
                                    + (command.mValue ? " 1" : "");
                            else if (command.mType == CompiledQuestCommandType::EnableFastTravel)
                                failure = "EnableFastTravel "
                                    + std::to_string(static_cast<int>(command.mValue)) + " "
                                    + std::to_string(static_cast<int>(command.mSecondaryValue)) + " "
                                    + std::to_string(command.mObjective);
                            else
                                failure = "SayTo " + ESM::RefId(command.mQuest).serializeText();
                            mUnsupportedStageCommands.push_back(failure);
                            Log(Debug::Warning) << "FNV/ESM4 behavior: quest stage external effect failed: "
                                                << failure;
                            continue;
                        }
                        throw std::logic_error("preflighted Fallout quest command became invalid during execution");
                    }
                }
                if (!conditionalStack.empty())
                    throw std::logic_error("preflighted Fallout quest conditional stack remained open");
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
                [](const auto& value) { return value.second != 0.f; })
            || !state.mAllies.empty() || !state.mEnemies.empty();
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
            writer.writeHNT("ALCT", static_cast<std::uint32_t>(state.mAllies.size()));
            for (const auto& [first, second] : state.mAllies)
            {
                writer.writeFormId(first, true, "ALF1");
                writer.writeFormId(second, true, "ALF2");
            }
            writer.writeHNT("ENCT", static_cast<std::uint32_t>(state.mEnemies.size()));
            for (const ESM4QuestState::EnemyRelation& relation : state.mEnemies)
            {
                writer.writeFormId(relation.mFirst, true, "ENF1");
                writer.writeFormId(relation.mSecond, true, "ENF2");
                writer.writeHNT("ENR1", static_cast<std::uint8_t>(relation.mFirstTreatsSecondAsNeutral));
                writer.writeHNT("ENR2", static_cast<std::uint8_t>(relation.mSecondTreatsFirstAsNeutral));
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

        std::vector<std::pair<std::string, float>> variables;
        while (reader.isNextSub("VNAM"))
        {
            const std::string name = reader.getHString();
            float value = 0.f;
            reader.getHNT(value, "VVAL");
            variables.emplace_back(name, value);
        }

        std::vector<std::pair<ESM::FormId, ESM::FormId>> allies;
        if (reader.isNextSub("ALCT"))
        {
            std::uint32_t allyCount = 0;
            reader.getHT(allyCount);
            if (allyCount > 65536)
                throw std::runtime_error("Fallout quest save has an invalid allied-faction count");
            allies.reserve(allyCount);
            for (std::uint32_t i = 0; i < allyCount; ++i)
            {
                ESM::FormId first = reader.getFormId(true, "ALF1");
                ESM::FormId second = reader.getFormId(true, "ALF2");
                const bool firstAvailable = reader.applyContentFileMapping(first);
                const bool secondAvailable = reader.applyContentFileMapping(second);
                if (firstAvailable && secondAvailable)
                    allies.emplace_back(first, second);
            }
        }

        std::vector<ESM4QuestState::EnemyRelation> enemies;
        if (reader.isNextSub("ENCT"))
        {
            std::uint32_t enemyCount = 0;
            reader.getHT(enemyCount);
            if (enemyCount > 65536)
                throw std::runtime_error("Fallout quest save has an invalid enemy-faction count");
            enemies.reserve(enemyCount);
            for (std::uint32_t i = 0; i < enemyCount; ++i)
            {
                ESM::FormId first = reader.getFormId(true, "ENF1");
                ESM::FormId second = reader.getFormId(true, "ENF2");
                std::uint8_t firstNeutral = 0;
                std::uint8_t secondNeutral = 0;
                reader.getHNT(firstNeutral, "ENR1");
                reader.getHNT(secondNeutral, "ENR2");
                if (firstNeutral > 1 || secondNeutral > 1)
                    throw std::runtime_error("Fallout quest save has an invalid enemy-faction reaction");
                const bool firstAvailable = reader.applyContentFileMapping(first);
                const bool secondAvailable = reader.applyContentFileMapping(second);
                if (firstAvailable && secondAvailable)
                    enemies.push_back({ first, second, firstNeutral != 0, secondNeutral != 0 });
            }
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
        state->mAllies.clear();
        for (const auto& [first, second] : allies)
        {
            if (mStore == nullptr || mStore->get<ESM4::Faction>().search(ESM::RefId(first)) == nullptr
                || mStore->get<ESM4::Faction>().search(ESM::RefId(second)) == nullptr || !mSetAllyHandler
                || !mSetAllyHandler(first, second))
                throw std::runtime_error("Fallout quest save could not restore an allied-faction relation");
            recordAllyPair(*state, first, second);
        }
        state->mEnemies.clear();
        for (const ESM4QuestState::EnemyRelation& relation : enemies)
        {
            if (mStore == nullptr
                || mStore->get<ESM4::Faction>().search(ESM::RefId(relation.mFirst)) == nullptr
                || mStore->get<ESM4::Faction>().search(ESM::RefId(relation.mSecond)) == nullptr
                || !mSetEnemyHandler
                || !mSetEnemyHandler(relation.mFirst, relation.mSecond,
                    relation.mFirstTreatsSecondAsNeutral, relation.mSecondTreatsFirstAsNeutral))
                throw std::runtime_error("Fallout quest save could not restore an enemy-faction relation");
            recordEnemyRelation(*state, relation.mFirst, relation.mSecond,
                relation.mFirstTreatsSecondAsNeutral, relation.mSecondTreatsFirstAsNeutral);
        }
        for (auto& [_, value] : state->mVariables)
            value = 0.f;
        for (const auto& [name, value] : variables)
            if (const auto found = state->mVariables.find(Misc::StringUtils::lowerCase(name));
                found != state->mVariables.end())
                found->second = value;
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

    ESM::FormId ESM4QuestRuntime::resolveReference(std::string_view id)
    {
        const std::string key = Misc::StringUtils::lowerCase(id);
        if (const auto cached = mReferenceIds.find(key); cached != mReferenceIds.end())
            return cached->second;

        ESM::FormId result;
        const auto searchRecords = [&id, &result](const auto& records) {
            for (const auto& record : records)
            {
                if (Misc::StringUtils::ciEqual(record.mEditorId, id))
                {
                    result = record.mId;
                    return true;
                }
            }
            return false;
        };
        if (mStore != nullptr && !searchRecords(mStore->get<ESM4::Reference>())
            && !searchRecords(mStore->get<ESM4::ActorCharacter>()))
            searchRecords(mStore->get<ESM4::ActorCreature>());

        mReferenceIds.emplace(key, result);
        return result;
    }

    ESM::FormId ESM4QuestRuntime::resolveFaction(std::string_view id)
    {
        const std::string key = Misc::StringUtils::lowerCase(id);
        if (const auto cached = mFactionIds.find(key); cached != mFactionIds.end())
            return cached->second;

        ESM::FormId result;
        if (mStore != nullptr)
        {
            for (const ESM4::Faction& faction : mStore->get<ESM4::Faction>())
            {
                if (Misc::StringUtils::ciEqual(faction.mEditorId, id))
                {
                    result = faction.mId;
                    break;
                }
            }
        }
        mFactionIds.emplace(key, result);
        return result;
    }

    ESM::FormId ESM4QuestRuntime::resolveInventoryItem(std::string_view id)
    {
        const std::string key = Misc::StringUtils::lowerCase(id);
        if (const auto cached = mInventoryItemIds.find(key); cached != mInventoryItemIds.end())
            return cached->second;

        ESM::FormId result;
        const auto searchRecords = [&id, &result](const auto& records) {
            for (const auto& record : records)
            {
                if (!Misc::StringUtils::ciEqual(record.mEditorId, id))
                    continue;
                result = record.mId;
                return true;
            }
            return false;
        };
        if (mStore != nullptr
            && !searchRecords(mStore->get<ESM4::Ammunition>())
            && !searchRecords(mStore->get<ESM4::Armor>())
            && !searchRecords(mStore->get<ESM4::Book>())
            && !searchRecords(mStore->get<ESM4::Clothing>())
            && !searchRecords(mStore->get<ESM4::Ingredient>())
            && !searchRecords(mStore->get<ESM4::ItemMod>())
            && !searchRecords(mStore->get<ESM4::Key>())
            && !searchRecords(mStore->get<ESM4::Light>())
            && !searchRecords(mStore->get<ESM4::MiscItem>())
            && !searchRecords(mStore->get<ESM4::Potion>()))
            searchRecords(mStore->get<ESM4::Weapon>());

        mInventoryItemIds.emplace(key, result);
        return result;
    }

    ESM::FormId ESM4QuestRuntime::resolveFormList(std::string_view id)
    {
        const std::string key = Misc::StringUtils::lowerCase(id);
        if (const auto cached = mFormListIds.find(key); cached != mFormListIds.end())
            return cached->second;

        ESM::FormId result;
        if (mStore != nullptr)
        {
            for (const ESM4::FormIdList& list : mStore->get<ESM4::FormIdList>())
            {
                if (Misc::StringUtils::ciEqual(list.mEditorId, id))
                {
                    result = list.mId;
                    break;
                }
            }
        }
        mFormListIds.emplace(key, result);
        return result;
    }

    ESM::FormId ESM4QuestRuntime::resolveReputation(std::string_view id)
    {
        const std::string key = Misc::StringUtils::lowerCase(id);
        if (const auto cached = mReputationIds.find(key); cached != mReputationIds.end())
            return cached->second;

        ESM::FormId result;
        if (mStore != nullptr)
        {
            for (const ESM4::Reputation& reputation : mStore->get<ESM4::Reputation>())
            {
                if (!Misc::StringUtils::ciEqual(reputation.mEditorId, id))
                    continue;
                result = reputation.mId;
                break;
            }
        }
        mReputationIds.emplace(key, result);
        return result;
    }

    ESM::FormId ESM4QuestRuntime::resolveNote(std::string_view id)
    {
        const std::string key = Misc::StringUtils::lowerCase(id);
        if (const auto cached = mNoteIds.find(key); cached != mNoteIds.end())
            return cached->second;

        ESM::FormId result;
        if (mStore != nullptr)
        {
            for (const ESM4::Note& note : mStore->get<ESM4::Note>())
            {
                if (!Misc::StringUtils::ciEqual(note.mEditorId, id))
                    continue;
                result = note.mId;
                break;
            }
        }
        mNoteIds.emplace(key, result);
        return result;
    }

    ESM::FormId ESM4QuestRuntime::resolvePerk(std::string_view id)
    {
        const std::string key = Misc::StringUtils::lowerCase(id);
        if (const auto cached = mPerkIds.find(key); cached != mPerkIds.end())
            return cached->second;

        ESM::FormId result;
        if (mStore != nullptr)
        {
            for (const ESM4::Perk& perk : mStore->get<ESM4::Perk>())
            {
                if (!Misc::StringUtils::ciEqual(perk.mEditorId, id))
                    continue;
                result = perk.mId;
                break;
            }
        }
        mPerkIds.emplace(key, result);
        return result;
    }

    ESM::FormId ESM4QuestRuntime::resolveActorBase(std::string_view id)
    {
        const std::string key = Misc::StringUtils::lowerCase(id);
        if (const auto cached = mActorBaseIds.find(key); cached != mActorBaseIds.end())
            return cached->second;

        ESM::FormId result;
        const auto searchRecords = [&id, &result](const auto& records) {
            for (const auto& record : records)
            {
                if (!Misc::StringUtils::ciEqual(record.mEditorId, id))
                    continue;
                result = record.mId;
                return true;
            }
            return false;
        };
        if (mStore != nullptr && !searchRecords(mStore->get<ESM4::Npc>()))
            searchRecords(mStore->get<ESM4::Creature>());
        mActorBaseIds.emplace(key, result);
        return result;
    }

    ESM::FormId ESM4QuestRuntime::resolveSpell(std::string_view id)
    {
        const std::string key = Misc::StringUtils::lowerCase(id);
        if (const auto cached = mSpellIds.find(key); cached != mSpellIds.end())
            return cached->second;

        ESM::FormId result;
        if (mStore != nullptr)
        {
            for (const ESM4::Spell& spell : mStore->get<ESM4::Spell>())
            {
                if (!Misc::StringUtils::ciEqual(spell.mEditorId, id))
                    continue;
                result = spell.mId;
                break;
            }
        }
        mSpellIds.emplace(key, result);
        return result;
    }

    ESM::FormId ESM4QuestRuntime::resolveMessage(std::string_view id)
    {
        const std::string key = Misc::StringUtils::lowerCase(id);
        if (const auto cached = mMessageIds.find(key); cached != mMessageIds.end())
            return cached->second;

        ESM::FormId result;
        if (mStore != nullptr)
        {
            for (const ESM4::Message& message : mStore->get<ESM4::Message>())
            {
                if (!Misc::StringUtils::ciEqual(message.mEditorId, id))
                    continue;
                result = message.mId;
                break;
            }
        }
        mMessageIds.emplace(key, result);
        return result;
    }

    ESM::FormId ESM4QuestRuntime::resolveIdleAnimation(std::string_view id)
    {
        const std::string key = Misc::StringUtils::lowerCase(id);
        if (const auto cached = mIdleAnimationIds.find(key); cached != mIdleAnimationIds.end())
            return cached->second;

        ESM::FormId result;
        if (mStore != nullptr)
        {
            for (const ESM4::IdleAnimation& idle : mStore->get<ESM4::IdleAnimation>())
            {
                if (!Misc::StringUtils::ciEqual(idle.mEditorId, id))
                    continue;
                result = idle.mId;
                break;
            }
        }
        mIdleAnimationIds.emplace(key, result);
        return result;
    }

    ESM::FormId ESM4QuestRuntime::resolveSound(std::string_view id)
    {
        const std::string key = Misc::StringUtils::lowerCase(id);
        if (const auto cached = mSoundIds.find(key); cached != mSoundIds.end())
            return cached->second;

        ESM::FormId result;
        const auto searchRecords = [&id, &result](const auto& records) {
            for (const auto& sound : records)
            {
                if (!Misc::StringUtils::ciEqual(sound.mEditorId, id))
                    continue;
                result = sound.mId;
                return true;
            }
            return false;
        };
        if (mStore != nullptr && !searchRecords(mStore->get<ESM4::Sound>()))
            searchRecords(mStore->get<ESM4::SoundReference>());
        mSoundIds.emplace(key, result);
        return result;
    }

    ESM::FormId ESM4QuestRuntime::resolveOwner(std::string_view id)
    {
        // Fallout's XOWN field accepts either an NPC base or a faction. Keep
        // that distinction in the native record identity instead of
        // translating ownership into quest-local state.
        ESM::FormId result = resolveFaction(id);
        if (result.isZeroOrUnset())
            result = resolveActorBase(id);
        return result;
    }

    ESM::FormId ESM4QuestRuntime::resolveCell(std::string_view id)
    {
        const std::string key = Misc::StringUtils::lowerCase(id);
        if (const auto cached = mCellIds.find(key); cached != mCellIds.end())
            return cached->second;

        ESM::FormId result;
        if (mStore != nullptr)
        {
            for (const ESM4::Cell& cell : mStore->get<ESM4::Cell>())
            {
                if (!Misc::StringUtils::ciEqual(cell.mEditorId, id))
                    continue;
                if (const ESM::FormId* const formId = cell.mId.getIf<ESM::FormId>())
                    result = *formId;
                break;
            }
        }
        mCellIds.emplace(key, result);
        return result;
    }

    bool ESM4QuestRuntime::executeReferenceCommand(ESM4QuestReferenceCommand command, std::string_view id)
    {
        const ESM::FormId reference = resolveReference(id);
        return !reference.isZeroOrUnset() && mReferenceCommandHandler
            && mReferenceCommandHandler(command, reference);
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

        ESM4QuestState* ownerState = nullptr;
        if (ownerQuest)
        {
            const auto state = mStates.find(*ownerQuest);
            if (state != mStates.end())
                ownerState = &state->second;
        }

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
                if (quest != nullptr)
                    if (const std::optional<float> value = valueFromQuest(*quest, normalised))
                        return value;
            }
            if (ownerActor)
                if (const std::optional<float> value = valueFromActor(*ownerActor, normalised))
                    return value;
            if (ownerReference)
                if (const std::optional<float> value = valueFromReference(*ownerReference, normalised))
                    return value;

            // Stage source uses global editor IDs directly in expressions,
            // including reward globals such as NVDLC03Act3XP and calendar
            // values such as GameDaysPassed. Local variables shadow globals,
            // matching the authored script scope.
            if (mGlobals != nullptr)
            {
                const GlobalVariableName name(normalised);
                if (mGlobals->getType(name) != ' ')
                    return (*mGlobals)[name].getFloat();
            }
            if (mStore != nullptr)
            {
                for (const ESM4::GlobalVariable& global : mStore->get<ESM4::GlobalVariable>())
                {
                    if (!Misc::StringUtils::ciEqual(global.mEditorId, normalised))
                        continue;
                    return global.mValue;
                }
            }
            return std::nullopt;
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
                                     &actionReference, ownerActor, ownerReference](const SourceTokens& tokens,
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
                if (command == "getdead")
                {
                    const ESM::FormId actor = resolveReference(subject);
                    if (actor.isZeroOrUnset() || !mActorDeadHandler)
                        return std::nullopt;
                    const std::optional<bool> dead = mActorDeadHandler(actor);
                    return dead ? std::optional<float>(*dead ? 1.f : 0.f) : std::nullopt;
                }
                if (command == "getav" || command == "getactorvalue")
                {
                    if (index >= tokens.size() || !mActorValueHandler)
                        return std::nullopt;
                    const ESM::FormId actor = Misc::StringUtils::ciEqual(subject, "player")
                            || Misc::StringUtils::ciEqual(subject, "playerref")
                        ? ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 }
                        : resolveReference(subject);
                    if (actor.isZeroOrUnset())
                        return std::nullopt;
                    return mActorValueHandler(actor, tokens[index++]);
                }
                if (command == "getinfaction" || command == "getfactionrank")
                {
                    if (index >= tokens.size() || !mActorFactionMembershipHandler)
                        return std::nullopt;
                    const ESM::FormId actor = Misc::StringUtils::ciEqual(subject, "player")
                            || Misc::StringUtils::ciEqual(subject, "playerref")
                        ? ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 }
                        : resolveReference(subject);
                    const ESM::FormId faction = resolveFaction(tokens[index++]);
                    if (actor.isZeroOrUnset() || faction.isZeroOrUnset())
                        return std::nullopt;
                    const std::optional<ESM4QuestFactionMembership> membership
                        = mActorFactionMembershipHandler(actor, faction);
                    if (!membership)
                        return std::nullopt;
                    return command == "getinfaction"
                        ? std::optional<float>(membership->mMember ? 1.f : 0.f)
                        : std::optional<float>(
                            membership->mMember ? static_cast<float>(membership->mRank) : -1.f);
                }
                if (command == "getunconscious" || command == "getrestrained"
                    || command == "getplayerteammate" || command == "getignorecrime"
                    || command == "getisghost" || command == "getignorefriendlyhits"
                    || command == "getisalerted")
                {
                    if (!mActorFlagHandler)
                        return std::nullopt;
                    const ESM::FormId actor = Misc::StringUtils::ciEqual(subject, "player")
                            || Misc::StringUtils::ciEqual(subject, "playerref")
                        ? ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 }
                        : resolveReference(subject);
                    const std::optional<ESM4QuestActorFlag> flag
                        = actorFlagFromSourceCommand(command);
                    if (actor.isZeroOrUnset() || !flag)
                        return std::nullopt;
                    const std::optional<bool> enabled = mActorFlagHandler(actor, *flag);
                    return enabled ? std::optional<float>(*enabled ? 1.f : 0.f) : std::nullopt;
                }
                if (command == "getitemcount")
                {
                    if (index >= tokens.size() || !mItemCountHandler)
                        return std::nullopt;
                    const ESM::FormId owner = Misc::StringUtils::ciEqual(subject, "player")
                        ? ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 }
                        : resolveReference(subject);
                    const ESM::FormId item = resolveInventoryItem(tokens[index++]);
                    if (owner.isZeroOrUnset() || item.isZeroOrUnset())
                        return std::nullopt;
                    const std::optional<int> count = mItemCountHandler(owner, item);
                    return count && *count >= 0 ? std::optional<float>(static_cast<float>(*count)) : std::nullopt;
                }
                if (command == "gethasnote")
                {
                    if (index >= tokens.size() || !mKnownNoteHandler
                        || (!Misc::StringUtils::ciEqual(subject, "player")
                            && !Misc::StringUtils::ciEqual(subject, "playerref")))
                        return std::nullopt;
                    const ESM::FormId note = resolveNote(tokens[index++]);
                    if (note.isZeroOrUnset())
                        return std::nullopt;
                    const std::optional<bool> known = mKnownNoteHandler(note);
                    return known ? std::optional<float>(*known ? 1.f : 0.f) : std::nullopt;
                }
                if (command == "hasperk")
                {
                    if (index >= tokens.size() || !mPlayerHasPerkHandler
                        || (!Misc::StringUtils::ciEqual(subject, "player")
                            && !Misc::StringUtils::ciEqual(subject, "playerref")))
                        return std::nullopt;
                    const ESM::FormId perk = resolvePerk(tokens[index++]);
                    if (perk.isZeroOrUnset())
                        return std::nullopt;
                    const std::optional<bool> present = mPlayerHasPerkHandler(perk);
                    return present ? std::optional<float>(*present ? 1.f : 0.f) : std::nullopt;
                }
                if (command == "getdisabled" || command == "getdestroyed")
                {
                    const MWWorld::Ptr reference = resolveAuthoredReference(subject);
                    if (reference.isEmpty())
                        return std::nullopt;
                    return command == "getdisabled"
                        ? (reference.getRefData().isEnabled() ? 0.f : 1.f)
                        : (reference.getRefData().isDestroyed() ? 1.f : 0.f);
                }
                if (command == "getincell")
                {
                    if (index >= tokens.size() || mStore == nullptr)
                        return std::nullopt;
                    const MWWorld::Ptr actor = resolveAuthoredReference(subject);
                    const std::string_view cellEditorId = tokens[index++];
                    if (actor.isEmpty() || !actor.isInCell())
                        return 0.f;
                    for (const ESM4::Cell& cell : mStore->get<ESM4::Cell>())
                        if (Misc::StringUtils::ciEqual(cell.mEditorId, cellEditorId))
                            return actor.getCell()->getCell()->getId() == cell.mId ? 1.f : 0.f;
                    return std::nullopt;
                }
                if (command == "getinworldspace")
                {
                    if (index >= tokens.size() || mStore == nullptr)
                        return std::nullopt;
                    const MWWorld::Ptr actor = resolveAuthoredReference(subject);
                    const std::string_view worldEditorId = tokens[index++];
                    if (actor.isEmpty() || !actor.isInCell())
                        return 0.f;
                    for (const ESM4::World& world : mStore->get<ESM4::World>())
                        if (Misc::StringUtils::ciEqual(world.mEditorId, worldEditorId))
                            return actor.getCell()->getCell()->getWorldSpace() == ESM::RefId(world.mId) ? 1.f : 0.f;
                    return std::nullopt;
                }
                if (command == "getdistance")
                {
                    if (index >= tokens.size())
                        return std::nullopt;
                    const MWWorld::Ptr actor = resolveAuthoredReference(subject);
                    const MWWorld::Ptr target = resolveAuthoredReference(tokens[index++]);
                    if (actor.isEmpty() || target.isEmpty() || !actor.isInCell() || !target.isInCell())
                        return std::nullopt;
                    if (actor.getCell() != target.getCell())
                    {
                        const auto* const actorCell = actor.getCell()->getCell();
                        const auto* const targetCell = target.getCell()->getCell();
                        if (!actorCell->isExterior() || !targetCell->isExterior()
                            || actorCell->getWorldSpace() != targetCell->getWorldSpace())
                            return std::nullopt;
                    }
                    return (actor.getRefData().getPosition().asVec3()
                               - target.getRefData().getPosition().asVec3())
                        .length();
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
            if (token == "getquestrunning" || token == "getqr" || token == "getquestcompleted")
            {
                if (index >= tokens.size())
                    return std::nullopt;
                const ESM4::Quest* const quest = resolveQuest(tokens[index++]);
                const ESM4QuestState* const state = quest != nullptr ? findState(*quest) : nullptr;
                if (state == nullptr)
                    return std::nullopt;
                const std::uint8_t flag = token == "getquestcompleted"
                    ? ESM4QuestState::Flag_Completed
                    : ESM4QuestState::Flag_Running;
                return (state->mFlags & flag) != 0 ? 1.f : 0.f;
            }
            if (token == "getdead")
            {
                if (index >= tokens.size() || !mActorDeadHandler)
                    return std::nullopt;
                const ESM::FormId actor = resolveReference(tokens[index++]);
                if (actor.isZeroOrUnset())
                    return std::nullopt;
                const std::optional<bool> dead = mActorDeadHandler(actor);
                return dead ? std::optional<float>(*dead ? 1.f : 0.f) : std::nullopt;
            }
            if ((token == "getav" || token == "getactorvalue") && (ownerActor || ownerReference))
            {
                if (index >= tokens.size() || !mActorValueHandler)
                    return std::nullopt;
                return mActorValueHandler(ownerActor ? *ownerActor : *ownerReference, tokens[index++]);
            }
            if ((token == "getinfaction" || token == "getfactionrank")
                && (ownerActor || ownerReference))
            {
                if (index >= tokens.size() || !mActorFactionMembershipHandler)
                    return std::nullopt;
                const ESM::FormId faction = resolveFaction(tokens[index++]);
                if (faction.isZeroOrUnset())
                    return std::nullopt;
                const std::optional<ESM4QuestFactionMembership> membership
                    = mActorFactionMembershipHandler(ownerActor ? *ownerActor : *ownerReference, faction);
                if (!membership)
                    return std::nullopt;
                return token == "getinfaction"
                    ? std::optional<float>(membership->mMember ? 1.f : 0.f)
                    : std::optional<float>(
                        membership->mMember ? static_cast<float>(membership->mRank) : -1.f);
            }
            if ((token == "getunconscious" || token == "getrestrained"
                    || token == "getplayerteammate" || token == "getignorecrime"
                    || token == "getisghost" || token == "getignorefriendlyhits"
                    || token == "getisalerted")
                && (ownerActor || ownerReference))
            {
                if (!mActorFlagHandler)
                    return std::nullopt;
                const std::optional<ESM4QuestActorFlag> flag = actorFlagFromSourceCommand(token);
                if (!flag)
                    return std::nullopt;
                const std::optional<bool> enabled
                    = mActorFlagHandler(ownerActor ? *ownerActor : *ownerReference, *flag);
                return enabled ? std::optional<float>(*enabled ? 1.f : 0.f) : std::nullopt;
            }
            if (token == "gethasnote")
            {
                if (index >= tokens.size() || !mKnownNoteHandler)
                    return std::nullopt;
                const ESM::FormId note = resolveNote(tokens[index++]);
                if (note.isZeroOrUnset())
                    return std::nullopt;
                const std::optional<bool> known = mKnownNoteHandler(note);
                return known ? std::optional<float>(*known ? 1.f : 0.f) : std::nullopt;
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

        const auto sourceInteger = [&sourceExpression](const SourceTokens& tokens, std::size_t& index)
            -> std::optional<std::int32_t> {
            const std::optional<float> value = sourceExpression(tokens, index);
            if (!value || !std::isfinite(*value))
                return std::nullopt;
            const float rounded = std::round(*value);
            if (std::abs(*value - rounded) > 0.0001f
                || rounded < static_cast<float>(std::numeric_limits<std::int32_t>::min())
                || rounded > static_cast<float>(std::numeric_limits<std::int32_t>::max()))
                return std::nullopt;
            return static_cast<std::int32_t>(rounded);
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

        const auto executeSourceReputationValueCommand
            = [this, &sourceExpression, &sourceInteger](
                  std::string_view command, const std::vector<std::string_view>& tokens) {
                  if (tokens.size() < 4 || !mReputationValueCommandHandler)
                      return false;

                  const bool set = Misc::StringUtils::ciEqual(command, "SetReputation");
                  const bool addExact = Misc::StringUtils::ciEqual(command, "AddReputationExact");
                  if (!set && !addExact)
                      return false;

                  const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                  std::size_t argument = 2;
                  const std::optional<std::int32_t> fame = sourceInteger(sourceTokens, argument);
                  const std::optional<float> amount = sourceExpression(sourceTokens, argument);
                  const ESM::FormId reputation = resolveReputation(tokens[1]);
                  if (!fame || (*fame != 0 && *fame != 1) || !amount || !std::isfinite(*amount)
                      || argument != sourceTokens.size() || reputation.isZeroOrUnset())
                      return false;

                  const ESM4QuestReputationValueCommand type = set
                      ? ESM4QuestReputationValueCommand::Set
                      : ESM4QuestReputationValueCommand::AddExact;
                  if (!mReputationValueCommandHandler(reputation, type, *fame != 0, *amount))
                      return false;
                  Log(Debug::Info) << "FNV/ESM4 behavior: " << command << " reputation=" << tokens[1]
                                   << " fame=" << (*fame != 0) << " amount=" << *amount;
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

                const auto sourceOwnerId = [this, subject]() {
                    if (Misc::StringUtils::ciEqual(subject, "player")
                        || Misc::StringUtils::ciEqual(subject, "playerref"))
                        return ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 };
                    return this->resolveReference(subject);
                };
                const auto sourceReferenceId = [this](std::string_view editorId) {
                    if (Misc::StringUtils::ciEqual(editorId, "player")
                        || Misc::StringUtils::ciEqual(editorId, "playerref"))
                        return ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 };
                    return this->resolveReference(editorId);
                };
                const bool playerSubject = Misc::StringUtils::ciEqual(subject, "player")
                    || Misc::StringUtils::ciEqual(subject, "playerref");

                if (Misc::StringUtils::ciEqual(command, "Activate")
                    && tokens.size() >= 1 && tokens.size() <= 3)
                {
                    const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                    const ESM::FormId target = sourceOwnerId();
                    ESM::FormId activator{ .mIndex = 0x14, .mContentFile = 0 };
                    std::size_t argument = 1;
                    bool valid = !target.isZeroOrUnset();
                    if (tokens.size() >= 2)
                    {
                        activator = sourceReferenceId(tokens[1]);
                        valid = !activator.isZeroOrUnset();
                        argument = 2;
                    }
                    bool runOnActivateBlock = false;
                    if (valid && argument < sourceTokens.size())
                    {
                        const std::optional<std::int32_t> runBlock = sourceInteger(sourceTokens, argument);
                        valid = runBlock && (*runBlock == 0 || *runBlock == 1);
                        runOnActivateBlock = valid && *runBlock != 0;
                    }
                    if (valid && argument == sourceTokens.size() && mReferenceActivationHandler
                        && mReferenceActivationHandler(target, activator, runOnActivateBlock))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: Activate target=" << subject
                                         << " activator=" << ESM::RefId(activator).serializeText()
                                         << " runOnActivateBlock=" << runOnActivateBlock;
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "PlayIdle")
                    && tokens.size() == 2)
                {
                    const ESM::FormId actor = sourceOwnerId();
                    const ESM::FormId idle = resolveIdleAnimation(tokens[1]);
                    if (!actor.isZeroOrUnset() && !idle.isZeroOrUnset() && mActorIdleHandler
                        && mActorIdleHandler(actor, idle))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: PlayIdle actor=" << subject
                                         << " idle=" << tokens[1];
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "PlayGroup")
                    && tokens.size() >= 2 && tokens.size() <= 3)
                {
                    const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                    std::size_t argument = 2;
                    int mode = 0;
                    bool valid = true;
                    if (argument < sourceTokens.size())
                    {
                        const std::optional<std::int32_t> parsedMode = sourceInteger(sourceTokens, argument);
                        valid = parsedMode && *parsedMode >= 0 && *parsedMode <= 2;
                        if (valid)
                            mode = *parsedMode;
                    }
                    const ESM::FormId reference = sourceOwnerId();
                    if (valid && argument == sourceTokens.size() && !reference.isZeroOrUnset()
                        && mReferenceAnimationGroupHandler
                        && mReferenceAnimationGroupHandler(reference, tokens[1], mode))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: PlayGroup reference=" << subject
                                         << " group=" << tokens[1] << " mode=" << mode;
                        continue;
                    }
                }
                else if ((Misc::StringUtils::ciEqual(command, "PlaySound")
                             || Misc::StringUtils::ciEqual(command, "PlaySound3D"))
                    && tokens.size() == 2)
                {
                    const ESM::FormId reference = sourceOwnerId();
                    const ESM::FormId sound = resolveSound(tokens[1]);
                    const bool positional = Misc::StringUtils::ciEqual(command, "PlaySound3D");
                    if (!reference.isZeroOrUnset() && !sound.isZeroOrUnset() && mSoundCommandHandler
                        && mSoundCommandHandler(sound, reference, positional))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: " << command
                                         << " reference=" << subject << " sound=" << tokens[1];
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "SetOwnership")
                    && tokens.size() <= 2)
                {
                    const ESM::FormId reference = sourceOwnerId();
                    std::optional<ESM::FormId> owner;
                    bool valid = !reference.isZeroOrUnset();
                    if (tokens.size() == 2)
                    {
                        const ESM::FormId resolved = resolveOwner(tokens[1]);
                        valid = !resolved.isZeroOrUnset();
                        if (valid)
                            owner = resolved;
                    }
                    if (valid && mReferenceOwnershipHandler
                        && mReferenceOwnershipHandler(reference, owner))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: SetOwnership reference=" << subject
                                         << " owner="
                                         << (owner ? ESM::RefId(*owner).serializeText()
                                                   : std::string("none"));
                        continue;
                    }
                }
                else if ((Misc::StringUtils::ciEqual(command, "AddItem")
                        || Misc::StringUtils::ciEqual(command, "RemoveItem"))
                    && tokens.size() >= 3 && tokens.size() <= 4)
                {
                    const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                    std::size_t argument = 2;
                    const std::optional<std::int32_t> count = sourceInteger(sourceTokens, argument);
                    bool valid = count && *count > 0;
                    if (valid && argument < sourceTokens.size())
                    {
                        const std::optional<std::int32_t> silent = sourceInteger(sourceTokens, argument);
                        valid = silent && (*silent == 0 || *silent == 1);
                    }
                    const ESM::FormId owner = sourceOwnerId();
                    const ESM::FormId item = resolveInventoryItem(tokens[1]);
                    if (valid && argument == sourceTokens.size() && !owner.isZeroOrUnset()
                        && !item.isZeroOrUnset())
                    {
                        const bool add = Misc::StringUtils::ciEqual(command, "AddItem");
                        const bool executed = add
                            ? (mAddItemHandler && mAddItemHandler(owner, item, *count))
                            : (mRemoveItemHandler && mRemoveItemHandler(owner, item, *count));
                        if (executed)
                        {
                            Log(Debug::Info) << "FNV/ESM4 behavior: " << (add ? "AddItem" : "RemoveItem")
                                             << " owner=" << subject << " item=" << tokens[1]
                                             << " count=" << *count;
                            continue;
                        }
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "AddItemHealthPercent")
                    && tokens.size() >= 4)
                {
                    const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                    std::size_t argument = 2;
                    const std::optional<std::int32_t> count = sourceInteger(sourceTokens, argument);
                    const std::optional<float> healthPercent = sourceExpression(sourceTokens, argument);
                    const ESM::FormId owner = sourceOwnerId();
                    const ESM::FormId item = resolveInventoryItem(tokens[1]);
                    if (count && *count > 0 && healthPercent && std::isfinite(*healthPercent)
                        && *healthPercent >= 0.f && *healthPercent <= 1.f
                        && argument == sourceTokens.size() && !owner.isZeroOrUnset()
                        && !item.isZeroOrUnset() && mAddItemHealthPercentHandler
                        && mAddItemHealthPercentHandler(owner, item, *count, *healthPercent))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: AddItemHealthPercent owner=" << subject
                                         << " item=" << tokens[1] << " count=" << *count
                                         << " healthPercent=" << *healthPercent;
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "RemoveAllItems")
                    && tokens.size() >= 1 && tokens.size() <= 4)
                {
                    const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                    const ESM::FormId owner = sourceOwnerId();
                    std::optional<ESM::FormId> destination;
                    std::size_t argument = 1;
                    bool valid = !owner.isZeroOrUnset();
                    if (tokens.size() >= 2)
                    {
                        const ESM::FormId resolved = sourceReferenceId(tokens[1]);
                        valid = !resolved.isZeroOrUnset();
                        if (valid)
                            destination = resolved;
                        argument = 2;
                    }
                    bool retainOwnership = false;
                    if (valid && argument < sourceTokens.size())
                    {
                        const std::optional<std::int32_t> retain = sourceInteger(sourceTokens, argument);
                        valid = retain && (*retain == 0 || *retain == 1);
                        retainOwnership = valid && *retain != 0;
                    }
                    if (valid && argument < sourceTokens.size())
                    {
                        const std::optional<std::int32_t> silent = sourceInteger(sourceTokens, argument);
                        valid = silent && (*silent == 0 || *silent == 1);
                    }
                    if (valid && argument == sourceTokens.size() && mRemoveAllItemsHandler
                        && mRemoveAllItemsHandler(owner, destination, retainOwnership))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: RemoveAllItems owner=" << subject
                                         << " destination="
                                         << (destination ? ESM::RefId(*destination).serializeText()
                                                         : std::string("none"))
                                         << " retainOwnership=" << retainOwnership;
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "RemoveAllTypedItems")
                    && tokens.size() >= 1 && tokens.size() <= 6)
                {
                    const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                    const ESM::FormId owner = sourceOwnerId();
                    std::optional<ESM::FormId> destination;
                    std::optional<ESM::FormId> exceptionList;
                    std::size_t argument = 1;
                    bool valid = !owner.isZeroOrUnset();
                    if (tokens.size() >= 2)
                    {
                        const ESM::FormId resolved = sourceReferenceId(tokens[1]);
                        valid = !resolved.isZeroOrUnset();
                        if (valid)
                            destination = resolved;
                        argument = 2;
                    }
                    bool retainOwnership = false;
                    if (valid && argument < sourceTokens.size())
                    {
                        const std::optional<std::int32_t> retain = sourceInteger(sourceTokens, argument);
                        valid = retain && (*retain == 0 || *retain == 1);
                        retainOwnership = valid && *retain != 0;
                    }
                    if (valid && argument < sourceTokens.size())
                    {
                        const std::optional<std::int32_t> silent = sourceInteger(sourceTokens, argument);
                        valid = silent && (*silent == 0 || *silent == 1);
                    }
                    std::int32_t type = -1;
                    if (valid && argument < sourceTokens.size())
                    {
                        const std::optional<std::int32_t> parsedType = sourceInteger(sourceTokens, argument);
                        valid = parsedType.has_value();
                        if (valid)
                            type = *parsedType;
                    }
                    if (valid && argument < sourceTokens.size())
                    {
                        const ESM::FormId resolved = resolveFormList(sourceTokens[argument++]);
                        valid = !resolved.isZeroOrUnset();
                        if (valid)
                            exceptionList = resolved;
                    }
                    if (valid && argument == sourceTokens.size() && mRemoveAllTypedItemsHandler
                        && mRemoveAllTypedItemsHandler(owner, destination, retainOwnership, type, exceptionList))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: RemoveAllTypedItems owner=" << subject
                                         << " destination="
                                         << (destination ? ESM::RefId(*destination).serializeText()
                                                         : std::string("none"))
                                         << " retainOwnership=" << retainOwnership << " type=" << type
                                         << " exceptionList="
                                         << (exceptionList ? ESM::RefId(*exceptionList).serializeText()
                                                           : std::string("none"));
                        continue;
                    }
                }
                else if ((Misc::StringUtils::ciEqual(command, "EquipItem")
                             || Misc::StringUtils::ciEqual(command, "UnequipItem"))
                    && tokens.size() >= 2 && tokens.size() <= 4)
                {
                    const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                    std::size_t argument = 2;
                    bool valid = true;
                    while (valid && argument < sourceTokens.size())
                    {
                        const std::optional<std::int32_t> flag = sourceInteger(sourceTokens, argument);
                        valid = flag && (*flag == 0 || *flag == 1);
                    }
                    const ESM::FormId actor = sourceOwnerId();
                    const ESM::FormId item = resolveInventoryItem(tokens[1]);
                    const bool equip = Misc::StringUtils::ciEqual(command, "EquipItem");
                    if (valid && argument == sourceTokens.size() && !actor.isZeroOrUnset()
                        && !item.isZeroOrUnset() && mEquipmentCommandHandler
                        && mEquipmentCommandHandler(actor, item, equip))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: "
                                         << (equip ? "EquipItem" : "UnequipItem")
                                         << " actor=" << subject << " item=" << tokens[1];
                        continue;
                    }
                }
                else if ((Misc::StringUtils::ciEqual(command, "AddSpell")
                             || Misc::StringUtils::ciEqual(command, "RemoveSpell"))
                    && tokens.size() == 2)
                {
                    const ESM::FormId actor = sourceOwnerId();
                    const ESM::FormId spell = resolveSpell(tokens[1]);
                    const bool add = Misc::StringUtils::ciEqual(command, "AddSpell");
                    if (!actor.isZeroOrUnset() && !spell.isZeroOrUnset()
                        && mActorEffectCommandHandler
                        && mActorEffectCommandHandler(actor, spell, add))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: "
                                         << (add ? "AddSpell" : "RemoveSpell")
                                         << " actor=" << subject << " spell=" << tokens[1];
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "SetActorFullName")
                    && tokens.size() == 2)
                {
                    const ESM::FormId actor = sourceOwnerId();
                    const ESM::FormId messageId = resolveMessage(tokens[1]);
                    const ESM4::Message* const message = !messageId.isZeroOrUnset() && mStore != nullptr
                        ? mStore->get<ESM4::Message>().search(ESM::RefId(messageId))
                        : nullptr;
                    if (!actor.isZeroOrUnset() && message != nullptr && !message->mFullName.empty()
                        && mActorNameCommandHandler
                        && mActorNameCommandHandler(actor, message->mFullName))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: SetActorFullName actor="
                                         << subject << " message=" << message->mEditorId
                                         << " name=\"" << message->mFullName << "\"";
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "ResetInventory")
                    && tokens.size() == 1)
                {
                    const ESM::FormId actor = sourceOwnerId();
                    if (!actor.isZeroOrUnset() && mActorCommandHandler
                        && mActorCommandHandler(
                            ESM4QuestActorCommand::ResetInventory, actor, ESM::FormId{}, false))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: ResetInventory actor=" << subject;
                        continue;
                    }
                }
                else if ((Misc::StringUtils::ciEqual(command, "SetUnconscious")
                             || Misc::StringUtils::ciEqual(command, "SetRestrained")
                             || Misc::StringUtils::ciEqual(command, "SetPlayerTeammate")
                             || Misc::StringUtils::ciEqual(command, "IgnoreCrime")
                             || Misc::StringUtils::ciEqual(command, "SetGhost")
                             || Misc::StringUtils::ciEqual(command, "SetIgnoreFriendlyHits")
                             || Misc::StringUtils::ciEqual(command, "SetAlert"))
                    && tokens.size() >= 2)
                {
                    const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                    std::size_t argument = 1;
                    const std::optional<std::int32_t> value = sourceInteger(sourceTokens, argument);
                    const std::optional<ESM4QuestActorFlag> flag
                        = actorFlagFromSourceCommand(command);
                    const ESM::FormId actor = sourceOwnerId();
                    if (value && (*value == 0 || *value == 1) && flag
                        && argument == sourceTokens.size() && !actor.isZeroOrUnset()
                        && mActorFlagCommandHandler
                        && mActorFlagCommandHandler(actor, *flag, *value != 0))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: " << command
                                         << " actor=" << subject << " enabled=" << (*value != 0);
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "Look")
                    && tokens.size() >= 2 && tokens.size() <= 3)
                {
                    const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                    std::size_t argument = 2;
                    bool rotateBody = false;
                    bool valid = true;
                    if (argument < sourceTokens.size())
                    {
                        const std::optional<std::int32_t> rotate = sourceInteger(sourceTokens, argument);
                        valid = rotate && (*rotate == 0 || *rotate == 1);
                        rotateBody = valid && *rotate != 0;
                    }
                    const ESM::FormId actor = sourceOwnerId();
                    const ESM::FormId target = sourceReferenceId(tokens[1]);
                    if (valid && argument == sourceTokens.size() && !actor.isZeroOrUnset()
                        && !target.isZeroOrUnset() && mActorCommandHandler
                        && mActorCommandHandler(
                            ESM4QuestActorCommand::Look, actor, target, rotateBody))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: Look actor=" << subject
                                         << " target=" << tokens[1] << " rotateBody=" << rotateBody;
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "StartCombat") && tokens.size() == 2)
                {
                    const ESM::FormId actor = sourceOwnerId();
                    const ESM::FormId target = sourceReferenceId(tokens[1]);
                    if (!actor.isZeroOrUnset() && !target.isZeroOrUnset() && mActorCommandHandler
                        && mActorCommandHandler(
                            ESM4QuestActorCommand::StartCombat, actor, target, false))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: StartCombat actor=" << subject
                                         << " target=" << tokens[1];
                        continue;
                    }
                }
                else if ((Misc::StringUtils::ciEqual(command, "StopCombat") && tokens.size() == 1)
                    || (Misc::StringUtils::ciEqual(command, "ResetHealth") && tokens.size() == 1)
                    || (Misc::StringUtils::ciEqual(command, "StopLook")
                        && (tokens.size() == 1 || tokens.size() == 2)))
                {
                    const ESM::FormId actor = sourceOwnerId();
                    ESM4QuestActorCommand actorCommand = ESM4QuestActorCommand::StopLook;
                    if (Misc::StringUtils::ciEqual(command, "StopCombat"))
                        actorCommand = ESM4QuestActorCommand::StopCombat;
                    else if (Misc::StringUtils::ciEqual(command, "ResetHealth"))
                        actorCommand = ESM4QuestActorCommand::ResetHealth;
                    const ESM::FormId target = tokens.size() == 2
                        ? sourceReferenceId(tokens[1])
                        : ESM::FormId{};
                    if (!actor.isZeroOrUnset() && mActorCommandHandler
                        && (tokens.size() == 1 || !target.isZeroOrUnset())
                        && mActorCommandHandler(actorCommand, actor, target, false))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: " << command
                                         << " actor=" << subject;
                        continue;
                    }
                }
                else if ((Misc::StringUtils::ciEqual(command, "SetAV")
                             || Misc::StringUtils::ciEqual(command, "ModAV")
                             || Misc::StringUtils::ciEqual(command, "RestoreAV"))
                    && tokens.size() >= 3)
                {
                    const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                    std::size_t argument = 2;
                    const std::optional<float> value = sourceExpression(sourceTokens, argument);
                    const ESM::FormId actor = sourceOwnerId();
                    ESM4QuestActorValueCommand actorValueCommand = ESM4QuestActorValueCommand::Set;
                    if (Misc::StringUtils::ciEqual(command, "ModAV"))
                        actorValueCommand = ESM4QuestActorValueCommand::Mod;
                    else if (Misc::StringUtils::ciEqual(command, "RestoreAV"))
                        actorValueCommand = ESM4QuestActorValueCommand::Restore;
                    if (value && std::isfinite(*value) && argument == sourceTokens.size()
                        && !actor.isZeroOrUnset() && mActorValueCommandHandler
                        && mActorValueCommandHandler(actor, actorValueCommand, tokens[1], *value))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: " << command
                                         << " actor=" << subject << " actorValue=" << tokens[1]
                                         << " value=" << *value;
                        continue;
                    }
                }
                else if ((Misc::StringUtils::ciEqual(command, "AddToFaction")
                             || Misc::StringUtils::ciEqual(command, "RemoveFromFaction")
                             || Misc::StringUtils::ciEqual(command, "SetFactionRank"))
                    && tokens.size() >= 2 && tokens.size() <= 3)
                {
                    const bool add = Misc::StringUtils::ciEqual(command, "AddToFaction");
                    const bool setRank = Misc::StringUtils::ciEqual(command, "SetFactionRank");
                    std::optional<int> rank;
                    bool valid = !add && !setRank && tokens.size() == 2;
                    if (add || setRank)
                    {
                        valid = add ? tokens.size() >= 2 : tokens.size() == 3;
                        if (valid)
                        {
                            rank = 0;
                            if (tokens.size() == 3)
                            {
                                const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                                std::size_t argument = 2;
                                const std::optional<std::int32_t> parsed = sourceInteger(sourceTokens, argument);
                                valid = parsed
                                    && *parsed >= std::numeric_limits<std::int8_t>::min()
                                    && *parsed <= std::numeric_limits<std::int8_t>::max()
                                    && argument == sourceTokens.size();
                                if (valid)
                                    rank = setRank && *parsed == -1 ? std::nullopt : std::optional<int>(*parsed);
                            }
                        }
                    }
                    const ESM::FormId actor = sourceOwnerId();
                    const ESM::FormId faction = resolveFaction(tokens[1]);
                    if (valid && !actor.isZeroOrUnset() && !faction.isZeroOrUnset()
                        && mActorFactionCommandHandler
                        && mActorFactionCommandHandler(actor, faction, rank))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: " << command
                                         << " actor=" << subject << " faction=" << tokens[1]
                                         << " rank=" << (rank ? std::to_string(*rank) : std::string("removed"));
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "RewardXP")
                    && Misc::StringUtils::ciEqual(subject, "player") && tokens.size() >= 2)
                {
                    const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                    std::size_t argument = 1;
                    const std::optional<std::int32_t> amount = sourceInteger(sourceTokens, argument);
                    if (amount && *amount > 0 && argument == sourceTokens.size()
                        && mRewardXpHandler && mRewardXpHandler(*amount))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: RewardXP amount=" << *amount;
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "AddReputation")
                    && playerSubject && tokens.size() == 4)
                {
                    const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                    std::size_t argument = 2;
                    const std::optional<std::int32_t> fame = sourceInteger(sourceTokens, argument);
                    const std::optional<std::int32_t> bump = sourceInteger(sourceTokens, argument);
                    const ESM::FormId reputation = resolveReputation(tokens[1]);
                    if (fame && bump && *fame >= 0 && *fame <= 2 && *bump >= 1 && *bump <= 5
                        && argument == sourceTokens.size() && !reputation.isZeroOrUnset()
                        && mAddReputationHandler && mAddReputationHandler(reputation, *fame != 0, *bump))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: AddReputation reputation=" << tokens[1]
                                         << " fame=" << (*fame != 0) << " bump=" << *bump;
                        continue;
                    }
                }
                else if ((Misc::StringUtils::ciEqual(command, "SetReputation")
                             || Misc::StringUtils::ciEqual(command, "AddReputationExact"))
                    && playerSubject && executeSourceReputationValueCommand(command, tokens))
                {
                    continue;
                }
                else if ((Misc::StringUtils::ciEqual(command, "AddNote")
                             || Misc::StringUtils::ciEqual(command, "RemoveNote"))
                    && playerSubject && tokens.size() == 2)
                {
                    const ESM::FormId note = resolveNote(tokens[1]);
                    const bool known = Misc::StringUtils::ciEqual(command, "AddNote");
                    if (!note.isZeroOrUnset() && mNoteHandler && mNoteHandler(note, known))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: " << (known ? "AddNote" : "RemoveNote")
                                         << " note=" << tokens[1];
                        continue;
                    }
                }
                else if ((Misc::StringUtils::ciEqual(command, "AddPerk")
                             || Misc::StringUtils::ciEqual(command, "RemovePerk"))
                    && playerSubject && tokens.size() == 2)
                {
                    const ESM::FormId perk = resolvePerk(tokens[1]);
                    const bool add = Misc::StringUtils::ciEqual(command, "AddPerk");
                    if (!perk.isZeroOrUnset() && mPlayerPerkHandler && mPlayerPerkHandler(perk, add))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: " << (add ? "AddPerk" : "RemovePerk")
                                         << " perk=" << tokens[1];
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "RewardKarma")
                    && playerSubject && tokens.size() >= 2)
                {
                    const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                    std::size_t argument = 1;
                    const std::optional<std::int32_t> amount = sourceInteger(sourceTokens, argument);
                    if (amount && *amount != 0 && argument == sourceTokens.size()
                        && mRewardKarmaHandler && mRewardKarmaHandler(*amount))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: RewardKarma amount=" << *amount;
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "AddSpecialPoints")
                    && playerSubject && tokens.size() >= 2)
                {
                    const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                    std::size_t argument = 1;
                    const std::optional<std::int32_t> amount = sourceInteger(sourceTokens, argument);
                    if (amount && *amount > 0 && argument == sourceTokens.size()
                        && mAddSpecialPointsHandler && mAddSpecialPointsHandler(*amount))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: AddSpecialPoints amount=" << *amount;
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "Unlock") && tokens.size() == 1
                    && executeReferenceCommand(ESM4QuestReferenceCommand::Unlock, subject))
                    continue;
                else if (Misc::StringUtils::ciEqual(command, "Lock") && tokens.size() >= 1)
                {
                    std::optional<int> lockLevel;
                    bool valid = tokens.size() == 1;
                    if (!valid)
                    {
                        const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                        std::size_t argument = 1;
                        const std::optional<std::int32_t> parsed = sourceInteger(sourceTokens, argument);
                        valid = parsed && *parsed >= 0 && *parsed <= 255
                            && argument == sourceTokens.size();
                        if (valid)
                            lockLevel = *parsed;
                    }
                    const ESM::FormId reference = this->resolveReference(subject);
                    if (valid && !reference.isZeroOrUnset() && mLockHandler
                        && mLockHandler(reference, lockLevel))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: Lock reference=" << subject
                                         << " level=" << lockLevel.value_or(100);
                        continue;
                    }
                }
                else if (Misc::StringUtils::ciEqual(command, "Kill")
                    && (tokens.size() == 1
                        || (tokens.size() == 2 && Misc::StringUtils::ciEqual(tokens[1], "player")))
                    && executeReferenceCommand(ESM4QuestReferenceCommand::Kill, subject))
                    continue;
                else if (Misc::StringUtils::ciEqual(command, "ResetAI") && tokens.size() == 1
                    && executeReferenceCommand(ESM4QuestReferenceCommand::ResetAi, subject))
                    continue;
                else if (tokens.size() >= 2 && Misc::StringUtils::ciEqual(command, "MoveTo"))
                {
                    const ESM::FormId reference = sourceOwnerId();
                    const ESM::FormId marker = sourceReferenceId(removeQuotes(tokens[1]));
                    if (tokens.size() == 2 && !reference.isZeroOrUnset() && !marker.isZeroOrUnset()
                        && mMoveToHandler && mMoveToHandler(reference, marker))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: MoveTo reference=" << subject
                                         << " marker=" << removeQuotes(tokens[1]);
                        continue;
                    }

                    Log(Debug::Warning) << "FNV/ESM4 behavior: MoveTo failed reference=" << subject
                                        << " marker=" << removeQuotes(tokens[1]);
                }
                else if (tokens.size() == 2 && Misc::StringUtils::ciEqual(command, "AddScriptPackage")
                    && mStore != nullptr)
                {
                    const std::string packageEditorId = removeQuotes(tokens[1]);
                    const ESM4::AIPackage* package = nullptr;
                    for (const ESM4::AIPackage& candidate : mStore->get<ESM4::AIPackage>())
                        if (Misc::StringUtils::ciEqual(candidate.mEditorId, packageEditorId))
                        {
                            package = &candidate;
                            break;
                        }
                    const ESM::FormId actor = sourceOwnerId();
                    if (!actor.isZeroOrUnset() && package != nullptr && mScriptPackageHandler
                        && mScriptPackageHandler(actor, package->mId))
                    {
                        Log(Debug::Info) << "FNV/ESM4 behavior: AddScriptPackage actor=" << subject
                                         << " package=" << package->mEditorId
                                         << " form=" << ESM::RefId(package->mId);
                        continue;
                    }
                }
                else if (tokens.size() == 1 && Misc::StringUtils::ciEqual(command, "RemoveScriptPackage"))
                {
                    const ESM::FormId actor = sourceOwnerId();
                    if (!actor.isZeroOrUnset() && mScriptPackageHandler
                        && mScriptPackageHandler(actor, std::nullopt))
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
            else if (Misc::StringUtils::ciEqual(tokens[0], "PlaySound") && tokens.size() == 2)
            {
                const ESM::FormId sound = resolveSound(tokens[1]);
                if (!sound.isZeroOrUnset() && mSoundCommandHandler
                    && mSoundCommandHandler(sound, std::nullopt, false))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: PlaySound sound=" << tokens[1];
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "Autosave") && tokens.size() == 1)
            {
                if (mAutosaveHandler && mAutosaveHandler())
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: Autosave";
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "SetCellOwnership")
                && tokens.size() >= 2 && tokens.size() <= 3)
            {
                const ESM::FormId cell = resolveCell(tokens[1]);
                std::optional<ESM::FormId> owner;
                bool valid = !cell.isZeroOrUnset();
                if (tokens.size() == 3)
                {
                    const ESM::FormId resolved = resolveOwner(tokens[2]);
                    valid = !resolved.isZeroOrUnset();
                    if (valid)
                        owner = resolved;
                }
                if (valid && mCellOwnershipHandler && mCellOwnershipHandler(cell, owner))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: SetCellOwnership cell=" << tokens[1]
                                     << " owner="
                                     << (owner ? ESM::RefId(*owner).serializeText()
                                               : std::string("none"));
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "RewardXP") && tokens.size() >= 2)
            {
                const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                std::size_t argument = 1;
                const std::optional<std::int32_t> amount = sourceInteger(sourceTokens, argument);
                if (amount && *amount > 0 && argument == sourceTokens.size()
                    && mRewardXpHandler && mRewardXpHandler(*amount))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: RewardXP amount=" << *amount;
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "AddReputation") && tokens.size() == 4)
            {
                const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                std::size_t argument = 2;
                const std::optional<std::int32_t> fame = sourceInteger(sourceTokens, argument);
                const std::optional<std::int32_t> bump = sourceInteger(sourceTokens, argument);
                const ESM::FormId reputation = resolveReputation(tokens[1]);
                if (fame && bump && *fame >= 0 && *fame <= 2 && *bump >= 1 && *bump <= 5
                    && argument == sourceTokens.size() && !reputation.isZeroOrUnset()
                    && mAddReputationHandler && mAddReputationHandler(reputation, *fame != 0, *bump))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: AddReputation reputation=" << tokens[1]
                                     << " fame=" << (*fame != 0) << " bump=" << *bump;
                    continue;
                }
            }
            else if ((Misc::StringUtils::ciEqual(tokens[0], "SetReputation")
                         || Misc::StringUtils::ciEqual(tokens[0], "AddReputationExact"))
                && executeSourceReputationValueCommand(tokens[0], tokens))
            {
                continue;
            }
            else if ((Misc::StringUtils::ciEqual(tokens[0], "AddNote")
                         || Misc::StringUtils::ciEqual(tokens[0], "RemoveNote"))
                && tokens.size() == 2)
            {
                const ESM::FormId note = resolveNote(tokens[1]);
                const bool known = Misc::StringUtils::ciEqual(tokens[0], "AddNote");
                if (!note.isZeroOrUnset() && mNoteHandler && mNoteHandler(note, known))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: " << (known ? "AddNote" : "RemoveNote")
                                     << " note=" << tokens[1];
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "RewardKarma") && tokens.size() >= 2)
            {
                const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                std::size_t argument = 1;
                const std::optional<std::int32_t> amount = sourceInteger(sourceTokens, argument);
                if (amount && *amount != 0 && argument == sourceTokens.size()
                    && mRewardKarmaHandler && mRewardKarmaHandler(*amount))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: RewardKarma amount=" << *amount;
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "AddSpecialPoints") && tokens.size() >= 2)
            {
                const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                std::size_t argument = 1;
                const std::optional<std::int32_t> amount = sourceInteger(sourceTokens, argument);
                if (amount && *amount > 0 && argument == sourceTokens.size()
                    && mAddSpecialPointsHandler && mAddSpecialPointsHandler(*amount))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: AddSpecialPoints amount=" << *amount;
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "SetQuestObject") && tokens.size() == 3)
            {
                const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                std::size_t argument = 2;
                const std::optional<std::int32_t> value = sourceInteger(sourceTokens, argument);
                const ESM::FormId item = resolveInventoryItem(tokens[1]);
                if (value && (*value == 0 || *value == 1) && argument == sourceTokens.size()
                    && !item.isZeroOrUnset() && mQuestObjectHandler
                    && mQuestObjectHandler(item, *value != 0))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: SetQuestObject item=" << tokens[1]
                                     << " questObject=" << (*value != 0);
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "AddAchievement") && tokens.size() >= 2)
            {
                const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                std::size_t argument = 1;
                const std::optional<std::int32_t> achievement = sourceInteger(sourceTokens, argument);
                if (achievement && *achievement > 0 && argument == sourceTokens.size()
                    && mAchievementHandler && mAchievementHandler(static_cast<std::uint32_t>(*achievement)))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: AddAchievement id=" << *achievement;
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "SetEssential") && tokens.size() == 3)
            {
                const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                std::size_t argument = 2;
                const std::optional<std::int32_t> value = sourceInteger(sourceTokens, argument);
                const ESM::FormId actorBase = resolveActorBase(tokens[1]);
                if (value && (*value == 0 || *value == 1) && argument == sourceTokens.size()
                    && !actorBase.isZeroOrUnset() && mSetEssentialHandler
                    && mSetEssentialHandler(actorBase, *value != 0))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: SetEssential actorBase=" << tokens[1]
                                     << " essential=" << (*value != 0);
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "RemoveAllItems")
                && (ownerActor || ownerReference) && tokens.size() >= 1 && tokens.size() <= 4)
            {
                const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                const ESM::FormId owner = ownerActor ? *ownerActor : *ownerReference;
                std::optional<ESM::FormId> destination;
                std::size_t argument = 1;
                bool valid = true;
                if (tokens.size() >= 2)
                {
                    const ESM::FormId resolved = Misc::StringUtils::ciEqual(tokens[1], "player")
                            || Misc::StringUtils::ciEqual(tokens[1], "playerref")
                        ? ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 }
                        : resolveReference(tokens[1]);
                    valid = !resolved.isZeroOrUnset();
                    if (valid)
                        destination = resolved;
                    argument = 2;
                }
                bool retainOwnership = false;
                if (valid && argument < sourceTokens.size())
                {
                    const std::optional<std::int32_t> retain = sourceInteger(sourceTokens, argument);
                    valid = retain && (*retain == 0 || *retain == 1);
                    retainOwnership = valid && *retain != 0;
                }
                if (valid && argument < sourceTokens.size())
                {
                    const std::optional<std::int32_t> silent = sourceInteger(sourceTokens, argument);
                    valid = silent && (*silent == 0 || *silent == 1);
                }
                if (valid && argument == sourceTokens.size() && mRemoveAllItemsHandler
                    && mRemoveAllItemsHandler(owner, destination, retainOwnership))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: RemoveAllItems owning actor="
                                     << ESM::RefId(owner).serializeText() << " destination="
                                     << (destination ? ESM::RefId(*destination).serializeText()
                                                     : std::string("none"))
                                     << " retainOwnership=" << retainOwnership;
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "RemoveAllTypedItems")
                && (ownerActor || ownerReference) && tokens.size() >= 1 && tokens.size() <= 6)
            {
                const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                const ESM::FormId owner = ownerActor ? *ownerActor : *ownerReference;
                std::optional<ESM::FormId> destination;
                std::optional<ESM::FormId> exceptionList;
                std::size_t argument = 1;
                bool valid = true;
                if (tokens.size() >= 2)
                {
                    const ESM::FormId resolved = Misc::StringUtils::ciEqual(tokens[1], "player")
                            || Misc::StringUtils::ciEqual(tokens[1], "playerref")
                        ? ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 }
                        : resolveReference(tokens[1]);
                    valid = !resolved.isZeroOrUnset();
                    if (valid)
                        destination = resolved;
                    argument = 2;
                }
                bool retainOwnership = false;
                if (valid && argument < sourceTokens.size())
                {
                    const std::optional<std::int32_t> retain = sourceInteger(sourceTokens, argument);
                    valid = retain && (*retain == 0 || *retain == 1);
                    retainOwnership = valid && *retain != 0;
                }
                if (valid && argument < sourceTokens.size())
                {
                    const std::optional<std::int32_t> silent = sourceInteger(sourceTokens, argument);
                    valid = silent && (*silent == 0 || *silent == 1);
                }
                std::int32_t type = -1;
                if (valid && argument < sourceTokens.size())
                {
                    const std::optional<std::int32_t> parsedType = sourceInteger(sourceTokens, argument);
                    valid = parsedType.has_value();
                    if (valid)
                        type = *parsedType;
                }
                if (valid && argument < sourceTokens.size())
                {
                    const ESM::FormId resolved = resolveFormList(sourceTokens[argument++]);
                    valid = !resolved.isZeroOrUnset();
                    if (valid)
                        exceptionList = resolved;
                }
                if (valid && argument == sourceTokens.size() && mRemoveAllTypedItemsHandler
                    && mRemoveAllTypedItemsHandler(owner, destination, retainOwnership, type, exceptionList))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: RemoveAllTypedItems owning actor="
                                     << ESM::RefId(owner).serializeText() << " destination="
                                     << (destination ? ESM::RefId(*destination).serializeText()
                                                     : std::string("none"))
                                     << " retainOwnership=" << retainOwnership << " type=" << type
                                     << " exceptionList="
                                     << (exceptionList ? ESM::RefId(*exceptionList).serializeText()
                                                       : std::string("none"));
                    continue;
                }
            }
            else if ((Misc::StringUtils::ciEqual(tokens[0], "AddSpell")
                         || Misc::StringUtils::ciEqual(tokens[0], "RemoveSpell"))
                && tokens.size() == 2)
            {
                // Quest-stage cure scripts use bare RemoveSpell for the player, while actor/reference scripts use
                // their normal implicit owner. This is the native implicit-reference split, not a quest ID rule.
                const ESM::FormId actor = ownerActor ? *ownerActor
                    : ownerReference ? *ownerReference
                                     : ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 };
                const ESM::FormId spell = resolveSpell(tokens[1]);
                const bool add = Misc::StringUtils::ciEqual(tokens[0], "AddSpell");
                if (!spell.isZeroOrUnset() && mActorEffectCommandHandler
                    && mActorEffectCommandHandler(actor, spell, add))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: "
                                     << (add ? "AddSpell" : "RemoveSpell")
                                     << " implicit actor=" << ESM::RefId(actor).serializeText()
                                     << " spell=" << tokens[1];
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "SetActorFullName")
                && tokens.size() == 2)
            {
                const ESM::FormId actor = ownerActor ? *ownerActor
                    : ownerReference ? *ownerReference
                                     : ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 };
                const ESM::FormId messageId = resolveMessage(tokens[1]);
                const ESM4::Message* const message = !messageId.isZeroOrUnset() && mStore != nullptr
                    ? mStore->get<ESM4::Message>().search(ESM::RefId(messageId))
                    : nullptr;
                if (message != nullptr && !message->mFullName.empty() && mActorNameCommandHandler
                    && mActorNameCommandHandler(actor, message->mFullName))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: SetActorFullName implicit actor="
                                     << ESM::RefId(actor).serializeText() << " message=" << message->mEditorId
                                     << " name=\"" << message->mFullName << "\"";
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "ResetInventory")
                && (ownerActor || ownerReference) && tokens.size() == 1)
            {
                const ESM::FormId actor = ownerActor ? *ownerActor : *ownerReference;
                if (mActorCommandHandler
                    && mActorCommandHandler(
                        ESM4QuestActorCommand::ResetInventory, actor, ESM::FormId{}, false))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: ResetInventory owning actor="
                                     << ESM::RefId(actor).serializeText();
                    continue;
                }
            }
            else if ((Misc::StringUtils::ciEqual(tokens[0], "EquipItem")
                         || Misc::StringUtils::ciEqual(tokens[0], "UnequipItem"))
                && (ownerActor || ownerReference) && tokens.size() >= 2 && tokens.size() <= 4)
            {
                const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                std::size_t argument = 2;
                bool valid = true;
                while (valid && argument < sourceTokens.size())
                {
                    const std::optional<std::int32_t> flag = sourceInteger(sourceTokens, argument);
                    valid = flag && (*flag == 0 || *flag == 1);
                }
                const ESM::FormId actor = ownerActor ? *ownerActor : *ownerReference;
                const ESM::FormId item = resolveInventoryItem(tokens[1]);
                const bool equip = Misc::StringUtils::ciEqual(tokens[0], "EquipItem");
                if (valid && argument == sourceTokens.size() && !item.isZeroOrUnset()
                    && mEquipmentCommandHandler
                    && mEquipmentCommandHandler(actor, item, equip))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: "
                                     << (equip ? "EquipItem" : "UnequipItem")
                                     << " owning actor=" << ESM::RefId(actor).serializeText()
                                     << " item=" << tokens[1];
                    continue;
                }
            }
            else if ((Misc::StringUtils::ciEqual(tokens[0], "SetUnconscious")
                          || Misc::StringUtils::ciEqual(tokens[0], "SetRestrained")
                          || Misc::StringUtils::ciEqual(tokens[0], "SetPlayerTeammate")
                          || Misc::StringUtils::ciEqual(tokens[0], "IgnoreCrime")
                          || Misc::StringUtils::ciEqual(tokens[0], "SetGhost")
                          || Misc::StringUtils::ciEqual(tokens[0], "SetIgnoreFriendlyHits")
                          || Misc::StringUtils::ciEqual(tokens[0], "SetAlert"))
                && (ownerActor || ownerReference) && tokens.size() >= 2)
            {
                const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                std::size_t argument = 1;
                const std::optional<std::int32_t> value = sourceInteger(sourceTokens, argument);
                const std::optional<ESM4QuestActorFlag> flag
                    = actorFlagFromSourceCommand(tokens[0]);
                const ESM::FormId actor = ownerActor ? *ownerActor : *ownerReference;
                if (value && (*value == 0 || *value == 1) && flag
                    && argument == sourceTokens.size() && mActorFlagCommandHandler
                    && mActorFlagCommandHandler(actor, *flag, *value != 0))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: " << tokens[0]
                                     << " owning actor=" << ESM::RefId(actor).serializeText()
                                     << " enabled=" << (*value != 0);
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "Look")
                && (ownerActor || ownerReference) && tokens.size() >= 2 && tokens.size() <= 3)
            {
                const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                std::size_t argument = 2;
                bool rotateBody = false;
                bool valid = true;
                if (argument < sourceTokens.size())
                {
                    const std::optional<std::int32_t> rotate = sourceInteger(sourceTokens, argument);
                    valid = rotate && (*rotate == 0 || *rotate == 1);
                    rotateBody = valid && *rotate != 0;
                }
                const ESM::FormId actor = ownerActor ? *ownerActor : *ownerReference;
                const ESM::FormId target = Misc::StringUtils::ciEqual(tokens[1], "player")
                        || Misc::StringUtils::ciEqual(tokens[1], "playerref")
                    ? ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 }
                    : resolveReference(tokens[1]);
                if (valid && argument == sourceTokens.size() && !target.isZeroOrUnset()
                    && mActorCommandHandler
                    && mActorCommandHandler(ESM4QuestActorCommand::Look, actor, target, rotateBody))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: Look owning actor="
                                     << ESM::RefId(actor).serializeText() << " target=" << tokens[1]
                                     << " rotateBody=" << rotateBody;
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "StartCombat")
                && (ownerActor || ownerReference) && tokens.size() == 2)
            {
                const ESM::FormId actor = ownerActor ? *ownerActor : *ownerReference;
                const ESM::FormId target = Misc::StringUtils::ciEqual(tokens[1], "player")
                        || Misc::StringUtils::ciEqual(tokens[1], "playerref")
                    ? ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 }
                    : resolveReference(tokens[1]);
                if (!target.isZeroOrUnset() && mActorCommandHandler
                    && mActorCommandHandler(ESM4QuestActorCommand::StartCombat, actor, target, false))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: StartCombat owning actor="
                                     << ESM::RefId(actor).serializeText() << " target=" << tokens[1];
                    continue;
                }
            }
            else if (((Misc::StringUtils::ciEqual(tokens[0], "StopCombat") && tokens.size() == 1)
                         || (Misc::StringUtils::ciEqual(tokens[0], "ResetHealth") && tokens.size() == 1)
                         || (Misc::StringUtils::ciEqual(tokens[0], "StopLook")
                              && (tokens.size() == 1 || tokens.size() == 2)))
                && (ownerActor || ownerReference))
            {
                const ESM::FormId actor = ownerActor ? *ownerActor : *ownerReference;
                ESM4QuestActorCommand command = ESM4QuestActorCommand::StopLook;
                if (Misc::StringUtils::ciEqual(tokens[0], "StopCombat"))
                    command = ESM4QuestActorCommand::StopCombat;
                else if (Misc::StringUtils::ciEqual(tokens[0], "ResetHealth"))
                    command = ESM4QuestActorCommand::ResetHealth;
                const ESM::FormId target = tokens.size() == 2
                    ? (Misc::StringUtils::ciEqual(tokens[1], "player")
                            || Misc::StringUtils::ciEqual(tokens[1], "playerref")
                        ? ESM::FormId{ .mIndex = 0x14, .mContentFile = 0 }
                        : resolveReference(tokens[1]))
                    : ESM::FormId{};
                if ((tokens.size() == 1 || !target.isZeroOrUnset())
                    && mActorCommandHandler && mActorCommandHandler(command, actor, target, false))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: " << tokens[0]
                                     << " owning actor=" << ESM::RefId(actor).serializeText();
                    continue;
                }
            }
            else if ((Misc::StringUtils::ciEqual(tokens[0], "SetAV")
                         || Misc::StringUtils::ciEqual(tokens[0], "ModAV")
                         || Misc::StringUtils::ciEqual(tokens[0], "RestoreAV"))
                && (ownerActor || ownerReference) && tokens.size() >= 3)
            {
                const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                std::size_t argument = 2;
                const std::optional<float> value = sourceExpression(sourceTokens, argument);
                ESM4QuestActorValueCommand command = ESM4QuestActorValueCommand::Set;
                if (Misc::StringUtils::ciEqual(tokens[0], "ModAV"))
                    command = ESM4QuestActorValueCommand::Mod;
                else if (Misc::StringUtils::ciEqual(tokens[0], "RestoreAV"))
                    command = ESM4QuestActorValueCommand::Restore;
                const ESM::FormId actor = ownerActor ? *ownerActor : *ownerReference;
                if (value && std::isfinite(*value) && argument == sourceTokens.size()
                    && mActorValueCommandHandler
                    && mActorValueCommandHandler(actor, command, tokens[1], *value))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: " << tokens[0]
                                     << " owning actor=" << ESM::RefId(actor).serializeText()
                                     << " actorValue=" << tokens[1] << " value=" << *value;
                    continue;
                }
            }
            else if ((Misc::StringUtils::ciEqual(tokens[0], "AddToFaction")
                         || Misc::StringUtils::ciEqual(tokens[0], "RemoveFromFaction")
                         || Misc::StringUtils::ciEqual(tokens[0], "SetFactionRank"))
                && (ownerActor || ownerReference) && tokens.size() >= 2 && tokens.size() <= 3)
            {
                const bool add = Misc::StringUtils::ciEqual(tokens[0], "AddToFaction");
                const bool setRank = Misc::StringUtils::ciEqual(tokens[0], "SetFactionRank");
                std::optional<int> rank;
                bool valid = !add && !setRank && tokens.size() == 2;
                if (add || setRank)
                {
                    valid = add ? tokens.size() >= 2 : tokens.size() == 3;
                    if (valid)
                    {
                        rank = 0;
                        if (tokens.size() == 3)
                        {
                            const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                            std::size_t argument = 2;
                            const std::optional<std::int32_t> parsed = sourceInteger(sourceTokens, argument);
                            valid = parsed
                                && *parsed >= std::numeric_limits<std::int8_t>::min()
                                && *parsed <= std::numeric_limits<std::int8_t>::max()
                                && argument == sourceTokens.size();
                            if (valid)
                                rank = setRank && *parsed == -1 ? std::nullopt : std::optional<int>(*parsed);
                        }
                    }
                }
                const ESM::FormId actor = ownerActor ? *ownerActor : *ownerReference;
                const ESM::FormId faction = resolveFaction(tokens[1]);
                if (valid && !faction.isZeroOrUnset() && mActorFactionCommandHandler
                    && mActorFactionCommandHandler(actor, faction, rank))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: " << tokens[0]
                                     << " owning actor=" << ESM::RefId(actor).serializeText()
                                     << " faction=" << tokens[1]
                                     << " rank=" << (rank ? std::to_string(*rank) : std::string("removed"));
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "Lock") && ownerReference
                && tokens.size() >= 1)
            {
                std::optional<int> lockLevel;
                bool valid = tokens.size() == 1;
                if (!valid)
                {
                    const SourceTokens sourceTokens = normaliseSourceTokens(tokens);
                    std::size_t argument = 1;
                    const std::optional<std::int32_t> parsed = sourceInteger(sourceTokens, argument);
                    valid = parsed && *parsed >= 0 && *parsed <= 255
                        && argument == sourceTokens.size();
                    if (valid)
                        lockLevel = *parsed;
                }
                if (valid && mLockHandler && mLockHandler(*ownerReference, lockLevel))
                {
                    Log(Debug::Info) << "FNV/ESM4 behavior: Lock owning reference="
                                     << ESM::RefId(*ownerReference).serializeText()
                                     << " level=" << lockLevel.value_or(100);
                    continue;
                }
            }
            else if (Misc::StringUtils::ciEqual(tokens[0], "CompleteAllObjectives") && tokens.size() == 2)
            {
                const ESM4::Quest* const quest = resolveQuest(tokens[1]);
                ESM4QuestState* const state = quest != nullptr ? findState(*quest) : nullptr;
                if (state != nullptr)
                {
                    for (auto& objective : state->mObjectiveStatus)
                        objective.second |= ESM4QuestState::Objective_Completed;
                    Log(Debug::Info) << "FNV/ESM4 behavior: CompleteAllObjectives quest="
                                     << quest->mEditorId << " count=" << state->mObjectiveStatus.size();
                    continue;
                }
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
            else if ((tokens.size() == 3 || tokens.size() == 5)
                && Misc::StringUtils::ciEqual(tokens[0], "SetEnemy"))
            {
                const ESM::FormId first = resolveFaction(tokens[1]);
                const ESM::FormId second = resolveFaction(tokens[2]);
                std::int32_t firstNeutral = 0;
                std::int32_t secondNeutral = 0;
                const bool flagsValid = tokens.size() == 3
                    || (parseInt(tokens[3], firstNeutral) && parseInt(tokens[4], secondNeutral)
                        && (firstNeutral == 0 || firstNeutral == 1)
                        && (secondNeutral == 0 || secondNeutral == 1));
                if (flagsValid && !first.isZeroOrUnset() && !second.isZeroOrUnset() && first != second
                    && mSetEnemyHandler
                    && mSetEnemyHandler(first, second, firstNeutral != 0, secondNeutral != 0))
                {
                    if (ownerState != nullptr)
                        recordEnemyRelation(
                            *ownerState, first, second, firstNeutral != 0, secondNeutral != 0);
                    continue;
                }
            }
            else if ((tokens.size() == 2 || tokens.size() == 3)
                && Misc::StringUtils::ciEqual(tokens[0], "ShowMap"))
            {
                std::string_view markerToken = tokens[1];
                if (!markerToken.empty() && markerToken.back() == ',')
                    markerToken.remove_suffix(1);
                std::int32_t canTravel = 0;
                const bool travelValid = tokens.size() == 2
                    || (parseInt(tokens[2], canTravel) && (canTravel == 0 || canTravel == 1));
                const ESM::FormId marker = resolveReference(markerToken);
                if (travelValid && !marker.isZeroOrUnset() && mShowMapHandler
                    && mShowMapHandler(marker, canTravel != 0))
                    continue;
            }
            else if (tokens.size() >= 2 && tokens.size() <= 4
                && Misc::StringUtils::ciEqual(tokens[0], "EnableFastTravel"))
            {
                std::array<std::int32_t, 3> values{ 0, 1, 0 };
                bool valid = true;
                for (std::size_t index = 1; index < tokens.size(); ++index)
                    valid = valid && parseInt(tokens[index], values[index - 1])
                        && (values[index - 1] == 0 || values[index - 1] == 1);
                if (valid && mEnableFastTravelHandler
                    && mEnableFastTravelHandler(values[0] != 0, values[1] != 0, values[2] != 0))
                    continue;
            }
            else if (tokens.size() == 3 && Misc::StringUtils::ciEqual(tokens[0], "SetAlly"))
            {
                const ESM::FormId first = resolveFaction(tokens[1]);
                const ESM::FormId second = resolveFaction(tokens[2]);
                if (!first.isZeroOrUnset() && !second.isZeroOrUnset() && first != second && mSetAllyHandler
                    && mSetAllyHandler(first, second))
                {
                    if (ownerState != nullptr)
                        recordAllyPair(*ownerState, first, second);
                    continue;
                }
            }
            else
            {
                const std::size_t separator = tokens[0].rfind('.');
                if (separator != std::string_view::npos && separator != 0 && separator + 1 < tokens[0].size())
                {
                    const std::string_view target = tokens[0].substr(0, separator);
                    const std::string_view command = tokens[0].substr(separator + 1);
                    if (Misc::StringUtils::ciEqual(command, "EnableFastTravel")
                        && Misc::StringUtils::ciEqual(target, "Player")
                        && tokens.size() >= 2 && tokens.size() <= 4)
                    {
                        std::array<std::int32_t, 3> values{ 0, 1, 0 };
                        bool valid = true;
                        for (std::size_t index = 1; index < tokens.size(); ++index)
                            valid = valid && parseInt(tokens[index], values[index - 1])
                                && (values[index - 1] == 0 || values[index - 1] == 1);
                        if (valid && mEnableFastTravelHandler
                            && mEnableFastTravelHandler(
                                values[0] != 0, values[1] != 0, values[2] != 0))
                            continue;
                    }
                    if (Misc::StringUtils::ciEqual(command, "Enable")
                        && executeReferenceCommand(ESM4QuestReferenceCommand::Enable, target))
                        continue;
                    if (Misc::StringUtils::ciEqual(command, "Disable")
                        && executeReferenceCommand(ESM4QuestReferenceCommand::Disable, target))
                        continue;
                    if (Misc::StringUtils::ciEqual(command, "SetDestroyed") && tokens.size() == 2)
                    {
                        std::int32_t destroyed = 0;
                        const ESM::FormId reference = resolveReference(target);
                        if (parseInt(tokens[1], destroyed) && (destroyed == 0 || destroyed == 1)
                            && !reference.isZeroOrUnset() && mSetDestroyedHandler
                            && mSetDestroyedHandler(reference, destroyed != 0))
                            continue;
                    }
                    if ((Misc::StringUtils::ciEqual(command, "evp")
                            || Misc::StringUtils::ciEqual(command, "EvaluatePackage"))
                        && executeReferenceCommand(ESM4QuestReferenceCommand::EvaluatePackage, target))
                        continue;
                }
            }
            mUnsupportedStageCommands.emplace_back(line);
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
