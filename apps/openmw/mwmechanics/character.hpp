#ifndef GAME_MWMECHANICS_CHARACTER_HPP
#define GAME_MWMECHANICS_CHARACTER_HPP

#include <cstdint>
#include <deque>
#include <optional>

#include <components/esm3/loadweap.hpp>

#include "../mwworld/ptr.hpp"

#include "../mwrender/animation.hpp"

#include "falloutcombat.hpp"

namespace ESM4
{
    struct Weapon;
}

namespace MWWorld
{
    class InventoryStore;
}

namespace MWRender
{
    class Animation;
    enum class FonvWeaponAction : std::uint8_t;
}

namespace MWMechanics
{

    struct Movement;
    class CreatureStats;

    enum Priority
    {
        Priority_Default,
        Priority_WeaponLowerBody,
        Priority_SneakIdleLowerBody,
        Priority_SwimIdle,
        Priority_Jump,
        Priority_Movement,
        Priority_Hit,
        Priority_Weapon,
        Priority_Block,
        Priority_Knockdown,
        Priority_Torch,
        Priority_Storm,
        Priority_Death,
        Priority_Scripted,

        Num_Priorities
    };

    enum CharacterState
    {
        CharState_None,

        CharState_SpecialIdle,
        CharState_Idle,
        CharState_IdleSwim,
        CharState_IdleSneak,

        CharState_WalkForward,
        CharState_WalkBack,
        CharState_WalkLeft,
        CharState_WalkRight,

        CharState_SwimWalkForward,
        CharState_SwimWalkBack,
        CharState_SwimWalkLeft,
        CharState_SwimWalkRight,

        CharState_RunForward,
        CharState_RunBack,
        CharState_RunLeft,
        CharState_RunRight,

        CharState_SwimRunForward,
        CharState_SwimRunBack,
        CharState_SwimRunLeft,
        CharState_SwimRunRight,

        CharState_SneakForward,
        CharState_SneakBack,
        CharState_SneakLeft,
        CharState_SneakRight,

        CharState_TurnLeft,
        CharState_TurnRight,
        CharState_SwimTurnLeft,
        CharState_SwimTurnRight,

        CharState_Death1,
        CharState_Death2,
        CharState_Death3,
        CharState_Death4,
        CharState_Death5,
        CharState_SwimDeath,
        CharState_SwimDeathKnockDown,
        CharState_SwimDeathKnockOut,
        CharState_DeathKnockDown,
        CharState_DeathKnockOut,

        CharState_Hit,
        CharState_SwimHit,
        CharState_KnockDown,
        CharState_KnockOut,
        CharState_SwimKnockDown,
        CharState_SwimKnockOut,
        CharState_Block
    };

    enum class UpperBodyState
    {
        None,
        Equipping,
        Unequipping,
        WeaponEquipped,
        AttackWindUp,
        AttackRelease,
        AttackEnd,
        Casting
    };

    enum JumpingState
    {
        JumpState_None,
        JumpState_InAir,
        JumpState_Landing
    };

    struct WeaponInfo;

    class CharacterController : public MWRender::Animation::TextKeyListener
    {
        MWWorld::Ptr mPtr;
        MWWorld::Ptr mWeapon;
        const ESM4::Weapon* mFalloutWeapon = nullptr;
        FalloutTriggerState mFalloutTriggerState;
        MWRender::Animation* mAnimation;

        struct AnimationQueueEntry
        {
            std::string mGroup;
            uint32_t mLoopCount;
            float mTime;
            bool mLooping;
            bool mScripted;
            std::string mStartKey;
            std::string mStopKey;
            float mSpeed;
        };
        typedef std::deque<AnimationQueueEntry> AnimationQueue;
        AnimationQueue mAnimQueue;
        bool mLuaAnimations{ false };

        CharacterState mIdleState{ CharState_None };
        std::string mCurrentIdle;
        std::string mLastMissingIdleAnimation;

        CharacterState mMovementState{ CharState_None };
        std::string mCurrentMovement;
        std::string mLastMissingMovementAnimation;
        float mMovementAnimSpeed{ 0.f };
        bool mAdjustMovementAnimSpeed{ false };
        bool mMovementAnimationHasMovement{ false };

        CharacterState mDeathState{ CharState_None };
        std::string mCurrentDeath;
        bool mFloatToSurface{ true };

        CharacterState mHitState{ CharState_None };
        std::string mCurrentHit;

        UpperBodyState mUpperBodyState{ UpperBodyState::None };

        JumpingState mJumpState{ JumpState_None };
        std::string mCurrentJump;
        bool mInJump{ false };

        int mWeaponType{ ESM::Weapon::None };
        std::string mCurrentWeapon;

        float mAttackStrength{ -1.f };
        bool mReadyToHit{ false };
        MWWorld::Ptr mAttackVictim;
        osg::Vec3f mAttackHitPos;
        bool mAttackSuccess{ false };

        bool mSkipAnim{ false };

        // counted for skill increase
        float mSecondsOfSwimming{ 0.f };
        float mSecondsOfRunning{ 0.f };

        MWWorld::ConstPtr mHeadTrackTarget;

        float mTurnAnimationThreshold{
            0.f
        }; // how long to continue playing turning animation after actor stopped turning

        std::string mAttackType; // slash, chop or thrust

        bool mCanCast{ false };

        bool mCastingScriptedSpell{ false };

        bool mIsMovingBackward{ false };
        osg::Vec2f mSmoothedSpeed;

        std::string_view getMovementBasedAttackType() const;

        void clearStateAnimation(std::string& anim) const;
        void resetCurrentJumpState();
        void resetCurrentMovementState();
        void resetCurrentIdleState();
        void resetCurrentHitState();
        void resetCurrentWeaponState();
        void resetCurrentDeathState();

        void refreshCurrentAnims(CharacterState idle, CharacterState movement, JumpingState jump, bool force = false);
        void refreshHitRecoilAnims();
        void refreshJumpAnims(JumpingState jump, bool force = false);
        void refreshMovementAnims(CharacterState movement, bool force = false);
        void refreshIdleAnims(CharacterState idle, bool force = false);

        bool updateWeaponState(float duration);
        bool updateFalloutWeaponState(int requestedWeaponType, bool weaponChanged,
            const ESM4::Weapon* requestedWeapon, const MWRender::AnimPriority& priorityWeapon, float duration);
        bool fireFalloutWeapon(const MWWorld::Ptr& vatsTarget = MWWorld::Ptr(),
            const std::optional<osg::Vec3f>& vatsAimPoint = std::nullopt,
            const FalloutVatsQueuedAction* vatsAction = nullptr, bool vatsTargetHit = true);
        bool strikeFalloutMelee(std::uint8_t animationType);
        void updateIdleStormState(bool inwater) const;

        std::string chooseRandomAttackAnimation() const;
        static bool isRandomAttackAnimation(std::string_view group);
        bool isLegacyRandomAttackAnimation(std::string_view group) const;

        bool isMovementAnimationControlled() const;

        void updateAnimQueue();
        void playAnimQueue(bool useLoopStart = false);

        void updateHeadTracking(float duration);

        void updateMagicEffects() const;

        void playDeath(float startpoint, CharacterState death);
        CharacterState chooseRandomDeathState() const;
        void playRandomDeath(float startpoint = 0.0f);

        /// choose a random animation group with \a prefix and numeric suffix
        /// @param num if non-nullptr, the chosen animation number will be written here
        std::string chooseRandomGroup(const std::string& prefix, int* num = nullptr) const;

        bool updateCarriedLeftVisible(int weaptype) const;

        std::string fallbackShortWeaponGroup(
            const std::string& baseGroupName, MWRender::Animation::BlendMask* blendMask = nullptr) const;

        std::string_view getWeaponAnimation(int weaponType);
        std::string_view getWeaponShortGroup(int weaponType) const;
        std::string_view getFalloutWeaponActionGroup(int weaponType, MWRender::FonvWeaponAction action);
        bool playFalloutWeaponAction(
            int weaponType, MWRender::FonvWeaponAction action, const MWRender::AnimPriority& priorityWeapon);
        bool restoreFalloutPrimaryWeaponGroup(int weaponType);

        bool getAttackingOrSpell() const;
        void setAttackingOrSpell(bool attackingOrSpell) const;

        std::string_view getDesiredAttackType() const;

        void prepareHit();

        void unpersistAnimationState();

        void playBlendedAnimation(const std::string& groupname, const MWRender::AnimPriority& priority, int blendMask,
            bool autodisable, float speedmult, std::string_view start, std::string_view stop, float startpoint,
            uint32_t loops, bool loopfallback = false) const;

    public:
        CharacterController(const MWWorld::Ptr& ptr, MWRender::Animation& anim);
        virtual ~CharacterController();

        CharacterController(const CharacterController&) = delete;
        CharacterController(CharacterController&&) = delete;

        void detachAnimation();

        const MWWorld::Ptr& getPtr() const { return mPtr; }

        void handleTextKey(std::string_view groupname, SceneUtil::TextKeyMap::ConstIterator key,
            const SceneUtil::TextKeyMap& map) override;

        // Be careful when to call this, see comment in Actors
        void updateContinuousVfx() const;

        void updatePtr(const MWWorld::Ptr& ptr);

        void update(float duration);

        bool onOpen() const;
        void onClose() const;

        void persistAnimationState() const;
        bool playGroup(std::string_view groupname, int mode, uint32_t count, bool scripted = false);
        bool playGroupLua(std::string_view groupname, float speed, std::string_view startKey, std::string_view stopKey,
            uint32_t loops, bool forceLoop);
        std::string getAnimationGroupFromSource(
            std::string_view sourceName, std::string_view groupPrefix = {}) const;
        bool setFalloutAnimatedObject(std::string_view model, std::string_view activeGroup);
        void enableLuaAnimations(bool enable);
        void skipAnim();
        bool isAnimPlaying(std::string_view groupName) const;
        bool isScriptedAnimPlaying() const;
        void clearAnimQueue(bool clearScriptedAnims = false);

        enum KillResult
        {
            Result_DeathAnimStarted,
            Result_DeathAnimPlaying,
            Result_DeathAnimJustFinished,
            Result_DeathAnimFinished
        };
        KillResult kill();

        void resurrect();
        bool isDead() const { return mDeathState != CharState_None; }

        void forceStateUpdate();

        bool isAttackPreparing() const;
        bool isCastingSpell() const;
        bool isReadyToBlock() const;
        bool isKnockedDown() const;
        bool isKnockedOut() const;
        bool isRecovery() const;
        bool isSneaking() const;
        bool isRunning() const;
        bool isTurning() const;
        bool isAttackingOrSpell() const;

        void setVisibility(float visibility) const;
        void castSpell(const ESM::RefId& spellId, bool scriptedSpell = false);
        void setAIAttackType(std::string_view attackType);
        static std::string_view getRandomAttackType();

        bool readyToPrepareAttack() const;
        bool readyToStartAttack() const;

        /// Execute an already queued and resolved VATS ranged hit through the ordinary Fallout weapon path. The
        /// caller owns chance resolution and supplies the selected body-part target point and damage multiplier.
        bool executeFalloutVatsRangedHit(
            const MWWorld::Ptr& target, const osg::Vec3f& targetPoint,
            const FalloutVatsQueuedAction& action, bool targetHit);

        bool executeFalloutProjectileImpact(const MWWorld::Ptr& target, const osg::Vec3f& segmentStart,
            const osg::Vec3f& hitPosition, const FalloutProjectileImpactContract& impact);
        bool executeFalloutExplosion(
            const osg::Vec3f& position, const FalloutProjectileImpactContract& impact);

        float calculateWindUp() const;

        float getAttackStrength() const;

        /// @see Animation::setActive
        void setActive(int active) const;

        /// Make this character turn its head towards \a target. To turn off head tracking, pass an empty Ptr.
        void setHeadTrackTarget(const MWWorld::ConstPtr& target);

        /// Apply the current target while gameplay simulation is paused by dialogue.
        void updateDialogueHeadTracking(float duration);

        void playSwishSound() const;

        float getAnimationMovementDirection() const;

        MWWorld::MovementDirectionFlags getSupportedMovementDirections() const;
    };
}

#endif /* GAME_MWMECHANICS_CHARACTER_HPP */
