#include "summoning.hpp"

#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/misc/resourcehelpers.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/manualref.hpp"
<<<<<<< HEAD
#include "../mwworld/worldmodel.hpp"
=======
>>>>>>> origin/main

#include "../mwrender/animation.hpp"

#include "aifollow.hpp"
#include "creaturestats.hpp"

namespace MWMechanics
{

<<<<<<< HEAD
    bool isSummoningEffect(ESM::RefId effectId)
    {
        if (effectId.empty())
            return false;
        static const std::array<ESM::RefId, 22> summonEffects{
            ESM::MagicEffect::SummonAncestralGhost,
            ESM::MagicEffect::SummonBonelord,
            ESM::MagicEffect::SummonBonewalker,
            ESM::MagicEffect::SummonCenturionSphere,
            ESM::MagicEffect::SummonClannfear,
            ESM::MagicEffect::SummonDaedroth,
            ESM::MagicEffect::SummonDremora,
            ESM::MagicEffect::SummonFabricant,
            ESM::MagicEffect::SummonFlameAtronach,
            ESM::MagicEffect::SummonFrostAtronach,
            ESM::MagicEffect::SummonGoldenSaint,
            ESM::MagicEffect::SummonGreaterBonewalker,
            ESM::MagicEffect::SummonHunger,
            ESM::MagicEffect::SummonScamp,
            ESM::MagicEffect::SummonSkeletalMinion,
            ESM::MagicEffect::SummonStormAtronach,
            ESM::MagicEffect::SummonWingedTwilight,
            ESM::MagicEffect::SummonWolf,
            ESM::MagicEffect::SummonBear,
            ESM::MagicEffect::SummonBonewolf,
            ESM::MagicEffect::SummonCreature04,
            ESM::MagicEffect::SummonCreature05,
        };
        return (std::find(summonEffects.begin(), summonEffects.end(), effectId) != summonEffects.end());
    }

    static const std::map<ESM::RefId, ESM::RefId>& getSummonMap()
    {
        static std::map<ESM::RefId, ESM::RefId> summonMap;
=======
    bool isSummoningEffect(int effectId)
    {
        return ((effectId >= ESM::MagicEffect::SummonScamp && effectId <= ESM::MagicEffect::SummonStormAtronach)
            || (effectId == ESM::MagicEffect::SummonCenturionSphere)
            || (effectId >= ESM::MagicEffect::SummonFabricant && effectId <= ESM::MagicEffect::SummonCreature05));
    }

    static const std::map<int, ESM::RefId>& getSummonMap()
    {
        static std::map<int, ESM::RefId> summonMap;
>>>>>>> origin/main

        if (summonMap.size() > 0)
            return summonMap;

<<<<<<< HEAD
        const std::map<ESM::RefId, std::string_view> summonMapToGameSetting{
=======
        const std::map<int, std::string_view> summonMapToGameSetting{
>>>>>>> origin/main
            { ESM::MagicEffect::SummonAncestralGhost, "sMagicAncestralGhostID" },
            { ESM::MagicEffect::SummonBonelord, "sMagicBonelordID" },
            { ESM::MagicEffect::SummonBonewalker, "sMagicLeastBonewalkerID" },
            { ESM::MagicEffect::SummonCenturionSphere, "sMagicCenturionSphereID" },
            { ESM::MagicEffect::SummonClannfear, "sMagicClannfearID" },
            { ESM::MagicEffect::SummonDaedroth, "sMagicDaedrothID" },
            { ESM::MagicEffect::SummonDremora, "sMagicDremoraID" },
            { ESM::MagicEffect::SummonFabricant, "sMagicFabricantID" },
            { ESM::MagicEffect::SummonFlameAtronach, "sMagicFlameAtronachID" },
            { ESM::MagicEffect::SummonFrostAtronach, "sMagicFrostAtronachID" },
            { ESM::MagicEffect::SummonGoldenSaint, "sMagicGoldenSaintID" },
            { ESM::MagicEffect::SummonGreaterBonewalker, "sMagicGreaterBonewalkerID" },
            { ESM::MagicEffect::SummonHunger, "sMagicHungerID" },
            { ESM::MagicEffect::SummonScamp, "sMagicScampID" },
            { ESM::MagicEffect::SummonSkeletalMinion, "sMagicSkeletalMinionID" },
            { ESM::MagicEffect::SummonStormAtronach, "sMagicStormAtronachID" },
            { ESM::MagicEffect::SummonWingedTwilight, "sMagicWingedTwilightID" },
            { ESM::MagicEffect::SummonWolf, "sMagicCreature01ID" },
            { ESM::MagicEffect::SummonBear, "sMagicCreature02ID" },
            { ESM::MagicEffect::SummonBonewolf, "sMagicCreature03ID" },
            { ESM::MagicEffect::SummonCreature04, "sMagicCreature04ID" },
            { ESM::MagicEffect::SummonCreature05, "sMagicCreature05ID" },
        };

        for (const auto& it : summonMapToGameSetting)
        {
            summonMap[it.first] = ESM::RefId::stringRefId(
                MWBase::Environment::get().getESMStore()->get<ESM::GameSetting>().find(it.second)->mValue.getString());
        }
        return summonMap;
    }

<<<<<<< HEAD
    ESM::RefId getSummonedCreature(ESM::RefId effectId)
=======
    ESM::RefId getSummonedCreature(int effectId)
>>>>>>> origin/main
    {
        const auto& summonMap = getSummonMap();
        auto it = summonMap.find(effectId);
        if (it != summonMap.end())
        {
            return it->second;
        }
        return ESM::RefId();
    }

<<<<<<< HEAD
    ESM::RefNum summonCreature(ESM::RefId effectId, const MWWorld::Ptr& summoner)
    {
        const ESM::RefId& creatureID = getSummonedCreature(effectId);
        ESM::RefNum creature;
=======
    int summonCreature(int effectId, const MWWorld::Ptr& summoner)
    {
        const ESM::RefId& creatureID = getSummonedCreature(effectId);
        int creatureActorId = -1;
>>>>>>> origin/main
        if (!creatureID.empty())
        {
            try
            {
                auto world = MWBase::Environment::get().getWorld();
                MWWorld::ManualRef ref(world->getStore(), creatureID, 1);
                MWWorld::Ptr placed = world->safePlaceObject(ref.getPtr(), summoner, summoner.getCell(), 0, 120.f);
<<<<<<< HEAD
                MWBase::Environment::get().getWorldModel()->registerPtr(placed);
                creature = placed.getCellRef().getRefNum();

                // Make the summoned creature follow its master and help in fights
                AiFollow package(summoner);
                placed.getClass().getCreatureStats(placed).getAiSequence().stack(package, placed);
=======

                MWMechanics::CreatureStats& summonedCreatureStats = placed.getClass().getCreatureStats(placed);

                // Make the summoned creature follow its master and help in fights
                AiFollow package(summoner);
                summonedCreatureStats.getAiSequence().stack(package, placed);
                creatureActorId = summonedCreatureStats.getActorId();
>>>>>>> origin/main

                MWRender::Animation* anim = world->getAnimation(placed);
                if (anim)
                {
                    const ESM::Static* fx
                        = world->getStore().get<ESM::Static>().search(ESM::RefId::stringRefId("VFX_Summon_Start"));
                    if (fx)
                        anim->addEffect(
                            Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(fx->mModel)).value(), "",
                            false);
                }
            }
            catch (std::exception& e)
            {
                Log(Debug::Error) << "Failed to spawn summoned creature: " << e.what();
                // still insert into creatureMap so we don't try to spawn again every frame, that would spam the warning
                // log
            }

<<<<<<< HEAD
            summoner.getClass().getCreatureStats(summoner).getSummonedCreatureMap().emplace(effectId, creature);
        }
        return creature;
=======
            summoner.getClass().getCreatureStats(summoner).getSummonedCreatureMap().emplace(effectId, creatureActorId);
        }
        return creatureActorId;
>>>>>>> origin/main
    }

    void updateSummons(const MWWorld::Ptr& summoner, bool cleanup)
    {
        MWMechanics::CreatureStats& creatureStats = summoner.getClass().getCreatureStats(summoner);
        auto& creatureMap = creatureStats.getSummonedCreatureMap();

<<<<<<< HEAD
=======
        std::vector<int> graveyard = creatureStats.getSummonedCreatureGraveyard();
        creatureStats.getSummonedCreatureGraveyard().clear();

        for (const int creature : graveyard)
            MWBase::Environment::get().getMechanicsManager()->cleanupSummonedCreature(summoner, creature);

>>>>>>> origin/main
        if (!cleanup)
            return;

        for (auto it = creatureMap.begin(); it != creatureMap.end();)
        {
<<<<<<< HEAD
            if (!it->second.isSet())
=======
            if (it->second == -1)
>>>>>>> origin/main
            {
                // Keep the spell effect active if we failed to spawn anything
                it++;
                continue;
            }
<<<<<<< HEAD
            MWWorld::Ptr ptr = MWBase::Environment::get().getWorldModel()->getPtr(it->second);
=======
            MWWorld::Ptr ptr = MWBase::Environment::get().getWorld()->searchPtrViaActorId(it->second);
>>>>>>> origin/main
            if (!ptr.isEmpty() && ptr.getClass().getCreatureStats(ptr).isDead()
                && ptr.getClass().getCreatureStats(ptr).isDeathAnimationFinished())
            {
                // Purge the magic effect so a new creature can be summoned if desired
                auto summon = *it;
                creatureMap.erase(it++);
                purgeSummonEffect(summoner, summon);
            }
            else
                ++it;
        }
    }

<<<<<<< HEAD
    void purgeSummonEffect(const MWWorld::Ptr& summoner, const std::pair<ESM::RefId, ESM::RefNum>& summon)
=======
    void purgeSummonEffect(const MWWorld::Ptr& summoner, const std::pair<int, int>& summon)
>>>>>>> origin/main
    {
        auto& creatureStats = summoner.getClass().getCreatureStats(summoner);
        creatureStats.getActiveSpells().purge(
            [summon](const auto& spell, const auto& effect) {
<<<<<<< HEAD
                return effect.mEffectId == summon.first && effect.getActor() == summon.second;
            },
            summoner);

        MWBase::Environment::get().getMechanicsManager()->cleanupSummonedCreature(summon.second);
=======
                return effect.mEffectId == summon.first && effect.getActorId() == summon.second;
            },
            summoner);

        MWBase::Environment::get().getMechanicsManager()->cleanupSummonedCreature(summoner, summon.second);
>>>>>>> origin/main
    }
}
