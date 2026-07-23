#include "esm4questruntime.hpp"

#include "esmstore.hpp"
#include "globals.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loaddial.hpp>
#include <components/esm4/loadglob.hpp>
#include <components/esm4/loadmesg.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadscpt.hpp>
#include <components/misc/strings/algorithm.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

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
            const std::size_t end = line.find_first_of(" \t\r");
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

    std::string_view trim(std::string_view value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.remove_prefix(1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.remove_suffix(1);
        return value;
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
                for (const ESM4::ScriptLocalVariableData& variable : script->mScript.localVarData)
                    if (!variable.variableName.empty())
                        state.mVariables.emplace(Misc::StringUtils::lowerCase(variable.variableName), 0.f);
            mStates.insert_or_assign(quest.mId, std::move(state));
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
        mReferenceIds.clear();
    }

    bool ESM4QuestRuntime::loadSavedProgress(const ESM4SavedQuestProgress& progress, std::string* error)
    {
        const auto fail = [error](std::string message) {
            if (error != nullptr)
                *error = std::move(message);
            return false;
        };
        if (mStore == nullptr)
            return fail("quest runtime is not initialized");

        QuestStateMap states = mStates;
        std::optional<ESM::FormId> activeQuest;
        std::set<ESM::FormId> savedStateIds;
        for (const ESM4SavedQuestProgress::State& saved : progress.mStates)
        {
            const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(saved.mQuest));
            const auto state = states.find(saved.mQuest);
            if (quest == nullptr || state == states.end())
                return fail("saved quest state references a missing QUST " + ESM::RefId(saved.mQuest).serializeText());
            if (!savedStateIds.insert(saved.mQuest).second)
                return fail("saved quest state contains a duplicate QUST identity");
            if ((saved.mFlags & 0x80u) != 0)
                return fail("saved quest state contains an unsupported runtime flag");
            state->second.mFlags = saved.mFlags;
        }

        for (const ESM4SavedQuestProgress::Stage& saved : progress.mStages)
        {
            const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(saved.mQuest));
            const auto state = states.find(saved.mQuest);
            if (quest == nullptr || state == states.end())
                return fail("saved quest stage references a missing QUST " + ESM::RefId(saved.mQuest).serializeText());
            const auto stage = state->second.mStageDone.find(saved.mStage);
            if (stage == state->second.mStageDone.end())
                return fail("saved quest stage index is absent from loaded QUST " + quest->mEditorId);

            stage->second = saved.mDone;
            if (saved.mDone)
            {
                state->second.mCurrentStage = std::max(state->second.mCurrentStage, saved.mStage);
                state->second.mFlags |= ESM4QuestState::Flag_Running | ESM4QuestState::Flag_ShownInPipBoy;
            }
        }

        for (const ESM4SavedQuestProgress::Objective& saved : progress.mObjectives)
        {
            const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(saved.mQuest));
            const auto state = states.find(saved.mQuest);
            if (quest == nullptr || state == states.end())
            {
                return fail(
                    "saved quest objective references a missing QUST " + ESM::RefId(saved.mQuest).serializeText());
            }
            const auto objective = state->second.mObjectiveStatus.find(saved.mObjective);
            if (objective == state->second.mObjectiveStatus.end())
                return fail("saved quest objective index is absent from loaded QUST " + quest->mEditorId);
            if ((saved.mStatus
                    & ~(ESM4QuestState::Objective_Displayed | ESM4QuestState::Objective_Completed))
                != 0)
            {
                return fail("saved quest objective contains unsupported status bits in " + quest->mEditorId);
            }

            objective->second = saved.mStatus;
            if (saved.mStatus != 0)
                state->second.mFlags |= ESM4QuestState::Flag_Running | ESM4QuestState::Flag_ShownInPipBoy;
        }

        std::set<std::pair<ESM::FormId, std::uint32_t>> savedVariableIds;
        for (const ESM4SavedQuestProgress::Variable& saved : progress.mVariables)
        {
            const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(saved.mQuest));
            const auto state = states.find(saved.mQuest);
            if (quest == nullptr || state == states.end())
                return fail("saved quest variable references a missing QUST " + ESM::RefId(saved.mQuest).serializeText());
            if (!savedVariableIds.emplace(saved.mQuest, saved.mIndex).second)
                return fail("saved quest variables contain a duplicate QUST/local index");
            const ESM4::Script* script = mStore->get<ESM4::Script>().search(ESM::RefId(quest->mQuestScript));
            if (script == nullptr)
                return fail("saved quest variable references a QUST without a loaded quest script " + quest->mEditorId);
            const auto variable = std::ranges::find(script->mScript.localVarData, saved.mIndex,
                &ESM4::ScriptLocalVariableData::index);
            if (variable == script->mScript.localVarData.end() || variable->variableName.empty())
                return fail("saved quest variable index is absent from the loaded quest script " + quest->mEditorId);
            const std::string name = Misc::StringUtils::lowerCase(variable->variableName);
            const auto destination = state->second.mVariables.find(name);
            if (destination == state->second.mVariables.end())
                return fail("saved quest variable has no initialized runtime slot " + quest->mEditorId + "." + name);
            destination->second = saved.mValue;
        }

        if (progress.mActiveQuest)
        {
            const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(*progress.mActiveQuest));
            const auto state = states.find(*progress.mActiveQuest);
            if (quest == nullptr || state == states.end())
            {
                return fail(
                    "saved active quest references a missing QUST " + ESM::RefId(*progress.mActiveQuest).serializeText());
            }
            state->second.mFlags |= ESM4QuestState::Flag_Running | ESM4QuestState::Flag_ShownInPipBoy;
            activeQuest = *progress.mActiveQuest;
        }

        mStates = std::move(states);
        mActiveQuest = activeQuest;
        Log(Debug::Info) << "FNV/ESM4 save: imported quest progress stages=" << progress.mStages.size()
                         << " objectives=" << progress.mObjectives.size() << " variables="
                         << progress.mVariables.size() << " states=" << progress.mStates.size() << " active="
                         << (mActiveQuest ? ESM::RefId(*mActiveQuest).serializeText() : std::string("<none>"));
        return true;
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

        for (const ESM4::ScriptBytecodeInstruction& instruction : instructions)
        {
            if (instruction.callingReferenceIndex
                && (*instruction.callingReferenceIndex == 0
                    || *instruction.callingReferenceIndex > script.references.size()))
                return false;

            if (instruction.opcode != 0x0015 && instruction.opcode != 0x1034 && instruction.opcode != 0x1036
                && instruction.opcode != 0x1037
                && instruction.opcode != 0x1039 && instruction.opcode != 0x1059 && instruction.opcode != 0x105e
                && instruction.opcode != 0x1071 && instruction.opcode != 0x11a2 && instruction.opcode != 0x11a3
                && instruction.opcode != 0x11dd)
            {
                prepared.mUseSourceFallback = true;
                prepared.mUnsupportedOpcodes.push_back(instruction.opcode);
                continue;
            }
            std::span<const std::uint8_t> argumentPayload = instruction.arguments;
            if (instruction.opcode == 0x0015) // set Quest.local to literal
            {
                if (instruction.callingReferenceIndex || argumentPayload.size() < 8 || argumentPayload[0] != 0x72
                    || (argumentPayload[3] != 0x66 && argumentPayload[3] != 0x73) || mStore == nullptr)
                    return false;
                const auto readUint16 = [](std::span<const std::uint8_t> bytes, std::size_t offset) {
                    return static_cast<std::uint16_t>(
                        bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
                };
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

                float value = 0.f;
                const std::string_view expression(
                    reinterpret_cast<const char*>(argumentPayload.data() + 8), expressionSize);
                if (!parseFloat(trim(expression), value))
                {
                    prepared.mUseSourceFallback = true;
                    prepared.mUnsupportedOpcodes.push_back(instruction.opcode);
                    continue;
                }
                CompiledQuestCommand command;
                command.mType = CompiledQuestCommandType::SetVariable;
                command.mQuest = questId;
                command.mVariable = Misc::StringUtils::lowerCase(variable->variableName);
                command.mNumber = value;
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
            // Zero-argument reference functions such as EVP have an empty frame rather
            // than the two-byte argument count used by ordinary command functions.
            if (!(instruction.opcode == 0x105e && argumentPayload.empty())
                && !ESM4::decodeFalloutScriptArguments(argumentPayload, script.references, arguments).succeeded())
                return false;

            if (instruction.opcode == 0x1034) // reference.SayTo listener topic
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
        if (command.mType == CompiledQuestCommandType::EvaluatePackage
            || command.mType == CompiledQuestCommandType::ShowMessage
            || command.mType == CompiledQuestCommandType::SayTo)
        {
            working.mExternalEffects.push_back({ command.mType, command.mQuest, command.mTarget, command.mTopic });
            return true;
        }

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
            case CompiledQuestCommandType::SetVariable:
            {
                const auto variable = state.mVariables.find(command.mVariable);
                if (variable == state.mVariables.end())
                    return false;
                variable->second = command.mNumber;
                return true;
            }
            case CompiledQuestCommandType::EvaluatePackage:
            case CompiledQuestCommandType::ShowMessage:
            case CompiledQuestCommandType::SayTo:
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
                case CompiledQuestCommandType::ShowMessage:
                    command = "ShowMessage ";
                    executed = mMessageHandler && mMessageHandler(effect.mTarget);
                    break;
                case CompiledQuestCommandType::SayTo:
                    command = "SayTo ";
                    executed = mSayToHandler && mSayToHandler(effect.mTarget, effect.mListener, effect.mTopic);
                    break;
                default:
                    throw std::logic_error("non-external command queued as a Fallout quest external effect");
            }
            command += ESM::RefId(effect.mTarget).serializeText();
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
        if (stage == quest->mStages.end())
            return false;

        const bool repeatedStages = (state->mFlags & ESM4QuestState::Flag_AllowRepeatedStages) != 0;
        if (state->mStageDone[stage->mIndex] && !repeatedStages)
            return true;
        if (stageContainsCompiledSetStage(*stage))
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
                executeStageSource(entry.mScript.scriptSource);
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
                        case CompiledQuestCommandType::SetVariable:
                        {
                            const ESM4::Quest* targetQuest
                                = mStore->get<ESM4::Quest>().search(ESM::RefId(command.mQuest));
                            executed = targetQuest != nullptr
                                && setQuestVariable(targetQuest->mEditorId, command.mVariable, command.mNumber);
                            break;
                        }
                        case CompiledQuestCommandType::EvaluatePackage:
                            executed = mReferenceCommandHandler
                                && mReferenceCommandHandler(ESM4QuestReferenceCommand::EvaluatePackage, command.mQuest);
                            break;
                        case CompiledQuestCommandType::ShowMessage:
                            executed = mMessageHandler && mMessageHandler(command.mQuest);
                            break;
                        case CompiledQuestCommandType::SayTo:
                            executed = mSayToHandler
                                && mSayToHandler(command.mQuest, command.mTarget, command.mTopic);
                            break;
                    }
                    if (!executed)
                    {
                        if (command.mType == CompiledQuestCommandType::EvaluatePackage
                            || command.mType == CompiledQuestCommandType::ShowMessage
                            || command.mType == CompiledQuestCommandType::SayTo)
                        {
                            std::string failure;
                            if (command.mType == CompiledQuestCommandType::EvaluatePackage)
                                failure = "EvaluatePackage " + ESM::RefId(command.mQuest).serializeText();
                            else if (command.mType == CompiledQuestCommandType::ShowMessage)
                                failure = "ShowMessage " + ESM::RefId(command.mQuest).serializeText();
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

    std::optional<bool> ESM4QuestRuntime::evaluateResultCondition(std::string_view expression) const
    {
        expression = trim(expression);
        while (expression.size() >= 2 && expression.front() == '(' && expression.back() == ')')
            expression = trim(expression.substr(1, expression.size() - 2));

        const std::vector<std::string_view> tokens = tokenize(expression);
        if (tokens.empty())
            return std::nullopt;

        const auto compare = [&tokens](float actual, std::size_t operatorIndex) -> std::optional<bool> {
            if (tokens.size() == operatorIndex)
                return actual != 0.f;
            if (tokens.size() != operatorIndex + 2)
                return std::nullopt;

            float expected = 0.f;
            if (!parseFloat(tokens[operatorIndex + 1], expected))
                return std::nullopt;
            const std::string_view op = tokens[operatorIndex];
            if (op == "==")
                return actual == expected;
            if (op == "!=")
                return actual != expected;
            if (op == ">")
                return actual > expected;
            if (op == ">=")
                return actual >= expected;
            if (op == "<")
                return actual < expected;
            if (op == "<=")
                return actual <= expected;
            return std::nullopt;
        };
        const auto questState = [this](std::string_view id) -> const ESM4QuestState* {
            const ESM4::Quest* quest = resolveQuest(id);
            return quest != nullptr ? findState(*quest) : nullptr;
        };

        if (Misc::StringUtils::ciEqual(tokens[0], "GetQuestRunning") && tokens.size() >= 2)
        {
            if (const ESM4QuestState* state = questState(tokens[1]))
                return compare((state->mFlags & ESM4QuestState::Flag_Running) != 0 ? 1.f : 0.f, 2);
            return std::nullopt;
        }
        if (Misc::StringUtils::ciEqual(tokens[0], "GetQuestCompleted") && tokens.size() >= 2)
        {
            if (const ESM4QuestState* state = questState(tokens[1]))
                return compare((state->mFlags & ESM4QuestState::Flag_Completed) != 0 ? 1.f : 0.f, 2);
            return std::nullopt;
        }
        if (Misc::StringUtils::ciEqual(tokens[0], "GetStage") && tokens.size() >= 2)
        {
            if (const ESM4QuestState* state = questState(tokens[1]))
                return compare(static_cast<float>(state->mCurrentStage), 2);
            return std::nullopt;
        }
        if (Misc::StringUtils::ciEqual(tokens[0], "GetStageDone") && tokens.size() >= 3)
        {
            std::int32_t stage = 0;
            const ESM4QuestState* state = questState(tokens[1]);
            if (state == nullptr || !parseInt(tokens[2], stage))
                return std::nullopt;
            const auto found = state->mStageDone.find(static_cast<std::int16_t>(stage));
            return compare(found != state->mStageDone.end() && found->second ? 1.f : 0.f, 3);
        }

        const std::size_t separator = tokens[0].find('.');
        if (separator == std::string_view::npos || separator == 0 || separator + 1 == tokens[0].size())
            return std::nullopt;
        const std::optional<float> value
            = getQuestVariable(tokens[0].substr(0, separator), tokens[0].substr(separator + 1));
        return value ? compare(*value, 1) : std::nullopt;
    }

    ESM::FormId ESM4QuestRuntime::resolveReference(std::string_view id)
    {
        const std::string key = Misc::StringUtils::lowerCase(id);
        if (const auto cached = mReferenceIds.find(key); cached != mReferenceIds.end())
            return cached->second;

        ESM::FormId result;
        const auto search = [&id, &result](const auto& records) {
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
        if (mStore != nullptr && !search(mStore->get<ESM4::Reference>())
            && !search(mStore->get<ESM4::ActorCharacter>()))
            search(mStore->get<ESM4::ActorCreature>());

        mReferenceIds.emplace(key, result);
        return result;
    }

    bool ESM4QuestRuntime::executeReferenceCommand(ESM4QuestReferenceCommand command, std::string_view id)
    {
        const ESM::FormId reference = resolveReference(id);
        if (reference.isZeroOrUnset() || !mReferenceCommandHandler)
            return false;
        return mReferenceCommandHandler(command, reference);
    }

    void ESM4QuestRuntime::executeStageSource(std::string_view source)
    {
        std::istringstream stream{ std::string(source) };
        for (std::string line; std::getline(stream, line);)
        {
            const std::vector<std::string_view> tokens = tokenize(line);
            if (tokens.empty())
                continue;

            if (tokens.size() >= 4 && Misc::StringUtils::ciEqual(tokens[0], "SetObjectiveDisplayed"))
            {
                std::int32_t objective = 0;
                std::int32_t displayed = 0;
                if (parseInt(tokens[2], objective) && parseInt(tokens[3], displayed)
                    && setObjectiveDisplayed(tokens[1], objective, displayed != 0))
                    continue;
            }
            else if (tokens.size() >= 4 && Misc::StringUtils::ciEqual(tokens[0], "set")
                && Misc::StringUtils::ciEqual(tokens[2], "to"))
            {
                const std::size_t separator = tokens[1].find('.');
                float value = 0.f;
                if (separator != std::string_view::npos && separator != 0 && separator + 1 < tokens[1].size()
                    && parseFloat(tokens[3], value)
                    && setQuestVariable(tokens[1].substr(0, separator), tokens[1].substr(separator + 1), value))
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
            else
            {
                const std::size_t separator = tokens[0].rfind('.');
                if (separator != std::string_view::npos && separator != 0 && separator + 1 < tokens[0].size())
                {
                    const std::string_view target = tokens[0].substr(0, separator);
                    const std::string_view command = tokens[0].substr(separator + 1);
                    if (Misc::StringUtils::ciEqual(command, "Enable")
                        && executeReferenceCommand(ESM4QuestReferenceCommand::Enable, target))
                        continue;
                    if (Misc::StringUtils::ciEqual(command, "Disable")
                        && executeReferenceCommand(ESM4QuestReferenceCommand::Disable, target))
                        continue;
                    if ((Misc::StringUtils::ciEqual(command, "evp")
                            || Misc::StringUtils::ciEqual(command, "EvaluatePackage"))
                        && executeReferenceCommand(ESM4QuestReferenceCommand::EvaluatePackage, target))
                        continue;
                }
            }

            mUnsupportedStageCommands.push_back(line);
        }
    }

    void ESM4QuestRuntime::executeResultSource(std::string_view source)
    {
        struct ConditionalFrame
        {
            bool mParentActive = true;
            bool mBranchTaken = false;
            bool mCurrentBranchActive = false;
            bool mIndeterminate = false;
            bool mSawElse = false;
        };

        std::vector<ConditionalFrame> conditionals;
        std::istringstream stream{ std::string(source) };
        for (std::string storage; std::getline(stream, storage);)
        {
            std::string_view line = trim(storage);
            if (const std::size_t comment = line.find(';'); comment != std::string_view::npos)
                line = trim(line.substr(0, comment));
            if (line.empty())
                continue;

            if (isControlKeyword(line, "if", true))
            {
                ConditionalFrame frame;
                frame.mParentActive = conditionals.empty() || conditionals.back().mCurrentBranchActive;
                if (frame.mParentActive)
                {
                    const std::optional<bool> condition = evaluateResultCondition(trim(line.substr(2)));
                    if (condition)
                    {
                        frame.mCurrentBranchActive = *condition;
                        frame.mBranchTaken = *condition;
                    }
                    else
                    {
                        frame.mIndeterminate = true;
                        mUnsupportedStageCommands.emplace_back(line);
                    }
                }
                conditionals.push_back(frame);
                continue;
            }
            if (isControlKeyword(line, "elseif", true))
            {
                if (conditionals.empty() || conditionals.back().mSawElse)
                {
                    mUnsupportedStageCommands.emplace_back(line);
                    continue;
                }
                ConditionalFrame& frame = conditionals.back();
                frame.mCurrentBranchActive = false;
                if (frame.mParentActive && !frame.mBranchTaken && !frame.mIndeterminate)
                {
                    const std::optional<bool> condition = evaluateResultCondition(trim(line.substr(6)));
                    if (condition)
                    {
                        frame.mCurrentBranchActive = *condition;
                        frame.mBranchTaken = *condition;
                    }
                    else
                    {
                        frame.mIndeterminate = true;
                        mUnsupportedStageCommands.emplace_back(line);
                    }
                }
                continue;
            }
            if (isControlKeyword(line, "else"))
            {
                if (conditionals.empty() || conditionals.back().mSawElse)
                {
                    mUnsupportedStageCommands.emplace_back(line);
                    continue;
                }
                ConditionalFrame& frame = conditionals.back();
                frame.mSawElse = true;
                frame.mCurrentBranchActive
                    = frame.mParentActive && !frame.mBranchTaken && !frame.mIndeterminate;
                frame.mBranchTaken |= frame.mCurrentBranchActive;
                continue;
            }
            if (isControlKeyword(line, "endif"))
            {
                if (conditionals.empty())
                    mUnsupportedStageCommands.emplace_back(line);
                else
                    conditionals.pop_back();
                continue;
            }

            if (conditionals.empty() || conditionals.back().mCurrentBranchActive)
                executeStageSource(line);
        }

        if (!conditionals.empty())
            mUnsupportedStageCommands.emplace_back("unterminated result-script conditional");
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
