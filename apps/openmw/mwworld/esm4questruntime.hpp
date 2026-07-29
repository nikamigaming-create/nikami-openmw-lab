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
#include <components/esm/position.hpp>
#include <components/esm/refid.hpp>
#include <components/esm4/imagespacecomposition.hpp>

namespace ESM4
{
    struct ImageSpaceModifier;
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

    // An intentionally narrow reading of a Bethesda-style opening script. The
    // runtime only accepts an unambiguous stage-zero source entry that contains
    // a player MoveTo anchor, a PlayBink command, and a direct SetStage handoff
    // to the same quest. The handoff differentiates the initial opening from
    // later in-game cinematic transitions. It exposes authored data; it does
    // not claim to implement the rest of the source script.
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

    // A small, data-derived slice of the Fallout-family GameMode scripting
    // pattern.  Bethesda's opening quests commonly use a local run flag and
    // countdown variable to move through authored quest stages.  We compile
    // only that unambiguous timer idiom from the quest's own source script;
    // this is deliberately not a name- or campaign-specific timeline.
    struct ESM4AuthoredGameModeTimer
    {
        std::string mRunVariable;
        std::string mTimerVariable;
        std::map<std::uint8_t, std::uint8_t> mStageTransitions;
    };

    enum class ESM4QuestReferenceCommand : std::uint8_t
    {
        Enable,
        Disable,
        Unlock,
        Kill,
        ResetAi,
        EvaluatePackage,
    };

    enum class ESM4QuestActorValueCommand : std::uint8_t
    {
        Set,
        Mod,
        Restore,
    };

    enum class ESM4QuestReputationValueCommand : std::uint8_t
    {
        Set,
        AddExact,
    };

    enum class ESM4QuestActorFlag : std::uint8_t
    {
        Unconscious,
        Restrained,
        PlayerTeammate,
        IgnoreCrime,
        Ghost,
        IgnoreFriendlyHits,
        Alert,
    };

    enum class ESM4QuestActorCommand : std::uint8_t
    {
        StartCombat,
        StopCombat,
        StopLook,
        ResetHealth,
        ResetInventory,
        Look,
    };

    struct ESM4QuestFactionMembership
    {
        bool mMember = false;
        std::int8_t mRank = -1;
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
        using AddItemHealthPercentHandler = std::function<bool(ESM::FormId, ESM::FormId, int, float)>;
        using RemoveItemHandler = std::function<bool(ESM::FormId, ESM::FormId, int)>;
        using RemoveAllItemsHandler
            = std::function<bool(ESM::FormId, std::optional<ESM::FormId>, bool)>;
        using RemoveAllTypedItemsHandler = std::function<bool(ESM::FormId, std::optional<ESM::FormId>, bool,
            std::int32_t, std::optional<ESM::FormId>)>;
        using EquipmentCommandHandler
            = std::function<bool(ESM::FormId, ESM::FormId, bool)>;
        using ReferenceActivationHandler = std::function<bool(ESM::FormId, ESM::FormId, bool)>;
        using ActorIdleHandler = std::function<bool(ESM::FormId, ESM::FormId)>;
        using ReferenceAnimationGroupHandler
            = std::function<bool(ESM::FormId, std::string_view, int)>;
        using SoundCommandHandler
            = std::function<bool(ESM::FormId, std::optional<ESM::FormId>, bool)>;
        using LockHandler = std::function<bool(ESM::FormId, std::optional<int>)>;
        using ActorDeadHandler = std::function<std::optional<bool>(ESM::FormId)>;
        using RewardXpHandler = std::function<bool(int)>;
        using AddReputationHandler = std::function<bool(ESM::FormId, bool, int)>;
        using ReputationValueCommandHandler
            = std::function<bool(ESM::FormId, ESM4QuestReputationValueCommand, bool, float)>;
        using SetDestroyedHandler = std::function<bool(ESM::FormId, bool)>;
        using ShowMapHandler = std::function<bool(ESM::FormId, bool)>;
        using EnableFastTravelHandler = std::function<bool(bool, bool, bool)>;
        using NoteHandler = std::function<bool(ESM::FormId, bool)>;
        using KnownNoteHandler = std::function<std::optional<bool>(ESM::FormId)>;
        using PlayerPerkHandler = std::function<bool(ESM::FormId, bool)>;
        using PlayerHasPerkHandler = std::function<std::optional<bool>(ESM::FormId)>;
        using QuestObjectHandler = std::function<bool(ESM::FormId, bool)>;
        using AchievementHandler = std::function<bool(std::uint32_t)>;
        using RewardKarmaHandler = std::function<bool(int)>;
        using AddSpecialPointsHandler = std::function<bool(int)>;
        using SetEssentialHandler = std::function<bool(ESM::FormId, bool)>;
        using ActorValueCommandHandler
            = std::function<bool(ESM::FormId, ESM4QuestActorValueCommand, std::string_view, float)>;
        using ActorValueHandler
            = std::function<std::optional<float>(ESM::FormId, std::string_view)>;
        // A rank value adds/updates membership; nullopt records RemoveFromFaction.
        using ActorFactionCommandHandler
            = std::function<bool(ESM::FormId, ESM::FormId, std::optional<int>)>;
        using ActorFactionMembershipHandler
            = std::function<std::optional<ESM4QuestFactionMembership>(ESM::FormId, ESM::FormId)>;
        using ActorFlagCommandHandler
            = std::function<bool(ESM::FormId, ESM4QuestActorFlag, bool)>;
        using ActorFlagHandler
            = std::function<std::optional<bool>(ESM::FormId, ESM4QuestActorFlag)>;
        using ActorCommandHandler
            = std::function<bool(ESM4QuestActorCommand, ESM::FormId, ESM::FormId, bool)>;
        using ActorEffectCommandHandler
            = std::function<bool(ESM::FormId, ESM::FormId, bool)>;
        using ActorNameCommandHandler
            = std::function<bool(ESM::FormId, std::string_view)>;

    private:
        using QuestStateMap = std::unordered_map<ESM::FormId, ESM4QuestState>;

        const ESMStore* mStore = nullptr;
        const Globals* mGlobals = nullptr;
        QuestStateMap mStates;
        std::optional<ESM::FormId> mActiveQuest;
        std::vector<std::string> mUnsupportedStageCommands;
        std::vector<std::uint16_t> mUnsupportedCompiledOpcodes;
        std::vector<std::uint32_t> mUnsupportedConditionFunctions;
        struct ActiveImageSpaceModifier
        {
            ESM4::ImageSpaceModifierRuntimeState mState;
            float mDuration = 0.f;
            bool mAnimatable = false;
        };
        std::vector<ActiveImageSpaceModifier> mActiveImageSpaceModifiers;
        // Trace-only state for source-driven cinematic commands.  Video state
        // itself remains owned by the window manager.
        std::vector<std::string> mPlayedStageVideos;
        // Fallout-family quest scripts own their progression in Begin
        // GameMode blocks.  Keep the source indexed by its owning quest so
        // locals such as `timer` can be resolved from the authored script,
        // rather than from a campaign-specific timeline.
        std::unordered_map<ESM::FormId, std::string> mAuthoredGameModeSources;
        std::unordered_map<ESM::FormId, std::vector<ESM4AuthoredGameModeTimer>> mAuthoredGameModeTimers;

        // OpenNV profiles can declare aliases for script commands supplied by
        // a licensed conversion or mod.  The aliases resolve only to a small,
        // engine-owned set of semantic capabilities; profile data can never
        // name an arbitrary C++ method or execute an arbitrary command.
        std::map<std::string, std::string, std::less<>> mAuthoredCompatibilityCommands;

        // MenuMode blocks are authored lifecycle code distinct from GameMode:
        // they run while a real engine menu is open, including character
        // generation handoffs.  Keep the source and the quest's local scope
        // together so this remains data driven.
        std::unordered_map<ESM::FormId, std::string> mAuthoredMenuModeSources;

        // Fallout also attaches GameMode scripts directly to placed actors.
        // Their locals belong to each placed reference, rather than to the
        // shared NPC base or to a quest.  Keep the source and locals together
        // so commands such as `CG00DadREF.doTalk` use the same authored
        // ownership model as the retail scripts.
        struct ActorScriptState
        {
            ESM::FormId mActor{};
            ESM::FormId mScript{};
            ESM::RefId mCell;
            std::string mEditorId;
            std::string mSource;
            std::map<std::string, float, std::less<>> mVariables;
        };

        struct PendingActorScriptEvent
        {
            ESM::FormId mActor{};
            std::string mEvent;
            std::string mArgument;
        };

        // Say/SayTo result scripts may chain another line while the prior
        // voice is still playing. Keep the next authored topic queued against
        // the placed actor instead of losing that command at the audio
        // boundary.
        struct PendingAuthoredSay
        {
            ESM::FormId mActor{};
            std::string mTopic;
            std::optional<ESM::FormId> mActorScript;
            bool mNotifyActorScript = false;
        };

        std::unordered_map<ESM::FormId, ActorScriptState> mActorScriptStates;
        std::unordered_map<std::string, ESM::FormId> mActorScriptEditorIds;
        std::unordered_map<std::string, bool> mAmbiguousActorScriptEditorIds;
        std::vector<PendingActorScriptEvent> mPendingActorScriptEvents;
        std::vector<PendingAuthoredSay> mPendingAuthoredSays;

        // Scripted placed references have their own local variables just as
        // actors do.  This covers authored trigger volumes and interactive
        // objects without making a campaign, cell, or editor ID special.
        struct ReferenceScriptTriggerParticipant
        {
            // Bethesda trigger blocks name the object which enters the
            // volume, for example `Begin OnTrigger Player` or `Begin
            // OnTrigger SomePlacedActorREF`.  Keep occupancy per named
            // participant: a single trigger can legitimately handle both a
            // player and a scene actor without their enter/leave state
            // clobbering one another.
            std::string mArgument;
            bool mHasEnter = false;
            bool mHasLeave = false;
            bool mHasTrigger = false;
            bool mWasInside = false;
            bool mLoggedUnavailableForRoute = false;
        };

        struct ReferenceScriptState
        {
            ESM::FormId mReference{};
            ESM::FormId mScript{};
            ESM::RefId mCell;
            std::string mEditorId;
            std::string mSource;
            std::map<std::string, float, std::less<>> mVariables;
            float mTriggerRadius = 0.f;
            bool mHasGameMode = false;
            bool mLoggedUnresolvedForRoute = false;
            bool mLoggedInvalidRadiusForRoute = false;
            std::vector<ReferenceScriptTriggerParticipant> mTriggerParticipants;
        };

        std::unordered_map<ESM::FormId, ReferenceScriptState> mReferenceScriptStates;
        std::unordered_map<std::string, ESM::FormId> mReferenceScriptEditorIds;
        std::unordered_map<std::string, bool> mAmbiguousReferenceScriptEditorIds;

        std::unordered_map<std::string, ESM::FormId> mReferenceIds;
        std::unordered_map<std::string, ESM::FormId> mFactionIds;
        std::unordered_map<std::string, ESM::FormId> mInventoryItemIds;
        std::unordered_map<std::string, ESM::FormId> mFormListIds;
        std::unordered_map<std::string, ESM::FormId> mReputationIds;
        std::unordered_map<std::string, ESM::FormId> mNoteIds;
        std::unordered_map<std::string, ESM::FormId> mPerkIds;
        std::unordered_map<std::string, ESM::FormId> mActorBaseIds;
        std::unordered_map<std::string, ESM::FormId> mSpellIds;
        std::unordered_map<std::string, ESM::FormId> mMessageIds;
        std::unordered_map<std::string, ESM::FormId> mIdleAnimationIds;
        std::unordered_map<std::string, ESM::FormId> mSoundIds;
        ReferenceCommandHandler mReferenceCommandHandler;
        MessageHandler mMessageHandler;
        SayToHandler mSayToHandler;
        SetAllyHandler mSetAllyHandler;
        SetEnemyHandler mSetEnemyHandler;
        ItemCountHandler mItemCountHandler;
        AddItemHandler mAddItemHandler;
        AddItemHealthPercentHandler mAddItemHealthPercentHandler;
        RemoveItemHandler mRemoveItemHandler;
        RemoveAllItemsHandler mRemoveAllItemsHandler;
        RemoveAllTypedItemsHandler mRemoveAllTypedItemsHandler;
        EquipmentCommandHandler mEquipmentCommandHandler;
        ReferenceActivationHandler mReferenceActivationHandler;
        ActorIdleHandler mActorIdleHandler;
        ReferenceAnimationGroupHandler mReferenceAnimationGroupHandler;
        SoundCommandHandler mSoundCommandHandler;
        LockHandler mLockHandler;
        ActorDeadHandler mActorDeadHandler;
        RewardXpHandler mRewardXpHandler;
        AddReputationHandler mAddReputationHandler;
        ReputationValueCommandHandler mReputationValueCommandHandler;
        SetDestroyedHandler mSetDestroyedHandler;
        ShowMapHandler mShowMapHandler;
        EnableFastTravelHandler mEnableFastTravelHandler;
        NoteHandler mNoteHandler;
        KnownNoteHandler mKnownNoteHandler;
        PlayerPerkHandler mPlayerPerkHandler;
        PlayerHasPerkHandler mPlayerHasPerkHandler;
        QuestObjectHandler mQuestObjectHandler;
        AchievementHandler mAchievementHandler;
        RewardKarmaHandler mRewardKarmaHandler;
        AddSpecialPointsHandler mAddSpecialPointsHandler;
        SetEssentialHandler mSetEssentialHandler;
        ActorValueCommandHandler mActorValueCommandHandler;
        ActorValueHandler mActorValueHandler;
        ActorFactionCommandHandler mActorFactionCommandHandler;
        ActorFactionMembershipHandler mActorFactionMembershipHandler;
        ActorFlagCommandHandler mActorFlagCommandHandler;
        ActorFlagHandler mActorFlagHandler;
        ActorCommandHandler mActorCommandHandler;
        ActorEffectCommandHandler mActorEffectCommandHandler;
        ActorNameCommandHandler mActorNameCommandHandler;

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
            CompleteAllObjectives,
            ForceActiveQuest,
            SetVariable,
            SetVariableFromItemCount,
            SetAlly,
            SetEnemy,
            Enable,
            Disable,
            Unlock,
            Kill,
            ResetAi,
            AddItem,
            RemoveItem,
            EvaluatePackage,
            ShowMessage,
            SayTo,
            RewardXp,
            AddReputation,
            SetDestroyed,
            ShowMap,
            EnableFastTravel,
        };

        enum class CompiledConditionValueType : std::uint8_t
        {
            QuestVariable,
            GetStage,
            GetStageDone,
            GetObjectiveCompleted,
            GetObjectiveDisplayed,
            GetDead,
            GetQuestRunning,
            GetQuestCompleted,
        };

        enum class CompiledConditionTokenType : std::uint8_t
        {
            Value,
            Number,
            Equal,
            NotEqual,
            Less,
            LessEqual,
            Greater,
            GreaterEqual,
            LogicalAnd,
            LogicalOr,
        };

        struct CompiledConditionToken
        {
            CompiledConditionTokenType mType = CompiledConditionTokenType::Value;
            CompiledConditionValueType mValueType = CompiledConditionValueType::QuestVariable;
            ESM::FormId mQuest{};
            std::string mVariable;
            std::int32_t mStage = 0;
            float mNumber = 0.f;
        };

        struct CompiledQuestCondition
        {
            std::vector<CompiledConditionToken> mPostfix;
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
            bool mHasLiveCondition = false;
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
        ActorScriptState* findActorScriptState(ESM::FormId actor);
        const ActorScriptState* findActorScriptState(ESM::FormId actor) const;
        std::optional<ESM::FormId> resolveActorScript(std::string_view id) const;
        bool setActorScriptVariable(ESM::FormId actor, std::string_view variable, float value);
        ReferenceScriptState* findReferenceScriptState(ESM::FormId reference);
        const ReferenceScriptState* findReferenceScriptState(ESM::FormId reference) const;
        std::optional<ESM::FormId> resolveReferenceScript(std::string_view id) const;
        bool setReferenceScriptVariable(ESM::FormId reference, std::string_view variable, float value);
        std::optional<float> evaluateConditionValue(const ESM4::TargetCondition& condition);
        std::optional<float> evaluateConditionValue(
            const ESM4::TargetCondition& condition, const QuestStateMap& states, bool recordUnsupported);
        bool evaluateConditions(const std::vector<ESM4::TargetCondition>& conditions, const QuestStateMap& states,
            bool recordUnsupported);
        bool isStateDirty(ESM::FormId id, const ESM4QuestState& state) const;
        bool prepareStageScript(const ESM4::ScriptDefinition& script, CompiledStageScript& prepared) const;
        bool stageContainsCompiledSetStage(const ESM4::QuestStage& stage) const;
        bool stageContainsCompiledLiveCondition(const ESM4::QuestStage& stage) const;
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
        const ESM4::ImageSpaceModifier* resolveImageSpaceModifier(std::string_view id) const;
        bool applyImageSpaceModifier(std::string_view id);
        bool removeImageSpaceModifier(std::string_view id);
        void updateImageSpaceModifiers(float duration);
        void executeStageSource(std::string_view source, std::optional<ESM::FormId> ownerQuest = {},
            float secondsPassed = 0.f, std::optional<ESM::FormId> ownerActor = {},
            std::string_view selectedBlock = "gamemode", std::string_view selectedBlockArgument = {},
            std::optional<ESM::FormId> ownerReference = {}, std::string_view actionReference = {});
        static std::vector<ESM4AuthoredGameModeTimer> compileAuthoredGameModeTimers(
            std::string_view source, std::string_view questEditorId);
        void flushCompiledExternalEffects(const std::vector<PendingExternalEffect>& effects);
        ESM::FormId resolveReference(std::string_view id);
        ESM::FormId resolveFaction(std::string_view id);
        ESM::FormId resolveInventoryItem(std::string_view id);
        ESM::FormId resolveFormList(std::string_view id);
        ESM::FormId resolveReputation(std::string_view id);
        ESM::FormId resolveNote(std::string_view id);
        ESM::FormId resolvePerk(std::string_view id);
        ESM::FormId resolveActorBase(std::string_view id);
        ESM::FormId resolveSpell(std::string_view id);
        ESM::FormId resolveMessage(std::string_view id);
        ESM::FormId resolveIdleAnimation(std::string_view id);
        ESM::FormId resolveSound(std::string_view id);
        bool executeReferenceCommand(ESM4QuestReferenceCommand command, std::string_view id);

    public:
        // Parses the profile-local [OpenNV Compatibility] command map.  Each
        // comma-separated entry is `script-command:capability`; unsupported
        // capabilities are deliberately ignored.
        static std::map<std::string, std::string, std::less<>> parseAuthoredCompatibilityCommandMappings(
            std::string_view mappings);

        void initialize(const ESMStore& store, const Globals* globals = nullptr);
        void clear();
        void update(float duration, bool paused);
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
        void setAddItemHealthPercentHandler(AddItemHealthPercentHandler handler)
        {
            mAddItemHealthPercentHandler = std::move(handler);
        }
        void setRemoveItemHandler(RemoveItemHandler handler) { mRemoveItemHandler = std::move(handler); }
        void setRemoveAllItemsHandler(RemoveAllItemsHandler handler)
        {
            mRemoveAllItemsHandler = std::move(handler);
        }
        void setRemoveAllTypedItemsHandler(RemoveAllTypedItemsHandler handler)
        {
            mRemoveAllTypedItemsHandler = std::move(handler);
        }
        void setEquipmentCommandHandler(EquipmentCommandHandler handler)
        {
            mEquipmentCommandHandler = std::move(handler);
        }
        void setReferenceActivationHandler(ReferenceActivationHandler handler)
        {
            mReferenceActivationHandler = std::move(handler);
        }
        void setActorIdleHandler(ActorIdleHandler handler)
        {
            mActorIdleHandler = std::move(handler);
        }
        void setReferenceAnimationGroupHandler(ReferenceAnimationGroupHandler handler)
        {
            mReferenceAnimationGroupHandler = std::move(handler);
        }
        void setSoundCommandHandler(SoundCommandHandler handler)
        {
            mSoundCommandHandler = std::move(handler);
        }
        void setLockHandler(LockHandler handler) { mLockHandler = std::move(handler); }
        void setActorDeadHandler(ActorDeadHandler handler) { mActorDeadHandler = std::move(handler); }
        void setRewardXpHandler(RewardXpHandler handler) { mRewardXpHandler = std::move(handler); }
        void setAddReputationHandler(AddReputationHandler handler)
        {
            mAddReputationHandler = std::move(handler);
        }
        void setReputationValueCommandHandler(ReputationValueCommandHandler handler)
        {
            mReputationValueCommandHandler = std::move(handler);
        }
        void setSetDestroyedHandler(SetDestroyedHandler handler)
        {
            mSetDestroyedHandler = std::move(handler);
        }
        void setShowMapHandler(ShowMapHandler handler)
        {
            mShowMapHandler = std::move(handler);
        }
        void setEnableFastTravelHandler(EnableFastTravelHandler handler)
        {
            mEnableFastTravelHandler = std::move(handler);
        }
        void setNoteHandler(NoteHandler handler) { mNoteHandler = std::move(handler); }
        void setKnownNoteHandler(KnownNoteHandler handler) { mKnownNoteHandler = std::move(handler); }
        void setPlayerPerkHandler(PlayerPerkHandler handler) { mPlayerPerkHandler = std::move(handler); }
        void setPlayerHasPerkHandler(PlayerHasPerkHandler handler)
        {
            mPlayerHasPerkHandler = std::move(handler);
        }
        void setQuestObjectHandler(QuestObjectHandler handler)
        {
            mQuestObjectHandler = std::move(handler);
        }
        void setAchievementHandler(AchievementHandler handler)
        {
            mAchievementHandler = std::move(handler);
        }
        void setRewardKarmaHandler(RewardKarmaHandler handler)
        {
            mRewardKarmaHandler = std::move(handler);
        }
        void setAddSpecialPointsHandler(AddSpecialPointsHandler handler)
        {
            mAddSpecialPointsHandler = std::move(handler);
        }
        void setSetEssentialHandler(SetEssentialHandler handler)
        {
            mSetEssentialHandler = std::move(handler);
        }
        void setActorValueCommandHandler(ActorValueCommandHandler handler)
        {
            mActorValueCommandHandler = std::move(handler);
        }
        void setActorValueHandler(ActorValueHandler handler)
        {
            mActorValueHandler = std::move(handler);
        }
        void setActorFactionCommandHandler(ActorFactionCommandHandler handler)
        {
            mActorFactionCommandHandler = std::move(handler);
        }
        void setActorFactionMembershipHandler(ActorFactionMembershipHandler handler)
        {
            mActorFactionMembershipHandler = std::move(handler);
        }
        void setActorFlagCommandHandler(ActorFlagCommandHandler handler)
        {
            mActorFlagCommandHandler = std::move(handler);
        }
        void setActorFlagHandler(ActorFlagHandler handler)
        {
            mActorFlagHandler = std::move(handler);
        }
        void setActorCommandHandler(ActorCommandHandler handler)
        {
            mActorCommandHandler = std::move(handler);
        }
        void setActorEffectCommandHandler(ActorEffectCommandHandler handler)
        {
            mActorEffectCommandHandler = std::move(handler);
        }
        void setActorNameCommandHandler(ActorNameCommandHandler handler)
        {
            mActorNameCommandHandler = std::move(handler);
        }

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
        // Called from the engine's ordinary activation path.  It only runs a
        // matching authored OnActivate block; standard Lua/default activation
        // continues unchanged after this notification.
        [[nodiscard]] bool onReferenceActivated(const MWWorld::Ptr& reference, const MWWorld::Ptr& actor);
        // Called by the data-driven ESM4 package runtime when a package has
        // genuinely finished playing on its actor.  It dispatches the
        // package's authored On End source and then the actor's matching
        // OnPackageDone block by the package's data ID.
        void onActorScriptPackageDone(const MWWorld::Ptr& actor, ESM::FormId package);
        // Lets the generic Fallout package selector choose a one-shot AI
        // package only when the package's own On End source or the actor's
        // source declares an exact matching OnPackageDone block. Ambient
        // packages remain repeatable.
        bool packageCompletionHasAuthoredHandler(const MWWorld::Ptr& actor, ESM::FormId package) const;
        bool actorScriptHandlesPackageDone(const MWWorld::Ptr& actor, ESM::FormId package) const;
        bool evaluateConditions(const std::vector<ESM4::TargetCondition>& conditions);

        int countSavedGameRecords() const;
        void write(ESM::ESMWriter& writer) const;
        void readRecord(ESM::ESMReader& reader);

        const ESM4QuestState* search(std::string_view id) const;
        const ESM4QuestState* search(ESM::FormId id) const;
        std::optional<float> getQuestVariable(std::string_view id, std::string_view variable) const;
        std::vector<std::string> getStartGameEnabledQuestEditorIds() const;
        std::optional<ESM4AuthoredStartPlacement> findAuthoredStartPlacement() const;
        bool playImageSpaceModifier(ESM::FormId modifier, float strength);
        std::vector<ESM4::ImageSpaceModifierRuntimeState> getActiveImageSpaceModifiers() const;
        std::optional<ESM::FormId> getActiveQuest() const { return mActiveQuest; }
        const std::vector<std::string>& getUnsupportedStageCommands() const { return mUnsupportedStageCommands; }
        const std::vector<std::uint16_t>& getUnsupportedCompiledOpcodes() const { return mUnsupportedCompiledOpcodes; }
        const std::vector<std::uint32_t>& getUnsupportedConditionFunctions() const
        {
            return mUnsupportedConditionFunctions;
        }
        const std::vector<std::string>& getPlayedStageVideos() const { return mPlayedStageVideos; }
    };
}

#endif
