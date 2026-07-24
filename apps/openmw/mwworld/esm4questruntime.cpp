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
#include <components/esm4/loadfact.hpp>
#include <components/esm4/loadglob.hpp>
#include <components/esm4/loadmesg.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadrepu.hpp>
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
        mFactionIds.clear();
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
        std::size_t skippedMissingRecords = 0;
        const auto skipMissingRecord = [&](std::string_view role, ESM::FormId id) {
            ++skippedMissingRecords;
            Log(Debug::Warning) << "FNV/ESM4 save: skipped stale " << role << " for absent QUST "
                                << ESM::RefId(id).serializeText();
        };
        for (const ESM4SavedQuestProgress::State& saved : progress.mStates)
        {
            const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(saved.mQuest));
            const auto state = states.find(saved.mQuest);
            if (quest == nullptr || state == states.end())
            {
                skipMissingRecord("quest state", saved.mQuest);
                continue;
            }
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
            {
                skipMissingRecord("quest stage", saved.mQuest);
                continue;
            }
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
                skipMissingRecord("quest objective", saved.mQuest);
                continue;
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
            {
                skipMissingRecord("quest variable", saved.mQuest);
                continue;
            }
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
                skipMissingRecord("active quest", *progress.mActiveQuest);
            }
            else
            {
                state->second.mFlags |= ESM4QuestState::Flag_Running | ESM4QuestState::Flag_ShownInPipBoy;
                activeQuest = *progress.mActiveQuest;
            }
        }

        mStates = std::move(states);
        mActiveQuest = activeQuest;
        Log(Debug::Info) << "FNV/ESM4 save: imported quest progress stages=" << progress.mStages.size()
                         << " objectives=" << progress.mObjectives.size() << " variables="
                         << progress.mVariables.size() << " states=" << progress.mStates.size() << " active="
                         << (mActiveQuest ? ESM::RefId(*mActiveQuest).serializeText() : std::string("<none>"))
                         << " skippedMissing=" << skippedMissingRecords;
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
                && instruction.opcode != 0x1021 && instruction.opcode != 0x1022
                && instruction.opcode != 0x1034 && instruction.opcode != 0x1036
                && instruction.opcode != 0x1037
                && instruction.opcode != 0x1039 && instruction.opcode != 0x1052
                && instruction.opcode != 0x1055 && instruction.opcode != 0x1059 && instruction.opcode != 0x105e
                && instruction.opcode != 0x1071 && instruction.opcode != 0x1073
                && instruction.opcode != 0x1078 && instruction.opcode != 0x1079
                && instruction.opcode != 0x108b
                && instruction.opcode != 0x10cc
                && instruction.opcode != 0x1111
                && instruction.opcode != 0x1177
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
            if (!((instruction.opcode == 0x105e || instruction.opcode == 0x11fa)
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
            || command.mType == CompiledQuestCommandType::ShowMessage
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
            case CompiledQuestCommandType::AddItem:
            case CompiledQuestCommandType::RemoveItem:
            case CompiledQuestCommandType::EvaluatePackage:
            case CompiledQuestCommandType::ShowMessage:
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
                case CompiledQuestCommandType::ShowMessage:
                    command = "ShowMessage ";
                    executed = mMessageHandler && mMessageHandler(effect.mTarget);
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
        if (stageContainsCompiledSetStage(*stage) || stageContainsCompiledLiveCondition(*stage))
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
                executeStageSource(entry.mScript.scriptSource, state);
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
                            || command.mType == CompiledQuestCommandType::ShowMessage
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
                            else if (command.mType == CompiledQuestCommandType::ShowMessage)
                                failure = "ShowMessage " + ESM::RefId(command.mQuest).serializeText();
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

    bool ESM4QuestRuntime::executeReferenceCommand(ESM4QuestReferenceCommand command, std::string_view id)
    {
        const ESM::FormId reference = resolveReference(id);
        if (reference.isZeroOrUnset() || !mReferenceCommandHandler)
            return false;
        return mReferenceCommandHandler(command, reference);
    }

    void ESM4QuestRuntime::executeStageSource(std::string_view source, ESM4QuestState* ownerState)
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

            if (!conditionals.empty() && !conditionals.back().mCurrentBranchActive)
                continue;

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

        if (!conditionals.empty())
            mUnsupportedStageCommands.emplace_back("unterminated quest-stage conditional");
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
