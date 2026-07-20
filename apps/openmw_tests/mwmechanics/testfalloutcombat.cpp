#include <apps/openmw/mwmechanics/falloutcombat.hpp>
#include <apps/openmw/mwmechanics/actors.hpp>
#include <apps/openmw/mwworld/ptr.hpp>

#include <components/esm4/loadproj.hpp>
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

    TEST(FalloutCombatTest, EmptyFollowTargetHasNoSidingActors)
    {
        const MWMechanics::Actors actors;
        EXPECT_TRUE(actors.getActorsSidingWith(MWWorld::Ptr{}).empty());
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
        EXPECT_FLOAT_EQ(contract->mSpread, 1.5f);
        EXPECT_TRUE(contract->mAuthoredHitscan);
    }

    TEST(FalloutCombatTest, UsesImmediateRayFallbackForAuthoredNonHitscanProjectile)
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

    TEST(FalloutCombatTest, BuildsUnitRayAtAuthoredSpreadConeBoundary)
    {
        const auto center = MWMechanics::buildFalloutRayDirection(
            osg::Vec3f(0.f, 2.f, 0.f), 10.f, osg::Vec2f(0.f, 0.f));
        ASSERT_TRUE(center);
        EXPECT_FLOAT_EQ(center->x(), 0.f);
        EXPECT_FLOAT_EQ(center->y(), 1.f);
        EXPECT_FLOAT_EQ(center->z(), 0.f);

        const auto edge = MWMechanics::buildFalloutRayDirection(
            osg::Vec3f(0.f, 1.f, 0.f), 10.f, osg::Vec2f(1.f, 0.f));
        ASSERT_TRUE(edge);
        EXPECT_NEAR(edge->length(), 1.f, 1e-6f);
        EXPECT_NEAR(std::acos((*edge) * osg::Vec3f(0.f, 1.f, 0.f)), osg::DegreesToRadians(10.f), 1e-6f);
    }

    TEST(FalloutCombatTest, RejectsMalformedSpreadAndRayInputsBeforeFiring)
    {
        ESM4::Weapon weapon;
        weapon.mData.hasBallistics = true;
        weapon.mData.projectile = id(0x426d);
        weapon.mData.ammoUse = 1;
        weapon.mData.numProjectiles = 1;
        weapon.mData.spread = std::numeric_limits<float>::quiet_NaN();
        ESM4::Projectile projectile;
        projectile.mId = id(0x426d);
        projectile.mData.present = true;
        projectile.mData.range = 10000.f;

        MWMechanics::FalloutShotFailure failure;
        EXPECT_FALSE(MWMechanics::buildFalloutRayShotContract(weapon, projectile, id(0x4240), failure));
        EXPECT_EQ(failure, MWMechanics::FalloutShotFailure::InvalidSpread);
        EXPECT_FALSE(MWMechanics::buildFalloutRayDirection(
            osg::Vec3f(0.f, 0.f, 0.f), 1.f, osg::Vec2f(0.f, 0.f)));
        EXPECT_FALSE(MWMechanics::buildFalloutRayDirection(
            osg::Vec3f(0.f, 1.f, 0.f), 1.f, osg::Vec2f(1.1f, 0.f)));
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
