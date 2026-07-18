#ifndef OPENMW_COMPONENTS_ESM4_LIGHTINGCOMPOSITION_H
#define OPENMW_COMPONENTS_ESM4_LIGHTINGCOMPOSITION_H

#include "lighting.hpp"

#include <cstdint>

namespace ESM4
{
    enum LightingTemplateInheritFlag : std::uint32_t
    {
        LightingTemplateInherit_Ambient = 0x00000001,
        LightingTemplateInherit_Directional = 0x00000002,
        LightingTemplateInherit_FogColor = 0x00000004,
        LightingTemplateInherit_FogNear = 0x00000008,
        LightingTemplateInherit_FogFar = 0x00000010,
        LightingTemplateInherit_DirectionalRotation = 0x00000020,
        LightingTemplateInherit_DirectionalFade = 0x00000040,
        LightingTemplateInherit_FogClipDistance = 0x00000080,
        LightingTemplateInherit_FogPower = 0x00000100,
    };

    /// Apply the CELL LNAM inheritance mask to its XCLL data and referenced LGTM data.
    Lighting composeLighting(const Lighting& cell, const Lighting& lightingTemplate, std::uint32_t inheritFlags);
}

#endif
