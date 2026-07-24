#include "esm4questruntime.hpp"

#include "esmstore.hpp"
#include "globals.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm4/loadglob.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadscpt.hpp>
#include <components/misc/strings/algorithm.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

namespace
{
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
        mUnsupportedConditionFunctions.clear();
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
        ESM4QuestState* state = quest != nullptr ? findState(*quest) : nullptr;
        if (state == nullptr)
            return false;
        state->mFlags |= ESM4QuestState::Flag_Running;
        return true;
    }

    bool ESM4QuestRuntime::stopQuest(std::string_view id)
    {
        const ESM4::Quest* quest = resolveQuest(id);
        ESM4QuestState* state = quest != nullptr ? findState(*quest) : nullptr;
        if (state == nullptr)
            return false;
        state->mFlags &= ~ESM4QuestState::Flag_Running;
        return true;
    }

    bool ESM4QuestRuntime::completeQuest(std::string_view id)
    {
        const ESM4::Quest* quest = resolveQuest(id);
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

        const bool wasRunning = (state->mFlags & ESM4QuestState::Flag_Running) != 0;
        state->mFlags |= ESM4QuestState::Flag_Running;
        state->mCurrentStage = stageIndex;
        state->mStageDone[stage->mIndex] = true;

        bool executedEntry = false;
        for (const ESM4::QuestStageEntry& entry : stage->mEntries)
        {
            if (!evaluateConditions(entry.mConditions))
                continue;
            executedEntry = true;
            executeStageSource(entry.mScript.scriptSource);
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

        if (MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager())
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
        if (mStore == nullptr)
            return std::nullopt;

        const ESM::FormId parameter = ESM::FormId::fromUint32(condition.param1);
        const auto findQuestState = [this, parameter]() -> const ESM4QuestState* {
            const ESM4::Quest* quest = mStore->get<ESM4::Quest>().search(ESM::RefId(parameter));
            return quest != nullptr ? findState(*quest) : nullptr;
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
                    if (const ESM4QuestState* state = findState(*quest))
                        if (const ESM4::Script* script
                            = mStore->get<ESM4::Script>().search(ESM::RefId(quest->mQuestScript)))
                            for (const ESM4::ScriptLocalVariableData& variable : script->mScript.localVarData)
                                if (variable.index == condition.param2)
                                {
                                    const auto found = state->mVariables.find(
                                        Misc::StringUtils::lowerCase(variable.variableName));
                                    return found != state->mVariables.end() ? found->second : 0.f;
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
                if (std::find(mUnsupportedConditionFunctions.begin(), mUnsupportedConditionFunctions.end(),
                        condition.functionIndex)
                    == mUnsupportedConditionFunctions.end())
                    mUnsupportedConditionFunctions.push_back(condition.functionIndex);
                return std::nullopt;
        }
    }

    bool ESM4QuestRuntime::evaluateConditions(const std::vector<ESM4::TargetCondition>& conditions)
    {
        for (std::size_t i = 0; i < conditions.size(); ++i)
        {
            const auto evaluate = [this](const ESM4::TargetCondition& condition) {
                const std::optional<float> actual = evaluateConditionValue(condition);
                if (!actual)
                    return false;

                float expected = condition.comparison;
                if ((condition.condition & ESM4::CTF_UseGlobal) != 0)
                {
                    ESM4::TargetCondition globalCondition;
                    globalCondition.functionIndex = ESM4::FUN_GetGlobalValue;
                    globalCondition.param1 = std::bit_cast<std::uint32_t>(condition.comparison);
                    const std::optional<float> globalValue = evaluateConditionValue(globalCondition);
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

            mUnsupportedStageCommands.push_back(line);
        }
    }

    void ESM4QuestRuntime::executeResultSource(std::string_view source)
    {
        executeStageSource(source);
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
