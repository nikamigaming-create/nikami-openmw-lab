#include "action.hpp"

#include <cstdlib>

#include <components/debug/debuglog.hpp>

#include "../mwbase/environment.hpp"

#include "../mwbase/soundmanager.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwmechanics/actorutil.hpp"

const MWWorld::Ptr& MWWorld::Action::getTarget() const
{
    return mTarget;
}

void MWWorld::Action::setTarget(const MWWorld::Ptr& target)
{
    mTarget = target;
}

MWWorld::Action::Action(bool keepSound, const Ptr& target)
    : mKeepSound(keepSound)
    , mSoundOffset(0)
    , mTarget(target)
{
}

MWWorld::Action::~Action() {}

void MWWorld::Action::execute(const Ptr& actor, bool noSound)
{
    const bool interactionAudit = std::getenv("OPENMW_FNV_INTERACTION_AUDIT") != nullptr;
    if (interactionAudit)
        Log(Debug::Info) << "FNV interaction audit: Action execute begin actor=" << actor.toString()
                         << " target=" << mTarget.toString() << " sound=" << mSoundId.toDebugString()
                         << " noSound=" << (noSound ? 1 : 0);
    if (!mSoundId.empty() && !noSound)
    {
        MWSound::PlayMode envType = MWSound::PlayMode::Normal;

        // Action sounds should not have a distortion in GUI mode
        // example: take an item or drink a potion underwater
        if (actor == MWMechanics::getPlayer() && MWBase::Environment::get().getWindowManager()->isGuiMode())
        {
            envType = MWSound::PlayMode::NoEnv;
        }

        if (mKeepSound && actor == MWMechanics::getPlayer())
            MWBase::Environment::get().getSoundManager()->playSound(
                mSoundId, 1.0, 1.0, MWSound::Type::Sfx, envType, mSoundOffset);
        else
        {
            bool local = mTarget.isEmpty() || !mTarget.isInCell(); // no usable target
            if (mKeepSound)
                MWBase::Environment::get().getSoundManager()->playSound3D(
                    (local ? actor : mTarget).getRefData().getPosition().asVec3(), mSoundId, 1.0, 1.0,
                    MWSound::Type::Sfx, envType, mSoundOffset);
            else
                MWBase::Environment::get().getSoundManager()->playSound3D(
                    local ? actor : mTarget, mSoundId, 1.0, 1.0, MWSound::Type::Sfx, envType, mSoundOffset);
        }
        if (interactionAudit)
            Log(Debug::Info) << "FNV interaction audit: Action sound complete sound=" << mSoundId.toDebugString();
    }

    if (interactionAudit)
        Log(Debug::Info) << "FNV interaction audit: Action calling executeImp";
    executeImp(actor);
    if (interactionAudit)
        Log(Debug::Info) << "FNV interaction audit: Action execute end";
}

void MWWorld::Action::setSound(const ESM::RefId& id)
{
    mSoundId = id;
}

void MWWorld::Action::setSoundOffset(float offset)
{
    mSoundOffset = offset;
}
