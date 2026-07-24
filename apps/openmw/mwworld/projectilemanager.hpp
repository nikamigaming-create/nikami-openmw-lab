#ifndef OPENMW_MWWORLD_PROJECTILEMANAGER_H
#define OPENMW_MWWORLD_PROJECTILEMANAGER_H

#include <string>

#include <osg/PositionAttitudeTransform>
#include <osg/ref_ptr>

#include <components/esm3/effectlist.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwbase/soundmanager.hpp"
#include "../mwmechanics/falloutcombat.hpp"

#include "ptr.hpp"

namespace MWPhysics
{
    class PhysicsSystem;
}

namespace Loading
{
    class Listener;
}

namespace ESM4
{
    struct Projectile;
}

namespace osg
{
    class Group;
    class Quat;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MWRender
{
    class EffectAnimationTime;
    class RenderingManager;
}

namespace MWWorld
{

    class ProjectileManager
    {
    public:
        ProjectileManager(osg::Group* parent, Resource::ResourceSystem* resourceSystem,
            MWRender::RenderingManager* rendering, MWPhysics::PhysicsSystem* physics);

        /// If caster is an actor, the actor's facing orientation is used. Otherwise fallbackDirection is used.
        void launchMagicBolt(const ESM::RefId& spellId, const MWWorld::Ptr& caster, const osg::Vec3f& fallbackDirection,
            ESM::RefNum item);

        void launchProjectile(const MWWorld::Ptr& actor, const MWWorld::ConstPtr& projectile, const osg::Vec3f& pos,
            const osg::Quat& orient, const MWWorld::Ptr& bow, float speed, float attackStrength);

        bool launchFalloutProjectile(const MWWorld::Ptr& actor, ESM::FormId projectile,
            const osg::Vec3f& pos, const osg::Vec3f& direction,
            const MWMechanics::FalloutProjectileImpactContract& impact);
        bool launchFalloutHitscanTracer(
            ESM::FormId projectile, const osg::Vec3f& origin, const osg::Vec3f& destination);

        /// Count queued V.A.T.S. projectiles that must resolve before the cinematic transaction can finish.
        /// Persistent mines/remote explosives are excluded because their authored lifetime is open-ended.
        std::size_t countPendingFalloutVatsProjectiles(const MWWorld::Ptr& actor);

        /// Arm the detonation state for every settled, remotely triggered Fallout explosive placed by actor.
        /// Returns the number of charges accepted by the authored PROJ Detonates contract.
        unsigned int detonateFalloutPlacedExplosives(const MWWorld::Ptr& actor);

        void updateCasters();

        void update(float dt);

        void processHits();

        /// Removes all current projectiles. Should be called when switching to a new worldspace.
        void clear();

        void write(ESM::ESMWriter& writer, Loading::Listener& progress) const;
        bool readRecord(ESM::ESMReader& reader, uint32_t type);
        int countSavedGameRecords() const;

    private:
        osg::ref_ptr<osg::Group> mParent;
        Resource::ResourceSystem* mResourceSystem;
        MWRender::RenderingManager* mRendering;
        MWPhysics::PhysicsSystem* mPhysics;
        float mCleanupTimer;

        struct State
        {
            osg::ref_ptr<osg::PositionAttitudeTransform> mNode;
            std::shared_ptr<MWRender::EffectAnimationTime> mEffectAnimationTime;

            int mActorId;
            int mProjectileId;

            // TODO: this will break when the game is saved and reloaded, since there is currently
            // no way to write identifiers for non-actors to a savegame.
            MWWorld::Ptr mCasterHandle;

            MWWorld::Ptr getCaster();

            // MW-ids of a magic projectile
            std::vector<ESM::RefId> mIdMagic;

            // MW-id of an arrow projectile
            ESM::RefId mIdArrow;

            bool mToDelete;
        };

        struct MagicBoltState : public State
        {
            ESM::RefId mSpellId;

            // Name of item to display as effect source in magic menu (in case we casted an enchantment)
            std::string mSourceName;

            ESM::EffectList mEffects;

            float mSpeed;
            // Refnum of the casting item
            ESM::RefNum mItem;

            std::vector<MWBase::Sound*> mSounds;
            std::set<ESM::RefId> mSoundIds;
        };

        struct ProjectileState : public State
        {
            // RefID of the bow or crossbow the actor was using when this projectile was fired (may be empty)
            ESM::RefId mBowId;

            osg::Vec3f mVelocity;
            float mAttackStrength;
            bool mThrown;
        };

        struct FalloutProjectileState : public State
        {
            ESM::FormId mProjectile;
            osg::Vec3f mVelocity;
            osg::Vec3f mRotationVelocity;
            osg::Vec3f mPreviousPosition;
            float mGravity = 0.f;
            float mMaximumRange = 0.f;
            float mDistanceTravelled = 0.f;
            float mElapsedTime = 0.f;
            std::uint8_t mBounceCount = 0;
            bool mRotates = false;
            bool mSettled = false;
            bool mDetonate = false;
            bool mArmed = false;
            MWMechanics::FalloutProjectileImpactContract mImpact;
        };

        struct FalloutHitscanTracerState : public State
        {
            osg::Vec3f mOrigin;
            osg::Vec3f mDestination;
            float mElapsedTime = 0.f;
            float mLifetime = 0.f;
        };

        std::vector<MagicBoltState> mMagicBolts;
        std::vector<ProjectileState> mProjectiles;
        std::vector<FalloutProjectileState> mFalloutProjectiles;
        std::vector<FalloutHitscanTracerState> mFalloutHitscanTracers;

        void cleanupProjectile(ProjectileState& state);
        void cleanupFalloutProjectile(FalloutProjectileState& state);
        void cleanupFalloutHitscanTracer(FalloutHitscanTracerState& state);
        void cleanupMagicBolt(MagicBoltState& state);
        void periodicCleanup(float dt);

        void moveProjectiles(float dt);
        void moveFalloutProjectiles(float dt);
        void moveFalloutHitscanTracers(float dt);
        bool bounceFalloutProjectile(FalloutProjectileState& state, const ESM4::Projectile& projectile,
            const osg::Vec3f& hitPosition, const osg::Vec3f& hitNormal);
        void moveMagicBolts(float dt);

        void createModel(State& state, VFS::Path::NormalizedView model, const osg::Vec3f& pos, const osg::Quat& orient,
            bool rotate, bool createLight, osg::Vec4 lightDiffuseColor, const std::string& texture = "");
        void update(State& state, float duration);

        void operator=(const ProjectileManager&);
        ProjectileManager(const ProjectileManager&);
    };

}

#endif
