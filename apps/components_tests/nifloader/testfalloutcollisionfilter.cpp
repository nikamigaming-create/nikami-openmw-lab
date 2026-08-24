#include <components/nifbullet/falloutcollisionfilter.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#ifndef OPENMW_PROJECT_SOURCE_DIR
#define OPENMW_PROJECT_SOURCE_DIR "."
#endif

namespace
{
    constexpr std::uint32_t kNoSystemGroup = 0U;
    constexpr std::uint32_t kFirstSystemGroup = 1U;
    constexpr std::uint32_t kSecondSystemGroup = 2U;
    constexpr std::uint32_t kSharedSystemGroup = 3U;
    constexpr std::uint32_t kFirstSubfield = 1U;
    constexpr std::uint32_t kSecondSubfield = 6U;
    constexpr std::uint32_t kAlternateFirstSubfield = 7U;
    constexpr std::uint32_t kAlternateSecondSubfield = 8U;
    constexpr std::uint32_t kNoSubfield = 0U;
    constexpr std::uint32_t kOrdinaryLayer = 1U;
    constexpr std::uint32_t kSetBit = 1U;

    std::uint64_t parseUnsigned(const std::string& token)
    {
        std::size_t consumed = 0;
        const std::uint64_t result = std::stoull(token, &consumed, 0);
        if (consumed != token.size())
            throw std::runtime_error("invalid Fallout collision-filter value: " + token);
        return result;
    }

    NifBullet::FalloutCollisionFilterConfig loadConfig()
    {
        const std::filesystem::path path
            = std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "apps" / "components_tests" / "data"
            / "fallout_collision_filter.cfg";
        std::ifstream input(path);
        if (!input)
            throw std::runtime_error("unable to open Fallout collision-filter config: " + path.string());

        NifBullet::FalloutCollisionFilterConfig config;
        std::string line;
        while (std::getline(input, line))
        {
            const std::size_t comment = line.find('#');
            if (comment != std::string::npos)
                line.resize(comment);

            std::istringstream values(line);
            std::string key;
            std::string token;
            if (!(values >> key))
                continue;
            if (!(values >> token))
                throw std::runtime_error("missing Fallout collision-filter value for: " + key);

            const std::uint64_t value = parseUnsigned(token);
            const auto asWord = [&]() {
                if (value > std::numeric_limits<std::uint32_t>::max())
                    throw std::runtime_error("Fallout collision-filter value exceeds 32 bits: " + token);
                return static_cast<std::uint32_t>(value);
            };
            if (key == "layer_mask")
                config.mLayerMask = asWord();
            else if (key == "subfield_mask")
                config.mSubfieldMask = asWord();
            else if (key == "disable_bit")
                config.mDisableBit = asWord();
            else if (key == "alternate_rule_bit")
                config.mAlternateRuleBit = asWord();
            else if (key == "biped_layer")
                config.mBipedLayer = asWord();
            else if (key == "dead_biped_layer")
                config.mDeadBipedLayer = asWord();
            else if (key == "ordered_disable_exception_layer")
                config.mOrderedDisableExceptionLayer = asWord();
            else if (key == "group_shift")
                config.mGroupShift = asWord();
            else if (key == "subfield_shift")
                config.mSubfieldShift = asWord();
            else if (key == "adjacent_subfield_distance")
                config.mAdjacentSubfieldDistance = asWord();
            else if (key == "primary_layer_mask")
                config.mPrimaryLayerMasks.push_back(value);
            else if (key == "biped_subfield_mask")
                config.mBipedSubfieldMasks.push_back(asWord());
            else
                throw std::runtime_error("unknown Fallout collision-filter setting: " + key);
        }

        if (config.mPrimaryLayerMasks.empty() || config.mBipedSubfieldMasks.empty())
            throw std::runtime_error("Fallout collision-filter config has no lookup tables");
        return config;
    }

    const NifBullet::FalloutCollisionFilterConfig& collisionConfig()
    {
        static const NifBullet::FalloutCollisionFilterConfig config = loadConfig();
        return config;
    }

    std::uint32_t makeWord(const NifBullet::FalloutCollisionFilterConfig& config, std::uint32_t group,
        std::uint32_t subfield, std::uint32_t layer, std::uint32_t flags = {})
    {
        return (group << config.mGroupShift) | (subfield << config.mSubfieldShift)
            | (layer & config.mLayerMask) | flags;
    }

    void expectResult(const NifBullet::FalloutCollisionFilterConfig& config, std::uint32_t first,
        std::uint32_t second, bool expected)
    {
        const std::optional<bool> result = NifBullet::evaluateFalloutCollisionFilter(first, second, config);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, expected);
    }
}

TEST(FalloutCollisionFilterTest, matchesEveryPrimaryLayerPair)
{
    const auto& config = collisionConfig();
    for (std::size_t firstLayer = 0; firstLayer < config.mPrimaryLayerMasks.size(); ++firstLayer)
    {
        for (std::size_t secondLayer = 0; secondLayer < config.mPrimaryLayerMasks.size(); ++secondLayer)
        {
            SCOPED_TRACE(::testing::Message() << "firstLayer=" << firstLayer << " secondLayer=" << secondLayer);
            const bool expected = ((config.mPrimaryLayerMasks[firstLayer] >> secondLayer) & kSetBit) != 0;
            expectResult(config, makeWord(config, kFirstSystemGroup, {}, static_cast<std::uint32_t>(firstLayer)),
                makeWord(config, kSecondSystemGroup, {}, static_cast<std::uint32_t>(secondLayer)), expected);
        }
    }
}

TEST(FalloutCollisionFilterTest, matchesEveryBipedSubfieldPair)
{
    const auto& config = collisionConfig();
    for (std::size_t firstSubfield = 0; firstSubfield < config.mBipedSubfieldMasks.size(); ++firstSubfield)
    {
        for (std::size_t secondSubfield = 0; secondSubfield < config.mBipedSubfieldMasks.size(); ++secondSubfield)
        {
            SCOPED_TRACE(
                ::testing::Message() << "firstSubfield=" << firstSubfield << " secondSubfield=" << secondSubfield);
            const bool expected = ((config.mBipedSubfieldMasks[firstSubfield] >> secondSubfield) & kSetBit) != 0;
            expectResult(config,
                makeWord(config, kSharedSystemGroup, static_cast<std::uint32_t>(firstSubfield), config.mBipedLayer),
                makeWord(config, kSharedSystemGroup, static_cast<std::uint32_t>(secondSubfield), config.mBipedLayer),
                expected);
        }
    }
}

TEST(FalloutCollisionFilterTest, capturedMatricesAreSymmetric)
{
    const auto& config = collisionConfig();
    for (std::size_t first = 0; first < config.mPrimaryLayerMasks.size(); ++first)
        for (std::size_t second = 0; second < config.mPrimaryLayerMasks.size(); ++second)
            EXPECT_EQ((config.mPrimaryLayerMasks[first] >> second) & kSetBit,
                (config.mPrimaryLayerMasks[second] >> first) & kSetBit);

    for (std::size_t first = 0; first < config.mBipedSubfieldMasks.size(); ++first)
        for (std::size_t second = 0; second < config.mBipedSubfieldMasks.size(); ++second)
            EXPECT_EQ((config.mBipedSubfieldMasks[first] >> second) & kSetBit,
                (config.mBipedSubfieldMasks[second] >> first) & kSetBit);
}

TEST(FalloutCollisionFilterTest, acceptsBeforeMatrixLookupWhenEitherSystemGroupIsZero)
{
    const auto& config = collisionConfig();
    expectResult(config, makeWord(config, kNoSystemGroup, kFirstSubfield, kOrdinaryLayer),
        makeWord(config, kSecondSystemGroup, kSecondSubfield, kOrdinaryLayer), true);
    expectResult(config, makeWord(config, kFirstSystemGroup, kFirstSubfield, kOrdinaryLayer),
        makeWord(config, kNoSystemGroup, kSecondSubfield, kOrdinaryLayer), true);
    expectResult(config, makeWord(config, kNoSystemGroup, {}, kOrdinaryLayer, config.mDisableBit),
        makeWord(config, kSecondSystemGroup, kSecondSubfield, kOrdinaryLayer), false);
}

TEST(FalloutCollisionFilterTest, appliesTheOrderedBit14ExceptionOnlyToFirstLayer)
{
    const auto& config = collisionConfig();
    expectResult(config, makeWord(config, kFirstSystemGroup, {}, kOrdinaryLayer),
        makeWord(config, kSecondSystemGroup, {}, config.mOrderedDisableExceptionLayer, config.mDisableBit), false);
    expectResult(config, makeWord(config, kFirstSystemGroup, {}, config.mOrderedDisableExceptionLayer),
        makeWord(config, kSecondSystemGroup, {}, kOrdinaryLayer, config.mDisableBit), true);
}

TEST(FalloutCollisionFilterTest, appliesSameGroupAlternateSubfieldRule)
{
    const auto& config = collisionConfig();
    expectResult(config,
        makeWord(config, kFirstSystemGroup, kNoSubfield, kOrdinaryLayer, config.mAlternateRuleBit),
        makeWord(config, kFirstSystemGroup, kNoSubfield, kOrdinaryLayer, config.mAlternateRuleBit), true);
    expectResult(config,
        makeWord(config, kFirstSystemGroup, kNoSubfield, kOrdinaryLayer, config.mAlternateRuleBit),
        makeWord(config, kFirstSystemGroup, kNoSubfield + config.mAdjacentSubfieldDistance, kOrdinaryLayer,
            config.mAlternateRuleBit),
        false);
    expectResult(config,
        makeWord(config, kFirstSystemGroup, kNoSubfield, kOrdinaryLayer, config.mAlternateRuleBit),
        makeWord(config, kFirstSystemGroup,
            kNoSubfield + config.mAdjacentSubfieldDistance + config.mAdjacentSubfieldDistance, kOrdinaryLayer,
            config.mAlternateRuleBit),
        true);
}

TEST(FalloutCollisionFilterTest, restrictsOrdinarySameGroupPairsToBipedLayers)
{
    const auto& config = collisionConfig();
    expectResult(config,
        makeWord(config, kFirstSystemGroup, kFirstSubfield, config.mDeadBipedLayer),
        makeWord(config, kFirstSystemGroup, kSecondSubfield, config.mBipedLayer), true);
    expectResult(config,
        makeWord(config, kFirstSystemGroup, kFirstSubfield, kOrdinaryLayer),
        makeWord(config, kFirstSystemGroup, kSecondSubfield, config.mDeadBipedLayer), false);
    expectResult(config,
        makeWord(config, kFirstSystemGroup, kAlternateFirstSubfield, config.mBipedLayer, config.mAlternateRuleBit),
        makeWord(config, kFirstSystemGroup, kAlternateSecondSubfield, config.mBipedLayer, config.mAlternateRuleBit),
        true);
}

TEST(FalloutCollisionFilterTest, reportsLayersOutsideTheInjectedTableAsUnsupported)
{
    const auto& config = collisionConfig();
    const auto firstOutside = static_cast<std::uint32_t>(config.mPrimaryLayerMasks.size());
    EXPECT_EQ(NifBullet::evaluateFalloutCollisionFilter(firstOutside, {}, config), std::nullopt);
    EXPECT_EQ(NifBullet::evaluateFalloutCollisionFilter({}, config.mLayerMask, config), std::nullopt);
}

TEST(FalloutCollisionFilterTest, rejectsAnIncompleteInjectedPolicy)
{
    const NifBullet::FalloutCollisionFilterConfig incomplete;
    EXPECT_EQ(NifBullet::evaluateFalloutCollisionFilter({}, {}, incomplete), std::nullopt);
}
