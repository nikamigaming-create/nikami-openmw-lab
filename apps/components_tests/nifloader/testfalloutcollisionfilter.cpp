#include <components/nifbullet/falloutcollisionfilter.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace
{
    constexpr std::array<std::uint64_t, 43> sExpectedPrimaryMasks{
        0xfffffffbff9ffff7ULL,
        0xfffffff97e1b6fffULL,
        0xfffffffdfe1b6fffULL,
        0xfffffdd9fc134f3eULL,
        0xfffffbb97e1b7effULL,
        0xfffffbb1761b7effULL,
        0xfffffbb17d927f77ULL,
        0xfffffd8168127f37ULL,
        0xffffffb91c037bcfULL,
        0xfffffff97e1b6fffULL,
        0xffffffb97e1b6effULL,
        0xfffff9c37c036fffULL,
        0xfffff980e81041f1ULL,
        0xfffffff97e1b6ff7ULL,
        0xffffffb9fe1b7fffULL,
        0xfffffc1108000001ULL,
        0xffffffb9fc036f3fULL,
        0xfffffff97e1b6fffULL,
        0xfffffc1308200001ULL,
        0xfffffb81223a6637ULL,
        0xffffffdbfe3a76ffULL,
        0xfffffc1342dc0000ULL,
        0xfffff98050200000ULL,
        0xfffff98008200041ULL,
        0xfffffb8048000041ULL,
        0xfffffb83223a6637ULL,
        0xfffffdd9fc136f7fULL,
        0xfffffbd07d97ffdfULL,
        0xfffffdddfc536f7fULL,
        0xfffffbb93e1b7effULL,
        0xffffff99dd737effULL,
        0xfffff9815411500dULL,
        0xfffffffbf63fefffULL,
        0xfffffc1102340801ULL,
        0x0000000010000004ULL,
        0xfffff9b17413671fULL,
        0xfffffdbb7c37e77fULL,
        0xfffff9b920036777ULL,
        0xfffff9c11c122a0fULL,
        0xfffffdf9ffdb7fffULL,
        0xfffffdf9ffdb7fffULL,
        0xfffff8016b1b6777ULL,
        0xfffffd935437e78fULL,
    };

    constexpr std::array<std::uint32_t, 32> sExpectedBipedMasks{
        0x00000000U,
        0x004230c0U,
        0x014030c0U,
        0x014030c0U,
        0x014230c0U,
        0x00403000U,
        0x0042791eU,
        0x0041ff1eU,
        0x0145f0c0U,
        0x0145e080U,
        0x0144e080U,
        0x004000c0U,
        0x004241feU,
        0x0045c7feU,
        0x014437c0U,
        0x01442780U,
        0x01442380U,
        0x00401052U,
        0x0041e700U,
        0x00000000U,
        0x00400000U,
        0x00400000U,
        0x0037fffeU,
        0x00000000U,
        0x0001c71cU,
        0x00000000U,
        0x00000000U,
        0x00000000U,
        0x00000000U,
        0x00000000U,
        0x00000000U,
        0x00000000U,
    };

    void expectResult(std::uint32_t first, std::uint32_t second, bool expected)
    {
        const std::optional<bool> result = NifBullet::evaluateFalloutCollisionFilter(first, second);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, expected);
    }
}

TEST(FalloutCollisionFilterTest, matchesEveryPrimaryLayerPair)
{
    for (std::uint32_t firstLayer = 0; firstLayer < sExpectedPrimaryMasks.size(); ++firstLayer)
    {
        for (std::uint32_t secondLayer = 0; secondLayer < sExpectedPrimaryMasks.size(); ++secondLayer)
        {
            SCOPED_TRACE(::testing::Message() << "firstLayer=" << firstLayer << " secondLayer=" << secondLayer);
            const bool expected = ((sExpectedPrimaryMasks[firstLayer] >> secondLayer) & 1) != 0;
            expectResult((1U << 16) | firstLayer, (2U << 16) | secondLayer, expected);
        }
    }
}

TEST(FalloutCollisionFilterTest, matchesEveryBipedSubfieldPair)
{
    for (std::uint32_t firstSubfield = 0; firstSubfield < sExpectedBipedMasks.size(); ++firstSubfield)
    {
        for (std::uint32_t secondSubfield = 0; secondSubfield < sExpectedBipedMasks.size(); ++secondSubfield)
        {
            SCOPED_TRACE(
                ::testing::Message() << "firstSubfield=" << firstSubfield << " secondSubfield=" << secondSubfield);
            const bool expected = ((sExpectedBipedMasks[firstSubfield] >> secondSubfield) & 1) != 0;
            expectResult((3U << 16) | (firstSubfield << 8) | 8U, (3U << 16) | (secondSubfield << 8) | 8U, expected);
        }
    }
}

TEST(FalloutCollisionFilterTest, capturedMatricesAreSymmetric)
{
    for (std::size_t first = 0; first < sExpectedPrimaryMasks.size(); ++first)
        for (std::size_t second = 0; second < sExpectedPrimaryMasks.size(); ++second)
            EXPECT_EQ((sExpectedPrimaryMasks[first] >> second) & 1, (sExpectedPrimaryMasks[second] >> first) & 1);

    for (std::size_t first = 0; first < sExpectedBipedMasks.size(); ++first)
        for (std::size_t second = 0; second < sExpectedBipedMasks.size(); ++second)
            EXPECT_EQ((sExpectedBipedMasks[first] >> second) & 1, (sExpectedBipedMasks[second] >> first) & 1);
}

TEST(FalloutCollisionFilterTest, acceptsBeforeMatrixLookupWhenEitherSystemGroupIsZero)
{
    expectResult(1U, (2U << 16) | 15U, true);
    expectResult((1U << 16) | 15U, 1U, true);
    expectResult(0x4000U | 1U, (2U << 16) | 15U, false);
}

TEST(FalloutCollisionFilterTest, appliesTheOrderedBit14ExceptionOnlyToFirstLayer40)
{
    expectResult((1U << 16) | 1U, (2U << 16) | 0x4000U | 40U, false);
    expectResult((1U << 16) | 40U, (2U << 16) | 0x4000U, true);
}

TEST(FalloutCollisionFilterTest, appliesSameGroupBit15SubfieldDifferenceRule)
{
    expectResult((1U << 16) | 0x8000U | 1U, (1U << 16) | 0x8000U | 1U, true);
    expectResult((1U << 16) | 0x8000U | 1U, (1U << 16) | 0x8100U | 1U, false);
    expectResult((1U << 16) | 0x8000U | 1U, (1U << 16) | 0x8200U | 1U, true);
    expectResult((1U << 16) | 0x8000U | 1U, (1U << 16) | 0x8000U | 15U, false);
}

TEST(FalloutCollisionFilterTest, restrictsOrdinarySameGroupPairsToBipedLayers)
{
    expectResult((1U << 16) | (1U << 8) | 29U, (1U << 16) | (6U << 8) | 8U, true);
    expectResult((1U << 16) | (1U << 8) | 1U, (1U << 16) | (6U << 8) | 29U, false);
    expectResult((1U << 16) | 0x8000U | (7U << 8) | 8U, (1U << 16) | 0x8000U | (8U << 8) | 8U, true);
}

TEST(FalloutCollisionFilterTest, reportsLayersOutsideTheRetailTableAsUnsupported)
{
    EXPECT_EQ(NifBullet::evaluateFalloutCollisionFilter(43U, 0U), std::nullopt);
    EXPECT_EQ(NifBullet::evaluateFalloutCollisionFilter(0U, 127U), std::nullopt);
}
