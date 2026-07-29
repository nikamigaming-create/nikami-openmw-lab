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
        bool areCompiledStageConditionsPure(const std::vector<ESM4::TargetCondition>& conditions) const;
        bool preflightPureCompiledStage(
            ESM::FormId id, std::uint8_t stage, std::vector<CompiledStageKey>& stack) const;
        bool executePureCompiledStage(ESM::FormId id, std::uint8_t stage, CompiledStageWorkingState& working);
        bool executePureCompiledCommand(
            const CompiledQuestCommand& command, CompiledStageWorkingState& working);
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

    public:
        // Parses the profile-local [OpenNV Compatibility] command map.  Each
        // comma-separated entry is `script-command:capability`; unsupported
        // capabilities are deliberately ignored.
        static std::map<std::string, std::string, std::less<>> parseAuthoredCompatibilityCommandMappings(
            std::string_view mappings);

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
