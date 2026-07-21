#include "projectilemanager.hpp"

#include <cmath>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>

#include <osg/PositionAttitudeTransform>

#include <components/debug/debuglog.hpp>

#include <components/esm3/esmwriter.hpp>
#include <components/esm3/loadench.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadrace.hpp>
#include <components/esm3/projectilestate.hpp>

#include <components/esm4/loadproj.hpp>

#include <components/esm/quaternion.hpp>
#include <components/esm/vector3.hpp>

#include <components/misc/constants.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/resourcehelpers.hpp>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>

#include <components/sceneutil/controller.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/visitor.hpp>

#include <components/settings/values.hpp>

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/manualref.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/combat.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/spellcasting.hpp"
#include "../mwmechanics/weapontype.hpp"

#include "../mwrender/animation.hpp"
#include "../mwrender/renderingmanager.hpp"
#include "../mwrender/util.hpp"
#include "../mwrender/vismask.hpp"

#include "../mwsound/sound.hpp"

#include "../mwphysics/physicssystem.hpp"
#include "../mwphysics/projectile.hpp"

//## VR_PATCH BEGIN
#include <components/vr/vr.hpp>
#include "../mwvr/vrutil.hpp"
#include "../mwvr/openxrinput.hpp"

//## VR_PATCH END
namespace
{
    ESM::EffectList getMagicBoltData(std::vector<ESM::RefId>& projectileIDs, std::set<ESM::RefId>& sounds, float& speed,
        std::string& texture, std::string& sourceName, const ESM::RefId& id)
    {
        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();
        const ESM::EffectList* effects;
        if (const ESM::Spell* spell = esmStore.get<ESM::Spell>().search(id)) // check if it's a spell
        {
            sourceName = spell->mName;
            effects = &spell->mEffects;
        }
        else // check if it's an enchanted item
        {
            MWWorld::ManualRef ref(esmStore, id);
            MWWorld::Ptr ptr = ref.getPtr();
            const ESM::Enchantment* ench = esmStore.get<ESM::Enchantment>().find(ptr.getClass().getEnchantment(ptr));
            sourceName = ptr.getClass().getName(ptr);
            effects = &ench->mEffects;
        }

        int count = 0;
        speed = 0.0f;
        ESM::EffectList projectileEffects;
        for (const ESM::IndexedENAMstruct& effect : effects->mList)
        {
            const ESM::MagicEffect* magicEffect
                = MWBase::Environment::get().getESMStore()->get<ESM::MagicEffect>().find(effect.mData.mEffectID);

            // Speed of multi-effect projectiles should be the average of the constituent effects,
            // based on observation of the original engine.
            speed += magicEffect->mData.mSpeed;
            count++;

            if (effect.mData.mRange != ESM::RT_Target)
                continue;

            if (magicEffect->mBolt.empty())
                projectileIDs.emplace_back(ESM::RefId::stringRefId("VFX_DefaultBolt"));
            else
                projectileIDs.push_back(magicEffect->mBolt);

            if (!magicEffect->mBoltSound.empty())
                sounds.emplace(magicEffect->mBoltSound);
            else
                sounds.emplace(MWBase::Environment::get()
                                   .getESMStore()
                                   ->get<ESM::Skill>()
                                   .find(magicEffect->mData.mSchool)
                                   ->mSchool->mBoltSound);
            projectileEffects.mList.push_back(effect);
        }

        if (count != 0)
            speed /= count;

        // the particle texture is only used if there is only one projectile
        if (projectileEffects.mList.size() == 1)
        {
            const ESM::MagicEffect* magicEffect
                = MWBase::Environment::get().getESMStore()->get<ESM::MagicEffect>().find(
                    effects->mList.begin()->mData.mEffectID);
            texture = magicEffect->mParticle;
        }

        // insert a VFX_Multiple projectile if there are multiple projectile effects
        if (projectileEffects.mList.size() > 1)
        {
            const ESM::RefId projectileId
                = ESM::RefId::stringRefId("VFX_Multiple" + std::to_string(effects->mList.size()));
            projectileIDs.insert(projectileIDs.begin(), projectileId);
        }

        return projectileEffects;
    }

    osg::Vec4 getMagicBoltLightDiffuseColor(const ESM::EffectList& effects)
    {
        // Calculate combined light diffuse color from magical effects
        osg::Vec4 lightDiffuseColor;
        for (const ESM::IndexedENAMstruct& enam : effects.mList)
        {
            const ESM::MagicEffect* magicEffect
                = MWBase::Environment::get().getESMStore()->get<ESM::MagicEffect>().find(enam.mData.mEffectID);
            lightDiffuseColor += magicEffect->getColor();
        }
        int numberOfEffects = effects.mList.size();
        lightDiffuseColor /= numberOfEffects;

        return lightDiffuseColor;
    }
}

namespace MWWorld
{

    ProjectileManager::ProjectileManager(osg::Group* parent, Resource::ResourceSystem* resourceSystem,
        MWRender::RenderingManager* rendering, MWPhysics::PhysicsSystem* physics)
        : mParent(parent)
        , mResourceSystem(resourceSystem)
        , mRendering(rendering)
        , mPhysics(physics)
        , mCleanupTimer(0.0f)
    {
    }

    /// Rotates an osg::PositionAttitudeTransform over time.
    class RotateCallback : public SceneUtil::NodeCallback<RotateCallback, osg::PositionAttitudeTransform*>
    {
    public:
        RotateCallback(const osg::Vec3f& axis = osg::Vec3f(0, -1, 0), float rotateSpeed = osg::PI * 2)
            : mAxis(axis)
            , mRotateSpeed(rotateSpeed)
        {
        }

        void operator()(osg::PositionAttitudeTransform* node, osg::NodeVisitor* nv)
        {
            double time = nv->getFrameStamp()->getSimulationTime();

            osg::Quat orient = osg::Quat(time * mRotateSpeed, mAxis);
            node->setAttitude(orient);

            traverse(node, nv);
        }

    private:
        osg::Vec3f mAxis;
        float mRotateSpeed;
    };

    void ProjectileManager::createModel(State& state, VFS::Path::NormalizedView model, const osg::Vec3f& pos,
        const osg::Quat& orient, bool rotate, bool createLight, osg::Vec4 lightDiffuseColor, const std::string& texture)
    {
        state.mNode = new osg::PositionAttitudeTransform;
        state.mNode->setNodeMask(MWRender::Mask_Effect);
        state.mNode->setPosition(pos);
        state.mNode->setAttitude(orient);

        osg::Group* attachTo = state.mNode;

        if (rotate)
        {
            osg::ref_ptr<osg::PositionAttitudeTransform> rotateNode(new osg::PositionAttitudeTransform);
            rotateNode->addUpdateCallback(new RotateCallback());
            state.mNode->addChild(rotateNode);
            attachTo = rotateNode;
        }

        osg::ref_ptr<osg::Node> projectile = mResourceSystem->getSceneManager()->getInstance(model, attachTo);

        if (state.mIdMagic.size() > 1)
        {
            for (size_t iter = 1; iter != state.mIdMagic.size(); ++iter)
            {
                std::ostringstream nodeName;
                nodeName << "Dummy" << std::setw(2) << std::setfill('0') << iter;
                const ESM::Weapon* weapon
                    = MWBase::Environment::get().getESMStore()->get<ESM::Weapon>().find(state.mIdMagic.at(iter));
                std::string nameToFind = nodeName.str();
                SceneUtil::FindByNameVisitor findVisitor(nameToFind);
                attachTo->accept(findVisitor);
                if (findVisitor.mFoundNode)
                    mResourceSystem->getSceneManager()->getInstance(
                        Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(weapon->mModel)),
                        findVisitor.mFoundNode);
            }
        }

        if (createLight)
        {
            osg::ref_ptr<osg::Light> projectileLight(new osg::Light);
            projectileLight->setAmbient(osg::Vec4(1.0f, 1.0f, 1.0f, 1.0f));
            projectileLight->setDiffuse(lightDiffuseColor);
            projectileLight->setSpecular(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f));
            projectileLight->setConstantAttenuation(0.f);
            projectileLight->setLinearAttenuation(0.1f);
            projectileLight->setQuadraticAttenuation(0.f);
            projectileLight->setPosition(osg::Vec4(pos, 1.0));

            SceneUtil::LightSource* projectileLightSource = new SceneUtil::LightSource;
            projectileLightSource->setNodeMask(MWRender::Mask_Lighting);
            projectileLightSource->setRadius(66.f);

            state.mNode->addChild(projectileLightSource);
            projectileLightSource->setLight(projectileLight);
        }

        state.mNode->addCullCallback(new SceneUtil::LightListCallback);

        mParent->addChild(state.mNode);

        state.mEffectAnimationTime = std::make_shared<MWRender::EffectAnimationTime>();

        SceneUtil::AssignControllerSourcesVisitor assignVisitor(state.mEffectAnimationTime);
        state.mNode->accept(assignVisitor);

        MWRender::overrideFirstRootTexture(texture, mResourceSystem, *projectile);
    }

    void ProjectileManager::update(State& state, float duration)
    {
        state.mEffectAnimationTime->addTime(duration);
    }

    void ProjectileManager::launchMagicBolt(
        const ESM::RefId& spellId, const Ptr& caster, const osg::Vec3f& fallbackDirection, ESM::RefNum item)
    {
        osg::Vec3f pos = caster.getRefData().getPosition().asVec3();
        if (caster.getClass().isActor())
        {
            // Note: we ignore the collision box offset, this is required to make some flying creatures work as
            // intended.
            pos.z() += mPhysics->getRenderingHalfExtents(caster).z() * 2 * Constants::TorsoHeight;
        }

        // Actors can't cast target spells underwater
        if (caster.getClass().isActor() && MWBase::Environment::get().getWorld()->isUnderwater(caster.getCell(), pos))
            return;

        osg::Quat orient;
//## VR_PATCH BEGIN
        if (VR::getVR() && !VR::getKBMouseModeActive()
            && caster == MWBase::Environment::get().getWorld()->getPlayerPtr())
        {
            auto tp = MWVR::OpenXRInput::instance().getSpace(VR::getPreferredAimPath())->locateInWorld();
            if (!!tp.status)
            {
                pos = tp.pose.position.asMWUnits();
                orient = tp.pose.orientation;
            }
            // TODO:
            //MWVR::Util::getWeaponPose()
            //Stereo::Pose weaponPose = MWBase::Environment::get().getWorld()->getVRWeaponPose();
            //pos = weaponPose.position.asMWUnits();
            //orient = weaponPose.orientation;
        }
        else
            if (caster.getClass().isActor())
//## VR_PATCH END
            orient = osg::Quat(caster.getRefData().getPosition().rot[0], osg::Vec3f(-1, 0, 0))
                * osg::Quat(caster.getRefData().getPosition().rot[2], osg::Vec3f(0, 0, -1));
        else
            orient.makeRotate(osg::Vec3f(0, 1, 0), osg::Vec3f(fallbackDirection));

        MagicBoltState state;
        state.mSpellId = spellId;
        state.mCasterHandle = caster;
        state.mItem = item;
        if (caster.getClass().isActor())
            state.mActorId = caster.getClass().getCreatureStats(caster).getActorId();
        else
            state.mActorId = -1;

        std::string texture;

        state.mEffects = getMagicBoltData(
            state.mIdMagic, state.mSoundIds, state.mSpeed, texture, state.mSourceName, state.mSpellId);

        // Non-projectile should have been removed by getMagicBoltData
        if (state.mEffects.mList.empty())
            return;

        if (!caster.getClass().isActor() && fallbackDirection.length2() <= 0)
        {
            Log(Debug::Warning) << "Unable to launch magic bolt (direction to target is empty)";
            return;
        }

        MWWorld::ManualRef ref(*MWBase::Environment::get().getESMStore(), state.mIdMagic.at(0));
        MWWorld::Ptr ptr = ref.getPtr();

        osg::Vec4 lightDiffuseColor = getMagicBoltLightDiffuseColor(state.mEffects);

        VFS::Path::Normalized model = ptr.getClass().getCorrectedModel(ptr);
        createModel(state, model, pos, orient, true, true, lightDiffuseColor, texture);

        MWBase::SoundManager* sndMgr = MWBase::Environment::get().getSoundManager();
        for (const auto& soundid : state.mSoundIds)
        {
            MWBase::Sound* sound
                = sndMgr->playSound3D(pos, soundid, 1.0f, 1.0f, MWSound::Type::Sfx, MWSound::PlayMode::Loop);
            if (sound)
                state.mSounds.push_back(sound);
        }

        // in case there are multiple effects, the model is a dummy without geometry. Use the second effect for physics
        // shape
        if (state.mIdMagic.size() > 1)
        {
            model = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(
                MWBase::Environment::get().getESMStore()->get<ESM::Weapon>().find(state.mIdMagic[1])->mModel));
        }
        state.mProjectileId = mPhysics->addProjectile(caster, pos, model, true);
        state.mToDelete = false;
        mMagicBolts.push_back(std::move(state));
    }

    void ProjectileManager::launchProjectile(const Ptr& actor, const ConstPtr& projectile, const osg::Vec3f& pos,
        const osg::Quat& orient, const Ptr& bow, float speed, float attackStrength)
    {
        ProjectileState state;
        state.mActorId = actor.getClass().getCreatureStats(actor).getActorId();
        state.mBowId = bow.getCellRef().getRefId();
        state.mVelocity = orient * osg::Vec3f(0, 1, 0) * speed;
        state.mIdArrow = projectile.getCellRef().getRefId();
        state.mCasterHandle = actor;
        state.mAttackStrength = attackStrength;
        int type = projectile.get<ESM::Weapon>()->mBase->mData.mType;
        state.mThrown = MWMechanics::getWeaponType(type)->mWeaponClass == ESM::WeaponType::Thrown;

        MWWorld::ManualRef ref(*MWBase::Environment::get().getESMStore(), projectile.getCellRef().getRefId());
        MWWorld::Ptr ptr = ref.getPtr();

        const VFS::Path::Normalized model = ptr.getClass().getCorrectedModel(ptr);
        createModel(state, model, pos, orient, false, false, osg::Vec4(0, 0, 0, 0));
        if (!ptr.getClass().getEnchantment(ptr).empty())
            SceneUtil::addEnchantedGlow(state.mNode, mResourceSystem, ptr.getClass().getEnchantmentColor(ptr));

        state.mProjectileId = mPhysics->addProjectile(actor, pos, model, false);
        state.mToDelete = false;
        mProjectiles.push_back(std::move(state));
    }

    bool ProjectileManager::launchFalloutProjectile(const Ptr& actor, ESM::FormId projectileId,
        const osg::Vec3f& pos, const osg::Vec3f& requestedDirection,
        const MWMechanics::FalloutProjectileImpactContract& impact)
    {
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        const ESM4::Projectile* projectile
            = store != nullptr ? store->get<ESM4::Projectile>().search(projectileId) : nullptr;
        osg::Vec3f direction = requestedDirection;
        if (projectile == nullptr || !projectile->mData.present || projectile->mModel.empty()
            || !std::isfinite(projectile->mData.speed) || projectile->mData.speed <= 0.f
            || !std::isfinite(projectile->mData.range) || projectile->mData.range <= 0.f
            || !std::isfinite(projectile->mData.gravity) || projectile->mData.gravity < 0.f
            || direction.normalize() == 0.f)
            return false;
        const bool hasExplosion = (projectile->mData.flags & ESM4::Projectile::Explosion) != 0;
        if (hasExplosion != !impact.mExplosion.isZeroOrUnset()
            || (hasExplosion && impact.mExplosion != projectile->mData.explosion)
            || !std::isfinite(impact.mExplosionDamageMultiplier)
            || impact.mExplosionDamageMultiplier < 0.f)
            return false;

        FalloutProjectileState state;
        state.mActorId = actor.getClass().getCreatureStats(actor).getActorId();
        state.mCasterHandle = actor;
        state.mProjectile = projectileId;
        state.mVelocity = direction * projectile->mData.speed;
        state.mRotationVelocity = osg::Vec3f(projectile->mData.rotationX,
            projectile->mData.rotationY, projectile->mData.rotationZ);
        state.mPreviousPosition = pos;
        state.mGravity = projectile->mData.gravity;
        state.mMaximumRange = projectile->mData.range;
        state.mDistanceTravelled = 0.f;
        state.mElapsedTime = 0.f;
        state.mBounceCount = 0;
        state.mRotates = (projectile->mData.flags & ESM4::Projectile::Rotates) != 0;
        state.mSettled = false;
        state.mDetonate = false;
        state.mImpact = impact;
        state.mToDelete = false;

        osg::Quat orient;
        orient.makeRotate(osg::Vec3f(0.f, 1.f, 0.f), direction);
        const VFS::Path::Normalized model
            = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(projectile->mModel));
        createModel(state, model, pos, orient, false, false, osg::Vec4(0.f, 0.f, 0.f, 0.f));
        state.mProjectileId
            = mPhysics->addProjectile(actor, pos, model, projectile->mData.type == ESM4::Projectile::Lobber);
        mFalloutProjectiles.push_back(std::move(state));

        Log(Debug::Info) << "FNV moving projectile launched: actor=" << actor.toString()
                         << " projectile=" << ESM::RefId::formIdRefId(projectileId)
                         << " speed=" << projectile->mData.speed << " gravity=" << projectile->mData.gravity
                         << " range=" << projectile->mData.range << " rawDamage=" << impact.mRawDamage;
        return true;
    }

    void ProjectileManager::updateCasters()
    {
        for (auto& state : mProjectiles)
            mPhysics->setCaster(state.mProjectileId, state.getCaster());
        for (auto& state : mFalloutProjectiles)
            mPhysics->setCaster(state.mProjectileId, state.getCaster());

        for (auto& state : mMagicBolts)
        {
            // casters are identified by actor id in the savegame. objects doesn't have one so they can't be identified
            // back.
            // TODO: should object-type caster be restored from savegame?
            if (state.mActorId == -1)
                continue;

            auto caster = state.getCaster();
            if (caster.isEmpty())
            {
                Log(Debug::Error) << "Couldn't find caster with ID " << state.mActorId;
                cleanupMagicBolt(state);
                continue;
            }
            mPhysics->setCaster(state.mProjectileId, caster);
        }
    }

    void ProjectileManager::update(float dt)
    {
        periodicCleanup(dt);
        moveProjectiles(dt);
        moveFalloutProjectiles(dt);
        moveMagicBolts(dt);
    }

    void ProjectileManager::periodicCleanup(float dt)
    {
        mCleanupTimer -= dt;
        if (mCleanupTimer <= 0.0f)
        {
            mCleanupTimer = 2.0f;

            auto isCleanable = [](const ProjectileManager::State& state) -> bool {
                const float farawayThreshold = 72000.0f;
                osg::Vec3 playerPos = MWMechanics::getPlayer().getRefData().getPosition().asVec3();
                return (state.mNode->getPosition() - playerPos).length2() >= farawayThreshold * farawayThreshold;
            };

            for (auto& projectileState : mProjectiles)
            {
                if (isCleanable(projectileState))
                    cleanupProjectile(projectileState);
            }
            for (auto& projectileState : mFalloutProjectiles)
            {
                if (isCleanable(projectileState))
                    projectileState.mToDelete = true;
            }

            for (auto& magicBoltState : mMagicBolts)
            {
                if (isCleanable(magicBoltState))
                    cleanupMagicBolt(magicBoltState);
            }
        }
    }

    void ProjectileManager::moveMagicBolts(float duration)
    {
        const bool normaliseRaceSpeed = Settings::game().mNormaliseRaceSpeed;
        for (auto& magicBoltState : mMagicBolts)
        {
            if (magicBoltState.mToDelete)
                continue;

            auto* projectile = mPhysics->getProjectile(magicBoltState.mProjectileId);
            if (!projectile->isActive())
                continue;
            // If the actor caster is gone, the magic bolt needs to be removed from the scene during the next frame.
            MWWorld::Ptr caster = magicBoltState.getCaster();
            if (!caster.isEmpty() && caster.getClass().isActor())
            {
                if (caster.getCellRef().getCount() <= 0 || caster.getClass().getCreatureStats(caster).isDead())
                {
                    cleanupMagicBolt(magicBoltState);
                    continue;
                }
            }

            const auto& store = *MWBase::Environment::get().getESMStore();
            osg::Quat orient = magicBoltState.mNode->getAttitude();
            static float fTargetSpellMaxSpeed
                = store.get<ESM::GameSetting>().find("fTargetSpellMaxSpeed")->mValue.getFloat();
            float speed = fTargetSpellMaxSpeed * magicBoltState.mSpeed;
            if (!normaliseRaceSpeed && !caster.isEmpty() && caster.getClass().isNpc())
            {
                const auto npc = caster.get<ESM::NPC>()->mBase;
                const auto race = store.get<ESM::Race>().find(npc->mRace);
                speed *= npc->isMale() ? race->mData.mMaleWeight : race->mData.mFemaleWeight;
            }
            osg::Vec3f direction = orient * osg::Vec3f(0, 1, 0);
            direction.normalize();
            projectile->setVelocity(direction * speed);

            update(magicBoltState, duration);

            for (const auto& sound : magicBoltState.mSounds)
            {
                sound->setVelocity(direction * speed);
            }

            // For AI actors, get combat targets to use in the ray cast. Only those targets will return a positive hit
            // result.
            std::vector<MWWorld::Ptr> targetActors;
            if (!caster.isEmpty() && caster.getClass().isActor() && caster != MWMechanics::getPlayer())
                caster.getClass().getCreatureStats(caster).getAiSequence().getCombatTargets(targetActors);
            projectile->setValidTargets(targetActors);
        }
    }

    void ProjectileManager::moveProjectiles(float duration)
    {
        for (auto& projectileState : mProjectiles)
        {
            if (projectileState.mToDelete)
                continue;

            auto* projectile = mPhysics->getProjectile(projectileState.mProjectileId);
            if (!projectile->isActive())
                continue;
            // gravity constant - must be way lower than the gravity affecting actors, since we're not
            // simulating aerodynamics at all
            projectileState.mVelocity
                -= osg::Vec3f(0, 0, Constants::GravityConst * Constants::UnitsPerMeter * 0.1f) * duration;

            projectile->setVelocity(projectileState.mVelocity);

            // rotation does not work well for throwing projectiles - their roll angle will depend on shooting
            // direction.
            if (!projectileState.mThrown)
            {
                osg::Quat orient;
                orient.makeRotate(osg::Vec3f(0, 1, 0), projectileState.mVelocity);
                projectileState.mNode->setAttitude(orient);
            }

            update(projectileState, duration);

            MWWorld::Ptr caster = projectileState.getCaster();

            // For AI actors, get combat targets to use in the ray cast. Only those targets will return a positive hit
            // result.
            std::vector<MWWorld::Ptr> targetActors;
            if (!caster.isEmpty() && caster.getClass().isActor() && caster != MWMechanics::getPlayer())
                caster.getClass().getCreatureStats(caster).getAiSequence().getCombatTargets(targetActors);
            projectile->setValidTargets(targetActors);
        }
    }

    void ProjectileManager::moveFalloutProjectiles(float duration)
    {
        for (FalloutProjectileState& state : mFalloutProjectiles)
        {
            if (state.mToDelete)
                continue;
            const ESM4::Projectile* authored
                = MWBase::Environment::get().getESMStore()->get<ESM4::Projectile>().search(state.mProjectile);
            if (authored == nullptr || !authored->mData.present)
            {
                state.mToDelete = true;
                continue;
            }
            state.mElapsedTime += duration;
            const bool timedAlternateTrigger
                = (authored->mData.flags & ESM4::Projectile::Explosion) != 0
                && (authored->mData.flags & ESM4::Projectile::AlternateTrigger) != 0
                && (authored->mData.flags & ESM4::Projectile::Detonates) == 0
                && authored->mData.alternateProximity <= 0.f
                && std::isfinite(authored->mData.alternateTimer)
                && authored->mData.alternateTimer > 0.f;
            if (timedAlternateTrigger && state.mElapsedTime >= authored->mData.alternateTimer)
                state.mDetonate = true;
            if (state.mDetonate || state.mSettled)
                continue;
            MWPhysics::Projectile* projectile = mPhysics->getProjectile(state.mProjectileId);
            if (!projectile->isActive())
                continue;

            state.mVelocity -= osg::Vec3f(0.f, 0.f,
                Constants::GravityConst * Constants::UnitsPerMeter * 0.1f * state.mGravity * duration);
            projectile->setVelocity(state.mVelocity);

            if (state.mRotates)
            {
                const osg::Quat rotation(state.mRotationVelocity.x() * duration, osg::Vec3f(1.f, 0.f, 0.f),
                    state.mRotationVelocity.y() * duration, osg::Vec3f(0.f, 1.f, 0.f),
                    state.mRotationVelocity.z() * duration, osg::Vec3f(0.f, 0.f, 1.f));
                state.mNode->setAttitude(state.mNode->getAttitude() * rotation);
            }
            else if (state.mVelocity.length2() > 0.f)
            {
                osg::Quat orient;
                orient.makeRotate(osg::Vec3f(0.f, 1.f, 0.f), state.mVelocity);
                state.mNode->setAttitude(orient);
            }
            update(state, duration);

            MWWorld::Ptr caster = state.getCaster();
            std::vector<MWWorld::Ptr> targetActors;
            if (!caster.isEmpty() && caster.getClass().isActor() && caster != MWMechanics::getPlayer())
                caster.getClass().getCreatureStats(caster).getAiSequence().getCombatTargets(targetActors);
            projectile->setValidTargets(targetActors);
        }
    }

    bool ProjectileManager::bounceFalloutProjectile(FalloutProjectileState& state,
        const ESM4::Projectile& projectile, const osg::Vec3f& hitPosition, const osg::Vec3f& hitNormal)
    {
        MWMechanics::FalloutProjectileBounceFailure failure
            = MWMechanics::FalloutProjectileBounceFailure::None;
        const std::optional<osg::Vec3f> reflected = MWMechanics::resolveFalloutProjectileBounce(
            state.mVelocity, hitNormal, projectile.mData.bounciness, failure);
        if (!reflected)
        {
            Log(Debug::Error) << "FNV lobber bounce rejected: projectile="
                              << ESM::RefId::formIdRefId(state.mProjectile)
                              << " reason=" << MWMechanics::getFalloutProjectileBounceFailureName(failure);
            return false;
        }

        ++state.mBounceCount;
        constexpr float settleSpeed = 15.f;
        constexpr std::uint8_t maximumBounces = 24;
        osg::Vec3f normal = hitNormal;
        normal.normalize();
        if (reflected->length() <= settleSpeed || state.mBounceCount >= maximumBounces)
        {
            state.mVelocity.set(0.f, 0.f, 0.f);
            state.mPreviousPosition = hitPosition;
            state.mNode->setPosition(hitPosition);
            state.mSettled = true;
            Log(Debug::Info) << "FNV lobber settled: projectile="
                             << ESM::RefId::formIdRefId(state.mProjectile)
                             << " bounces=" << static_cast<unsigned int>(state.mBounceCount)
                             << " elapsed=" << state.mElapsedTime;
            return true;
        }

        const osg::Vec3f restartPosition = hitPosition + normal * 2.f;
        const VFS::Path::Normalized model
            = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(projectile.mModel));
        mPhysics->removeProjectile(state.mProjectileId);
        state.mProjectileId = mPhysics->addProjectile(
            state.getCaster(), restartPosition, model, true);
        state.mVelocity = *reflected;
        state.mPreviousPosition = restartPosition;
        state.mNode->setPosition(restartPosition);
        mPhysics->getProjectile(state.mProjectileId)->setVelocity(state.mVelocity);
        Log(Debug::Info) << "FNV lobber bounced: projectile="
                         << ESM::RefId::formIdRefId(state.mProjectile)
                         << " bounciness=" << projectile.mData.bounciness
                         << " speed=" << state.mVelocity.length()
                         << " bounces=" << static_cast<unsigned int>(state.mBounceCount)
                         << " elapsed=" << state.mElapsedTime;
        return true;
    }

    void ProjectileManager::processHits()
    {
        for (auto& projectileState : mProjectiles)
        {
            if (projectileState.mToDelete)
                continue;

            auto* projectile = mPhysics->getProjectile(projectileState.mProjectileId);

            const auto pos = projectile->getSimulationPosition();
            projectileState.mNode->setPosition(pos);

            if (projectile->isActive())
                continue;

            const auto target = projectile->getTarget();
            auto caster = projectileState.getCaster();
            assert(target != caster);

            if (caster.isEmpty())
                caster = target;

            // Try to get a Ptr to the bow that was used. It might no longer exist.
            MWWorld::ManualRef projectileRef(*MWBase::Environment::get().getESMStore(), projectileState.mIdArrow);
            MWWorld::Ptr bow = projectileRef.getPtr();
            if (!caster.isEmpty() && projectileState.mIdArrow != projectileState.mBowId)
            {
                MWWorld::InventoryStore& inv = caster.getClass().getInventoryStore(caster);
                MWWorld::ContainerStoreIterator invIt = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
                if (invIt != inv.end() && invIt->getCellRef().getRefId() == projectileState.mBowId)
                    bow = *invIt;
            }

            const auto hitPosition = Misc::Convert::toOsg(projectile->getHitPosition());

            if (projectile->getHitWater())
                mRendering->emitWaterRipple(hitPosition);

            MWMechanics::projectileHit(
                caster, target, bow, projectileRef.getPtr(), hitPosition, projectileState.mAttackStrength);
            projectileState.mToDelete = true;
        }
        for (FalloutProjectileState& projectileState : mFalloutProjectiles)
        {
            if (projectileState.mToDelete)
                continue;

            MWPhysics::Projectile* projectile = mPhysics->getProjectile(projectileState.mProjectileId);
            const osg::Vec3f position = projectile->getSimulationPosition();
            projectileState.mNode->setPosition(position);
            if (projectileState.mDetonate)
            {
                const MWWorld::Ptr caster = projectileState.getCaster();
                if (caster.isEmpty()
                    || !MWBase::Environment::get().getMechanicsManager()->executeFalloutExplosion(
                        caster, position, projectileState.mImpact))
                {
                    Log(Debug::Error) << "FNV timed projectile explosion rejected: caster="
                                      << (caster.isEmpty() ? std::string("none") : caster.toString())
                                      << " projectile="
                                      << ESM::RefId::formIdRefId(projectileState.mProjectile)
                                      << " explosion="
                                      << ESM::RefId::formIdRefId(projectileState.mImpact.mExplosion);
                }
                projectileState.mToDelete = true;
                continue;
            }
            if (projectileState.mSettled)
                continue;
            projectileState.mDistanceTravelled += (position - projectileState.mPreviousPosition).length();
            if (projectileState.mDistanceTravelled >= projectileState.mMaximumRange)
            {
                projectileState.mToDelete = true;
                continue;
            }

            if (projectile->isActive())
            {
                projectileState.mPreviousPosition = position;
                continue;
            }

            const MWWorld::Ptr target = projectile->getTarget();
            const MWWorld::Ptr caster = projectileState.getCaster();
            const osg::Vec3f hitPosition = Misc::Convert::toOsg(projectile->getHitPosition());
            if (projectile->getHitWater())
                mRendering->emitWaterRipple(hitPosition);
            if (!caster.isEmpty() && !target.isEmpty() && target.getClass().isActor())
            {
                const bool applied = MWBase::Environment::get().getMechanicsManager()
                    ->executeFalloutProjectileImpact(caster, target, projectileState.mPreviousPosition,
                        hitPosition, projectileState.mImpact);
                if (!applied)
                    Log(Debug::Error) << "FNV moving projectile impact rejected: caster=" << caster.toString()
                                      << " target=" << target.toString()
                                      << " projectile="
                                      << ESM::RefId::formIdRefId(projectileState.mProjectile);
            }
            const ESM4::Projectile* authoredProjectile
                = MWBase::Environment::get().getESMStore()->get<ESM4::Projectile>().search(
                    projectileState.mProjectile);
            const bool persistentLobber = authoredProjectile != nullptr
                && authoredProjectile->mData.type == ESM4::Projectile::Lobber
                && ((authoredProjectile->mData.flags & ESM4::Projectile::AlternateTrigger) != 0
                    || (authoredProjectile->mData.flags & ESM4::Projectile::Detonates) != 0);
            if (persistentLobber)
            {
                const osg::Vec3f hitNormal = Misc::Convert::toOsg(projectile->getHitNormal());
                if (!bounceFalloutProjectile(
                        projectileState, *authoredProjectile, hitPosition, hitNormal))
                    projectileState.mToDelete = true;
                continue;
            }
            const bool detonateOnImpact = authoredProjectile != nullptr
                && (authoredProjectile->mData.flags & ESM4::Projectile::Explosion) != 0
                && (authoredProjectile->mData.flags & ESM4::Projectile::AlternateTrigger) == 0
                && (authoredProjectile->mData.flags & ESM4::Projectile::Detonates) == 0
                && !projectileState.mImpact.mExplosion.isZeroOrUnset();
            if (detonateOnImpact)
            {
                if (caster.isEmpty()
                    || !MWBase::Environment::get().getMechanicsManager()->executeFalloutExplosion(
                        caster, hitPosition, projectileState.mImpact))
                {
                    Log(Debug::Error) << "FNV moving projectile explosion rejected: caster="
                                      << (caster.isEmpty() ? std::string("none") : caster.toString())
                                      << " projectile="
                                      << ESM::RefId::formIdRefId(projectileState.mProjectile)
                                      << " explosion="
                                      << ESM::RefId::formIdRefId(projectileState.mImpact.mExplosion);
                }
            }
            projectileState.mToDelete = true;
        }
        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();
        for (auto& magicBoltState : mMagicBolts)
        {
            if (magicBoltState.mToDelete)
                continue;

            auto* projectile = mPhysics->getProjectile(magicBoltState.mProjectileId);

            const auto pos = projectile->getSimulationPosition();
            magicBoltState.mNode->setPosition(pos);
            for (const auto& sound : magicBoltState.mSounds)
                sound->setPosition(pos);

            const Ptr caster = magicBoltState.getCaster();

            const MWBase::World& world = *MWBase::Environment::get().getWorld();
            const bool active = projectile->isActive();
            if (active && !world.isUnderwater(caster.getCell(), pos))
                continue;

            const Ptr target = !active ? projectile->getTarget() : Ptr();

            assert(target != caster);

            MWMechanics::CastSpell cast(caster, target);
            cast.mHitPosition = !active ? Misc::Convert::toOsg(projectile->getHitPosition()) : pos;
            cast.mId = magicBoltState.mSpellId;
            cast.mSourceName = magicBoltState.mSourceName;
            cast.mItem = magicBoltState.mItem;
            // Grab original effect list so the indices are correct
            const ESM::EffectList* effects;
            if (const ESM::Spell* spell = esmStore.get<ESM::Spell>().search(magicBoltState.mSpellId))
                effects = &spell->mEffects;
            else
            {
                MWWorld::ManualRef ref(esmStore, magicBoltState.mSpellId);
                const MWWorld::Ptr& ptr = ref.getPtr();
                effects = &esmStore.get<ESM::Enchantment>().find(ptr.getClass().getEnchantment(ptr))->mEffects;
            }
            cast.inflict(target, *effects, ESM::RT_Target);

            magicBoltState.mToDelete = true;
        }

        for (auto& projectileState : mProjectiles)
        {
            if (projectileState.mToDelete)
                cleanupProjectile(projectileState);
        }
        for (FalloutProjectileState& projectileState : mFalloutProjectiles)
        {
            if (projectileState.mToDelete)
                cleanupFalloutProjectile(projectileState);
        }

        for (auto& magicBoltState : mMagicBolts)
        {
            if (magicBoltState.mToDelete)
                cleanupMagicBolt(magicBoltState);
        }
        mProjectiles.erase(std::remove_if(mProjectiles.begin(), mProjectiles.end(),
                               [](const State& state) { return state.mToDelete; }),
            mProjectiles.end());
        mFalloutProjectiles.erase(std::remove_if(mFalloutProjectiles.begin(), mFalloutProjectiles.end(),
                                      [](const State& state) { return state.mToDelete; }),
            mFalloutProjectiles.end());
        mMagicBolts.erase(
            std::remove_if(mMagicBolts.begin(), mMagicBolts.end(), [](const State& state) { return state.mToDelete; }),
            mMagicBolts.end());
    }

    void ProjectileManager::cleanupProjectile(ProjectileManager::ProjectileState& state)
    {
        mParent->removeChild(state.mNode);
        mPhysics->removeProjectile(state.mProjectileId);
        state.mToDelete = true;
    }

    void ProjectileManager::cleanupFalloutProjectile(ProjectileManager::FalloutProjectileState& state)
    {
        mParent->removeChild(state.mNode);
        mPhysics->removeProjectile(state.mProjectileId);
        state.mToDelete = true;
    }

    void ProjectileManager::cleanupMagicBolt(ProjectileManager::MagicBoltState& state)
    {
        mParent->removeChild(state.mNode);
        mPhysics->removeProjectile(state.mProjectileId);
        state.mToDelete = true;
        for (size_t soundIter = 0; soundIter != state.mSounds.size(); soundIter++)
        {
            MWBase::Environment::get().getSoundManager()->stopSound(state.mSounds.at(soundIter));
        }
    }

    void ProjectileManager::clear()
    {
        for (auto& mProjectile : mProjectiles)
            cleanupProjectile(mProjectile);
        mProjectiles.clear();

        for (FalloutProjectileState& projectile : mFalloutProjectiles)
            cleanupFalloutProjectile(projectile);
        mFalloutProjectiles.clear();

        for (auto& mMagicBolt : mMagicBolts)
            cleanupMagicBolt(mMagicBolt);
        mMagicBolts.clear();
    }

    void ProjectileManager::write(ESM::ESMWriter& writer, Loading::Listener& progress) const
    {
        for (std::vector<ProjectileState>::const_iterator it = mProjectiles.begin(); it != mProjectiles.end(); ++it)
        {
            writer.startRecord(ESM::REC_PROJ);

            ESM::ProjectileState state;
            state.mId = it->mIdArrow;
            state.mPosition = ESM::Vector3(osg::Vec3f(it->mNode->getPosition()));
            state.mOrientation = ESM::Quaternion(osg::Quat(it->mNode->getAttitude()));
            state.mActorId = it->mActorId;

            state.mBowId = it->mBowId;
            state.mVelocity = it->mVelocity;
            state.mAttackStrength = it->mAttackStrength;

            state.save(writer);

            writer.endRecord(ESM::REC_PROJ);
        }

        for (const FalloutProjectileState& projectile : mFalloutProjectiles)
        {
            writer.startRecord(ESM::REC_FPRJ);

            ESM::FalloutProjectileState state;
            state.mId = ESM::RefId::formIdRefId(projectile.mProjectile);
            state.mPosition = ESM::Vector3(osg::Vec3f(projectile.mNode->getPosition()));
            state.mOrientation = ESM::Quaternion(osg::Quat(projectile.mNode->getAttitude()));
            state.mActorId = projectile.mActorId;
            state.mVelocity = projectile.mVelocity;
            state.mRotationVelocity = projectile.mRotationVelocity;
            state.mPreviousPosition = projectile.mPreviousPosition;
            state.mGravity = projectile.mGravity;
            state.mMaximumRange = projectile.mMaximumRange;
            state.mDistanceTravelled = projectile.mDistanceTravelled;
            state.mElapsedTime = projectile.mElapsedTime;
            state.mBounceCount = projectile.mBounceCount;
            state.mWeapon = ESM::RefId::formIdRefId(projectile.mImpact.mWeapon);
            if (!projectile.mImpact.mExplosion.isZeroOrUnset())
                state.mExplosion = ESM::RefId::formIdRefId(projectile.mImpact.mExplosion);
            state.mRawDamage = projectile.mImpact.mRawDamage;
            state.mLimbDamageMultiplier = projectile.mImpact.mLimbDamageMultiplier;
            state.mExplosionDamageMultiplier = projectile.mImpact.mExplosionDamageMultiplier;
            if (projectile.mRotates)
                state.mFlags |= ESM::FalloutProjectileState::Rotates;
            if (projectile.mImpact.mCritical)
                state.mFlags |= ESM::FalloutProjectileState::Critical;
            if (projectile.mImpact.mVatsTargetHit)
                state.mFlags |= ESM::FalloutProjectileState::VatsTargetHit;
            if (projectile.mSettled)
                state.mFlags |= ESM::FalloutProjectileState::Settled;
            if (projectile.mDetonate)
                state.mFlags |= ESM::FalloutProjectileState::Detonate;
            for (ESM::FormId effect : projectile.mImpact.mAmmoEffects)
                state.mAmmoEffects.push_back(ESM::RefId::formIdRefId(effect));

            if (projectile.mImpact.mVatsAction)
            {
                state.mFlags |= ESM::FalloutProjectileState::HasVatsAction;
                const MWMechanics::FalloutVatsQueuedAction& action = *projectile.mImpact.mVatsAction;
                state.mVats.mTarget = ESM::RefId::formIdRefId(action.mTarget);
                state.mVats.mBodyPart = action.mBodyPart;
                state.mVats.mDisplayedHitChance = action.mDisplayedHitChance;
                state.mVats.mHealthPercent = action.mHealthPercent;
                state.mVats.mActorValue = action.mActorValue;
                state.mVats.mActionPointCost = action.mActionPointCost;
                state.mVats.mHealthDamageMultiplier = action.mHealthDamageMultiplier;
                state.mVats.mLimbDamageMultiplier = action.mLimbDamageMultiplier;
                state.mVats.mBodyPartName = action.mBodyPartName;
                state.mVats.mTargetNode = action.mTargetNode;
            }

            state.save(writer);
            writer.endRecord(ESM::REC_FPRJ);
        }

        for (std::vector<MagicBoltState>::const_iterator it = mMagicBolts.begin(); it != mMagicBolts.end(); ++it)
        {
            writer.startRecord(ESM::REC_MPRJ);

            ESM::MagicBoltState state;
            state.mId = it->mIdMagic.at(0);
            state.mPosition = ESM::Vector3(osg::Vec3f(it->mNode->getPosition()));
            state.mOrientation = ESM::Quaternion(osg::Quat(it->mNode->getAttitude()));
            state.mActorId = it->mActorId;
            state.mItem = it->mItem;
            state.mSpellId = it->mSpellId;
            state.mSpeed = it->mSpeed;

            state.save(writer);

            writer.endRecord(ESM::REC_MPRJ);
        }
    }

    bool ProjectileManager::readRecord(ESM::ESMReader& reader, uint32_t type)
    {
        if (type == ESM::REC_PROJ)
        {
            ESM::ProjectileState esm;
            esm.load(reader);

            ProjectileState state;
            state.mActorId = esm.mActorId;
            state.mBowId = esm.mBowId;
            state.mVelocity = esm.mVelocity;
            state.mIdArrow = esm.mId;
            state.mAttackStrength = esm.mAttackStrength;
            state.mToDelete = false;

            VFS::Path::Normalized model;
            try
            {
                MWWorld::ManualRef ref(*MWBase::Environment::get().getESMStore(), esm.mId);
                MWWorld::Ptr ptr = ref.getPtr();
                model = ptr.getClass().getCorrectedModel(ptr);
                int weaponType = ptr.get<ESM::Weapon>()->mBase->mData.mType;
                state.mThrown = MWMechanics::getWeaponType(weaponType)->mWeaponClass == ESM::WeaponType::Thrown;

                state.mProjectileId
                    = mPhysics->addProjectile(state.getCaster(), osg::Vec3f(esm.mPosition), model, false);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "Failed to add projectile for " << esm.mId
                                    << " while reading projectile record: " << e.what();
                return true;
            }

            createModel(state, model, osg::Vec3f(esm.mPosition), osg::Quat(esm.mOrientation), false, false,
                osg::Vec4(0, 0, 0, 0));

            mProjectiles.push_back(std::move(state));
            return true;
        }
        if (type == ESM::REC_FPRJ)
        {
            ESM::FalloutProjectileState esm;
            esm.load(reader);

            const ESM::FormId* projectileId = esm.mId.getIf<ESM::FormId>();
            const ESM::FormId* weaponId = esm.mWeapon.getIf<ESM::FormId>();
            const auto finiteVector = [](const ESM::Vector3& value) {
                return std::isfinite(value.mValues[0]) && std::isfinite(value.mValues[1])
                    && std::isfinite(value.mValues[2]);
            };
            if (projectileId == nullptr || weaponId == nullptr || !finiteVector(esm.mPosition)
                || !finiteVector(esm.mVelocity) || !finiteVector(esm.mRotationVelocity)
                || !finiteVector(esm.mPreviousPosition) || !std::isfinite(esm.mGravity) || esm.mGravity < 0.f
                || !std::isfinite(esm.mMaximumRange) || esm.mMaximumRange <= 0.f
                || !std::isfinite(esm.mDistanceTravelled) || esm.mDistanceTravelled < 0.f
                || esm.mDistanceTravelled >= esm.mMaximumRange || !std::isfinite(esm.mRawDamage)
                || esm.mRawDamage < 0.f || !std::isfinite(esm.mLimbDamageMultiplier)
                || esm.mLimbDamageMultiplier < 0.f || !std::isfinite(esm.mExplosionDamageMultiplier)
                || esm.mExplosionDamageMultiplier < 0.f || !std::isfinite(esm.mElapsedTime)
                || esm.mElapsedTime < 0.f)
            {
                Log(Debug::Warning) << "Rejected malformed native Fallout projectile save state";
                return true;
            }

            FalloutProjectileState state;
            state.mActorId = esm.mActorId;
            state.mProjectile = *projectileId;
            state.mVelocity = esm.mVelocity;
            state.mRotationVelocity = esm.mRotationVelocity;
            state.mPreviousPosition = esm.mPreviousPosition;
            state.mGravity = esm.mGravity;
            state.mMaximumRange = esm.mMaximumRange;
            state.mDistanceTravelled = esm.mDistanceTravelled;
            state.mElapsedTime = esm.mElapsedTime;
            state.mBounceCount = esm.mBounceCount;
            state.mRotates = (esm.mFlags & ESM::FalloutProjectileState::Rotates) != 0;
            state.mSettled = (esm.mFlags & ESM::FalloutProjectileState::Settled) != 0;
            state.mDetonate = (esm.mFlags & ESM::FalloutProjectileState::Detonate) != 0;
            state.mImpact.mWeapon = *weaponId;
            if (!esm.mExplosion.empty())
            {
                const ESM::FormId* explosionId = esm.mExplosion.getIf<ESM::FormId>();
                if (explosionId == nullptr)
                {
                    Log(Debug::Warning) << "Rejected native Fallout projectile save state with malformed explosion";
                    return true;
                }
                state.mImpact.mExplosion = *explosionId;
            }
            state.mImpact.mRawDamage = esm.mRawDamage;
            state.mImpact.mLimbDamageMultiplier = esm.mLimbDamageMultiplier;
            state.mImpact.mExplosionDamageMultiplier = esm.mExplosionDamageMultiplier;
            state.mImpact.mCritical = (esm.mFlags & ESM::FalloutProjectileState::Critical) != 0;
            state.mImpact.mVatsTargetHit = (esm.mFlags & ESM::FalloutProjectileState::VatsTargetHit) != 0;
            state.mToDelete = false;

            for (const ESM::RefId& effect : esm.mAmmoEffects)
            {
                const ESM::FormId* effectId = effect.getIf<ESM::FormId>();
                if (effectId == nullptr)
                {
                    Log(Debug::Warning) << "Rejected native Fallout projectile save state with malformed ammo effect";
                    return true;
                }
                state.mImpact.mAmmoEffects.push_back(*effectId);
            }

            if ((esm.mFlags & ESM::FalloutProjectileState::HasVatsAction) != 0)
            {
                const ESM::FormId* targetId = esm.mVats.mTarget.getIf<ESM::FormId>();
                if (targetId == nullptr || esm.mVats.mBodyPartName.empty() || esm.mVats.mTargetNode.empty()
                    || !std::isfinite(esm.mVats.mActionPointCost) || esm.mVats.mActionPointCost < 0.f
                    || !std::isfinite(esm.mVats.mHealthDamageMultiplier)
                    || esm.mVats.mHealthDamageMultiplier < 0.f
                    || !std::isfinite(esm.mVats.mLimbDamageMultiplier)
                    || esm.mVats.mLimbDamageMultiplier < 0.f)
                {
                    Log(Debug::Warning) << "Rejected native Fallout projectile save state with malformed VATS action";
                    return true;
                }
                MWMechanics::FalloutVatsQueuedAction action;
                action.mTarget = *targetId;
                action.mBodyPart = esm.mVats.mBodyPart;
                action.mDisplayedHitChance = esm.mVats.mDisplayedHitChance;
                action.mHealthPercent = esm.mVats.mHealthPercent;
                action.mActorValue = esm.mVats.mActorValue;
                action.mActionPointCost = esm.mVats.mActionPointCost;
                action.mHealthDamageMultiplier = esm.mVats.mHealthDamageMultiplier;
                action.mLimbDamageMultiplier = esm.mVats.mLimbDamageMultiplier;
                action.mBodyPartName = std::move(esm.mVats.mBodyPartName);
                action.mTargetNode = std::move(esm.mVats.mTargetNode);
                state.mImpact.mVatsAction = std::move(action);
            }

            const ESM4::Projectile* projectile
                = MWBase::Environment::get().getESMStore()->get<ESM4::Projectile>().search(state.mProjectile);
            if (projectile == nullptr || !projectile->mData.present || projectile->mModel.empty())
            {
                Log(Debug::Warning) << "Failed to resolve native Fallout projectile " << esm.mId
                                    << " while reading projectile record";
                return true;
            }
            const bool hasExplosion = (projectile->mData.flags & ESM4::Projectile::Explosion) != 0;
            if (hasExplosion != !state.mImpact.mExplosion.isZeroOrUnset()
                || (hasExplosion && state.mImpact.mExplosion != projectile->mData.explosion))
            {
                Log(Debug::Warning) << "Rejected native Fallout projectile save state with mismatched explosion";
                return true;
            }

            const VFS::Path::Normalized model
                = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(projectile->mModel));
            createModel(state, model, osg::Vec3f(esm.mPosition), osg::Quat(esm.mOrientation), false, false,
                osg::Vec4(0.f, 0.f, 0.f, 0.f));
            state.mProjectileId
                = mPhysics->addProjectile(state.getCaster(), osg::Vec3f(esm.mPosition), model,
                    projectile->mData.type == ESM4::Projectile::Lobber);
            mFalloutProjectiles.push_back(std::move(state));
            return true;
        }
        if (type == ESM::REC_MPRJ)
        {
            ESM::MagicBoltState esm;
            esm.load(reader);

            MagicBoltState state;
            state.mIdMagic.push_back(esm.mId);
            state.mSpellId = esm.mSpellId;
            state.mActorId = esm.mActorId;
            state.mToDelete = false;
            state.mItem = esm.mItem;
            std::string texture;

            try
            {
                state.mEffects = getMagicBoltData(
                    state.mIdMagic, state.mSoundIds, state.mSpeed, texture, state.mSourceName, state.mSpellId);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "Failed to recreate magic projectile for " << esm.mId << " and spell "
                                    << state.mSpellId << " while reading projectile record: " << e.what();
                return true;
            }

            state.mSpeed = esm.mSpeed; // speed is derived from non-projectile effects as well as
                                       // projectile effects, so we can't calculate it from the save
                                       // file's effect list, which is already trimmed of non-projectile
                                       // effects. We need to use the stored value.

            VFS::Path::Normalized model;
            try
            {
                MWWorld::ManualRef ref(*MWBase::Environment::get().getESMStore(), state.mIdMagic.at(0));
                MWWorld::Ptr ptr = ref.getPtr();
                model = ptr.getClass().getCorrectedModel(ptr);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "Failed to get model for " << state.mIdMagic.at(0)
                                    << " while reading projectile record: " << e.what();
                return true;
            }

            osg::Vec4 lightDiffuseColor = getMagicBoltLightDiffuseColor(state.mEffects);
            createModel(state, model, osg::Vec3f(esm.mPosition), osg::Quat(esm.mOrientation), true, true,
                lightDiffuseColor, texture);
            state.mProjectileId = mPhysics->addProjectile(state.getCaster(), osg::Vec3f(esm.mPosition), model, true);

            MWBase::SoundManager* sndMgr = MWBase::Environment::get().getSoundManager();
            for (const auto& soundid : state.mSoundIds)
            {
                MWBase::Sound* sound = sndMgr->playSound3D(
                    esm.mPosition, soundid, 1.0f, 1.0f, MWSound::Type::Sfx, MWSound::PlayMode::Loop);
                if (sound)
                    state.mSounds.push_back(sound);
            }

            mMagicBolts.push_back(std::move(state));
            return true;
        }

        return false;
    }

    int ProjectileManager::countSavedGameRecords() const
    {
        return mMagicBolts.size() + mProjectiles.size() + mFalloutProjectiles.size();
    }

    MWWorld::Ptr ProjectileManager::State::getCaster()
    {
        if (!mCasterHandle.isEmpty())
            return mCasterHandle;

        return MWBase::Environment::get().getWorld()->searchPtrViaActorId(mActorId);
    }

}
