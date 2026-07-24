#include "aibreathe.hpp"

#include "../mwbase/environment.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"

#include "npcstats.hpp"

#include "movement.hpp"
#include "steering.hpp"

bool MWMechanics::AiBreathe::execute(
    const MWWorld::Ptr& actor, CharacterController& characterController, AiState& state, float duration)
{
    if (MWBase::Environment::get().getESMStore()->isFalloutNewVegas())
    {
        // FNV's current breath and maximum breath are native actor-process/save state. This TES3 package must not
        // infer them from fHoldBreathTime while that native state is explicitly uncovered.
        return true;
    }

    static const float fHoldBreathTime
        = MWBase::Environment::get().getESMStore()->get<ESM::GameSetting>().find("fHoldBreathTime")->mValue.getFloat();

    const MWWorld::Class& actorClass = actor.getClass();
    if (actorClass.isNpc())
    {
        if (actorClass.getNpcStats(actor).getTimeToStartDrowning() < fHoldBreathTime / 2)
        {
            actorClass.getCreatureStats(actor).setMovementFlag(CreatureStats::Flag_Run, true);

            actorClass.getMovementSettings(actor).mPosition[1] = 1;
            smoothTurn(actor, static_cast<float>(-osg::PI_2), 0);

            return false;
        }
    }

    return true;
}
