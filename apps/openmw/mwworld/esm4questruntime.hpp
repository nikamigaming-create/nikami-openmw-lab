#ifndef OPENMW_MWWORLD_ESM4QUESTRUNTIME_H
#define OPENMW_MWWORLD_ESM4QUESTRUNTIME_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <components/esm/formid.hpp>
#include <components/esm/position.hpp>
#include <components/esm/refid.hpp>

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
    class Ptr;

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

    // An intentionally narrow reading of a Bethesda-style opening script. The
    // runtime accepts an unambiguous stage-zero source entry with a player
    // MoveTo anchor, a direct same-quest SetStage handoff, and either an
    // authored cinematic or an explicit SetInCharGen command. This recognizes
    // the native Fallout 3 and New Vegas starts without naming a campaign,
    // cell, marker, or quest in engine code.
    struct ESM4AuthoredStartPlacement
    {
        ESM::FormId mQuest{};
        std::uint8_t mSourceStage = 0;
        std::uint8_t mActivationStage = 0;
        ESM::FormId mMarker{};
        ESM::RefId mCell;
        ESM::Position mPosition;
        std::string mQuestEditorId;
        std::string mMarkerEditorId;
        std::string mCinematicAsset;
    };

    // A small, data-derived slice of the Bethesda GameMode timer idiom. The
    // source owns the timer, its run flag, and each allowed stage transition;
    // this is intentionally not a campaign-specific timeline.
    struct ESM4AuthoredGameModeTimer
    {
        std::string mRunVariable;
        std::string mTimerVariable;
        std::map<std::uint8_t, std::uint8_t> mStageTransitions;
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
        // Quest-local GameMode and MenuMode source owns Fallout opening
        // progression.  Keep each source with the quest that declares its
        // locals instead of inventing an engine-side campaign timeline.
        std::unordered_map<ESM::FormId, std::string> mAuthoredGameModeSources;
        std::unordered_map<ESM::FormId, std::string> mAuthoredMenuModeSources;
        std::unordered_map<ESM::FormId, std::vector<ESM4AuthoredGameModeTimer>> mAuthoredGameModeTimers;

        // Placed Fallout actors are addressed by editor ID from authored
        // source (for example, `SomeActorREF.SayTo`).  The map is populated
        // from the loaded records and rejects ambiguous IDs rather than
        // choosing an arbitrary actor.
        struct ActorReferenceState
        {
            ESM::FormId mReference{};
            ESM::FormId mBase{};
            ESM::RefId mCell;
            std::string mEditorId;
        };

        std::unordered_map<ESM::FormId, ActorReferenceState> mActorReferenceStates;
        std::unordered_map<std::string, ESM::FormId> mActorReferenceEditorIds;
        std::unordered_map<std::string, bool> mAmbiguousActorReferenceEditorIds;
        // Fallout INFO records can be authored as Say Once.  Keep that state
        // with the quest runtime so consecutive scripted Say/SayTo calls walk
        // the authored response chain rather than repeating its first INFO.
        std::unordered_set<ESM::FormId> mSaidAuthoredDialogInfos;

        struct ReferenceScriptState
        {
            ESM::FormId mReference{};
            ESM::RefId mCell;
            ESM::Position mPosition;
            std::string mEditorId;
            std::string mSource;
            float mTriggerRadius = 0.f;
            float mScale = 1.f;
            bool mHasOnActivate = false;
            bool mHasTriggerEnter = false;
            bool mPlayerWasInside = false;
        };

        std::unordered_map<ESM::FormId, ReferenceScriptState> mReferenceScriptStates;

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
        std::optional<ESM::FormId> resolveActorReference(std::string_view id) const;
        bool executeAuthoredDialogue(ESM::FormId actorReference, std::string_view topicEditorId);
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
        void executeStageSource(std::string_view source, std::optional<ESM::FormId> ownerQuest = {},
            float secondsPassed = 0.f, std::string_view selectedBlock = {}, bool actionReferenceIsPlayer = false);
        static std::vector<ESM4AuthoredGameModeTimer> compileAuthoredGameModeTimers(
            std::string_view source, std::string_view questEditorId);

    public:
        void initialize(const ESMStore& store, const Globals* globals = nullptr);
        void clear();
        void update(float duration, bool paused);

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
        [[nodiscard]] bool onReferenceActivated(const MWWorld::Ptr& reference, const MWWorld::Ptr& actor);
        bool evaluateConditions(const std::vector<ESM4::TargetCondition>& conditions);

        int countSavedGameRecords() const;
        void write(ESM::ESMWriter& writer) const;
        void readRecord(ESM::ESMReader& reader);

        const ESM4QuestState* search(std::string_view id) const;
        const ESM4QuestState* search(ESM::FormId id) const;
        std::optional<float> getQuestVariable(std::string_view id, std::string_view variable) const;
        std::vector<std::string> getStartGameEnabledQuestEditorIds() const;
        std::optional<ESM4AuthoredStartPlacement> findAuthoredStartPlacement() const;
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
