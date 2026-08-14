#include "falloutlodselection.hpp"

#include <components/esm/common.hpp>
#include <components/esm4/common.hpp>

bool MWRender::isFalloutNewVegasVersion(int esmVersion)
{
    return esmVersion == ESM::VER_132 || esmVersion == ESM::VER_133 || esmVersion == ESM::VER_134;
}

bool MWRender::isEsm4ReferenceEnabledForPaging(std::uint32_t recordFlags)
{
    return !(recordFlags & ESM4::Rec_Disabled);
}

MWRender::FalloutDistantReferenceSelection MWRender::selectFalloutNewVegasDistantReference(
    std::uint32_t recordFlags, bool activeGrid, bool distantModelAvailable)
{
    if (activeGrid)
        return FalloutDistantReferenceSelection::FullModel;
    if (!(recordFlags & ESM4::Rec_VisDistant))
        return FalloutDistantReferenceSelection::Skip;
    if (distantModelAvailable)
        return FalloutDistantReferenceSelection::DistantModel;
    return FalloutDistantReferenceSelection::FullModel;
}
