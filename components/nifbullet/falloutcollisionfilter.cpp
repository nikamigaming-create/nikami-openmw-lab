#include "falloutcollisionfilter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace NifBullet
{
    namespace
    {
        // Row N contains the enabled second-layer bits for primary layer N. Retail has rows for layers 0 through 42;
        // its NULL layer at ordinal 43 has no row.
        constexpr std::array<std::uint64_t, 43> sPrimaryLayerMasks{
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

        // Same-system-group BIPED/DEADBIP pairs use an independent five-bit subfield matrix.
        constexpr std::array<std::uint32_t, 32> sBipedSubfieldMasks{
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

        constexpr std::uint32_t sLayerMask = 0x7f;
        constexpr std::uint32_t sSubfieldMask = 0x1f;
        constexpr std::uint32_t sDisableBit = 0x4000;
        constexpr std::uint32_t sAlternateRuleBit = 0x8000;
        constexpr std::uint32_t sBipedLayer = 8;
        constexpr std::uint32_t sDeadBipedLayer = 29;
        constexpr std::uint32_t sOrderedDisableExceptionLayer = 40;

        bool testPrimary(std::uint32_t firstLayer, std::uint32_t secondLayer)
        {
            return ((sPrimaryLayerMasks[firstLayer] >> secondLayer) & 1) != 0;
        }

        bool testBiped(std::uint32_t firstSubfield, std::uint32_t secondSubfield)
        {
            return ((sBipedSubfieldMasks[firstSubfield] >> secondSubfield) & 1) != 0;
        }

        bool isBipedLayer(std::uint32_t layer)
        {
            return layer == sBipedLayer || layer == sDeadBipedLayer;
        }
    }

    std::optional<bool> evaluateFalloutCollisionFilter(std::uint32_t first, std::uint32_t second)
    {
        const std::uint32_t firstLayer = first & sLayerMask;
        const std::uint32_t secondLayer = second & sLayerMask;
        if (firstLayer >= sPrimaryLayerMasks.size() || secondLayer >= sPrimaryLayerMasks.size())
            return std::nullopt;

        if (firstLayer != sOrderedDisableExceptionLayer && ((first | second) & sDisableBit) != 0)
            return false;

        const std::uint32_t firstGroup = first >> 16;
        const std::uint32_t secondGroup = second >> 16;
        if (firstGroup == 0 || secondGroup == 0)
            return true;

        const bool sameGroup = firstGroup == secondGroup;
        const std::uint32_t firstSubfield = (first >> 8) & sSubfieldMask;
        const std::uint32_t secondSubfield = (second >> 8) & sSubfieldMask;
        if (sameGroup && firstLayer == sBipedLayer && secondLayer == sBipedLayer)
            return testBiped(firstSubfield, secondSubfield);

        if (!sameGroup)
            return testPrimary(firstLayer, secondLayer);

        if ((first & second & sAlternateRuleBit) != 0)
        {
            if (!testPrimary(firstLayer, secondLayer))
                return false;
            const int difference = static_cast<int>(firstSubfield) - static_cast<int>(secondSubfield);
            return difference != 1 && difference != -1;
        }

        if (!isBipedLayer(firstLayer) || !isBipedLayer(secondLayer))
            return false;
        return testBiped(firstSubfield, secondSubfield);
    }
}
