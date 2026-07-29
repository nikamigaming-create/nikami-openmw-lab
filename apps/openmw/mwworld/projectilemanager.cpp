#include "projectilemanager.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>

#include <osg/PositionAttitudeTransform>
#include <osg/BlendFunc>
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/LineWidth>

#include <components/debug/debuglog.hpp>

#include <components/esm3/actoridconverter.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/loadench.hpp>
#include <components/esm3/loadgmst.hpp>
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
#include "../mwworld/cellstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/manualref.hpp"
#include "../mwworld/worldmodel.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/combat.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/spellcasting.hpp"

#include "../mwrender/animation.hpp"
#include "../mwrender/renderingmanager.hpp"
#include "../mwrender/util.hpp"
#include "../mwrender/vismask.hpp"

#include "../mwsound/sound.hpp"

#include "../mwphysics/physicssystem.hpp"
#include "../mwphysics/projectile.hpp"

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
        size_t numberOfEffects = effects.mList.size();
        lightDiffuseColor /= static_cast<float>(numberOfEffects);

        return lightDiffuseColor;
    }

    osg::Quat lookAt(const osg::Vec3f& pos)
    {
        // Rotate the forward vector towards the position (used for gravity-affected projectiles)
        // Can't use Quat::makeRotate as the shortest angle contains undesirable local roll
        const float dist = pos.length();
        if (dist < 1e-4f)
            return {};

        const osg::Vec3f dir = pos / dist;
        osg::Vec3f right = dir ^ osg::Z_AXIS;
        if (right.normalize() < 1e-4f)
            right = osg::X_AXIS;

        const osg::Vec3f up = right ^ dir;

        osg::Matrixf mat(right.x(), right.y(), right.z(), 0.f, dir.x(), dir.y(), dir.z(), 0.f, up.x(), up.y(), up.z(),
            0.f, 0.f, 0.f, 0.f, 1.f);

        osg::Quat orient;
        orient.set(mat);
        return orient;
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

        MWRender::overrideFirstRootTexture(VFS::Path::toNormalized(texture), mResourceSystem, *projectile);
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
        if (caster.getClass().isActor())
            orient = osg::Quat(caster.getRefData().getPosition().rot[0], osg::Vec3f(-1, 0, 0))
                * osg::Quat(caster.getRefData().getPosition().rot[2], osg::Vec3f(0, 0, -1));
        else
            orient.makeRotate(osg::Vec3f(0, 1, 0), osg::Vec3f(fallbackDirection));

        MagicBoltState state;
        state.mSpellId = spellId;
        state.mCasterHandle = caster;
        state.mItem = item;
        MWBase::Environment::get().getWorldModel()->registerPtr(caster);
        state.mCaster = caster.getCellRef().getRefNum();

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
        state.mCaster = actor.getCellRef().getRefNum();
        state.mBowId = bow.getCellRef().getRefId();
        state.mVelocity = orient * osg::Vec3f(0, 1, 0) * speed;
        state.mIdArrow = projectile.getCellRef().getRefId();
        state.mCasterHandle = actor;
        state.mAttackStrength = attackStrength;

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
            || impact.mExplosionDamageMultiplier < 0.f || !std::isfinite(impact.mProjectileSkill)
            || impact.mProjectileSkill < 0.f)
            return false;

        ProjectileState state;
        state.mFallout = std::make_unique<FalloutProjectileData>();
        FalloutProjectileData& fallout = *state.mFallout;
        state.mCaster = actor.getCellRef().getRefNum();
        state.mCasterHandle = actor;
        fallout.mProjectile = projectileId;
        state.mVelocity = direction * projectile->mData.speed;
        fallout.mRotationVelocity = osg::Vec3f(projectile->mData.rotationX,
            projectile->mData.rotationY, projectile->mData.rotationZ);
        fallout.mPreviousPosition = pos;
        fallout.mGravity = projectile->mData.gravity;
        fallout.mMaximumRange = projectile->mData.range;
        fallout.mDistanceTravelled = 0.f;
        fallout.mElapsedTime = 0.f;
        fallout.mBounceCount = 0;
        fallout.mRotates = (projectile->mData.flags & ESM4::Projectile::Rotates) != 0;
        fallout.mSettled = false;
        fallout.mDetonate = false;
        fallout.mArmed = false;
        fallout.mImpact = impact;
        state.mToDelete = false;

        osg::Quat orient;
        orient.makeRotate(osg::Vec3f(0.f, 1.f, 0.f), direction);
        const VFS::Path::Normalized model
            = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(projectile->mModel));
        createModel(state, model, pos, orient, false, false, osg::Vec4(0.f, 0.f, 0.f, 0.f));
        state.mProjectileId
            = mPhysics->addProjectile(actor, pos, model, projectile->mData.type == ESM4::Projectile::Lobber);
        mProjectiles.push_back(std::move(state));

        Log(Debug::Info) << "FNV moving projectile launched: actor=" << actor.toString()
                         << " projectile=" << ESM::RefId::formIdRefId(projectileId)
                         << " speed=" << projectile->mData.speed << " gravity=" << projectile->mData.gravity
                         << " range=" << projectile->mData.range << " rawDamage=" << impact.mRawDamage;
        return true;
    }

    bool ProjectileManager::launchFalloutHitscanTracer(
        ESM::FormId projectileId, const osg::Vec3f& origin, const osg::Vec3f& destination)
    {
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        const ESM4::Projectile* projectile
            = store != nullptr ? store->get<ESM4::Projectile>().search(projectileId) : nullptr;
        osg::Vec3f direction = destination - origin;
        const float distance = direction.length();
        if (projectile == nullptr || !std::isfinite(distance) || distance <= 0.f || direction.normalize() == 0.f)
            return false;

        FalloutHitscanTracerState state;
        state.mProjectileId = -1;
        state.mOrigin = origin;
        state.mDestination = destination;
        state.mElapsedTime = 0.f;
        // Keep an authored hitscan round visible for several frames without
        // changing its instantaneous gameplay hit resolution.
        state.mLifetime = std::clamp(distance / 30000.f, 0.055f, 0.12f);
        state.mToDelete = false;

        osg::Quat orientation;
        orientation.makeRotate(osg::Vec3f(0.f, 1.f, 0.f), direction);
        // Hitscan PROJ records are allowed to omit a world model: the impact is instantaneous and the visible
        // streak is presentation owned by the firing weapon. Requiring MODL here silently suppressed the generated
        // tracer for ordinary firearms even though the shot, damage, muzzle flash and impact had all succeeded.
        state.mNode = new osg::PositionAttitudeTransform;
        state.mNode->setNodeMask(MWRender::Mask_Effect);
        state.mNode->setPosition(origin);
        state.mNode->setAttitude(orientation);
        state.mNode->addCullCallback(new SceneUtil::LightListCallback);
        mParent->addChild(state.mNode);
        state.mEffectAnimationTime = std::make_shared<MWRender::EffectAnimationTime>();

        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back(osg::Vec3f(0.f, -42.f, 0.f));
        vertices->push_back(osg::Vec3f(0.f, 8.f, 0.f));
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        colors->push_back(osg::Vec4f(2.4f, 1.05f, 0.25f, 0.95f));
        osg::ref_ptr<osg::Geometry> streak = new osg::Geometry;
        streak->setVertexArray(vertices);
        streak->setColorArray(colors, osg::Array::BIND_OVERALL);
        streak->addPrimitiveSet(new osg::DrawArrays(GL_LINES, 0, 2));
        osg::StateSet* stateSet = streak->getOrCreateStateSet();
        stateSet->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        stateSet->setMode(GL_BLEND, osg::StateAttribute::ON);
        stateSet->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE), osg::StateAttribute::ON);
        stateSet->setAttributeAndModes(new osg::LineWidth(3.f), osg::StateAttribute::ON);
        stateSet->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(streak);
        state.mNode->addChild(geode);

        mFalloutHitscanTracers.push_back(std::move(state));
        Log(Debug::Info) << "FNV hitscan tracer launched: projectile="
                         << ESM::RefId::formIdRefId(projectileId) << " origin=" << origin
                         << " destination=" << destination << " distance=" << distance
                         << " authoredModel=" << projectile->mModel;
        return true;
    }

    std::size_t ProjectileManager::countPendingFalloutVatsProjectiles(const MWWorld::Ptr& actor)
    {
        if (actor.isEmpty())
            return 0;
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        if (store == nullptr)
            return 0;

        std::size_t count = 0;
        for (ProjectileState& state : mProjectiles)
        {
            if (!state.mFallout)
                continue;
            const FalloutProjectileData& fallout = *state.mFallout;
            if (state.mToDelete || !fallout.mImpact.mVatsAction || state.getCaster() != actor)
                continue;
            const ESM4::Projectile* projectile = store->get<ESM4::Projectile>().search(fallout.mProjectile);
            if (projectile == nullptr || !projectile->mData.present)
                continue;
            const bool openEnded = (projectile->mData.flags & ESM4::Projectile::Detonates) != 0
                || ((projectile->mData.flags & ESM4::Projectile::AlternateTrigger) != 0
                    && projectile->mData.alternateProximity > 0.f);
            if (!openEnded)
                ++count;
        }
        return count;
    }

    unsigned int ProjectileManager::detonateFalloutPlacedExplosives(const MWWorld::Ptr& actor)
    {
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        if (store == nullptr || actor.isEmpty() || !actor.getClass().isActor())
            return 0;

        unsigned int count = 0;
        for (ProjectileState& state : mProjectiles)
        {
            if (!state.mFallout)
                continue;
            FalloutProjectileData& fallout = *state.mFallout;
            if (state.mToDelete || fallout.mDetonate || state.getCaster() != actor)
                continue;
            const ESM4::Projectile* projectile
                = store->get<ESM4::Projectile>().search(fallout.mProjectile);
            if (projectile == nullptr
                || !MWMechanics::isFalloutRemoteDetonationCandidate(
                    *projectile, fallout.mSettled, fallout.mImpact.mExplosion))
                continue;
            fallout.mDetonate = true;
            ++count;
            Log(Debug::Info) << "FNV placed explosive remotely triggered: actor=" << actor.toString()
                             << " projectile=" << ESM::RefId::formIdRefId(fallout.mProjectile)
                             << " explosion=" << ESM::RefId::formIdRefId(fallout.mImpact.mExplosion)
                             << " position=" << state.mNode->getPosition()
                             << " elapsed=" << fallout.mElapsedTime;
        }
        return count;
    }

    void ProjectileManager::updateCasters()
    {
        for (auto& state : mProjectiles)
        {
            state.mCasterHandle = state.getCaster();
            mPhysics->setCaster(state.mProjectileId, state.mCasterHandle);
        }

        for (auto& state : mMagicBolts)
        {
            if (!state.mCaster.isSet())
                continue;

            state.mCasterHandle = state.getCaster();
            if (state.mCasterHandle.isEmpty())
            {
                Log(Debug::Error) << "Couldn't find caster with ID " << state.mCaster;
                cleanupMagicBolt(state);
                continue;
            }
            mPhysics->setCaster(state.mProjectileId, state.mCasterHandle);
        }
    }

    void ProjectileManager::update(float dt)
    {
        periodicCleanup(dt);
        moveProjectiles(dt);
        moveFalloutProjectiles(dt);
        moveFalloutHitscanTracers(dt);
        moveMagicBolts(dt);
    }

    void ProjectileManager::moveFalloutHitscanTracers(float duration)
    {
        for (FalloutHitscanTracerState& state : mFalloutHitscanTracers)
        {
            if (state.mToDelete)
                continue;
            state.mElapsedTime += duration;
            if (state.mElapsedTime >= state.mLifetime)
            {
                state.mToDelete = true;
                continue;
            }
            const float progress = std::clamp(state.mElapsedTime / state.mLifetime, 0.f, 1.f);
            state.mNode->setPosition(state.mOrigin + (state.mDestination - state.mOrigin) * progress);
            update(state, duration);
        }
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
            if (projectileState.mToDelete || projectileState.mFallout)
                continue;

            auto* projectile = mPhysics->getProjectile(projectileState.mProjectileId);
            if (!projectile->isActive())
                continue;
            // gravity constant - must be way lower than the gravity affecting actors, since we're not
            // simulating aerodynamics at all
            projectileState.mVelocity
                -= osg::Vec3f(0, 0, Constants::GravityConst * Constants::UnitsPerMeter * 0.1f) * duration;

            projectile->setVelocity(projectileState.mVelocity);

            projectileState.mNode->setAttitude(lookAt(projectileState.mVelocity));

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
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        MWBase::MechanicsManager* mechanics = MWBase::Environment::get().getMechanicsManager();
        MWBase::SoundManager* sound = MWBase::Environment::get().getSoundManager();
        if (store == nullptr || mechanics == nullptr || sound == nullptr)
            return;
        const ESM::GameSetting* minesDelaySetting = store->get<ESM::GameSetting>().search("fMinesDelayMin");
        const ESM::GameSetting* exteriorRadiusSetting
            = store->get<ESM::GameSetting>().search("fMineExteriorRadiusMult");
        const float minesDelayMin = minesDelaySetting != nullptr
            ? minesDelaySetting->mValue.getFloat()
            : std::numeric_limits<float>::quiet_NaN();
        const float exteriorRadiusMultiplier = exteriorRadiusSetting != nullptr
            ? exteriorRadiusSetting->mValue.getFloat()
            : std::numeric_limits<float>::quiet_NaN();

        for (ProjectileState& state : mProjectiles)
        {
            if (state.mToDelete || !state.mFallout)
                continue;
            FalloutProjectileData& fallout = *state.mFallout;
            const ESM4::Projectile* authored = store->get<ESM4::Projectile>().search(fallout.mProjectile);
            if (authored == nullptr || !authored->mData.present)
            {
                state.mToDelete = true;
                continue;
            }
            fallout.mElapsedTime += duration;
            const MWWorld::Ptr caster = state.getCaster();
            if ((authored->mData.flags & ESM4::Projectile::Explosion) != 0)
            {
                const bool exterior = !caster.isEmpty() && caster.isInCell() && caster.getCell()->isExterior();
                MWMechanics::FalloutProjectileTriggerFailure triggerFailure
                    = MWMechanics::FalloutProjectileTriggerFailure::None;
                const std::optional<MWMechanics::FalloutProjectileTrigger> trigger
                    = MWMechanics::buildFalloutProjectileTrigger(*authored, fallout.mImpact.mProjectileSkill,
                        minesDelayMin, exteriorRadiusMultiplier, exterior, triggerFailure);
                if (!trigger)
                {
                    Log(Debug::Error) << "FNV projectile trigger rejected: projectile="
                                      << ESM::RefId::formIdRefId(fallout.mProjectile) << " reason="
                                      << MWMechanics::getFalloutProjectileTriggerFailureName(triggerFailure);
                    state.mToDelete = true;
                    continue;
                }
                if (trigger->mMode == MWMechanics::FalloutProjectileTriggerMode::Timed
                    && fallout.mElapsedTime >= trigger->mDelay)
                    fallout.mDetonate = true;
                else if (trigger->mMode == MWMechanics::FalloutProjectileTriggerMode::Proximity
                    && fallout.mSettled && fallout.mElapsedTime >= trigger->mDelay)
                {
                    if (!fallout.mArmed)
                    {
                        fallout.mArmed = true;
                        if (!authored->mData.countdownSound.isZeroOrUnset())
                        {
                            sound->playSound3D(state.mNode->getPosition(),
                                ESM::RefId::formIdRefId(authored->mData.countdownSound), 1.f, 1.f);
                        }
                        Log(Debug::Info) << "FNV proximity explosive armed: projectile="
                                         << ESM::RefId::formIdRefId(fallout.mProjectile) << " delay="
                                         << trigger->mDelay << " radius=" << trigger->mProximityRadius
                                         << " elapsed=" << fallout.mElapsedTime;
                    }

                    if (!caster.isEmpty())
                    {
                        std::vector<MWWorld::Ptr> nearby;
                        mechanics->getActorsInRange(state.mNode->getPosition(), trigger->mProximityRadius, nearby);
                        for (const MWWorld::Ptr& candidate : nearby)
                        {
                            if (candidate.isEmpty() || candidate == caster || !candidate.getClass().isActor()
                                || candidate.getClass().getCreatureStats(candidate).isDead())
                                continue;
                            const bool hostile = candidate.getClass().getCreatureStats(candidate)
                                                     .getAiSequence()
                                                     .isInCombat(caster)
                                || mechanics->isAggressive(candidate, caster);
                            if (!hostile)
                                continue;
                            fallout.mDetonate = true;
                            Log(Debug::Info) << "FNV proximity explosive triggered: projectile="
                                             << ESM::RefId::formIdRefId(fallout.mProjectile)
                                             << " source=" << caster.toString()
                                             << " target=" << candidate.toString()
                                             << " radius=" << trigger->mProximityRadius
                                             << " elapsed=" << fallout.mElapsedTime;
                            break;
                        }
                    }
                }
            }
            if (fallout.mDetonate || fallout.mSettled)
                continue;
            MWPhysics::Projectile* projectile = mPhysics->getProjectile(state.mProjectileId);
            if (!projectile->isActive())
                continue;

            state.mVelocity -= osg::Vec3f(0.f, 0.f,
                Constants::GravityConst * Constants::UnitsPerMeter * 0.1f * fallout.mGravity * duration);
            projectile->setVelocity(state.mVelocity);

            if (fallout.mRotates)
            {
                const osg::Quat rotation(fallout.mRotationVelocity.x() * duration, osg::Vec3f(1.f, 0.f, 0.f),
                    fallout.mRotationVelocity.y() * duration, osg::Vec3f(0.f, 1.f, 0.f),
                    fallout.mRotationVelocity.z() * duration, osg::Vec3f(0.f, 0.f, 1.f));
                state.mNode->setAttitude(state.mNode->getAttitude() * rotation);
            }
            else if (state.mVelocity.length2() > 0.f)
            {
                osg::Quat orient;
                orient.makeRotate(osg::Vec3f(0.f, 1.f, 0.f), state.mVelocity);
                state.mNode->setAttitude(orient);
            }
            update(state, duration);

            // Package targeting controls aim, not collision; any actor in the flight path remains solid.
            projectile->setValidTargets({});
        }
    }

    bool ProjectileManager::bounceFalloutProjectile(ProjectileState& state, FalloutProjectileData& fallout,
        const ESM4::Projectile& projectile, const osg::Vec3f& hitPosition, const osg::Vec3f& hitNormal)
    {
        MWMechanics::FalloutProjectileBounceFailure failure
            = MWMechanics::FalloutProjectileBounceFailure::None;
        const std::optional<osg::Vec3f> reflected = MWMechanics::resolveFalloutProjectileBounce(
            state.mVelocity, hitNormal, projectile.mData.bounciness, failure);
        if (!reflected)
        {
            Log(Debug::Error) << "FNV lobber bounce rejected: projectile="
                              << ESM::RefId::formIdRefId(fallout.mProjectile)
                              << " reason=" << MWMechanics::getFalloutProjectileBounceFailureName(failure);
            return false;
        }

        ++fallout.mBounceCount;
        constexpr float settleSpeed = 15.f;
        constexpr std::uint8_t maximumBounces = 24;
        osg::Vec3f normal = hitNormal;
        normal.normalize();
        if (reflected->length() <= settleSpeed || fallout.mBounceCount >= maximumBounces)
        {
            state.mVelocity.set(0.f, 0.f, 0.f);
            fallout.mPreviousPosition = hitPosition;
            state.mNode->setPosition(hitPosition);
            fallout.mSettled = true;
            Log(Debug::Info) << "FNV lobber settled: projectile="
                             << ESM::RefId::formIdRefId(fallout.mProjectile)
                             << " bounces=" << static_cast<unsigned int>(fallout.mBounceCount)
                             << " elapsed=" << fallout.mElapsedTime;
            return true;
        }

        const osg::Vec3f restartPosition = hitPosition + normal * 2.f;
        const VFS::Path::Normalized model
            = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(projectile.mModel));
        mPhysics->removeProjectile(state.mProjectileId);
        state.mProjectileId = mPhysics->addProjectile(state.getCaster(), restartPosition, model, true);
        state.mVelocity = *reflected;
        fallout.mPreviousPosition = restartPosition;
        state.mNode->setPosition(restartPosition);
        mPhysics->getProjectile(state.mProjectileId)->setVelocity(state.mVelocity);
        Log(Debug::Info) << "FNV lobber bounced: projectile=" << ESM::RefId::formIdRefId(fallout.mProjectile)
                         << " bounciness=" << projectile.mData.bounciness
                         << " speed=" << state.mVelocity.length()
                         << " bounces=" << static_cast<unsigned int>(fallout.mBounceCount)
                         << " elapsed=" << fallout.mElapsedTime;
        return true;
    }

    void ProjectileManager::processHits()
    {
        for (auto& projectileState : mProjectiles)
        {
            if (projectileState.mToDelete || projectileState.mFallout)
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
        for (ProjectileState& projectileState : mProjectiles)
        {
            if (projectileState.mToDelete || !projectileState.mFallout)
                continue;
            FalloutProjectileData& fallout = *projectileState.mFallout;

            MWPhysics::Projectile* projectile = mPhysics->getProjectile(projectileState.mProjectileId);
            const osg::Vec3f position = projectile->getSimulationPosition();
            projectileState.mNode->setPosition(position);
            if (fallout.mDetonate)
            {
                const MWWorld::Ptr caster = projectileState.getCaster();
                if (caster.isEmpty()
                    || !MWBase::Environment::get().getMechanicsManager()->executeFalloutExplosion(
                        caster, position, fallout.mImpact))
                {
                    Log(Debug::Error) << "FNV timed projectile explosion rejected: caster="
                                      << (caster.isEmpty() ? std::string("none") : caster.toString())
                                      << " projectile=" << ESM::RefId::formIdRefId(fallout.mProjectile)
                                      << " explosion="
                                      << ESM::RefId::formIdRefId(fallout.mImpact.mExplosion);
                }
                projectileState.mToDelete = true;
                continue;
            }
            if (fallout.mSettled)
                continue;
            const ESM4::Projectile* authoredProjectile
                = MWBase::Environment::get().getESMStore()->get<ESM4::Projectile>().search(
                    fallout.mProjectile);
            fallout.mDistanceTravelled += (position - fallout.mPreviousPosition).length();
            const bool projectileActive = projectile->isActive();
            if (MWMechanics::shouldResolveFalloutProjectileRangeExpiry(
                    projectileActive, fallout.mDistanceTravelled, fallout.mMaximumRange))
            {
                const std::optional<ESM::FormId> queuedTarget = authoredProjectile != nullptr
                    ? MWMechanics::getFalloutVatsProjectileRangeExpiryTarget(
                        *authoredProjectile, fallout.mImpact)
                    : std::nullopt;
                if (queuedTarget)
                {
                    MWBase::World* world = MWBase::Environment::get().getWorld();
                    MWWorld::Ptr candidate
                        = world != nullptr ? world->searchPtr(ESM::RefId(*queuedTarget), true, false) : MWWorld::Ptr();
                    if (candidate.isEmpty() && world != nullptr)
                    {
                        std::vector<MWWorld::Ptr> nearby;
                        MWBase::Environment::get().getMechanicsManager()->getActorsInRange(
                            position, fallout.mMaximumRange, nearby);
                        const auto activeTarget = std::ranges::find_if(nearby, [&](const MWWorld::Ptr& actor) {
                            return !actor.isEmpty() && actor.getClass().isActor()
                                && actor.getCellRef().getRefNum() == *queuedTarget;
                        });
                        if (activeTarget != nearby.end())
                            candidate = *activeTarget;
                    }

                    if (!candidate.isEmpty() && candidate.getClass().isActor())
                    {
                        osg::Vec3f resolvedHitPosition = world->getActorHeadTransform(candidate).getTrans();
                        const MWMechanics::FalloutVatsQueuedAction& action
                            = *fallout.mImpact.mVatsAction;
                        if (MWRender::Animation* animation = world->getAnimation(candidate))
                        {
                            if (const osg::Node* targetNode = animation->getNode(action.mTargetNode))
                            {
                                const osg::NodePathList paths = targetNode->getParentalNodePaths();
                                if (!paths.empty())
                                    resolvedHitPosition = osg::computeLocalToWorld(paths.front()).getTrans();
                            }
                        }
                        projectileState.mNode->setPosition(resolvedHitPosition);

                        const MWWorld::Ptr caster = projectileState.getCaster();
                        const bool applied = !caster.isEmpty()
                            && MWBase::Environment::get().getMechanicsManager()->executeFalloutProjectileImpact(
                                caster, candidate, fallout.mPreviousPosition, resolvedHitPosition,
                                fallout.mImpact);
                        if (!applied)
                        {
                            Log(Debug::Error) << "FNV moving projectile VATS range impact rejected: caster="
                                              << (caster.isEmpty() ? std::string("none") : caster.toString())
                                              << " target=" << candidate.toString() << " projectile="
                                              << ESM::RefId::formIdRefId(fallout.mProjectile);
                        }

                        const bool detonateOnImpact
                            = (authoredProjectile->mData.flags & ESM4::Projectile::Explosion) != 0
                            && (authoredProjectile->mData.flags & ESM4::Projectile::AlternateTrigger) == 0
                            && (authoredProjectile->mData.flags & ESM4::Projectile::Detonates) == 0
                            && !fallout.mImpact.mExplosion.isZeroOrUnset();
                        if (detonateOnImpact
                            && (caster.isEmpty()
                                || !MWBase::Environment::get().getMechanicsManager()->executeFalloutExplosion(
                                    caster, resolvedHitPosition, fallout.mImpact)))
                        {
                            Log(Debug::Error) << "FNV moving projectile VATS range explosion rejected: caster="
                                              << (caster.isEmpty() ? std::string("none") : caster.toString())
                                              << " projectile="
                                              << ESM::RefId::formIdRefId(fallout.mProjectile)
                                              << " explosion="
                                              << ESM::RefId::formIdRefId(fallout.mImpact.mExplosion);
                        }

                        Log(Debug::Info) << "FNV moving projectile VATS range resolution: projectile="
                                         << ESM::RefId::formIdRefId(fallout.mProjectile)
                                         << " target=" << candidate.toString()
                                         << " range=" << fallout.mMaximumRange
                                         << " travelled=" << fallout.mDistanceTravelled
                                         << " resolvedHitPosition=" << resolvedHitPosition;
                    }
                    else
                    {
                        Log(Debug::Warning) << "FNV moving projectile VATS range target unavailable: projectile="
                                            << ESM::RefId::formIdRefId(fallout.mProjectile)
                                            << " target=" << ESM::RefId::formIdRefId(*queuedTarget);
                    }
                }
                projectileState.mToDelete = true;
                continue;
            }

            if (projectileActive)
            {
                fallout.mPreviousPosition = position;
                continue;
            }

            const MWWorld::Ptr target = projectile->getTarget();
            const MWWorld::Ptr caster = projectileState.getCaster();
            const osg::Vec3f physicalHitPosition = Misc::Convert::toOsg(projectile->getHitPosition());
            if (projectile->getHitWater())
                mRendering->emitWaterRipple(physicalHitPosition);

            const bool persistentLobber = authoredProjectile != nullptr
                && MWMechanics::doesFalloutProjectileRemainAfterImpact(*authoredProjectile);
            if (persistentLobber)
            {
                const osg::Vec3f hitNormal = Misc::Convert::toOsg(projectile->getHitNormal());
                if (!bounceFalloutProjectile(
                        projectileState, fallout, *authoredProjectile, physicalHitPosition, hitNormal))
                    projectileState.mToDelete = true;
                continue;
            }

            MWWorld::Ptr resolvedTarget = target;
            osg::Vec3f resolvedHitPosition = physicalHitPosition;
            const bool terminalDirectImpact = authoredProjectile != nullptr && !persistentLobber;
            const std::optional<ESM::FormId> queuedTarget = terminalDirectImpact
                ? MWMechanics::getAuthoritativeFalloutVatsProjectileTarget(fallout.mImpact)
                : std::nullopt;
            if (queuedTarget)
            {
                MWBase::World* world = MWBase::Environment::get().getWorld();
                MWWorld::Ptr candidate
                    = world != nullptr ? world->searchPtr(ESM::RefId(*queuedTarget), true, false) : MWWorld::Ptr();
                if (candidate.isEmpty() && world != nullptr)
                {
                    std::vector<MWWorld::Ptr> nearby;
                    MWBase::Environment::get().getMechanicsManager()->getActorsInRange(
                        physicalHitPosition, fallout.mMaximumRange, nearby);
                    const auto activeTarget = std::ranges::find_if(nearby, [&](const MWWorld::Ptr& actor) {
                        return !actor.isEmpty() && actor.getClass().isActor()
                            && actor.getCellRef().getRefNum() == *queuedTarget;
                    });
                    if (activeTarget != nearby.end())
                        candidate = *activeTarget;
                }
                if (!candidate.isEmpty() && candidate.getClass().isActor())
                {
                    resolvedTarget = candidate;
                    resolvedHitPosition = world->getActorHeadTransform(candidate).getTrans();
                    const MWMechanics::FalloutVatsQueuedAction& action = *fallout.mImpact.mVatsAction;
                    if (MWRender::Animation* animation = world->getAnimation(candidate))
                    {
                        if (const osg::Node* targetNode = animation->getNode(action.mTargetNode))
                        {
                            const osg::NodePathList paths = targetNode->getParentalNodePaths();
                            if (!paths.empty())
                                resolvedHitPosition = osg::computeLocalToWorld(paths.front()).getTrans();
                        }
                    }
                    projectileState.mNode->setPosition(resolvedHitPosition);
                    Log(Debug::Info) << "FNV moving projectile VATS resolution: projectile="
                                     << ESM::RefId::formIdRefId(fallout.mProjectile)
                                     << " queuedTarget=" << candidate.toString() << " physicalTarget="
                                     << (target.isEmpty() ? std::string("scenery") : target.toString())
                                     << " physicalHitPosition=" << physicalHitPosition
                                     << " resolvedHitPosition=" << resolvedHitPosition
                                     << " redirected=" << (target != candidate);
                }
                else
                {
                    Log(Debug::Warning) << "FNV moving projectile VATS target unavailable: projectile="
                                        << ESM::RefId::formIdRefId(fallout.mProjectile)
                                        << " target=" << ESM::RefId::formIdRefId(*queuedTarget);
                }
            }

            if (!caster.isEmpty() && !resolvedTarget.isEmpty() && resolvedTarget.getClass().isActor())
            {
                const bool applied = MWBase::Environment::get().getMechanicsManager()
                                         ->executeFalloutProjectileImpact(caster, resolvedTarget,
                                             fallout.mPreviousPosition, resolvedHitPosition, fallout.mImpact);
                if (!applied)
                    Log(Debug::Error) << "FNV moving projectile impact rejected: caster=" << caster.toString()
                                      << " target=" << resolvedTarget.toString() << " projectile="
                                      << ESM::RefId::formIdRefId(fallout.mProjectile);
            }
            const bool detonateOnImpact = authoredProjectile != nullptr
                && (authoredProjectile->mData.flags & ESM4::Projectile::Explosion) != 0
                && (authoredProjectile->mData.flags & ESM4::Projectile::AlternateTrigger) == 0
                && (authoredProjectile->mData.flags & ESM4::Projectile::Detonates) == 0
                && !fallout.mImpact.mExplosion.isZeroOrUnset();
            if (detonateOnImpact)
            {
                if (caster.isEmpty()
                    || !MWBase::Environment::get().getMechanicsManager()->executeFalloutExplosion(
                        caster, resolvedHitPosition, fallout.mImpact))
                {
                    Log(Debug::Error) << "FNV moving projectile explosion rejected: caster="
                                      << (caster.isEmpty() ? std::string("none") : caster.toString())
                                      << " projectile=" << ESM::RefId::formIdRefId(fallout.mProjectile)
                                      << " explosion="
                                      << ESM::RefId::formIdRefId(fallout.mImpact.mExplosion);
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
            cast.mHitPosition = !active ? Misc::Convert::makeOsgVec3f(projectile->getHitPosition()) : pos;
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
        for (FalloutHitscanTracerState& tracer : mFalloutHitscanTracers)
        {
            if (tracer.mToDelete)
                cleanupFalloutHitscanTracer(tracer);
        }

        for (auto& magicBoltState : mMagicBolts)
        {
            if (magicBoltState.mToDelete)
                cleanupMagicBolt(magicBoltState);
        }
        mProjectiles.erase(std::remove_if(mProjectiles.begin(), mProjectiles.end(),
                               [](const State& state) { return state.mToDelete; }),
            mProjectiles.end());
        mFalloutHitscanTracers.erase(
            std::remove_if(mFalloutHitscanTracers.begin(), mFalloutHitscanTracers.end(),
                [](const State& state) { return state.mToDelete; }),
            mFalloutHitscanTracers.end());
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

    void ProjectileManager::cleanupFalloutHitscanTracer(FalloutHitscanTracerState& state)
    {
        mParent->removeChild(state.mNode);
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

        for (FalloutHitscanTracerState& tracer : mFalloutHitscanTracers)
            cleanupFalloutHitscanTracer(tracer);
        mFalloutHitscanTracers.clear();

        for (auto& mMagicBolt : mMagicBolts)
            cleanupMagicBolt(mMagicBolt);
        mMagicBolts.clear();
    }

    void ProjectileManager::write(ESM::ESMWriter& writer, Loading::Listener& progress) const
    {
        for (const ProjectileState& projectile : mProjectiles)
        {
            if (projectile.mFallout)
                continue;
            writer.startRecord(ESM::REC_PROJ);

            ESM::ProjectileState state;
            state.mId = projectile.mIdArrow;
            state.mPosition = ESM::Vector3(osg::Vec3f(projectile.mNode->getPosition()));
            state.mOrientation = ESM::Quaternion(osg::Quat(projectile.mNode->getAttitude()));
            state.mCaster = projectile.mCaster;

            state.mBowId = projectile.mBowId;
            state.mVelocity = projectile.mVelocity;
            state.mAttackStrength = projectile.mAttackStrength;

            state.save(writer);

            writer.endRecord(ESM::REC_PROJ);
        }

        for (const MagicBoltState& bolt : mMagicBolts)
        {
            writer.startRecord(ESM::REC_MPRJ);

            ESM::MagicBoltState state;
            state.mId = bolt.mIdMagic.at(0);
            state.mPosition = ESM::Vector3(osg::Vec3f(bolt.mNode->getPosition()));
            state.mOrientation = ESM::Quaternion(osg::Quat(bolt.mNode->getAttitude()));
            state.mCaster = bolt.mCaster;
            state.mItem = bolt.mItem;
            state.mSpellId = bolt.mSpellId;
            state.mSpeed = bolt.mSpeed;

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
            state.mCaster = esm.mCaster;
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
        if (type == ESM::REC_MPRJ)
        {
            ESM::MagicBoltState esm;
            esm.load(reader);

            MagicBoltState state;
            state.mIdMagic.push_back(esm.mId);
            state.mSpellId = esm.mSpellId;
            state.mCaster = esm.mCaster;
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

    size_t ProjectileManager::countSavedGameRecords() const
    {
        return mMagicBolts.size()
            + std::count_if(mProjectiles.begin(), mProjectiles.end(),
                [](const ProjectileState& projectile) { return !projectile.mFallout; });
    }

    void ProjectileManager::saveLoaded(const ESM::ESMReader& reader)
    {
        // Can't do this in readRecord because the vectors might get reallocated as they grow
        if (reader.mActorIdConverter)
        {
            for (ProjectileState& projectile : mProjectiles)
            {
                if (!projectile.mFallout)
                    reader.mActorIdConverter->convert(projectile.mCaster, projectile.mCaster.mIndex);
            }
            for (MagicBoltState& bolt : mMagicBolts)
                reader.mActorIdConverter->convert(bolt.mCaster, bolt.mCaster.mIndex);
        }
    }

    MWWorld::Ptr ProjectileManager::State::getCaster()
    {
        if (!mCasterHandle.isEmpty())
            return mCasterHandle;

        return MWBase::Environment::get().getWorldModel()->getPtr(mCaster);
    }

}
