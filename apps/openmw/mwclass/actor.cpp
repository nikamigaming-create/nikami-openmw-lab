#include "actor.hpp"

#include <map>
#include <string>

#include <components/esm3/loadmgef.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/magiceffects.hpp"
#include "../mwmechanics/movement.hpp"

#include "../mwphysics/physicssystem.hpp"

#include "../mwworld/inventorystore.hpp"
#include "../mwworld/worldmodel.hpp"

namespace MWClass
{
    void Actor::adjustPosition(const MWWorld::Ptr& ptr, bool force) const
    {
        MWBase::Environment::get().getWorld()->adjustPosition(ptr, force);
    }

    void Actor::insertObject(const MWWorld::Ptr& ptr, const std::string& model, const osg::Quat& rotation,
        MWPhysics::PhysicsSystem& physics) const
    {
        physics.addActor(ptr, VFS::Path::toNormalized(model));
        if (getCreatureStats(ptr).isDead() && getCreatureStats(ptr).isDeathAnimationFinished())
            MWBase::Environment::get().getWorld()->enableActorCollision(ptr, false);
    }

    bool Actor::useAnim() const
    {
        return true;
    }

    osg::Vec3f Actor::getRotationVector(const MWWorld::Ptr& ptr) const
    {
        MWMechanics::Movement& movement = getMovementSettings(ptr);
        osg::Vec3f vec(movement.mRotation[0], movement.mRotation[1], movement.mRotation[2]);
        movement.mRotation[0] = 0.0f;
        movement.mRotation[1] = 0.0f;
        movement.mRotation[2] = 0.0f;
        return vec;
    }

    float Actor::getEncumbrance(const MWWorld::Ptr& ptr) const
    {
        float weight = getContainerStore(ptr).getWeight();
        const MWMechanics::MagicEffects& effects = getCreatureStats(ptr).getMagicEffects();
        weight -= effects.getOrDefault(MWMechanics::EffectKey(ESM::MagicEffect::Feather)).getMagnitude();
        if (ptr != MWMechanics::getPlayer() || !MWBase::Environment::get().getWorld()->getGodModeState())
            weight += effects.getOrDefault(MWMechanics::EffectKey(ESM::MagicEffect::Burden)).getMagnitude();
        return (weight < 0) ? 0.0f : weight;
    }

    bool Actor::allowTelekinesis(const MWWorld::ConstPtr& ptr) const
    {
        return false;
    }

    bool Actor::isActor() const
    {
        return true;
    }

    float Actor::getCurrentSpeed(const MWWorld::Ptr& ptr) const
    {
        const MWMechanics::Movement& movementSettings = ptr.getClass().getMovementSettings(ptr);
        float moveSpeed = this->getMaxSpeed(ptr) * movementSettings.mSpeedFactor;
        if (movementSettings.mIsStrafing)
            moveSpeed *= 0.75f;
        return moveSpeed;
    }

    bool Actor::consume(const MWWorld::Ptr& consumable, const MWWorld::Ptr& actor) const
    {
        MWMechanics::CastSpell cast(actor, actor);
        const ESM::RefId& recordId = consumable.getCellRef().getRefId();
        MWBase::Environment::get().getWorldModel()->registerPtr(consumable);
        MWBase::Environment::get().getLuaManager()->itemConsumed(consumable, actor);
        actor.getClass().getContainerStore(actor).remove(consumable, 1);
        if (cast.cast(recordId))
        {
            MWBase::Environment::get().getWorld()->breakInvisibility(actor);
            return true;
        }
        return false;
    }

    void Actor::onHit(const MWWorld::Ptr& ptr, const std::map<std::string, float>& damages, ESM::RefId object,
        const MWWorld::Ptr& attacker, bool successful, const MWMechanics::DamageSourceType sourceType) const
    {
        (void)sourceType;
        MWMechanics::CreatureStats& stats = getCreatureStats(ptr);
        const bool wasDead = stats.isDead();

        if (!attacker.isEmpty() && attacker.getClass().isActor() && !stats.getAiSequence().isInCombat(attacker))
        {
            stats.setAttacked(true);
            MWBase::Environment::get().getMechanicsManager()->actorAttacked(ptr, attacker);
        }

        if (!object.empty())
            stats.setLastHitAttemptObject(object);
        if (!successful)
            return;
        if (!object.empty())
            stats.setLastHitObject(object);
        if (ptr == MWMechanics::getPlayer() && MWBase::Environment::get().getWorld()->getGodModeState())
            return;

        bool damaged = false;
        for (const auto& [stat, damage] : damages)
        {
            if (damage < 0.001f)
                continue;
            damaged = true;
            if (stat == "health")
            {
                MWMechanics::DynamicStat<float> value(stats.getHealth());
                value.setCurrent(value.getCurrent() - damage);
                stats.setHealth(value);
            }
            else if (stat == "fatigue")
            {
                MWMechanics::DynamicStat<float> value(stats.getFatigue());
                value.setCurrent(value.getCurrent() - damage, true);
                stats.setFatigue(value);
            }
            else if (stat == "magicka")
            {
                MWMechanics::DynamicStat<float> value(stats.getMagicka());
                value.setCurrent(value.getCurrent() - damage);
                stats.setMagicka(value);
            }
        }

        if (damaged && !stats.isDead())
            stats.setHitRecovery(true);
        if (!wasDead && stats.isDead())
            MWBase::Environment::get().getMechanicsManager()->actorKilled(ptr, attacker);
    }
}
