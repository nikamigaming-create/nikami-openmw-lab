#ifndef OPENMW_MWWORLD_ESM4QUESTRUNTIME_H
#define OPENMW_MWWORLD_ESM4QUESTRUNTIME_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <components/esm/formid.hpp>

namespace ESM4
{
    struct Quest;
    struct QuestStage;
    struct ScriptDefinition;
    struct TargetCondition;
}

namespace ESM
{
    class ESMReader;
    class ESMWriter;
}

namespace MWWorld
{
    class ESMStore;
    class Globals;

    struct ESM4QuestState
    {
        enum Flag : std::uint8_t
        {
            Flag_Running = 0x01,
            Flag_Completed = 0x02,
            Flag_AllowRepeatedTopics = 0x04,
            Flag_AllowRepeatedStages = 0x08,
            Flag_RunAfterReset = 0x10,
            Flag_ShownInPipBoy = 0x20,
            Flag_Failed = 0x40,
        };

        enum ObjectiveFlag : std::uint8_t
        {
            Objective_Displayed = 0x01,
            Objective_Completed = 0x02,
        };

        std::uint8_t mFlags = 0;
        std::uint8_t mCurrentStage = 0;
        std::map<std::int16_t, bool> mStageDone;
        std::map<std::int32_t, std::uint8_t> mObjectiveStatus;
        std::map<std::string, float, std::less<>> mVariables;
    };

    struct ESM4SavedQuestProgress
    {
        struct Stage
        {
            ESM::FormId mQuest;
            std::uint8_t mStage = 0;
            std::uint8_t mLogEntry = 0;
        };

        struct Objective
        {
            ESM::FormId mQuest;
            std::int32_t mObjective = 0;
        };

        std::vector<Stage> mStages;
        std::vector<Objective> mObjectives;
        std::optional<ESM::FormId> mActiveQuest;
    };

    class ESM4QuestRuntime
    {
        using QuestStateMap = std::unordered_map<ESM::FormId, ESM4QuestState>;

        const ESMStore* mStore = nullptr;
        const Globals* mGlobals = nullptr;
        QuestStateMap mStates;
        std::optional<ESM::FormId> mActiveQuest;
        std::vector<std::string> mUnsupportedStageCommands;
        std::vector<std::uint16_t> mUnsupportedCompiledOpcodes;
        std::vector<std::uint32_t> mUnsupportedConditionFunctions;

        enum class CompiledQuestCommandType : std::uint8_t
        {
            StartQuest,
            StopQuest,
            CompleteQuest,
            SetStage,
            SetObjectiveCompleted,
            SetObjectiveDisplayed,
            ForceActiveQuest,
        };

        struct CompiledQuestCommand
        {
            CompiledQuestCommandType mType = CompiledQuestCommandType::ForceActiveQuest;
            ESM::FormId mQuest{};
            std::int32_t mObjective = 0;
            bool mValue = false;
            std::uint8_t mStage = 0;
        };

        struct CompiledStageScript
        {
            bool mUseSourceFallback = false;
            std::vector<CompiledQuestCommand> mCommands;
            std::vector<std::uint16_t> mUnsupportedOpcodes;
        };

        struct CompiledStageKey
        {
            ESM::FormId mQuest{};
            std::uint8_t mStage = 0;

            friend bool operator==(const CompiledStageKey&, const CompiledStageKey&) = default;
        };

        struct PendingStageEffect
        {
            ESM::FormId mQuest{};
            std::uint8_t mStage = 0;
            bool mWasRunning = false;
            bool mEntryExecuted = false;
            std::string mNotification;
        };

        struct CompiledStageWorkingState
        {
            QuestStateMap mStates;
            std::optional<ESM::FormId> mActiveQuest;
            std::vector<CompiledStageKey> mStack;
            std::vector<PendingStageEffect> mEffects;
        };

        const ESM4::Quest* resolveQuest(std::string_view id) const;
        ESM4QuestState* findState(const ESM4::Quest& quest);
        const ESM4QuestState* findState(const ESM4::Quest& quest) const;
        std::optional<float> evaluateConditionValue(const ESM4::TargetCondition& condition);
        std::optional<float> evaluateConditionValue(
            const ESM4::TargetCondition& condition, const QuestStateMap& states, bool recordUnsupported);
        bool evaluateConditions(const std::vector<ESM4::TargetCondition>& conditions, const QuestStateMap& states,
            bool recordUnsupported);
        bool isStateDirty(ESM::FormId id, const ESM4QuestState& state) const;
        bool prepareStageScript(const ESM4::ScriptDefinition& script, CompiledStageScript& prepared) const;
        bool stageContainsCompiledSetStage(const ESM4::QuestStage& stage) const;
        bool areCompiledStageConditionsPure(const std::vector<ESM4::TargetCondition>& conditions) const;
        bool preflightPureCompiledStage(
            ESM::FormId id, std::uint8_t stage, std::vector<CompiledStageKey>& stack) const;
        bool executePureCompiledStage(ESM::FormId id, std::uint8_t stage, CompiledStageWorkingState& working);
        bool executePureCompiledCommand(
            const CompiledQuestCommand& command, CompiledStageWorkingState& working);
        bool executeCompiledStageTransaction(ESM::FormId id, std::uint8_t stage);
        void flushCompiledStageEffects(const std::vector<PendingStageEffect>& effects);
        std::optional<bool> evaluateResultCondition(std::string_view expression) const;
        void executeStageSource(std::string_view source);

    public:
        void initialize(const ESMStore& store, const Globals* globals = nullptr);
        void clear();

        // Import decoded retail save progress without executing quest stage scripts. Validation is transactional:
        // no runtime state changes unless every quest, stage and objective resolves against the loaded content.
        bool loadSavedProgress(const ESM4SavedQuestProgress& progress, std::string* error = nullptr);

        bool startQuest(std::string_view id);
        bool startQuest(ESM::FormId id);
        bool stopQuest(std::string_view id);
        bool stopQuest(ESM::FormId id);
        bool completeQuest(std::string_view id);
        bool completeQuest(ESM::FormId id);
        bool failQuest(std::string_view id);
        bool setStage(std::string_view id, std::uint8_t stage);
        bool setStage(ESM::FormId id, std::uint8_t stage);
        bool setObjectiveDisplayed(std::string_view id, std::int32_t objective, bool displayed);
        bool setObjectiveDisplayed(ESM::FormId id, std::int32_t objective, bool displayed);
        bool setObjectiveCompleted(std::string_view id, std::int32_t objective, bool completed);
        bool setObjectiveCompleted(ESM::FormId id, std::int32_t objective, bool completed);
        bool setQuestVariable(std::string_view id, std::string_view variable, float value);
        bool forceActiveQuest(std::string_view id);
        bool forceActiveQuest(ESM::FormId id);
        void executeResultSource(std::string_view source);
        bool evaluateConditions(const std::vector<ESM4::TargetCondition>& conditions);

        int countSavedGameRecords() const;
        void write(ESM::ESMWriter& writer) const;
        void readRecord(ESM::ESMReader& reader);

        const ESM4QuestState* search(std::string_view id) const;
        const ESM4QuestState* search(ESM::FormId id) const;
        std::optional<float> getQuestVariable(std::string_view id, std::string_view variable) const;
        std::optional<ESM::FormId> getActiveQuest() const { return mActiveQuest; }
        const std::vector<std::string>& getUnsupportedStageCommands() const { return mUnsupportedStageCommands; }
        const std::vector<std::uint16_t>& getUnsupportedCompiledOpcodes() const { return mUnsupportedCompiledOpcodes; }
        const std::vector<std::uint32_t>& getUnsupportedConditionFunctions() const
        {
            return mUnsupportedConditionFunctions;
        }
    };
}

#endif
