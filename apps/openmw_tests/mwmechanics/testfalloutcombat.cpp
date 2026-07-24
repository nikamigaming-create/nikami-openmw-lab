#include <apps/openmw/mwmechanics/falloutcombat.hpp>
#include <apps/openmw/mwmechanics/actors.hpp>
#include <apps/openmw/mwworld/ptr.hpp>

#include <components/esm4/loadproj.hpp>
#include <components/esm4/loadscpt.hpp>
#include <components/esm4/loadweap.hpp>

#include <gtest/gtest.h>
#include <osg/Math>

#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

namespace
{
    ESM::FormId id(std::uint32_t value)
    {
        return ESM::FormId::fromUint32(value);
    }

    ESM4::ActorFaction membership(std::uint32_t value)
    {
        return ESM4::ActorFaction{ value, 0, 0, 0, 0 };
    }

    MWMechanics::FalloutExplosionKnockdownTuning retailExplosionKnockdownTuning()
    {
        return { 0.3f, 0.f, 1.f, 0.f, 0.25f, 50.f, 75.f };
    }

    TEST(FalloutCombatTest, EmptyFollowTargetHasNoSidingActors)
    {
        const MWMechanics::Actors actors;
        EXPECT_TRUE(actors.getActorsSidingWith(MWWorld::Ptr{}).empty());
    }

    TEST(FalloutCombatTest, DefendsSharedFactionAndAuthoredAlliesAfterAssault)
    {
        using Reaction = ESM4::Faction::GroupCombatReaction;
        const std::array victim{ membership(0x01104c6e), membership(0x0116311a) };
        const std::array sameFaction{ membership(0x01104c6e) };
        const std::array unrelated{ membership(0x0113f89b) };
        const std::array noFaction{ membership(0) };

        EXPECT_TRUE(MWMechanics::shouldFalloutActorDefendVictim(sameFaction, victim, Reaction::Neutral));
        EXPECT_TRUE(MWMechanics::shouldFalloutActorDefendVictim(unrelated, victim, Reaction::Ally));
        EXPECT_TRUE(MWMechanics::shouldFalloutActorDefendVictim(unrelated, victim, Reaction::Friend));
        EXPECT_FALSE(MWMechanics::shouldFalloutActorDefendVictim(unrelated, victim, Reaction::Neutral));
        EXPECT_FALSE(MWMechanics::shouldFalloutActorDefendVictim(unrelated, victim, Reaction::Enemy));
        EXPECT_FALSE(MWMechanics::shouldFalloutActorDefendVictim(noFaction, victim, std::nullopt));
    }

    TEST(FalloutCombatTest, AppliesCategoricalAggressionWithoutMorrowindFightBiases)
    {
        using Reaction = ESM4::Faction::GroupCombatReaction;

        for (Reaction reaction : { Reaction::Neutral, Reaction::Enemy, Reaction::Ally, Reaction::Friend })
            EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(0, reaction));

        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(1, Reaction::Neutral));
        EXPECT_TRUE(MWMechanics::shouldFalloutActorInitiateCombat(1, Reaction::Enemy));
        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(1, Reaction::Ally));
        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(1, Reaction::Friend));

        EXPECT_TRUE(MWMechanics::shouldFalloutActorInitiateCombat(2, Reaction::Neutral));
        EXPECT_TRUE(MWMechanics::shouldFalloutActorInitiateCombat(2, Reaction::Enemy));
        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(2, Reaction::Ally));
        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(2, Reaction::Friend));

        for (Reaction reaction : { Reaction::Neutral, Reaction::Enemy, Reaction::Ally, Reaction::Friend })
            EXPECT_TRUE(MWMechanics::shouldFalloutActorInitiateCombat(3, reaction));

        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(1, std::nullopt));
        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(2, std::nullopt));
        EXPECT_TRUE(MWMechanics::shouldFalloutActorInitiateCombat(3, std::nullopt));
        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(4, Reaction::Enemy));
    }

    TEST(FalloutCombatTest, AppliesCategoricalConfidenceWithoutMorrowindDistanceBias)
    {
        EXPECT_TRUE(MWMechanics::shouldFalloutActorFlee(0));
        for (std::uint8_t confidence = 1; confidence <= 4; ++confidence)
            EXPECT_FALSE(MWMechanics::shouldFalloutActorFlee(confidence));
        EXPECT_FALSE(MWMechanics::shouldFalloutActorFlee(5));
        EXPECT_FALSE(MWMechanics::shouldFalloutActorFlee(255));
    }

    TEST(FalloutCombatTest, KeepsExactGoodspringsSettlerAndEasyPeteAggressionOneNeutralToPlayerFaction)
    {
        using Reaction = ESM4::Faction::GroupCombatReaction;
        constexpr std::uint32_t goodspringsFaction = 0x01104c6e;
        constexpr std::uint32_t goodspringsDialogueFaction = 0x0116311a;
        constexpr std::uint32_t goodspringsMilitiaFaction = 0x0115ec58;
        constexpr std::uint32_t mojaveCivilianDialogueFaction = 0x0113f89b;
        constexpr std::uint32_t mojaveRancherDialogueFaction = 0x0113f89e;

        std::map<ESM::FormId, ESM4::Faction> factions;
        for (std::uint32_t value : { goodspringsFaction, goodspringsDialogueFaction, goodspringsMilitiaFaction,
                 mojaveCivilianDialogueFaction, mojaveRancherDialogueFaction })
        {
            ESM4::Faction faction;
            faction.mId = id(value);
            factions.emplace(faction.mId, faction);
        }

        // GSSettlerAM 0x00104f07 delegates factions and AI to GSSettlerAAM 0x00104f02. This is the effective
        // template membership order; both records author aggression 1 (Enemy only).
        const std::array actorFactions{ membership(mojaveRancherDialogueFaction),
            membership(mojaveCivilianDialogueFaction), membership(goodspringsFaction),
            membership(goodspringsDialogueFaction) };
        // Exact winning FalloutNV.esm Player NPC_ 0x01000007 membership order, including the inherited note/share
        // factions and PlayerFaction. None has an authored XNAM relation from either Goodsprings actor fixture.
        const std::array playerFactions{ membership(0x01047cd7), membership(0x01047cd6), membership(0x01047cd5),
            membership(0x01047cd4), membership(0x01047cd3), membership(0x01047cd2), membership(0x01047cd1),
            membership(0x01047cd0), membership(0x01047ccf), membership(0x01047cce), membership(0x01047ccd),
            membership(0x01047ccc), membership(0x01047ccb), membership(0x01047cca), membership(0x01047cc9),
            membership(0x01047cc8), membership(0x01047cc7), membership(0x01047cc6), membership(0x01047cc5),
            membership(0x0101b2a4), membership(0x010c3370), membership(0x0107e712) };
        const auto reaction = MWMechanics::resolveFalloutFactionReaction(actorFactions, playerFactions,
            [&](ESM::FormId factionId) -> const ESM4::Faction* {
                const auto found = factions.find(factionId);
                return found != factions.end() ? &found->second : nullptr;
            });

        ASSERT_EQ(reaction, Reaction::Neutral);
        // FNV aggression 1 means "attack enemies", regardless of conversational distance.
        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(1, reaction));

        // Easy Pete 0x00104c7f authors aggression 1 and these two SNAM memberships in this order.
        const std::array easyPeteFactions{ membership(goodspringsMilitiaFaction), membership(goodspringsFaction) };
        const auto easyPeteReaction = MWMechanics::resolveFalloutFactionReaction(easyPeteFactions, playerFactions,
            [&](ESM::FormId factionId) -> const ESM4::Faction* {
                const auto found = factions.find(factionId);
                return found != factions.end() ? &found->second : nullptr;
            });
        ASSERT_EQ(easyPeteReaction, Reaction::Neutral);
        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(1, easyPeteReaction));
    }

    TEST(FalloutCombatTest, AggressiveGoodspringsMantisInitiatesAgainstPlayerFaction)
    {
        using Reaction = ESM4::Faction::GroupCombatReaction;
        constexpr std::uint32_t mantisFaction = 0x010e60e2;
        constexpr std::uint32_t creatureFaction = 0x01000013;
        constexpr std::uint32_t playerFaction = 0x0101b2a4;

        std::map<ESM::FormId, ESM4::Faction> factions;
        ESM4::Faction creature;
        creature.mId = id(creatureFaction);
        factions.emplace(creature.mId, creature);
        ESM4::Faction mantis;
        mantis.mId = id(mantisFaction);
        mantis.mRelations.push_back({ id(playerFaction), 0, Reaction::Enemy });
        factions.emplace(mantis.mId, mantis);

        // GSGiantMantisNymph 0x0111d584: prove both normalized memberships are consumed by putting the neutral
        // CreatureFaction first and the enemy-bearing MantisFaction second.
        const std::array actorFactions{ membership(creatureFaction), membership(mantisFaction) };
        const std::array playerFactions{ membership(playerFaction) };
        const auto reaction = MWMechanics::resolveFalloutFactionReaction(actorFactions, playerFactions,
            [&](ESM::FormId factionId) -> const ESM4::Faction* {
                const auto found = factions.find(factionId);
                return found != factions.end() ? &found->second : nullptr;
            });

        ASSERT_EQ(reaction, Reaction::Enemy);
        EXPECT_TRUE(MWMechanics::shouldFalloutActorInitiateCombat(1, reaction));
    }

    TEST(FalloutCombatTest, UnknownFactionIdentityFailsClosed)
    {
        const std::array actorFactions{ membership(0x00dead01) };
        const std::array playerFactions{ membership(0x0001b2a4) };
        const auto reaction = MWMechanics::resolveFalloutFactionReaction(
            actorFactions, playerFactions, [](ESM::FormId) -> const ESM4::Faction* { return nullptr; });

        EXPECT_FALSE(reaction.has_value());
        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(1, reaction));
        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(2, reaction));
    }

    TEST(FalloutCombatTest, ActorWithoutFactionIsNeutralToKnownTargetFaction)
    {
        using Reaction = ESM4::Faction::GroupCombatReaction;
        const std::array<ESM4::ActorFaction, 0> actorFactions{};
        const std::array playerFactions{ membership(0x0001b2a4) };
        const auto reaction = MWMechanics::resolveFalloutFactionReaction(
            actorFactions, playerFactions, [](ESM::FormId) -> const ESM4::Faction* { return nullptr; });

        ASSERT_EQ(reaction, Reaction::Neutral);
        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(1, reaction));
        EXPECT_TRUE(MWMechanics::shouldFalloutActorInitiateCombat(2, reaction));
    }

    TEST(FalloutCombatTest, TargetWithoutFactionIdentityFailsClosed)
    {
        const std::array actorFactions{ membership(0x000e60e2) };
        const std::array<ESM4::ActorFaction, 0> targetFactions{};
        const auto reaction = MWMechanics::resolveFalloutFactionReaction(
            actorFactions, targetFactions, [](ESM::FormId) -> const ESM4::Faction* { return nullptr; });

        EXPECT_FALSE(reaction.has_value());
        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(1, reaction));
        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(2, reaction));
    }

    TEST(FalloutCombatTest, AuthoredAllyRelationPreventsConflictingEnemyInitiation)
    {
        using Reaction = ESM4::Faction::GroupCombatReaction;
        constexpr std::uint32_t enemyFaction = 0x01000010;
        constexpr std::uint32_t sharedFaction = 0x01000020;

        std::map<ESM::FormId, ESM4::Faction> factions;
        ESM4::Faction enemy;
        enemy.mId = id(enemyFaction);
        enemy.mRelations.push_back({ id(sharedFaction), 0, Reaction::Enemy });
        factions.emplace(enemy.mId, enemy);
        ESM4::Faction shared;
        shared.mId = id(sharedFaction);
        shared.mRelations.push_back({ id(sharedFaction), 0, Reaction::Ally });
        factions.emplace(shared.mId, shared);

        const std::array actorFactions{ membership(enemyFaction), membership(sharedFaction) };
        const std::array targetFactions{ membership(sharedFaction) };
        const auto reaction = MWMechanics::resolveFalloutFactionReaction(actorFactions, targetFactions,
            [&](ESM::FormId factionId) -> const ESM4::Faction* {
                const auto found = factions.find(factionId);
                return found != factions.end() ? &found->second : nullptr;
            });

        EXPECT_EQ(reaction, Reaction::Ally);
        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(1, reaction));
        EXPECT_FALSE(MWMechanics::shouldFalloutActorInitiateCombat(2, reaction));
    }

    TEST(FalloutCombatTest, SetAllyUpdatesBothCombatDirectionsWithoutDuplicateRelations)
    {
        using Reaction = ESM4::Faction::GroupCombatReaction;
        ESM4::Faction sunny;
        sunny.mId = id(0x001691ea);
        sunny.mRelations.push_back({ id(0x0001b2a4), -25, Reaction::Enemy });
        sunny.mRelations.push_back({ id(0x00000042), 10, Reaction::Friend });
        ESM4::Faction player;
        player.mId = id(0x0001b2a4);

        ASSERT_TRUE(MWMechanics::setFalloutFactionsAllied(sunny, player));
        ASSERT_EQ(sunny.mRelations.size(), 2);
        EXPECT_EQ(sunny.mRelations[0].mFaction, player.mId);
        EXPECT_EQ(sunny.mRelations[0].mModifier, -25);
        EXPECT_EQ(sunny.mRelations[0].mGroupCombatReaction, Reaction::Ally);
        ASSERT_EQ(player.mRelations.size(), 1);
        EXPECT_EQ(player.mRelations[0].mFaction, sunny.mId);
        EXPECT_EQ(player.mRelations[0].mModifier, 0);
        EXPECT_EQ(player.mRelations[0].mGroupCombatReaction, Reaction::Ally);

        ASSERT_TRUE(MWMechanics::setFalloutFactionsAllied(sunny, player));
        EXPECT_EQ(sunny.mRelations.size(), 2);
        EXPECT_EQ(player.mRelations.size(), 1);

        const std::array sunnyMembership{ membership(sunny.mId.toUint32()) };
        const std::array playerMembership{ membership(player.mId.toUint32()) };
        const auto lookup = [&](ESM::FormId faction) -> const ESM4::Faction* {
            if (faction == sunny.mId)
                return &sunny;
            if (faction == player.mId)
                return &player;
            return nullptr;
        };
        EXPECT_EQ(MWMechanics::resolveFalloutFactionReaction(sunnyMembership, playerMembership, lookup),
            Reaction::Ally);
        EXPECT_EQ(MWMechanics::resolveFalloutFactionReaction(playerMembership, sunnyMembership, lookup),
            Reaction::Ally);
    }

    TEST(FalloutCombatTest, SetEnemyAppliesDirectionalNeutralFlagsWithoutDuplicateRelations)
    {
        using Reaction = ESM4::Faction::GroupCombatReaction;
        ESM4::Faction goodsprings;
        goodsprings.mId = id(0x00104c6e);
        goodsprings.mRelations.push_back({ id(0x0001b2a4), 35, Reaction::Ally });
        ESM4::Faction player;
        player.mId = id(0x0001b2a4);
        player.mRelations.push_back({ goodsprings.mId, -10, Reaction::Friend });

        ASSERT_TRUE(MWMechanics::setFalloutFactionsEnemy(goodsprings, player, false, true));
        ASSERT_EQ(goodsprings.mRelations.size(), 1);
        EXPECT_EQ(goodsprings.mRelations[0].mModifier, 35);
        EXPECT_EQ(goodsprings.mRelations[0].mGroupCombatReaction, Reaction::Enemy);
        ASSERT_EQ(player.mRelations.size(), 1);
        EXPECT_EQ(player.mRelations[0].mModifier, -10);
        EXPECT_EQ(player.mRelations[0].mGroupCombatReaction, Reaction::Neutral);

        ASSERT_TRUE(MWMechanics::setFalloutFactionsEnemy(goodsprings, player, true, true));
        EXPECT_EQ(goodsprings.mRelations.size(), 1);
        EXPECT_EQ(player.mRelations.size(), 1);
        EXPECT_EQ(goodsprings.mRelations[0].mGroupCombatReaction, Reaction::Neutral);
        EXPECT_EQ(player.mRelations[0].mGroupCombatReaction, Reaction::Neutral);
    }

    TEST(FalloutCombatTest, SelectsFirstAvailableAmmoInAuthoredListOrder)
    {
        const std::array candidates{ id(0x10), id(0x20), id(0x30) };
        const std::unordered_map<std::uint32_t, int> counts{ { 0x10, 0 }, { 0x20, 2 }, { 0x30, 99 } };

        const auto selected = MWMechanics::selectAuthoredFalloutAmmo(candidates, 1,
            [](ESM::FormId candidate) { return candidate.toUint32() != 0x10; },
            [&](ESM::FormId candidate) { return counts.at(candidate.toUint32()); });

        ASSERT_TRUE(selected);
        EXPECT_EQ(selected->toUint32(), 0x20);
    }

    TEST(FalloutCombatTest, DoesNotInventFallbackAmmo)
    {
        const std::array candidates{ id(0x10), id(0x20) };
        const auto selected = MWMechanics::selectAuthoredFalloutAmmo(candidates, 2,
            [](ESM::FormId) { return true; }, [](ESM::FormId) { return 1; });
        EXPECT_FALSE(selected);
    }

    TEST(FalloutCombatTest, PreservesExactRetailServiceRifleShotContract)
    {
        ESM4::Weapon weapon;
        weapon.mData.hasBallistics = true;
        weapon.mData.projectile = id(0x426d);
        weapon.mData.ammoUse = 1;
        weapon.mData.numProjectiles = 1;
        weapon.mData.damage = 18;
        weapon.mData.minRange = 768.f;
        weapon.mData.maxRange = 3548.f;

        ESM4::Projectile projectile;
        projectile.mId = id(0x426d);
        projectile.mData.present = true;
        projectile.mData.flags = ESM4::Projectile::Hitscan;
        projectile.mData.range = 10000.f;

        MWMechanics::FalloutShotFailure failure;
        const auto contract = MWMechanics::buildFalloutRayShotContract(weapon, projectile, id(0x4240), failure);

        ASSERT_TRUE(contract);
        EXPECT_EQ(failure, MWMechanics::FalloutShotFailure::None);
        EXPECT_EQ(contract->mAmmo.toUint32(), 0x4240);
        EXPECT_EQ(contract->mProjectile.toUint32(), 0x426d);
        EXPECT_EQ(contract->mAmmoUse, 1);
        EXPECT_EQ(contract->mProjectileCount, 1);
        EXPECT_FLOAT_EQ(contract->mDamage, 18.f);
        EXPECT_FLOAT_EQ(contract->mMinRange, 768.f);
        EXPECT_FLOAT_EQ(contract->mMaxRange, 3548.f);
        EXPECT_FLOAT_EQ(contract->mProjectileRange, 10000.f);
        EXPECT_FLOAT_EQ(contract->damagePerProjectile(), 18.f);
        EXPECT_TRUE(contract->mAuthoredHitscan);
    }

    TEST(FalloutCombatTest, PreservesWeaponAuthoredVatsContractWithoutFallback)
    {
        ESM4::Weapon weapon;
        weapon.mData.hasBallistics = true;
        weapon.mData.flags2 = 0x00000008;
        weapon.mData.overrideActionPoints = 22.f;
        weapon.mData.baseVatsChance = 42;
        weapon.mData.limbDamageMult = 0.75f;
        weapon.mData.skillActorValue = 32;

        MWMechanics::FalloutVatsWeaponFailure failure;
        const auto contract = MWMechanics::buildFalloutVatsWeaponContract(weapon, failure);
        ASSERT_TRUE(contract);
        EXPECT_EQ(failure, MWMechanics::FalloutVatsWeaponFailure::None);
        EXPECT_FLOAT_EQ(contract->mActionPointCost, 22.f);
        EXPECT_EQ(contract->mBaseHitChance, 42);
        EXPECT_FLOAT_EQ(contract->mLimbDamageMultiplier, 0.75f);
        EXPECT_EQ(contract->mSkillActorValue, 32);
    }

    TEST(FalloutCombatTest, RefusesToInventVatsActionPointCost)
    {
        ESM4::Weapon weapon;
        weapon.mData.hasBallistics = true;
        weapon.mData.overrideActionPoints = 22.f;

        MWMechanics::FalloutVatsWeaponFailure failure;
        EXPECT_FALSE(MWMechanics::buildFalloutVatsWeaponContract(weapon, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutVatsWeaponFailure::MissingAuthoredActionPointOverride);
    }

    TEST(FalloutCombatTest, QueuesObservedVatsTargetLimbChanceAndReservesActionPoints)
    {
        MWMechanics::FalloutVatsWeaponContract weapon{ 22.f, 42, 0.75f, 32 };
        const MWMechanics::FalloutVatsQueuedAction first{ id(0x100), 0, 71, 22.f, 1.f, 1.f, 60 };
        MWMechanics::FalloutVatsQueueFailure failure;
        const auto action = MWMechanics::queueFalloutVatsAction(
            std::span(&first, 1), id(0x200), 1, 83, 2.f, 20, 50.f, 2, weapon, failure);

        ASSERT_TRUE(action);
        EXPECT_EQ(failure, MWMechanics::FalloutVatsQueueFailure::None);
        EXPECT_EQ(action->mTarget, id(0x200));
        EXPECT_EQ(action->mBodyPart, 1);
        EXPECT_EQ(action->mDisplayedHitChance, 83);
        EXPECT_FLOAT_EQ(action->mActionPointCost, 22.f);
        EXPECT_FLOAT_EQ(action->mHealthDamageMultiplier, 2.f);
        EXPECT_FLOAT_EQ(action->mLimbDamageMultiplier, 0.75f);
        EXPECT_EQ(action->mHealthPercent, 20);
        EXPECT_FLOAT_EQ(MWMechanics::getFalloutVatsReservedActionPoints(std::span(&first, 1)), 22.f);
    }

    TEST(FalloutCombatTest, RejectsVatsQueueWhenReservedActionPointsExceedCurrentValue)
    {
        MWMechanics::FalloutVatsWeaponContract weapon{ 22.f, 42, 1.f, 32 };
        const MWMechanics::FalloutVatsQueuedAction first{ id(0x100), 0, 71, 22.f, 1.f, 1.f, 60 };
        MWMechanics::FalloutVatsQueueFailure failure;
        EXPECT_FALSE(MWMechanics::queueFalloutVatsAction(
            std::span(&first, 1), id(0x200), 1, 83, 1.f, 20, 40.f, 2, weapon, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutVatsQueueFailure::InsufficientActionPoints);
    }

    TEST(FalloutCombatTest, RejectsVatsQueueBeyondAvailableAuthoredAmmunition)
    {
        MWMechanics::FalloutVatsWeaponContract weapon{ 10.f, 42, 1.f, 32 };
        const MWMechanics::FalloutVatsQueuedAction first{ id(0x100), 0, 71, 10.f, 1.f, 1.f, 60 };
        MWMechanics::FalloutVatsQueueFailure failure;
        EXPECT_FALSE(MWMechanics::queueFalloutVatsAction(
            std::span(&first, 1), id(0x200), 1, 83, 1.f, 20, 80.f, 1, weapon, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutVatsQueueFailure::InsufficientAmmunition);
    }

    TEST(FalloutCombatTest, PreservesAuthoredVatsBodyPartContract)
    {
        ESM4::BodyPartData::BodyPart bodyPart;
        bodyPart.mPartName = "Head";
        bodyPart.mVATSTarget = "Bip01 Head";
        bodyPart.mData.actorValue = 25;
        bodyPart.mData.toHitChance = 35;
        bodyPart.mData.damageMult = 2.f;
        bodyPart.mData.healthPercent = 20;
        bodyPart.mData.flags = 0x40;
        MWMechanics::FalloutVatsBodyPartFailure failure;
        const auto contract = MWMechanics::buildFalloutVatsBodyPartContract(bodyPart, 1, failure);

        ASSERT_TRUE(contract);
        EXPECT_EQ(failure, MWMechanics::FalloutVatsBodyPartFailure::None);
        EXPECT_EQ(contract->mIndex, 1);
        EXPECT_EQ(contract->mName, "Head");
        EXPECT_EQ(contract->mTargetNode, "Bip01 Head");
        EXPECT_EQ(contract->mActorValue, 25);
        EXPECT_EQ(contract->mBaseHitChance, 35);
        EXPECT_EQ(contract->mHealthPercent, 20);
        EXPECT_FLOAT_EQ(contract->mHealthDamageMultiplier, 2.f);
        EXPECT_TRUE(contract->mAbsoluteHitChance);
    }

    TEST(FalloutCombatTest, ComputesEveryDisplayedVatsLimbChanceFromAuthoredContracts)
    {
        const MWMechanics::FalloutVatsWeaponContract weapon{ 22.f, 42, 1.f, 32 };
        const MWMechanics::FalloutVatsBodyPartContract relative{
            1, "Head", "Bip01 Head", 25, 35, 20, 2.f, false };
        const MWMechanics::FalloutVatsBodyPartContract capped{
            2, "Torso", "Bip01 Spine2", 26, 75, 60, 1.f, false };
        const MWMechanics::FalloutVatsBodyPartContract absolute{
            3, "Left Arm", "Bip01 L UpperArm", 27, 31, 25, 1.f, true };

        EXPECT_EQ(MWMechanics::getFalloutVatsDisplayedHitChance(relative, weapon), 77u);
        EXPECT_EQ(MWMechanics::getFalloutVatsDisplayedHitChance(capped, weapon), 100u);
        EXPECT_EQ(MWMechanics::getFalloutVatsDisplayedHitChance(absolute, weapon), 31u);
    }

    TEST(FalloutCombatTest, FramesVatsCameraOnRenderedActorFront)
    {
        const auto pose = MWMechanics::buildFalloutVatsFrontalCameraPose(
            osg::Vec3f(10.f, 20.f, 30.f), 100.f, osg::Vec3f(2.f, 0.f, 5.f));
        EXPECT_EQ(pose.mFocus, osg::Vec3f(10.f, 20.f, 30.f));
        EXPECT_NEAR(pose.mEye.x(), 225.f, 0.001f);
        EXPECT_NEAR(pose.mEye.y(), 20.f, 0.001f);
        EXPECT_NEAR(pose.mEye.z(), 30.f, 0.001f);

        const auto fallback = MWMechanics::buildFalloutVatsFrontalCameraPose(
            osg::Vec3f(), std::numeric_limits<float>::quiet_NaN(), osg::Vec3f());
        EXPECT_EQ(fallback.mFocus, osg::Vec3f());
        EXPECT_NEAR(fallback.mEye.x(), 0.f, 0.001f);
        EXPECT_NEAR(fallback.mEye.y(), 172.f, 0.001f);
        EXPECT_NEAR(fallback.mEye.z(), 0.f, 0.001f);
    }

    TEST(FalloutCombatTest, RunsVatsQueueAsOneActionPointTransaction)
    {
        MWMechanics::FalloutVatsRuntime runtime;
        ASSERT_TRUE(runtime.enter(80.f));
        const MWMechanics::FalloutVatsBodyPartContract head{ 1, "Head", "Bip01 Head", 25, 35, 20, 2.f, false };
        ASSERT_TRUE(runtime.select(id(0x1234), head, 73));

        const MWMechanics::FalloutVatsWeaponContract weapon{ 22.f, 42, 0.75f, 32 };
        MWMechanics::FalloutVatsQueueFailure failure;
        ASSERT_TRUE(runtime.queueSelected(weapon, 2, failure));
        ASSERT_TRUE(runtime.queueSelected(weapon, 2, failure));
        EXPECT_EQ(runtime.getPhase(), MWMechanics::FalloutVatsPhase::Targeting);
        EXPECT_FLOAT_EQ(runtime.getReservedActionPoints(), 44.f);

        const std::optional<float> actionPointsAfter = runtime.beginExecution();
        ASSERT_TRUE(actionPointsAfter);
        EXPECT_FLOAT_EQ(*actionPointsAfter, 36.f);
        ASSERT_NE(runtime.getExecutingAction(), nullptr);
        EXPECT_EQ(runtime.getExecutingAction()->mTarget, id(0x1234));
        EXPECT_EQ(runtime.getExecutingAction()->mBodyPart, 1);
        EXPECT_EQ(runtime.getExecutingAction()->mDisplayedHitChance, 73);
        EXPECT_FLOAT_EQ(runtime.getExecutingAction()->mHealthDamageMultiplier, 2.f);
        EXPECT_FLOAT_EQ(runtime.getExecutingAction()->mLimbDamageMultiplier, 0.75f);
        EXPECT_EQ(runtime.getExecutingAction()->mHealthPercent, 20);
        EXPECT_EQ(runtime.getExecutingAction()->mBodyPartName, "Head");
        EXPECT_EQ(runtime.getExecutingAction()->mTargetNode, "Bip01 Head");
        EXPECT_EQ(runtime.getExecutingAction()->mActorValue, 25);

        EXPECT_TRUE(runtime.advanceExecution());
        ASSERT_NE(runtime.getExecutingAction(), nullptr);
        EXPECT_TRUE(runtime.advanceExecution());
        EXPECT_TRUE(runtime.isExecutionComplete());
        EXPECT_EQ(runtime.getPhase(), MWMechanics::FalloutVatsPhase::Executing);
        EXPECT_TRUE(runtime.finishExecution());
        EXPECT_EQ(runtime.getPhase(), MWMechanics::FalloutVatsPhase::Inactive);
        EXPECT_TRUE(runtime.getQueue().empty());
    }

    TEST(FalloutCombatTest, ResolvesDisplayedVatsChanceAtExactPercentageBoundary)
    {
        EXPECT_FALSE(MWMechanics::doesFalloutVatsAttackHit(0, 0.f));
        EXPECT_TRUE(MWMechanics::doesFalloutVatsAttackHit(1, 0.f));
        EXPECT_TRUE(MWMechanics::doesFalloutVatsAttackHit(73, 0.72999f));
        EXPECT_FALSE(MWMechanics::doesFalloutVatsAttackHit(73, 0.73f));
        EXPECT_TRUE(MWMechanics::doesFalloutVatsAttackHit(100, 0.99999f));
        EXPECT_FALSE(MWMechanics::doesFalloutVatsAttackHit(100, 1.f));
    }

    TEST(FalloutCombatTest, ResolvesDeepestExactRenderedNodeToAuthoredBodyPart)
    {
        ESM4::BodyPartData bodyData;
        ESM4::BodyPartData::BodyPart torso;
        torso.mPartName = "Torso";
        torso.mNodeName = "Bip01 Spine2";
        torso.mVATSTarget = "Bip01 Spine2";
        torso.mData.actorValue = 26;
        torso.mData.healthPercent = 60;
        torso.mData.damageMult = 1.f;
        bodyData.mBodyParts.push_back(torso);
        ESM4::BodyPartData::BodyPart head;
        head.mPartName = "Head";
        head.mNodeName = "Bip01 Head";
        head.mVATSTarget = "Bip01 Head";
        head.mData.actorValue = 25;
        head.mData.healthPercent = 20;
        head.mData.damageMult = 2.f;
        bodyData.mBodyParts.push_back(head);

        const std::vector<std::string> path{ "Scene Root", "Bip01 Spine2", "Bip01 Head" };
        const auto resolved = MWMechanics::resolveFalloutBodyPartFromNodePath(bodyData, path);
        ASSERT_TRUE(resolved);
        EXPECT_EQ(resolved->mIndex, 1);
        EXPECT_EQ(resolved->mName, "Head");
        EXPECT_EQ(resolved->mActorValue, 25);
        EXPECT_FLOAT_EQ(resolved->mHealthDamageMultiplier, 2.f);

        const std::vector<std::string> nonExact{ "Scene Root", "Bip01 Headgear" };
        EXPECT_FALSE(MWMechanics::resolveFalloutBodyPartFromNodePath(bodyData, nonExact));
    }

    TEST(FalloutCombatTest, AppliesIndependentRetailLimbConditionAndCrippleTransition)
    {
        // A 25-percent limb on a 100-health actor has 25 condition. The health channel's armor result does not
        // enter this calculation: 10 raw hit damage * 1.5 weapon * 2 player-target tuning applies 30 limb damage.
        const auto impact = MWMechanics::resolveFalloutLimbImpact(100.f, 25, 20.f, 10.f, 1.5f, 2.f);
        ASSERT_TRUE(impact);
        EXPECT_FLOAT_EQ(impact->mMaximumCondition, 25.f);
        EXPECT_FLOAT_EQ(impact->mConditionBefore, 5.f);
        EXPECT_FLOAT_EQ(impact->mDamageApplied, 30.f);
        EXPECT_FLOAT_EQ(impact->mDamageTakenAfter, 50.f);
        EXPECT_FLOAT_EQ(impact->mConditionAfter, 0.f);
        EXPECT_TRUE(impact->mNewlyCrippled);

        const auto alreadyCrippled = MWMechanics::resolveFalloutLimbImpact(100.f, 25, 50.f, 10.f, 1.5f, 2.f);
        ASSERT_TRUE(alreadyCrippled);
        EXPECT_FALSE(alreadyCrippled->mNewlyCrippled);
        EXPECT_FALSE(MWMechanics::resolveFalloutLimbImpact(100.f, 0, 0.f, 10.f, 1.f, 1.f));
    }

    TEST(FalloutCombatTest, PreservesGenericShotgunRaysAndTotalAuthoredDamage)
    {
        ESM4::Weapon weapon;
        weapon.mData.hasBallistics = true;
        weapon.mData.projectile = id(0x426d);
        weapon.mData.ammoUse = 1;
        weapon.mData.numProjectiles = 7;
        weapon.mData.damage = 70;
        weapon.mData.minSpread = 0.5f;
        weapon.mData.spread = 1.5f;
        ESM4::Projectile projectile;
        projectile.mId = id(0x426d);
        projectile.mData.present = true;
        projectile.mData.flags = ESM4::Projectile::Hitscan;
        projectile.mData.range = 10000.f;

        MWMechanics::FalloutShotFailure failure;
        const auto contract = MWMechanics::buildFalloutRayShotContract(weapon, projectile, id(0x4240), failure);

        ASSERT_TRUE(contract);
        EXPECT_EQ(failure, MWMechanics::FalloutShotFailure::None);
        EXPECT_EQ(contract->mAmmoUse, 1);
        EXPECT_EQ(contract->mProjectileCount, 7);
        EXPECT_FLOAT_EQ(contract->mDamage, 70.f);
        EXPECT_FLOAT_EQ(contract->damagePerProjectile(), 10.f);
        EXPECT_FLOAT_EQ(contract->mMinSpread, 0.5f);
        EXPECT_FLOAT_EQ(contract->mLegacySpread, 1.5f);
        EXPECT_TRUE(contract->mAuthoredHitscan);
    }

    TEST(FalloutCombatTest, PreservesAuthoredNonHitscanProjectileForMovingDelivery)
    {
        ESM4::Weapon weapon;
        weapon.mData.hasBallistics = true;
        weapon.mData.projectile = id(0x9001);
        weapon.mData.ammoUse = 2;
        weapon.mData.numProjectiles = 1;
        weapon.mData.damage = 42;
        weapon.mData.spread = 0.75f;

        ESM4::Projectile projectile;
        projectile.mId = id(0x9001);
        projectile.mData.present = true;
        projectile.mData.flags = ESM4::Projectile::Rotates;
        projectile.mData.range = 4096.f;

        MWMechanics::FalloutShotFailure failure;
        const auto contract = MWMechanics::buildFalloutRayShotContract(weapon, projectile, id(0x9002), failure);

        ASSERT_TRUE(contract);
        EXPECT_EQ(failure, MWMechanics::FalloutShotFailure::None);
        EXPECT_EQ(contract->mAmmoUse, 2);
        EXPECT_EQ(contract->mProjectileCount, 1);
        EXPECT_FLOAT_EQ(contract->mDamage, 42.f);
        EXPECT_FLOAT_EQ(contract->mProjectileRange, 4096.f);
        EXPECT_FALSE(contract->mAuthoredHitscan);
    }

    TEST(FalloutCombatTest, BuildsThrownWeaponContractThatConsumesExactlyOneWeapon)
    {
        ESM4::Weapon weapon;
        weapon.mId = id(0x14d2ac);
        weapon.mData.animationType = 13;
        weapon.mData.hasBallistics = true;
        weapon.mData.projectile = id(0x9001);
        weapon.mData.ammoUse = 0;
        weapon.mData.numProjectiles = 1;
        weapon.mData.damage = 50;
        ESM4::Projectile projectile;
        projectile.mId = id(0x9001);
        projectile.mData.present = true;
        projectile.mData.range = 4096.f;

        MWMechanics::FalloutShotFailure failure;
        const auto contract
            = MWMechanics::buildFalloutRayShotContract(weapon, projectile, weapon.mId, failure);

        ASSERT_TRUE(contract);
        EXPECT_EQ(failure, MWMechanics::FalloutShotFailure::None);
        EXPECT_TRUE(contract->mConsumesWeapon);
        EXPECT_EQ(contract->mAmmo, weapon.mId);
        EXPECT_EQ(contract->mAmmoUse, 1);
        EXPECT_FALSE(contract->mAuthoredHitscan);

        EXPECT_FALSE(MWMechanics::buildFalloutRayShotContract(weapon, projectile, ESM::FormId{}, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutShotFailure::MissingAmmo);
    }

    TEST(FalloutCombatTest, ClassifiesAuthoredGrenadeMineAndPlacedExplosiveFamiliesAsConsumableWeapons)
    {
        ESM4::Weapon weapon;

        for (const std::uint8_t animationType : { 10, 11, 12, 13 })
        {
            weapon.mData.animationType = animationType;
            EXPECT_TRUE(MWMechanics::isFalloutThrownWeapon(weapon)) << "animationType=" << +animationType;
        }

        for (const std::uint8_t animationType : { 9, 14 })
        {
            weapon.mData.animationType = animationType;
            EXPECT_FALSE(MWMechanics::isFalloutThrownWeapon(weapon)) << "animationType=" << +animationType;
        }
    }

    TEST(FalloutCombatTest, SemiAutomaticTriggerFiresOncePerPress)
    {
        ESM4::Weapon weapon;
        MWMechanics::FalloutFireCadenceFailure failure;
        const auto cadence = MWMechanics::buildFalloutFireCadence(weapon, failure);
        ASSERT_TRUE(cadence);
        EXPECT_FALSE(cadence->mAutomatic);

        MWMechanics::FalloutTriggerState trigger;
        EXPECT_TRUE(MWMechanics::advanceFalloutTrigger(trigger, true, true, *cadence, 0.f));
        EXPECT_FALSE(MWMechanics::advanceFalloutTrigger(trigger, true, true, *cadence, 1.f));
        EXPECT_FALSE(MWMechanics::advanceFalloutTrigger(trigger, false, true, *cadence, 0.f));
        EXPECT_TRUE(MWMechanics::advanceFalloutTrigger(trigger, true, true, *cadence, 0.f));
    }

    TEST(FalloutCombatTest, FlatPlayerUseReachesWeaponOnlyDuringOrdinaryArmedGameplay)
    {
        using Phase = MWMechanics::FalloutVatsPhase;
        EXPECT_TRUE(MWMechanics::shouldApplyFalloutPlayerUseInput(
            Phase::Inactive, true, true, false, true, true));

        EXPECT_FALSE(MWMechanics::shouldApplyFalloutPlayerUseInput(
            Phase::Inactive, true, true, true, true, true));
        EXPECT_FALSE(MWMechanics::shouldApplyFalloutPlayerUseInput(
            Phase::Inactive, false, true, false, true, true));
        EXPECT_FALSE(MWMechanics::shouldApplyFalloutPlayerUseInput(
            Phase::Inactive, true, false, false, true, true));
        EXPECT_FALSE(MWMechanics::shouldApplyFalloutPlayerUseInput(
            Phase::Inactive, true, true, false, false, true));
        EXPECT_FALSE(MWMechanics::shouldApplyFalloutPlayerUseInput(
            Phase::Inactive, true, true, false, true, false));
        EXPECT_FALSE(MWMechanics::shouldApplyFalloutPlayerUseInput(
            Phase::Targeting, true, true, false, true, true));
        EXPECT_FALSE(MWMechanics::shouldApplyFalloutPlayerUseInput(
            Phase::Executing, true, true, false, true, true));
    }

    TEST(FalloutCombatTest, SelectsAuthoredDeliveryKeysOnlyForNonAutomaticEventRoutedAttacks)
    {
        using Event = MWMechanics::FalloutAttackDeliveryEvent;

        for (const std::uint8_t animationType : { 0, 1, 2 })
            EXPECT_EQ(MWMechanics::getFalloutAttackDeliveryEvent(animationType, true, false), Event::Hit);
        for (const std::uint8_t animationType : { 10, 11, 12, 13 })
            EXPECT_EQ(MWMechanics::getFalloutAttackDeliveryEvent(animationType, true, false), Event::Release);

        EXPECT_EQ(MWMechanics::getFalloutAttackDeliveryEvent(9, false, false), Event::Hit);
        EXPECT_EQ(MWMechanics::getFalloutAttackDeliveryEvent(3, true, false), Event::None);
        EXPECT_EQ(MWMechanics::getFalloutAttackDeliveryEvent(14, false, false), Event::None);

        EXPECT_EQ(MWMechanics::getFalloutAttackDeliveryEvent(2, true, true), Event::None);
        EXPECT_EQ(MWMechanics::getFalloutAttackDeliveryEvent(9, false, true), Event::None);
        EXPECT_EQ(MWMechanics::getFalloutAttackDeliveryEvent(10, false, true), Event::None);
    }

    TEST(FalloutCombatTest, FallsBackToImmediateDeliveryWhenAuthoredVisualCannotPlay)
    {
        using Event = MWMechanics::FalloutAttackDeliveryEvent;

        EXPECT_FALSE(MWMechanics::shouldDeliverFalloutAttackImmediately(Event::Hit, true));
        EXPECT_FALSE(MWMechanics::shouldDeliverFalloutAttackImmediately(Event::Release, true));
        EXPECT_TRUE(MWMechanics::shouldDeliverFalloutAttackImmediately(Event::Hit, false));
        EXPECT_TRUE(MWMechanics::shouldDeliverFalloutAttackImmediately(Event::Release, false));
        EXPECT_TRUE(MWMechanics::shouldDeliverFalloutAttackImmediately(Event::None, true));
    }

    TEST(FalloutCombatTest, AuthoredAttackDeliveryRequiresExactWeaponGroupAndKeyAndConsumesOnce)
    {
        using Event = MWMechanics::FalloutAttackDeliveryEvent;
        MWMechanics::FalloutAttackDelivery state;
        ASSERT_TRUE(MWMechanics::queueFalloutAttackDelivery(state, Event::Hit, id(0x1234), 2, "attack2"));

        EXPECT_FALSE(MWMechanics::consumeFalloutAttackDelivery(state, id(0x5678), "attack2", "hit"));
        EXPECT_FALSE(MWMechanics::consumeFalloutAttackDelivery(state, id(0x1234), "attack1", "hit"));
        EXPECT_FALSE(MWMechanics::consumeFalloutAttackDelivery(state, id(0x1234), "attack2", "release"));
        EXPECT_TRUE(state.isPending());

        const std::optional<MWMechanics::FalloutAttackDelivery> delivered
            = MWMechanics::consumeFalloutAttackDelivery(state, id(0x1234), "attack2", "hit");
        ASSERT_TRUE(delivered);
        EXPECT_EQ(delivered->mEvent, Event::Hit);
        EXPECT_EQ(delivered->mAnimationType, 2);
        EXPECT_EQ(delivered->mAnimationGroup, "attack2");
        EXPECT_FALSE(state.isPending());
        EXPECT_FALSE(MWMechanics::consumeFalloutAttackDelivery(state, id(0x1234), "attack2", "hit"));
    }

    TEST(FalloutCombatTest, AuthoredAttackDeliveryAcceptsNamespacedReleaseAndRejectsInvalidQueue)
    {
        using Event = MWMechanics::FalloutAttackDeliveryEvent;
        MWMechanics::FalloutAttackDelivery state;
        EXPECT_FALSE(MWMechanics::queueFalloutAttackDelivery(state, Event::None, id(0x1234), 10, "attack1"));
        EXPECT_FALSE(MWMechanics::queueFalloutAttackDelivery(state, Event::Release, id(0x1234), 14, "attack1"));
        EXPECT_FALSE(MWMechanics::queueFalloutAttackDelivery(state, Event::Release, id(0x1234), 10, {}));

        ASSERT_TRUE(MWMechanics::queueFalloutAttackDelivery(state, Event::Release, id(0x1234), 10, "attack1"));
        const std::optional<MWMechanics::FalloutAttackDelivery> delivered
            = MWMechanics::consumeFalloutAttackDelivery(state, id(0x1234), "attack1", "attack1: release");
        ASSERT_TRUE(delivered);
        EXPECT_EQ(delivered->mEvent, Event::Release);
        EXPECT_FALSE(state.isPending());
    }

    TEST(FalloutCombatTest, AutomaticTriggerRepeatsAtAuthoredDurationCadence)
    {
        ESM4::Weapon weapon;
        weapon.mData.weaponFlags1 = ESM4::Weapon::Data::Automatic;
        weapon.mData.animationMultiplier = 1.f;
        weapon.mData.animAttackMult = 1.f;
        weapon.mData.fireRate = 10.f;
        MWMechanics::FalloutFireCadenceFailure failure;
        const auto cadence = MWMechanics::buildFalloutFireCadence(weapon, failure);
        ASSERT_TRUE(cadence);
        ASSERT_TRUE(cadence->mAutomatic);
        EXPECT_FLOAT_EQ(cadence->mSecondsPerShot, 0.1f);

        MWMechanics::FalloutTriggerState trigger;
        EXPECT_TRUE(MWMechanics::advanceFalloutTrigger(trigger, true, true, *cadence, 0.f));
        EXPECT_FALSE(MWMechanics::advanceFalloutTrigger(trigger, true, true, *cadence, 0.05f));
        EXPECT_TRUE(MWMechanics::advanceFalloutTrigger(trigger, true, true, *cadence, 0.05f));
        EXPECT_FALSE(MWMechanics::advanceFalloutTrigger(trigger, true, false, *cadence, 0.1f));
        EXPECT_TRUE(MWMechanics::advanceFalloutTrigger(trigger, true, true, *cadence, 0.f));
    }

    TEST(FalloutCombatTest, RejectsAutomaticWeaponWithoutAuthoredCadence)
    {
        ESM4::Weapon weapon;
        weapon.mData.weaponFlags1 = ESM4::Weapon::Data::Automatic;
        MWMechanics::FalloutFireCadenceFailure failure;
        EXPECT_FALSE(MWMechanics::buildFalloutFireCadence(weapon, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutFireCadenceFailure::InvalidAnimationMultiplier);
    }

    TEST(FalloutCombatTest, PreservesAuthoredFalloutAiWeaponRanges)
    {
        ESM4::Weapon serviceRifle;
        serviceRifle.mData.hasBallistics = true;
        serviceRifle.mData.animationType = 5;
        serviceRifle.mData.maxRange = 3548.f;

        MWMechanics::FalloutAiCombatRangeFailure failure;
        const auto ranged
            = MWMechanics::buildFalloutAiCombatRange(&serviceRifle, 256.f, 1.f, failure);
        ASSERT_TRUE(ranged);
        EXPECT_EQ(failure, MWMechanics::FalloutAiCombatRangeFailure::None);
        EXPECT_TRUE(ranged->mRanged);
        EXPECT_FLOAT_EQ(ranged->mDistance, 3548.f);

        ESM4::Weapon knife;
        knife.mData.animationType = 1;
        knife.mData.reach = 0.8f;
        const auto melee = MWMechanics::buildFalloutAiCombatRange(&knife, 256.f, 1.f, failure);
        ASSERT_TRUE(melee);
        EXPECT_FALSE(melee->mRanged);
        EXPECT_FLOAT_EQ(melee->mDistance, 204.8f);

        const auto unarmed = MWMechanics::buildFalloutAiCombatRange(nullptr, 256.f, 1.f, failure);
        ASSERT_TRUE(unarmed);
        EXPECT_FALSE(unarmed->mRanged);
        EXPECT_FLOAT_EQ(unarmed->mDistance, 256.f);
    }

    TEST(FalloutCombatTest, RejectsMissingFalloutAiRangeDataInsteadOfUsingMorrowindFallbacks)
    {
        ESM4::Weapon ranged;
        ranged.mData.animationType = 5;
        MWMechanics::FalloutAiCombatRangeFailure failure;
        EXPECT_FALSE(MWMechanics::buildFalloutAiCombatRange(&ranged, 256.f, 1.f, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutAiCombatRangeFailure::MissingBallistics);

        ranged.mData.hasBallistics = true;
        EXPECT_FALSE(MWMechanics::buildFalloutAiCombatRange(&ranged, 256.f, 1.f, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutAiCombatRangeFailure::InvalidWeaponRange);

        EXPECT_FALSE(MWMechanics::buildFalloutAiCombatRange(nullptr, 0.f, 1.f, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutAiCombatRangeFailure::InvalidTuning);
    }

    TEST(FalloutCombatTest, AppliesNewVegasResistanceThenThresholdAndMinimumDamage)
    {
        MWMechanics::FalloutDamageMitigationFailure failure;
        const auto ordinary = MWMechanics::resolveFalloutDamageMitigation(80.f, 30.f, 20.f, 0.2f, 85.f, failure);
        ASSERT_TRUE(ordinary);
        EXPECT_EQ(failure, MWMechanics::FalloutDamageMitigationFailure::None);
        EXPECT_FLOAT_EQ(ordinary->mDamageAfterResistance, 56.f);
        EXPECT_FLOAT_EQ(ordinary->mHealthDamage, 36.f);
        EXPECT_FALSE(ordinary->mThresholdLimited);

        const auto capped = MWMechanics::resolveFalloutDamageMitigation(40.f, 100.f, 10.f, 0.2f, 85.f, failure);
        ASSERT_TRUE(capped);
        EXPECT_FLOAT_EQ(capped->mDamageResistance, 85.f);
        EXPECT_FLOAT_EQ(capped->mDamageAfterResistance, 6.f);
        EXPECT_FLOAT_EQ(capped->mMinimumDamage, 8.f);
        EXPECT_FLOAT_EQ(capped->mHealthDamage, 8.f);
        EXPECT_TRUE(capped->mThresholdLimited);
    }

    TEST(FalloutCombatTest, MitigatesEachShotgunPelletInsteadOfTheAggregatedTriggerDamage)
    {
        MWMechanics::FalloutDamageMitigationFailure failure;
        float total = 0.f;
        for (unsigned int pellet = 0; pellet < 7; ++pellet)
        {
            const auto impact
                = MWMechanics::resolveFalloutDamageMitigation(10.f, 0.f, 8.f, 0.2f, 85.f, failure);
            ASSERT_TRUE(impact);
            total += impact->mHealthDamage;
        }
        EXPECT_FLOAT_EQ(total, 14.f);
    }

    TEST(FalloutCombatTest, PreservesNegativeResistanceButNeverTurnsNegativeThresholdIntoBonusDamage)
    {
        MWMechanics::FalloutDamageMitigationFailure failure;
        const auto impact
            = MWMechanics::resolveFalloutDamageMitigation(20.f, -25.f, -10.f, 0.2f, 85.f, failure);
        ASSERT_TRUE(impact);
        EXPECT_FLOAT_EQ(impact->mDamageResistance, -25.f);
        EXPECT_FLOAT_EQ(impact->mDamageThreshold, 0.f);
        EXPECT_FLOAT_EQ(impact->mHealthDamage, 25.f);
    }

    TEST(FalloutCombatTest, AppliesVanillaArmorConditionPenaltyOnlyBelowHalfCondition)
    {
        EXPECT_FLOAT_EQ(*MWMechanics::resolveFalloutArmorConditionMultiplier(1.f, 1.f), 1.f);
        EXPECT_FLOAT_EQ(*MWMechanics::resolveFalloutArmorConditionMultiplier(0.5f, 1.f), 1.f);
        EXPECT_FLOAT_EQ(*MWMechanics::resolveFalloutArmorConditionMultiplier(0.25f, 1.f), 0.75f);
        EXPECT_FLOAT_EQ(*MWMechanics::resolveFalloutArmorConditionMultiplier(0.f, 1.f), 0.5f);
        EXPECT_FALSE(MWMechanics::resolveFalloutArmorConditionMultiplier(-0.01f, 1.f));
        EXPECT_FALSE(MWMechanics::resolveFalloutArmorConditionMultiplier(1.01f, 1.f));
    }

    MWMechanics::FalloutRangedDamageTuning retailRangedDamageTuning()
    {
        // The first value is the New Vegas engine default; the next two are winning FalloutNV.esm GMST values. New
        // Vegas keeps the final two values in engine state: damage degrades below 75% condition at a 0.67 rate.
        return MWMechanics::FalloutRangedDamageTuning{ 1.f, 0.5f, 0.5f, 0.75f, 0.67f };
    }

    TEST(FalloutCombatTest, AppliesRetailSkillAndWeaponConditionToRangedDamage)
    {
        MWMechanics::FalloutRangedDamageFailure failure;
        const auto fullCondition
            = MWMechanics::buildFalloutRangedDamage(20.f, 50.f, 1.f, retailRangedDamageTuning(), failure);
        ASSERT_TRUE(fullCondition);
        EXPECT_EQ(failure, MWMechanics::FalloutRangedDamageFailure::None);
        EXPECT_FLOAT_EQ(fullCondition->mSkillMultiplier, 0.75f);
        EXPECT_FLOAT_EQ(fullCondition->mConditionMultiplier, 1.f);
        EXPECT_FLOAT_EQ(fullCondition->mDamage, 15.f);

        const auto threshold
            = MWMechanics::buildFalloutRangedDamage(20.f, 50.f, 0.75f, retailRangedDamageTuning(), failure);
        ASSERT_TRUE(threshold);
        EXPECT_FLOAT_EQ(threshold->mConditionMultiplier, 1.f);
        EXPECT_FLOAT_EQ(threshold->mDamage, 15.f);

        const auto broken
            = MWMechanics::buildFalloutRangedDamage(20.f, 50.f, 0.f, retailRangedDamageTuning(), failure);
        ASSERT_TRUE(broken);
        EXPECT_FLOAT_EQ(broken->mConditionMultiplier, 0.4975f);
        EXPECT_FLOAT_EQ(broken->mDamage, 7.4625f);
    }

    TEST(FalloutCombatTest, RejectsMalformedRangedDamageInputsBeforeAShot)
    {
        MWMechanics::FalloutRangedDamageFailure failure;
        EXPECT_FALSE(MWMechanics::buildFalloutRangedDamage(
            20.f, -1.f, 1.f, retailRangedDamageTuning(), failure));
        EXPECT_EQ(failure, MWMechanics::FalloutRangedDamageFailure::InvalidSkill);

        EXPECT_FALSE(MWMechanics::buildFalloutRangedDamage(
            20.f, 50.f, 1.01f, retailRangedDamageTuning(), failure));
        EXPECT_EQ(failure, MWMechanics::FalloutRangedDamageFailure::InvalidCondition);

        MWMechanics::FalloutRangedDamageTuning invalid = retailRangedDamageTuning();
        invalid.mSkillMultiplier = std::numeric_limits<float>::quiet_NaN();
        EXPECT_FALSE(MWMechanics::buildFalloutRangedDamage(20.f, 50.f, 1.f, invalid, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutRangedDamageFailure::InvalidTuning);
    }

    TEST(FalloutCombatTest, ResolvesAuthoredExplosionDamageAcrossItsRadius)
    {
        MWMechanics::FalloutExplosionDamageFailure failure;
        const auto center
            = MWMechanics::resolveFalloutExplosionDamage(125.f, 0.75f, 900.f, 0.f, failure);
        ASSERT_TRUE(center);
        EXPECT_EQ(failure, MWMechanics::FalloutExplosionDamageFailure::None);
        EXPECT_FLOAT_EQ(center->mFalloff, 1.f);
        EXPECT_FLOAT_EQ(center->mDamage, 93.75f);

        const auto halfway
            = MWMechanics::resolveFalloutExplosionDamage(125.f, 0.75f, 900.f, 450.f, failure);
        ASSERT_TRUE(halfway);
        EXPECT_FLOAT_EQ(halfway->mFalloff, 0.5f);
        EXPECT_FLOAT_EQ(halfway->mDamage, 46.875f);

        const auto edge
            = MWMechanics::resolveFalloutExplosionDamage(125.f, 0.75f, 900.f, 900.f, failure);
        ASSERT_TRUE(edge);
        EXPECT_FLOAT_EQ(edge->mFalloff, 0.f);
        EXPECT_FLOAT_EQ(edge->mDamage, 0.f);

        const auto outside
            = MWMechanics::resolveFalloutExplosionDamage(125.f, 0.75f, 900.f, 1200.f, failure);
        ASSERT_TRUE(outside);
        EXPECT_FLOAT_EQ(outside->mFalloff, 0.f);
        EXPECT_FLOAT_EQ(outside->mDamage, 0.f);
    }

    TEST(FalloutCombatTest, RejectsMalformedExplosionDamageInputs)
    {
        MWMechanics::FalloutExplosionDamageFailure failure;
        EXPECT_FALSE(MWMechanics::resolveFalloutExplosionDamage(-1.f, 1.f, 900.f, 0.f, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutExplosionDamageFailure::InvalidDamage);
        EXPECT_FALSE(MWMechanics::resolveFalloutExplosionDamage(125.f,
            std::numeric_limits<float>::quiet_NaN(), 900.f, 0.f, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutExplosionDamageFailure::InvalidMultiplier);
        EXPECT_FALSE(MWMechanics::resolveFalloutExplosionDamage(125.f, 1.f, 0.f, 0.f, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutExplosionDamageFailure::InvalidRadius);
        EXPECT_FALSE(MWMechanics::resolveFalloutExplosionDamage(125.f, 1.f, 900.f, -1.f, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutExplosionDamageFailure::InvalidDistance);
    }

    TEST(FalloutCombatTest, AppliesRetailExplosionKnockdownThresholdsAndAgilityFormula)
    {
        MWMechanics::FalloutExplosionKnockdownFailure failure;
        const auto belowCurrentThreshold = MWMechanics::buildFalloutExplosionKnockdown(
            49.99f, 100.f, 100.f, 5.f, retailExplosionKnockdownTuning(), failure);
        ASSERT_TRUE(belowCurrentThreshold);
        EXPECT_EQ(failure, MWMechanics::FalloutExplosionKnockdownFailure::None);
        EXPECT_EQ(belowCurrentThreshold->mMode, MWMechanics::FalloutExplosionKnockdownMode::None);

        const auto cappedChance = MWMechanics::buildFalloutExplosionKnockdown(
            50.f, 100.f, 100.f, 5.f, retailExplosionKnockdownTuning(), failure);
        ASSERT_TRUE(cappedChance);
        EXPECT_EQ(cappedChance->mMode, MWMechanics::FalloutExplosionKnockdownMode::Chance);
        EXPECT_FLOAT_EQ(cappedChance->mChance, 0.25f);
        EXPECT_TRUE(MWMechanics::doesFalloutExplosionKnockDown(*cappedChance, 249));
        EXPECT_FALSE(MWMechanics::doesFalloutExplosionKnockDown(*cappedChance, 250));

        const auto agilityTen = MWMechanics::buildFalloutExplosionKnockdown(
            50.f, 100.f, 100.f, 10.f, retailExplosionKnockdownTuning(), failure);
        ASSERT_TRUE(agilityTen);
        EXPECT_EQ(agilityTen->mMode, MWMechanics::FalloutExplosionKnockdownMode::Chance);
        EXPECT_FLOAT_EQ(agilityTen->mChance, 0.15f);
        EXPECT_TRUE(MWMechanics::doesFalloutExplosionKnockDown(*agilityTen, 149));
        EXPECT_FALSE(MWMechanics::doesFalloutExplosionKnockDown(*agilityTen, 150));

        const auto forced = MWMechanics::buildFalloutExplosionKnockdown(
            75.01f, 100.f, 100.f, 10.f, retailExplosionKnockdownTuning(), failure);
        ASSERT_TRUE(forced);
        EXPECT_EQ(forced->mMode, MWMechanics::FalloutExplosionKnockdownMode::Forced);
        EXPECT_FLOAT_EQ(forced->mChance, 1.f);
        EXPECT_TRUE(MWMechanics::doesFalloutExplosionKnockDown(*forced, 999));
    }

    TEST(FalloutCombatTest, UsesCurrentHealthForExplosionKnockdownEligibility)
    {
        MWMechanics::FalloutExplosionKnockdownFailure failure;
        const auto below = MWMechanics::buildFalloutExplosionKnockdown(
            24.99f, 50.f, 100.f, 5.f, retailExplosionKnockdownTuning(), failure);
        ASSERT_TRUE(below);
        EXPECT_EQ(below->mMode, MWMechanics::FalloutExplosionKnockdownMode::None);

        const auto eligible = MWMechanics::buildFalloutExplosionKnockdown(
            25.f, 50.f, 100.f, 5.f, retailExplosionKnockdownTuning(), failure);
        ASSERT_TRUE(eligible);
        EXPECT_EQ(eligible->mMode, MWMechanics::FalloutExplosionKnockdownMode::Chance);
        EXPECT_FLOAT_EQ(eligible->mChance, 0.15f);
    }

    TEST(FalloutCombatTest, RejectsMalformedExplosionKnockdownInputs)
    {
        MWMechanics::FalloutExplosionKnockdownFailure failure;
        EXPECT_FALSE(MWMechanics::buildFalloutExplosionKnockdown(
            -1.f, 100.f, 100.f, 5.f, retailExplosionKnockdownTuning(), failure));
        EXPECT_EQ(failure, MWMechanics::FalloutExplosionKnockdownFailure::InvalidDamage);
        EXPECT_FALSE(MWMechanics::buildFalloutExplosionKnockdown(
            50.f, 100.f, 0.f, 5.f, retailExplosionKnockdownTuning(), failure));
        EXPECT_EQ(failure, MWMechanics::FalloutExplosionKnockdownFailure::InvalidHealth);
        EXPECT_FALSE(MWMechanics::buildFalloutExplosionKnockdown(
            50.f, 100.f, 100.f, -1.f, retailExplosionKnockdownTuning(), failure));
        EXPECT_EQ(failure, MWMechanics::FalloutExplosionKnockdownFailure::InvalidAgility);

        auto invalidTuning = retailExplosionKnockdownTuning();
        invalidTuning.mMaximumChance = 1.01f;
        EXPECT_FALSE(MWMechanics::buildFalloutExplosionKnockdown(
            50.f, 100.f, 100.f, 5.f, invalidTuning, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutExplosionKnockdownFailure::InvalidTuning);

        EXPECT_FALSE(MWMechanics::buildFalloutExplosionKnockdown(
            50.f, 100.f, 100.f, 0.f, retailExplosionKnockdownTuning(), failure));
        EXPECT_EQ(failure, MWMechanics::FalloutExplosionKnockdownFailure::InvalidResult);
    }

    TEST(FalloutCombatTest, ReflectsLobberVelocityUsingAuthoredBounciness)
    {
        MWMechanics::FalloutProjectileBounceFailure failure;
        const auto bounce = MWMechanics::resolveFalloutProjectileBounce(
            osg::Vec3f(10.f, 5.f, -20.f), osg::Vec3f(0.f, 0.f, 2.f), 0.5f, failure);
        ASSERT_TRUE(bounce);
        EXPECT_EQ(failure, MWMechanics::FalloutProjectileBounceFailure::None);
        EXPECT_FLOAT_EQ(bounce->x(), 10.f);
        EXPECT_FLOAT_EQ(bounce->y(), 5.f);
        EXPECT_FLOAT_EQ(bounce->z(), 10.f);

        const auto separating = MWMechanics::resolveFalloutProjectileBounce(
            osg::Vec3f(1.f, 2.f, 3.f), osg::Vec3f(0.f, 0.f, 1.f), 0.5f, failure);
        ASSERT_TRUE(separating);
        EXPECT_EQ(*separating, osg::Vec3f(1.f, 2.f, 3.f));
    }

    TEST(FalloutCombatTest, RejectsMalformedLobberBounceInputs)
    {
        MWMechanics::FalloutProjectileBounceFailure failure;
        EXPECT_FALSE(MWMechanics::resolveFalloutProjectileBounce(
            osg::Vec3f(1.f, 2.f, 3.f), osg::Vec3f(), 0.5f, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutProjectileBounceFailure::InvalidNormal);
        EXPECT_FALSE(MWMechanics::resolveFalloutProjectileBounce(
            osg::Vec3f(1.f, 2.f, 3.f), osg::Vec3f(0.f, 0.f, 1.f), -0.1f, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutProjectileBounceFailure::InvalidBounciness);
    }

    TEST(FalloutCombatTest, AimsGravityProjectileAlongReachableLowBallisticArc)
    {
        MWMechanics::FalloutBallisticAimFailure failure;
        constexpr float speed = 1750.f;
        constexpr float gravity = 94.067f;
        const osg::Vec3f displacement(2400.f, 600.f, 90.f);
        const auto direction
            = MWMechanics::buildFalloutBallisticAimDirection(displacement, speed, gravity, failure);
        ASSERT_TRUE(direction.has_value());
        EXPECT_EQ(failure, MWMechanics::FalloutBallisticAimFailure::None);
        EXPECT_NEAR(direction->length(), 1.f, 1e-5f);
        EXPECT_GT(direction->z(), displacement.z() / displacement.length());

        const float horizontalDistance = std::hypot(displacement.x(), displacement.y());
        const float horizontalSpeed = speed * std::hypot(direction->x(), direction->y());
        const float time = horizontalDistance / horizontalSpeed;
        EXPECT_NEAR(speed * direction->z() * time - 0.5f * gravity * time * time,
            displacement.z(), 0.02f);
    }

    TEST(FalloutCombatTest, RejectsMalformedOrUnreachableBallisticAim)
    {
        MWMechanics::FalloutBallisticAimFailure failure;
        EXPECT_FALSE(MWMechanics::buildFalloutBallisticAimDirection(
            osg::Vec3f(), 100.f, 10.f, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutBallisticAimFailure::InvalidDisplacement);
        EXPECT_FALSE(MWMechanics::buildFalloutBallisticAimDirection(
            osg::Vec3f(1000.f, 0.f, 1000.f), 10.f, 100.f, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutBallisticAimFailure::Unreachable);

        const auto zeroGravity = MWMechanics::buildFalloutBallisticAimDirection(
            osg::Vec3f(3.f, 4.f, 0.f), 10.f, 0.f, failure);
        ASSERT_TRUE(zeroGravity.has_value());
        EXPECT_EQ(failure, MWMechanics::FalloutBallisticAimFailure::None);
        EXPECT_NEAR(zeroGravity->x(), 0.6f, 1e-6f);
        EXPECT_NEAR(zeroGravity->y(), 0.8f, 1e-6f);
    }

    TEST(FalloutCombatTest, ResolvesOnlySuccessfulQueuedVatsProjectilesAuthoritatively)
    {
        MWMechanics::FalloutProjectileImpactContract impact;
        EXPECT_FALSE(MWMechanics::getAuthoritativeFalloutVatsProjectileTarget(impact));

        MWMechanics::FalloutVatsQueuedAction action;
        action.mTarget = id(0x1107077);
        impact.mVatsAction = action;
        impact.mVatsTargetHit = false;
        EXPECT_FALSE(MWMechanics::getAuthoritativeFalloutVatsProjectileTarget(impact));

        impact.mVatsTargetHit = true;
        const auto target = MWMechanics::getAuthoritativeFalloutVatsProjectileTarget(impact);
        ASSERT_TRUE(target.has_value());
        EXPECT_EQ(*target, action.mTarget);
    }

    TEST(FalloutCombatTest, ResolvesRetailFragMineAndRemoteProjectileTriggers)
    {
        ESM4::Projectile projectile;
        projectile.mData.present = true;
        projectile.mData.explosion = id(0x179da);
        projectile.mData.flags = ESM4::Projectile::Explosion | ESM4::Projectile::AlternateTrigger;
        projectile.mData.alternateTimer = 2.5f;
        MWMechanics::FalloutProjectileTriggerFailure failure;

        const auto frag = MWMechanics::buildFalloutProjectileTrigger(
            projectile, 50.f, 0.2f, 1.6f, true, failure);
        ASSERT_TRUE(frag);
        EXPECT_EQ(failure, MWMechanics::FalloutProjectileTriggerFailure::None);
        EXPECT_EQ(frag->mMode, MWMechanics::FalloutProjectileTriggerMode::Timed);
        EXPECT_FLOAT_EQ(frag->mDelay, 2.5f);
        EXPECT_FLOAT_EQ(frag->mProximityRadius, 0.f);

        projectile.mData.alternateTimer = 3.f;
        projectile.mData.alternateProximity = 100.f;
        const auto mine = MWMechanics::buildFalloutProjectileTrigger(
            projectile, 50.f, 0.2f, 1.6f, true, failure);
        ASSERT_TRUE(mine);
        EXPECT_EQ(mine->mMode, MWMechanics::FalloutProjectileTriggerMode::Proximity);
        EXPECT_FLOAT_EQ(mine->mDelay, 1.7f);
        EXPECT_FLOAT_EQ(mine->mProximityRadius, 160.f);

        projectile.mData.flags = ESM4::Projectile::Explosion | ESM4::Projectile::Detonates;
        const auto remote = MWMechanics::buildFalloutProjectileTrigger(
            projectile, 50.f, 0.2f, 1.6f, true, failure);
        ASSERT_TRUE(remote);
        EXPECT_EQ(remote->mMode, MWMechanics::FalloutProjectileTriggerMode::Remote);
        EXPECT_FLOAT_EQ(remote->mDelay, 0.f);
        EXPECT_FLOAT_EQ(remote->mProximityRadius, 0.f);

        projectile.mData.flags = ESM4::Projectile::Explosion;
        const auto impact = MWMechanics::buildFalloutProjectileTrigger(
            projectile, 50.f, 0.2f, 1.6f, false, failure);
        ASSERT_TRUE(impact);
        EXPECT_EQ(impact->mMode, MWMechanics::FalloutProjectileTriggerMode::Impact);
    }

    TEST(FalloutCombatTest, TreatsRetailThrowingSpearAsTerminalDespiteAlternateTriggerFlag)
    {
        ESM4::Projectile projectile;
        projectile.mData.present = true;
        projectile.mData.type = ESM4::Projectile::Missile;
        projectile.mData.flags = ESM4::Projectile::AlternateTrigger
            | ESM4::Projectile::CanBePickedUp | ESM4::Projectile::PinsLimbs;

        EXPECT_FALSE(MWMechanics::doesFalloutProjectileRemainAfterImpact(projectile));

        projectile.mData.type = ESM4::Projectile::Lobber;
        EXPECT_TRUE(MWMechanics::doesFalloutProjectileRemainAfterImpact(projectile));

        projectile.mData.flags = ESM4::Projectile::PinsLimbs;
        EXPECT_FALSE(MWMechanics::doesFalloutProjectileRemainAfterImpact(projectile));
    }

    TEST(FalloutCombatTest, GivesPhysicalCollisionPriorityOverRangeExpiry)
    {
        EXPECT_TRUE(MWMechanics::shouldResolveFalloutProjectileRangeExpiry(true, 6000.f, 6000.f));
        EXPECT_TRUE(MWMechanics::shouldResolveFalloutProjectileRangeExpiry(true, 6001.f, 6000.f));
        EXPECT_FALSE(MWMechanics::shouldResolveFalloutProjectileRangeExpiry(true, 5999.f, 6000.f));

        // An inactive physics projectile has a real contact to process, even when that contact's final segment
        // crossed or landed exactly on the authored range boundary.
        EXPECT_FALSE(MWMechanics::shouldResolveFalloutProjectileRangeExpiry(false, 6000.f, 6000.f));
        EXPECT_FALSE(MWMechanics::shouldResolveFalloutProjectileRangeExpiry(false, 6001.f, 6000.f));

        EXPECT_FALSE(MWMechanics::shouldResolveFalloutProjectileRangeExpiry(
            true, std::numeric_limits<float>::quiet_NaN(), 6000.f));
        EXPECT_FALSE(MWMechanics::shouldResolveFalloutProjectileRangeExpiry(true, 6000.f, 0.f));
    }

    TEST(FalloutCombatTest, ResolvesOnlySuccessfulTerminalVatsProjectileAtRangeExpiry)
    {
        ESM4::Projectile spear;
        spear.mData.present = true;
        spear.mData.type = ESM4::Projectile::Missile;
        spear.mData.flags = ESM4::Projectile::AlternateTrigger
            | ESM4::Projectile::CanBePickedUp | ESM4::Projectile::PinsLimbs;

        MWMechanics::FalloutProjectileImpactContract impact;
        MWMechanics::FalloutVatsQueuedAction action;
        action.mTarget = id(0x1107077);
        impact.mVatsAction = action;
        impact.mVatsTargetHit = true;

        const auto target = MWMechanics::getFalloutVatsProjectileRangeExpiryTarget(spear, impact);
        ASSERT_TRUE(target);
        EXPECT_EQ(*target, action.mTarget);

        impact.mVatsTargetHit = false;
        EXPECT_FALSE(MWMechanics::getFalloutVatsProjectileRangeExpiryTarget(spear, impact));

        impact.mVatsTargetHit = true;
        spear.mData.type = ESM4::Projectile::Lobber;
        EXPECT_FALSE(MWMechanics::getFalloutVatsProjectileRangeExpiryTarget(spear, impact));

        spear.mData.present = false;
        spear.mData.type = ESM4::Projectile::Missile;
        EXPECT_FALSE(MWMechanics::getFalloutVatsProjectileRangeExpiryTarget(spear, impact));
    }

    TEST(FalloutCombatTest, RejectsMalformedProjectileTriggerData)
    {
        ESM4::Projectile projectile;
        MWMechanics::FalloutProjectileTriggerFailure failure;
        EXPECT_FALSE(MWMechanics::buildFalloutProjectileTrigger(
            projectile, 50.f, 0.2f, 1.6f, false, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutProjectileTriggerFailure::MissingData);

        projectile.mData.present = true;
        projectile.mData.flags = ESM4::Projectile::Explosion | ESM4::Projectile::AlternateTrigger;
        EXPECT_FALSE(MWMechanics::buildFalloutProjectileTrigger(
            projectile, 50.f, 0.2f, 1.6f, false, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutProjectileTriggerFailure::MissingExplosion);

        projectile.mData.explosion = id(0x179da);
        projectile.mData.alternateTimer = -1.f;
        EXPECT_FALSE(MWMechanics::buildFalloutProjectileTrigger(
            projectile, 50.f, 0.2f, 1.6f, false, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutProjectileTriggerFailure::InvalidTimer);
    }

    TEST(FalloutCombatTest, ResolvesRetailDetonatorCommandOnlyInsideOnFireBlock)
    {
        constexpr std::string_view retailSource = R"(scn DetonatorOnFire

begin OnFire
     player.DetonatePlacedExplosives
end
)";
        EXPECT_EQ(MWMechanics::resolveFalloutWeaponOnFireAction(retailSource),
            MWMechanics::FalloutWeaponOnFireAction::DetonatePlacedExplosives);

        constexpr std::string_view notOnFire = R"(begin OnEquip
player.DetonatePlacedExplosives
end
begin OnFire
; player.DetonatePlacedExplosives
end
)";
        EXPECT_EQ(MWMechanics::resolveFalloutWeaponOnFireAction(notOnFire),
            MWMechanics::FalloutWeaponOnFireAction::None);

        EXPECT_EQ(MWMechanics::resolveFalloutWeaponOnFireAction(
                      "BEGIN\tONFIRE\r\n\tPLAYER.DETONATEPLACEDEXPLOSIVES\t\r\nEND\r\n"),
            MWMechanics::FalloutWeaponOnFireAction::DetonatePlacedExplosives);
    }

    TEST(FalloutCombatTest, SelectsOnlySettledRemoteChargeWithMatchingFrozenExplosion)
    {
        ESM4::Projectile c4;
        c4.mData.present = true;
        c4.mData.flags = ESM4::Projectile::Explosion | ESM4::Projectile::Detonates;
        c4.mData.explosion = id(0x130044);

        EXPECT_TRUE(MWMechanics::isFalloutRemoteDetonationCandidate(c4, true, id(0x130044)));
        EXPECT_FALSE(MWMechanics::isFalloutRemoteDetonationCandidate(c4, false, id(0x130044)));
        EXPECT_FALSE(MWMechanics::isFalloutRemoteDetonationCandidate(c4, true, id(0x179da)));

        c4.mData.flags = ESM4::Projectile::Explosion;
        EXPECT_FALSE(MWMechanics::isFalloutRemoteDetonationCandidate(c4, true, id(0x130044)));
    }

    ESM4::Weapon retailCriticalWeapon(bool automatic = false)
    {
        ESM4::Weapon weapon;
        weapon.mCriticalData.present = true;
        weapon.mCriticalData.damage = 20;
        weapon.mCriticalData.chanceMultiplier = 2.f;
        weapon.mCriticalData.flags = ESM4::Weapon::CriticalData::OnDeath;
        weapon.mCriticalData.effect = id(0x1234);
        if (automatic)
        {
            weapon.mData.weaponFlags1 = ESM4::Weapon::Data::Automatic;
            weapon.mData.fireRate = 8.f;
        }
        return weapon;
    }

    TEST(FalloutCombatTest, BuildsRetailCriticalChanceDamageAndEffectContract)
    {
        ESM4::Weapon weapon = retailCriticalWeapon();
        MWMechanics::FalloutCriticalFailure failure;
        const auto realTime
            = MWMechanics::buildFalloutCriticalContract(weapon, 5.f, false, 15.f, failure);
        ASSERT_TRUE(realTime);
        EXPECT_EQ(failure, MWMechanics::FalloutCriticalFailure::None);
        EXPECT_FLOAT_EQ(realTime->mChancePercent, 10.f);
        EXPECT_FLOAT_EQ(realTime->mDamage, 20.f);
        EXPECT_EQ(realTime->mEffect, id(0x1234));
        EXPECT_TRUE(realTime->mEffectOnDeath);
        EXPECT_FLOAT_EQ(realTime->damageForProjectile(3.f, false), 3.f);
        EXPECT_FLOAT_EQ(realTime->damageForProjectile(3.f, true), 23.f);

        const auto vats = MWMechanics::buildFalloutCriticalContract(weapon, 5.f, true, 15.f, failure);
        ASSERT_TRUE(vats);
        EXPECT_FLOAT_EQ(vats->mChancePercent, 25.f);
    }

    TEST(FalloutCombatTest, DividesAutomaticCriticalMultiplierByFireRate)
    {
        ESM4::Weapon weapon = retailCriticalWeapon(true);
        weapon.mCriticalData.chanceMultiplier = 1.f;
        MWMechanics::FalloutCriticalFailure failure;
        const auto realTime
            = MWMechanics::buildFalloutCriticalContract(weapon, 8.f, false, 15.f, failure);
        ASSERT_TRUE(realTime);
        EXPECT_FLOAT_EQ(realTime->mChancePercent, 1.f);

        const auto vats = MWMechanics::buildFalloutCriticalContract(weapon, 8.f, true, 15.f, failure);
        ASSERT_TRUE(vats);
        EXPECT_FLOAT_EQ(vats->mChancePercent, 16.f);
    }

    TEST(FalloutCombatTest, ResolvesCriticalRollAtExactPercentageBoundary)
    {
        EXPECT_TRUE(MWMechanics::doesFalloutCriticalHit(10.f, 0.09999f));
        EXPECT_FALSE(MWMechanics::doesFalloutCriticalHit(10.f, 0.1f));
        EXPECT_TRUE(MWMechanics::doesFalloutCriticalHit(100.f, 0.99999f));
        EXPECT_FALSE(MWMechanics::doesFalloutCriticalHit(0.f, 0.f));
        EXPECT_FALSE(MWMechanics::doesFalloutCriticalHit(10.f, 1.f));
    }

    TEST(FalloutCombatTest, RejectsMissingOrMalformedCriticalContracts)
    {
        ESM4::Weapon weapon;
        MWMechanics::FalloutCriticalFailure failure;
        EXPECT_FALSE(MWMechanics::buildFalloutCriticalContract(weapon, 5.f, false, 15.f, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutCriticalFailure::MissingCriticalData);

        weapon = retailCriticalWeapon(true);
        weapon.mData.fireRate = 0.f;
        EXPECT_FALSE(MWMechanics::buildFalloutCriticalContract(weapon, 5.f, false, 15.f, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutCriticalFailure::InvalidAutomaticFireRate);

        weapon = retailCriticalWeapon();
        weapon.mCriticalData.chanceMultiplier = 0.f;
        const auto disabled = MWMechanics::buildFalloutCriticalContract(weapon, 100.f, true, 15.f, failure);
        ASSERT_TRUE(disabled);
        EXPECT_FLOAT_EQ(disabled->mChancePercent, 0.f);
    }

    TEST(FalloutCombatTest, ResolvesOrderedNativeCriticalActorEffectChain)
    {
        ESM4::Spell spell;
        spell.mId = id(0x1000);
        spell.mData.present = true;
        spell.mData.type = ESM4::Spell::Type::ActorEffect;
        spell.mData.flags = ESM4::Spell::ScriptEffectAlwaysApplies;
        ESM4::Spell::Effect scripted;
        scripted.baseEffect = id(0x2000);
        scripted.magnitude = 12;
        scripted.duration = 3;
        scripted.range = ESM4::Spell::Range::Target;
        scripted.actorValue = 16;
        scripted.conditions.push_back({ 0x20, 1.f, ESM4::FUN_GetDead, 0, 0, 1, 0 });
        ESM4::Spell::Effect paralysis;
        paralysis.baseEffect = id(0x2001);
        paralysis.duration = 5;
        paralysis.range = ESM4::Spell::Range::Touch;
        spell.mEffects = { scripted, paralysis };

        ESM4::MagicEffect scriptEffect;
        scriptEffect.mId = scripted.baseEffect;
        scriptEffect.mData.present = true;
        scriptEffect.mData.archetype = ESM4::MagicEffect::Archetype::Script;
        scriptEffect.mData.associatedItem = id(0x3000);
        scriptEffect.mData.effectShader = id(0x4000);
        scriptEffect.mModel = "Effects\\Critical.nif";
        scriptEffect.mCounterEffects = { id(0x2002) };
        ESM4::MagicEffect paralysisEffect;
        paralysisEffect.mId = paralysis.baseEffect;
        paralysisEffect.mData.present = true;
        paralysisEffect.mData.archetype = ESM4::MagicEffect::Archetype::Paralysis;
        ESM4::Script scriptRecord;
        scriptRecord.mId = scriptEffect.mData.associatedItem;

        const std::map<ESM::FormId, const ESM4::MagicEffect*> magicEffects{
            { scriptEffect.mId, &scriptEffect }, { paralysisEffect.mId, &paralysisEffect }
        };
        MWMechanics::FalloutActorEffectFailure failure;
        const auto contract = MWMechanics::buildFalloutActorEffectContract(spell.mId,
            [&](ESM::FormId value) { return value == spell.mId ? &spell : nullptr; },
            [&](ESM::FormId value) {
                const auto found = magicEffects.find(value);
                return found != magicEffects.end() ? found->second : nullptr;
            },
            [&](ESM::FormId value) { return value == scriptRecord.mId ? &scriptRecord : nullptr; }, failure);

        ASSERT_TRUE(contract);
        EXPECT_EQ(failure, MWMechanics::FalloutActorEffectFailure::None);
        EXPECT_EQ(contract->mSpell, spell.mId);
        EXPECT_EQ(contract->mSpellData.flags, ESM4::Spell::ScriptEffectAlwaysApplies);
        ASSERT_EQ(contract->mEffects.size(), 2u);
        EXPECT_EQ(contract->mEffects[0].mEffect.baseEffect, scripted.baseEffect);
        EXPECT_EQ(contract->mEffects[0].mEffect.conditions.size(), 1u);
        EXPECT_EQ(contract->mEffects[0].mMagicEffect.effectShader, id(0x4000));
        EXPECT_EQ(contract->mEffects[0].mModel, "Effects\\Critical.nif");
        EXPECT_EQ(contract->mEffects[0].mCounterEffects, std::vector<ESM::FormId>{ id(0x2002) });
        EXPECT_EQ(contract->mEffects[0].mScript, scriptRecord.mId);
        EXPECT_EQ(contract->mEffects[1].mMagicEffect.archetype, ESM4::MagicEffect::Archetype::Paralysis);
        EXPECT_TRUE(contract->mEffects[1].mScript.isZeroOrUnset());
    }

    TEST(FalloutCombatTest, RejectsIncompleteNativeCriticalActorEffectChain)
    {
        ESM4::Spell spell;
        spell.mId = id(0x1000);
        spell.mData.present = true;
        spell.mData.type = ESM4::Spell::Type::ActorEffect;
        spell.mEffects.push_back({ id(0x2000), 0, 0, 0, ESM4::Spell::Range::Self, -1, {} });
        ESM4::MagicEffect effect;
        effect.mId = spell.mEffects.front().baseEffect;
        effect.mData.present = true;
        effect.mData.archetype = ESM4::MagicEffect::Archetype::Script;
        effect.mData.associatedItem = id(0x3000);

        MWMechanics::FalloutActorEffectFailure failure;
        EXPECT_FALSE(MWMechanics::buildFalloutActorEffectContract(spell.mId,
            [&](ESM::FormId) { return &spell; }, [&](ESM::FormId) { return &effect; },
            [&](ESM::FormId) -> const ESM4::Script* { return nullptr; }, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutActorEffectFailure::MissingScript);
        EXPECT_EQ(MWMechanics::getFalloutActorEffectFailureName(failure), "missing-script");

        spell.mData.type = ESM4::Spell::Type::Ability;
        EXPECT_FALSE(MWMechanics::buildFalloutActorEffectContract(spell.mId,
            [&](ESM::FormId) { return &spell; }, [&](ESM::FormId) { return &effect; },
            [&](ESM::FormId) -> const ESM4::Script* { return nullptr; }, failure));
        EXPECT_EQ(failure, MWMechanics::FalloutActorEffectFailure::UnsupportedSpellType);

        spell.mData.type = ESM4::Spell::Type::ActorEffect;
        effect.mData.associatedItem = {};
        effect.mData.effectShader = id(0x4000);
        const auto shaderOnly = MWMechanics::buildFalloutActorEffectContract(spell.mId,
            [&](ESM::FormId) { return &spell; }, [&](ESM::FormId) { return &effect; },
            [&](ESM::FormId) -> const ESM4::Script* { return nullptr; }, failure);
        ASSERT_TRUE(shaderOnly);
        EXPECT_EQ(failure, MWMechanics::FalloutActorEffectFailure::None);
        ASSERT_EQ(shaderOnly->mEffects.size(), 1u);
        EXPECT_TRUE(shaderOnly->mEffects.front().mScript.isZeroOrUnset());
        EXPECT_EQ(shaderOnly->mEffects.front().mMagicEffect.effectShader, id(0x4000));
    }

    TEST(FalloutCombatTest, AdvancesSaveableNativeFalloutStatusEffectsWithoutMorrowindMagic)
    {
        std::vector<ESM::FalloutActiveEffect> effects;
        const ESM::RefId spell = ESM::RefId::formIdRefId(id(0x1000));
        const ESM::RefId healthEffect = ESM::RefId::formIdRefId(id(0x2000));
        const ESM::RefId modifierEffect = ESM::RefId::formIdRefId(id(0x2001));
        const ESM::RefId paralysisEffect = ESM::RefId::formIdRefId(id(0x2002));
        EXPECT_TRUE(MWMechanics::addFalloutActiveEffect(effects, { spell, healthEffect,
            ESM::FalloutActiveEffectKind::HealthDamage, 0x75, 16, 2.f, 5.f, 5.f, 11 }));
        EXPECT_TRUE(MWMechanics::addFalloutActiveEffect(effects, { spell, modifierEffect,
            ESM::FalloutActiveEffectKind::ActorValueModifier, 0x76, 76, -1.f, 5.f, 5.f, 11 }));
        EXPECT_TRUE(MWMechanics::addFalloutActiveEffect(effects, { spell, paralysisEffect,
            ESM::FalloutActiveEffectKind::Paralysis, 0x73, 47, 1.f, 5.f, 5.f, 11 }));
        EXPECT_FLOAT_EQ(MWMechanics::getFalloutActorValueModifier(effects, 76), -1.f);
        EXPECT_TRUE(MWMechanics::hasFalloutParalysis(effects));

        const MWMechanics::FalloutActiveEffectsAdvance first
            = MWMechanics::advanceFalloutActiveEffects(effects, 2.f);
        EXPECT_FLOAT_EQ(first.mHealthDamage, 4.f);
        EXPECT_EQ(first.mExpired, 0u);
        EXPECT_EQ(first.mRejected, 0u);
        ASSERT_EQ(effects.size(), 3u);
        EXPECT_FLOAT_EQ(effects.front().mTimeLeft, 3.f);

        const MWMechanics::FalloutActiveEffectsAdvance second
            = MWMechanics::advanceFalloutActiveEffects(effects, 4.f);
        EXPECT_FLOAT_EQ(second.mHealthDamage, 6.f);
        EXPECT_EQ(second.mExpired, 3u);
        EXPECT_EQ(second.mRejected, 0u);
        EXPECT_TRUE(effects.empty());
        EXPECT_FLOAT_EQ(MWMechanics::getFalloutActorValueModifier(effects, 76), 0.f);
        EXPECT_FALSE(MWMechanics::hasFalloutParalysis(effects));
    }

    TEST(FalloutCombatTest, AppliesAuthoredAmmoEffectsInRcilOrder)
    {
        ESM4::AmmoEffect add;
        add.mType = ESM4::AmmoEffect::Type::Damage;
        add.mOperation = ESM4::AmmoEffect::Operation::Add;
        add.mValue = 2.f;
        ESM4::AmmoEffect multiply;
        multiply.mType = ESM4::AmmoEffect::Type::Damage;
        multiply.mOperation = ESM4::AmmoEffect::Operation::Multiply;
        multiply.mValue = 3.f;
        ESM4::AmmoEffect otherType;
        otherType.mType = ESM4::AmmoEffect::Type::Spread;
        otherType.mOperation = ESM4::AmmoEffect::Operation::Multiply;
        otherType.mValue = 100.f;
        ESM4::AmmoEffect subtract;
        subtract.mType = ESM4::AmmoEffect::Type::Damage;
        subtract.mOperation = ESM4::AmmoEffect::Operation::Subtract;
        subtract.mValue = 4.f;
        const std::array<const ESM4::AmmoEffect*, 4> effects{ &add, &multiply, &otherType, &subtract };

        MWMechanics::FalloutAmmoEffectFailure failure;
        const auto result = MWMechanics::applyFalloutAmmoEffects(
            10.f, ESM4::AmmoEffect::Type::Damage, effects, failure);
        ASSERT_TRUE(result);
        EXPECT_EQ(failure, MWMechanics::FalloutAmmoEffectFailure::None);
        EXPECT_FLOAT_EQ(*result, 32.f);
    }

    TEST(FalloutCombatTest, BuildsRetailWeaponConditionLossFromGmstOverrideAndAmmo)
    {
        ESM4::Weapon weapon;
        ESM4::AmmoEffect condition;
        condition.mType = ESM4::AmmoEffect::Type::WeaponCondition;
        condition.mOperation = ESM4::AmmoEffect::Operation::Multiply;
        condition.mValue = 0.75f;
        const std::array<const ESM4::AmmoEffect*, 1> effects{ &condition };

        MWMechanics::FalloutWeaponDegradationFailure failure;
        const auto ordinary = MWMechanics::buildFalloutWeaponDegradation(
            weapon, effects, 0.2f, false, 1.f, failure);
        ASSERT_TRUE(ordinary);
        EXPECT_EQ(failure, MWMechanics::FalloutWeaponDegradationFailure::None);
        EXPECT_FALSE(ordinary->mUsesWeaponOverride);
        EXPECT_FLOAT_EQ(ordinary->mBaseLoss, 0.2f);
        EXPECT_FLOAT_EQ(ordinary->mAmmoAdjustedLoss, 0.15f);
        EXPECT_FLOAT_EQ(ordinary->mConditionLoss, 0.15f);

        weapon.mData.flags2 = ESM4::Weapon::Data::OverrideDamageToWeapon;
        weapon.mData.damageToWeaponMult = 0.4f;
        condition.mValue = 1.5f;
        const auto vats = MWMechanics::buildFalloutWeaponDegradation(
            weapon, effects, 0.2f, true, 2.f, failure);
        ASSERT_TRUE(vats);
        EXPECT_TRUE(vats->mUsesWeaponOverride);
        EXPECT_FLOAT_EQ(vats->mBaseLoss, 0.4f);
        EXPECT_FLOAT_EQ(vats->mAmmoAdjustedLoss, 0.6f);
        EXPECT_FLOAT_EQ(vats->mConditionLoss, 1.2f);
    }

    TEST(FalloutCombatTest, AppliesRetailAmmoDamagePenetrationAndSpreadBeforeImpact)
    {
        ESM4::AmmoEffect damage;
        damage.mType = ESM4::AmmoEffect::Type::Damage;
        damage.mOperation = ESM4::AmmoEffect::Operation::Multiply;
        damage.mValue = 1.75f;
        ESM4::AmmoEffect threshold;
        threshold.mType = ESM4::AmmoEffect::Type::DamageThreshold;
        threshold.mOperation = ESM4::AmmoEffect::Operation::Subtract;
        threshold.mValue = 15.f;
        ESM4::AmmoEffect spread;
        spread.mType = ESM4::AmmoEffect::Type::Spread;
        spread.mOperation = ESM4::AmmoEffect::Operation::Multiply;
        spread.mValue = 0.35f;
        const std::array<const ESM4::AmmoEffect*, 3> effects{ &damage, &threshold, &spread };

        MWMechanics::FalloutAmmoEffectFailure effectFailure;
        const auto incoming = MWMechanics::applyFalloutAmmoEffects(
            30.f, ESM4::AmmoEffect::Type::Damage, effects, effectFailure);
        const auto targetThreshold = MWMechanics::applyFalloutAmmoEffects(
            10.f, ESM4::AmmoEffect::Type::DamageThreshold, effects, effectFailure);
        const auto shotSpread = MWMechanics::applyFalloutAmmoEffects(
            2.f, ESM4::AmmoEffect::Type::Spread, effects, effectFailure);
        ASSERT_TRUE(incoming);
        ASSERT_TRUE(targetThreshold);
        ASSERT_TRUE(shotSpread);
        EXPECT_FLOAT_EQ(*incoming, 52.5f);
        EXPECT_FLOAT_EQ(*targetThreshold, -5.f);
        EXPECT_FLOAT_EQ(*shotSpread, 0.7f);

        MWMechanics::FalloutDamageMitigationFailure mitigationFailure;
        const auto impact = MWMechanics::resolveFalloutDamageMitigation(
            *incoming, 0.f, *targetThreshold, 0.2f, 85.f, mitigationFailure);
        ASSERT_TRUE(impact);
        EXPECT_FLOAT_EQ(impact->mDamageThreshold, 0.f);
        EXPECT_FLOAT_EQ(impact->mHealthDamage, 52.5f);
    }

    TEST(FalloutCombatTest, RejectsMalformedAmmoEffectsAndWeaponWear)
    {
        MWMechanics::FalloutAmmoEffectFailure effectFailure;
        const std::array<const ESM4::AmmoEffect*, 1> missing{ nullptr };
        EXPECT_FALSE(MWMechanics::applyFalloutAmmoEffects(
            1.f, ESM4::AmmoEffect::Type::Damage, missing, effectFailure));
        EXPECT_EQ(effectFailure, MWMechanics::FalloutAmmoEffectFailure::MissingEffect);

        ESM4::AmmoEffect invalid;
        invalid.mType = ESM4::AmmoEffect::Type::Damage;
        invalid.mOperation = static_cast<ESM4::AmmoEffect::Operation>(3);
        invalid.mValue = 1.f;
        const std::array<const ESM4::AmmoEffect*, 1> invalidEffects{ &invalid };
        EXPECT_FALSE(MWMechanics::applyFalloutAmmoEffects(
            1.f, ESM4::AmmoEffect::Type::Damage, invalidEffects, effectFailure));
        EXPECT_EQ(effectFailure, MWMechanics::FalloutAmmoEffectFailure::InvalidOperation);

        ESM4::Weapon weapon;
        weapon.mData.flags2 = ESM4::Weapon::Data::OverrideDamageToWeapon;
        weapon.mData.damageToWeaponMult = -1.f;
        MWMechanics::FalloutWeaponDegradationFailure wearFailure;
        EXPECT_FALSE(MWMechanics::buildFalloutWeaponDegradation(
            weapon, {}, 0.2f, false, 1.f, wearFailure));
        EXPECT_EQ(wearFailure, MWMechanics::FalloutWeaponDegradationFailure::InvalidWeaponOverride);
    }

    TEST(FalloutCombatTest, BuildsUnitRaysAtAuthoredMedianAndMaximumSpread)
    {
        const auto center = MWMechanics::buildFalloutRayDirection(
            osg::Vec3f(0.f, 2.f, 0.f), 10.f, osg::Vec2f(0.f, 0.f));
        ASSERT_TRUE(center);
        EXPECT_FLOAT_EQ(center->x(), 0.f);
        EXPECT_FLOAT_EQ(center->y(), 1.f);
        EXPECT_FLOAT_EQ(center->z(), 0.f);

        const auto median = MWMechanics::buildFalloutRayDirection(
            osg::Vec3f(0.f, 1.f, 0.f), 10.f, osg::Vec2f(0.5f, 0.f));
        ASSERT_TRUE(median);
        EXPECT_NEAR(median->length(), 1.f, 1e-6f);
        EXPECT_NEAR(
            std::acos((*median) * osg::Vec3f(0.f, 1.f, 0.f)), osg::DegreesToRadians(10.f), 1e-6f);

        const auto edge = MWMechanics::buildFalloutRayDirection(
            osg::Vec3f(0.f, 1.f, 0.f), 10.f, osg::Vec2f(1.f, 0.f));
        ASSERT_TRUE(edge);
        EXPECT_NEAR(edge->length(), 1.f, 1e-6f);
        EXPECT_NEAR(std::acos((*edge) * osg::Vec3f(0.f, 1.f, 0.f)), osg::DegreesToRadians(20.f), 1e-6f);
    }

    TEST(FalloutCombatTest, IgnoresUnusedLegacySpreadButRejectsMalformedMinSpreadAndRayInputs)
    {
        ESM4::Weapon weapon;
        weapon.mData.hasBallistics = true;
        weapon.mData.projectile = id(0x426d);
        weapon.mData.ammoUse = 1;
        weapon.mData.numProjectiles = 1;
        weapon.mData.minSpread = 0.5f;
        weapon.mData.spread = std::numeric_limits<float>::quiet_NaN();
        ESM4::Projectile projectile;
        projectile.mId = id(0x426d);
        projectile.mData.present = true;
        projectile.mData.range = 10000.f;

        MWMechanics::FalloutShotFailure failure;
        const auto contract = MWMechanics::buildFalloutRayShotContract(weapon, projectile, id(0x4240), failure);
        ASSERT_TRUE(contract);
        EXPECT_EQ(failure, MWMechanics::FalloutShotFailure::None);
        EXPECT_TRUE(std::isnan(contract->mLegacySpread));

        weapon.mData.minSpread = std::numeric_limits<float>::quiet_NaN();
        EXPECT_FALSE(MWMechanics::buildFalloutRayShotContract(weapon, projectile, id(0x4240), failure));
        EXPECT_EQ(failure, MWMechanics::FalloutShotFailure::InvalidSpread);
        EXPECT_FALSE(MWMechanics::buildFalloutRayDirection(
            osg::Vec3f(0.f, 0.f, 0.f), 1.f, osg::Vec2f(0.f, 0.f)));
        EXPECT_FALSE(MWMechanics::buildFalloutRayDirection(
            osg::Vec3f(0.f, 1.f, 0.f), 1.f, osg::Vec2f(1.1f, 0.f)));
        EXPECT_FALSE(MWMechanics::buildFalloutRayDirection(
            osg::Vec3f(0.f, 1.f, 0.f), 45.f, osg::Vec2f(1.f, 0.f)));
    }

    MWMechanics::FalloutMeleeTuning retailMeleeTuning()
    {
        // The six damage values are exact winning FalloutNV.esm GMST values. The final two values are the shared
        // contact-distance defaults used after the FNV compatibility bridge. Keeping all of them in the fixture
        // proves the production formula consumes content/runtime tuning rather than hiding constants in code.
        return MWMechanics::FalloutMeleeTuning{ 0.5f, 0.5f, 0.5f, 0.05f, 0.5f, 0.f, 256.f, 1.f };
    }

    TEST(FalloutCombatTest, BuildsBareUnarmedDamageFromNativeSkillAndGmstValues)
    {
        MWMechanics::FalloutMeleeFailure failure;
        const auto contract = MWMechanics::buildFalloutMeleeContract(
            nullptr, 0, 50.f, 6.f, retailMeleeTuning(), failure);

        ASSERT_TRUE(contract);
        EXPECT_EQ(failure, MWMechanics::FalloutMeleeFailure::None);
        EXPECT_TRUE(contract->mUnarmedFamily);
        EXPECT_TRUE(contract->mBareHanded);
        EXPECT_FLOAT_EQ(contract->mDamage, 3.f);
        EXPECT_FLOAT_EQ(contract->mReach, 256.f);
    }

    TEST(FalloutCombatTest, BuildsAuthoredOneHandMeleeDamageAndReach)
    {
        ESM4::Weapon weapon;
        weapon.mData.animationType = 1;
        weapon.mData.damage = 20;
        weapon.mData.reach = 1.25f;

        MWMechanics::FalloutMeleeFailure failure;
        const auto contract = MWMechanics::buildFalloutMeleeContract(
            &weapon, weapon.mData.animationType, 50.f, 6.f, retailMeleeTuning(), failure);

        ASSERT_TRUE(contract);
        EXPECT_EQ(failure, MWMechanics::FalloutMeleeFailure::None);
        EXPECT_FALSE(contract->mUnarmedFamily);
        EXPECT_FALSE(contract->mBareHanded);
        // 20 * (0.5 + 0.5 * 50/100) + 6 * 0.5
        EXPECT_FLOAT_EQ(contract->mDamage, 18.f);
        EXPECT_FLOAT_EQ(contract->mReach, 320.f);
    }

    TEST(FalloutCombatTest, AppliesUnarmedActorDamageToAuthoredHandToHandWeapon)
    {
        ESM4::Weapon weapon;
        weapon.mData.animationType = 0;
        weapon.mData.damage = 10;
        weapon.mData.reach = 1.f;

        MWMechanics::FalloutMeleeFailure failure;
        const auto contract = MWMechanics::buildFalloutMeleeContract(
            &weapon, weapon.mData.animationType, 50.f, 6.f, retailMeleeTuning(), failure);

        ASSERT_TRUE(contract);
        EXPECT_TRUE(contract->mUnarmedFamily);
        EXPECT_FALSE(contract->mBareHanded);
        // 10 * (0.5 + 0.5 * 50/100) + (0.5 + 0.05 * 50)
        EXPECT_FLOAT_EQ(contract->mDamage, 10.5f);
    }

    TEST(FalloutCombatTest, BuildsCreatureStrikeFromWinningFNVDataPayload)
    {
        MWMechanics::FalloutMeleeFailure failure;
        const auto contract = MWMechanics::buildFalloutCreatureMeleeContract(
            12.f, 60.f, 5.f, retailMeleeTuning(), failure);

        ASSERT_TRUE(contract);
        EXPECT_EQ(failure, MWMechanics::FalloutMeleeFailure::None);
        EXPECT_TRUE(contract->mUnarmedFamily);
        EXPECT_TRUE(contract->mBareHanded);
        // CREA DATA damage 12 * (0.5 + 0.5 * combatSkill 60/100) + Strength 5 * 0.5.
        EXPECT_FLOAT_EQ(contract->mDamage, 12.1f);
        EXPECT_FLOAT_EQ(contract->mReach, 256.f);
    }

    TEST(FalloutCombatTest, RejectsNonMeleeAndMalformedMeleeContracts)
    {
        ESM4::Weapon weapon;
        weapon.mData.animationType = 3;
        weapon.mData.damage = 20;
        weapon.mData.reach = 1.f;
        MWMechanics::FalloutMeleeFailure failure;
        EXPECT_FALSE(MWMechanics::buildFalloutMeleeContract(
            &weapon, weapon.mData.animationType, 50.f, 6.f, retailMeleeTuning(), failure));
        EXPECT_EQ(failure, MWMechanics::FalloutMeleeFailure::NotMelee);

        EXPECT_FALSE(MWMechanics::buildFalloutMeleeContract(
            nullptr, 1, 50.f, 6.f, retailMeleeTuning(), failure));
        EXPECT_EQ(failure, MWMechanics::FalloutMeleeFailure::MissingWeapon);

        weapon.mData.animationType = 2;
        weapon.mData.reach = 0.f;
        EXPECT_FALSE(MWMechanics::buildFalloutMeleeContract(
            &weapon, weapon.mData.animationType, 50.f, 6.f, retailMeleeTuning(), failure));
        EXPECT_EQ(failure, MWMechanics::FalloutMeleeFailure::InvalidReach);
    }
}
