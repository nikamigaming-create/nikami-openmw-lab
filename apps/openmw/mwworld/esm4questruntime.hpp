#ifndef OPENMW_MWWORLD_ESM4QUESTRUNTIME_H
#define OPENMW_MWWORLD_ESM4QUESTRUNTIME_H

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
        struct EnemyRelation
        {
            ESM::FormId mFirst;
            ESM::FormId mSecond;
            bool mFirstTreatsSecondAsNeutral = false;
            bool mSecondTreatsFirstAsNeutral = false;

            bool operator==(const EnemyRelation&) const = default;
        };

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
        std::vector<std::pair<ESM::FormId, ESM::FormId>> mAllies;
        std::vector<EnemyRelation> mEnemies;
    };

    struct ESM4SavedQuestProgress
    {
        struct State
        {
            ESM::FormId mQuest;
            std::uint8_t mFlags = 0;

            bool operator==(const State&) const = default;
        };

        struct Stage
        {
            ESM::FormId mQuest;
            std::uint8_t mStage = 0;
            std::uint8_t mLogEntry = 0;
            bool mDone = true;
        };

        struct Objective
        {
            ESM::FormId mQuest;
            std::int32_t mObjective = 0;
            std::uint8_t mStatus = ESM4QuestState::Objective_Displayed;
        };

        struct Variable
        {
            ESM::FormId mQuest;
            std::uint32_t mIndex = 0;
            float mValue = 0.f;

            bool operator==(const Variable&) const = default;
        };

        std::vector<State> mStates;
        std::vector<Stage> mStages;
        std::vector<Objective> mObjectives;
        std::vector<Variable> mVariables;
        std::optional<ESM::FormId> mActiveQuest;
    };

    enum class ESM4QuestReferenceCommand : std::uint8_t
    {
        Enable,
        Disable,
        EvaluatePackage,
    };

    class ESM4QuestRuntime
    {
    public:
        using ReferenceCommandHandler = std::function<bool(ESM4QuestReferenceCommand, ESM::FormId)>;
        using MessageHandler = std::function<bool(ESM::FormId)>;
        using SayToHandler = std::function<bool(ESM::FormId, ESM::FormId, ESM::FormId)>;
        using SetAllyHandler = std::function<bool(ESM::FormId, ESM::FormId)>;
        using SetEnemyHandler = std::function<bool(ESM::FormId, ESM::FormId, bool, bool)>;
        using ItemCountHandler = std::function<std::optional<int>(ESM::FormId, ESM::FormId)>;
        using AddItemHandler = std::function<bool(ESM::FormId, ESM::FormId, int)>;

    private:
        using QuestStateMap = std::unordered_map<ESM::FormId, ESM4QuestState>;

        const ESMStore* mStore = nullptr;
        const Globals* mGlobals = nullptr;
        QuestStateMap mStates;
        std::optional<ESM::FormId> mActiveQuest;
        std::vector<std::string> mUnsupportedStageCommands;
        std::vector<std::uint16_t> mUnsupportedCompiledOpcodes;
        std::vector<std::uint32_t> mUnsupportedConditionFunctions;
        std::unordered_map<std::string, ESM::FormId> mReferenceIds;
        std::unordered_map<std::string, ESM::FormId> mFactionIds;
        ReferenceCommandHandler mReferenceCommandHandler;
        MessageHandler mMessageHandler;
        SayToHandler mSayToHandler;
        SetAllyHandler mSetAllyHandler;
        SetEnemyHandler mSetEnemyHandler;
        ItemCountHandler mItemCountHandler;
        AddItemHandler mAddItemHandler;

        enum class CompiledQuestCommandType : std::uint8_t
        {
            If,
            ElseIf,
            Else,
            EndIf,
            StartQuest,
            StopQuest,
            CompleteQuest,
            SetStage,
            SetObjectiveCompleted,
            SetObjectiveDisplayed,
            ForceActiveQuest,
            SetVariable,
            SetVariableFromItemCount,
            SetAlly,
            SetEnemy,
            Enable,
            Disable,
            AddItem,
            EvaluatePackage,
            ShowMessage,
            SayTo,
        };

        enum class CompiledConditionValueType : std::uint8_t
        {
            QuestVariable,
            GetStage,
            GetStageDone,
        };

        enum class CompiledConditionComparison : std::uint8_t
        {
            Truthy,
            Equal,
            NotEqual,
            Less,
            LessEqual,
            Greater,
            GreaterEqual,
        };

        struct CompiledQuestCondition
        {
            CompiledConditionValueType mValueType = CompiledConditionValueType::QuestVariable;
            CompiledConditionComparison mComparison = CompiledConditionComparison::Truthy;
            ESM::FormId mQuest{};
            std::string mVariable;
            std::int32_t mStage = 0;
            float mComparisonValue = 0.f;
        };

        struct CompiledQuestCommand
        {
            CompiledQuestCommandType mType = CompiledQuestCommandType::ForceActiveQuest;
            ESM::FormId mQuest{};
            std::int32_t mObjective = 0;
            bool mValue = false;
            std::uint8_t mStage = 0;
            ESM::FormId mTarget{};
            ESM::FormId mTopic{};
            std::string mVariable;
            float mNumber = 0.f;
            bool mSecondaryValue = false;
            std::optional<CompiledQuestCondition> mCondition;
        };

        struct CompiledConditionalFrame
        {
            bool mParentActive = false;
            bool mBranchTaken = false;
            bool mActive = false;
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

        struct PendingExternalEffect
        {
            CompiledQuestCommandType mType = CompiledQuestCommandType::EvaluatePackage;
            ESM::FormId mTarget{};
            ESM::FormId mListener{};
            ESM::FormId mTopic{};
            bool mValue = false;
            bool mSecondaryValue = false;
            std::int32_t mCount = 0;
        };

        struct CompiledStageWorkingState
        {
            QuestStateMap mStates;
            std::optional<ESM::FormId> mActiveQuest;
            std::vector<CompiledStageKey> mStack;
            std::vector<PendingStageEffect> mEffects;
            std::vector<PendingExternalEffect> mExternalEffects;
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
        std::optional<bool> evaluateCompiledCondition(
            const CompiledQuestCondition& condition, const QuestStateMap& states) const;
        bool updateCompiledConditionalState(const CompiledQuestCommand& command, const QuestStateMap& states,
            std::vector<CompiledConditionalFrame>& stack, bool& execute) const;
        bool executeCompiledStageTransaction(ESM::FormId id, std::uint8_t stage);
        void flushCompiledStageEffects(const std::vector<PendingStageEffect>& effects);
        void flushCompiledExternalEffects(const std::vector<PendingExternalEffect>& effects);
        std::optional<bool> evaluateResultCondition(std::string_view expression) const;
        ESM::FormId resolveReference(std::string_view id);
        ESM::FormId resolveFaction(std::string_view id);
        bool executeReferenceCommand(ESM4QuestReferenceCommand command, std::string_view id);
        void executeStageSource(std::string_view source, ESM4QuestState* ownerState = nullptr);

    public:
        void initialize(const ESMStore& store, const Globals* globals = nullptr);
        void clear();
        void setReferenceCommandHandler(ReferenceCommandHandler handler)
        {
            mReferenceCommandHandler = std::move(handler);
        }
        void setMessageHandler(MessageHandler handler) { mMessageHandler = std::move(handler); }
        void setSayToHandler(SayToHandler handler) { mSayToHandler = std::move(handler); }
        void setSetAllyHandler(SetAllyHandler handler) { mSetAllyHandler = std::move(handler); }
        void setSetEnemyHandler(SetEnemyHandler handler) { mSetEnemyHandler = std::move(handler); }
        void setItemCountHandler(ItemCountHandler handler) { mItemCountHandler = std::move(handler); }
        void setAddItemHandler(AddItemHandler handler) { mAddItemHandler = std::move(handler); }

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
