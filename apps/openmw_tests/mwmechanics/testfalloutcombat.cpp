#include <apps/openmw/mwmechanics/falloutcombat.hpp>

#include <components/esm4/loadproj.hpp>
#include <components/esm4/loadweap.hpp>

#include <gtest/gtest.h>

#include <array>
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
        const auto contract = MWMechanics::buildFalloutHitscanContract(weapon, projectile, id(0x4240), failure);

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
        const MWMechanics::FalloutVatsBodyPartContract head{
            1, "Head", "Bip01 Head", 25, 35, 20, 2.f, false };
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

    TEST(FalloutCombatTest, FailsClosedForAnUnimplementedMultiProjectileWeapon)
    {
        ESM4::Weapon weapon;
        weapon.mData.hasBallistics = true;
        weapon.mData.projectile = id(0x426d);
        weapon.mData.ammoUse = 1;
        weapon.mData.numProjectiles = 7;
        ESM4::Projectile projectile;
        projectile.mId = id(0x426d);
        projectile.mData.present = true;
        projectile.mData.flags = ESM4::Projectile::Hitscan;
        projectile.mData.range = 10000.f;

        MWMechanics::FalloutShotFailure failure;
        EXPECT_FALSE(MWMechanics::buildFalloutHitscanContract(weapon, projectile, id(0x4240), failure));
        EXPECT_EQ(failure, MWMechanics::FalloutShotFailure::UnsupportedProjectileCount);
    }
}
