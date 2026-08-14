#ifndef OPENMW_MWRENDER_FALLOUTLODSELECTION_H
#define OPENMW_MWRENDER_FALLOUTLODSELECTION_H

#include <cstdint>

namespace MWRender
{
    enum class FalloutDistantReferenceSelection
    {
        Skip,
        FullModel,
        DistantModel,
    };

    [[nodiscard]] bool isFalloutNewVegasVersion(int esmVersion);

    [[nodiscard]] bool isEsm4ReferenceEnabledForPaging(std::uint32_t recordFlags);

    [[nodiscard]] FalloutDistantReferenceSelection selectFalloutNewVegasDistantReference(
        std::uint32_t recordFlags, bool activeGrid, bool distantModelAvailable);
}

#endif
