#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>

#include <components/esm4/loadamef.hpp>

#include "apps/openmw/mwmechanics/falloutammoeffects.hpp"

namespace
{
    ESM4::AmmoEffect makeEffect(ESM4::AmmoEffect::Type type, ESM4::AmmoEffect::Operation operation, float value)
    {
        ESM4::AmmoEffect effect{};
        effect.mType = type;
        effect.mOperation = operation;
        effect.mValue = value;
        return effect;
    }

    MWMechanics::FalloutAmmoEffectFailure failureFor(float baseValue, ESM4::AmmoEffect::Type type,
        std::span<const ESM4::AmmoEffect* const> effects)
    {
        MWMechanics::FalloutAmmoEffectFailure failure = MWMechanics::FalloutAmmoEffectFailure::None;
        EXPECT_FALSE(MWMechanics::applyFalloutAmmoEffects(baseValue, type, effects, failure));
        return failure;
    }
}

TEST(FalloutAmmoEffectsTest, AppliesMatchingEffectsInAuthoredOrder)
{
    const ESM4::AmmoEffect add = makeEffect(ESM4::AmmoEffect::Type::WeaponCondition,
        ESM4::AmmoEffect::Operation::Add, 2.f);
    const ESM4::AmmoEffect ignored = makeEffect(ESM4::AmmoEffect::Type::Damage,
        ESM4::AmmoEffect::Operation::Multiply, 9.f);
    const ESM4::AmmoEffect multiply = makeEffect(ESM4::AmmoEffect::Type::WeaponCondition,
        ESM4::AmmoEffect::Operation::Multiply, 3.f);
    const ESM4::AmmoEffect subtract = makeEffect(ESM4::AmmoEffect::Type::WeaponCondition,
        ESM4::AmmoEffect::Operation::Subtract, 1.f);
    const std::array effects{ &add, &ignored, &multiply, &subtract };
    MWMechanics::FalloutAmmoEffectFailure failure = MWMechanics::FalloutAmmoEffectFailure::None;

    const auto result = MWMechanics::applyFalloutAmmoEffects(4.f, ESM4::AmmoEffect::Type::WeaponCondition, effects,
        failure);

    ASSERT_TRUE(result);
    EXPECT_FLOAT_EQ(*result, 17.f);
    EXPECT_EQ(failure, MWMechanics::FalloutAmmoEffectFailure::None);
}

TEST(FalloutAmmoEffectsTest, EmptyAndNonMatchingListsPreserveBaseValue)
{
    const ESM4::AmmoEffect damage = makeEffect(ESM4::AmmoEffect::Type::Damage,
        ESM4::AmmoEffect::Operation::Add, 5.f);
    const std::array effects{ &damage };
    MWMechanics::FalloutAmmoEffectFailure failure = MWMechanics::FalloutAmmoEffectFailure::None;

    const auto result = MWMechanics::applyFalloutAmmoEffects(4.f, ESM4::AmmoEffect::Type::WeaponCondition, effects,
        failure);

    ASSERT_TRUE(result);
    EXPECT_FLOAT_EQ(*result, 4.f);
    EXPECT_EQ(failure, MWMechanics::FalloutAmmoEffectFailure::None);
}

TEST(FalloutAmmoEffectsTest, RejectsMalformedInputsAndResults)
{
    const std::array missing{ static_cast<const ESM4::AmmoEffect*>(nullptr) };
    EXPECT_EQ(failureFor(1.f, ESM4::AmmoEffect::Type::Damage, missing),
        MWMechanics::FalloutAmmoEffectFailure::MissingEffect);

    const ESM4::AmmoEffect nonFinite = makeEffect(ESM4::AmmoEffect::Type::Damage,
        ESM4::AmmoEffect::Operation::Add, std::numeric_limits<float>::infinity());
    const std::array nonFiniteList{ &nonFinite };
    EXPECT_EQ(failureFor(1.f, ESM4::AmmoEffect::Type::Damage, nonFiniteList),
        MWMechanics::FalloutAmmoEffectFailure::InvalidEffectValue);

    const ESM4::AmmoEffect invalidOperation = makeEffect(ESM4::AmmoEffect::Type::Damage,
        static_cast<ESM4::AmmoEffect::Operation>(std::numeric_limits<std::uint32_t>::max()), 1.f);
    const std::array invalidOperationList{ &invalidOperation };
    EXPECT_EQ(failureFor(1.f, ESM4::AmmoEffect::Type::Damage, invalidOperationList),
        MWMechanics::FalloutAmmoEffectFailure::InvalidOperation);

    const ESM4::AmmoEffect overflow = makeEffect(ESM4::AmmoEffect::Type::Damage,
        ESM4::AmmoEffect::Operation::Multiply, std::numeric_limits<float>::max());
    const std::array overflowList{ &overflow };
    EXPECT_EQ(failureFor(2.f, ESM4::AmmoEffect::Type::Damage, overflowList),
        MWMechanics::FalloutAmmoEffectFailure::InvalidResult);

    const std::array empty = std::array<const ESM4::AmmoEffect*, 0>{};
    EXPECT_EQ(failureFor(std::numeric_limits<float>::quiet_NaN(), ESM4::AmmoEffect::Type::Damage, empty),
        MWMechanics::FalloutAmmoEffectFailure::InvalidBaseValue);
}
