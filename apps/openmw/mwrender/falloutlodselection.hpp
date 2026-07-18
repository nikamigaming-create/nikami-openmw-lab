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

    bool isFalloutNewVegasVersion(int esmVersion);

    bool isEsm4ReferenceEnabledForPaging(std::uint32_t recordFlags);

    FalloutDistantReferenceSelection selectFalloutNewVegasDistantReference(
        std::uint32_t recordFlags, bool activeGrid, bool distantModelAvailable);
}

#endif
