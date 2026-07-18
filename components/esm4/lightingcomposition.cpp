#include "lightingcomposition.hpp"

ESM4::Lighting ESM4::composeLighting(
    const Lighting& cell, const Lighting& lightingTemplate, std::uint32_t inheritFlags)
{
    Lighting result = cell;
    if (inheritFlags & LightingTemplateInherit_Ambient)
        result.ambient = lightingTemplate.ambient;
    if (inheritFlags & LightingTemplateInherit_Directional)
        result.directional = lightingTemplate.directional;
    if (inheritFlags & LightingTemplateInherit_FogColor)
        result.fogColor = lightingTemplate.fogColor;
    if (inheritFlags & LightingTemplateInherit_FogNear)
        result.fogNear = lightingTemplate.fogNear;
    if (inheritFlags & LightingTemplateInherit_FogFar)
        result.fogFar = lightingTemplate.fogFar;
    if (inheritFlags & LightingTemplateInherit_DirectionalRotation)
    {
        result.rotationXY = lightingTemplate.rotationXY;
        result.rotationZ = lightingTemplate.rotationZ;
    }
    if (inheritFlags & LightingTemplateInherit_DirectionalFade)
        result.fogDirFade = lightingTemplate.fogDirFade;
    if (inheritFlags & LightingTemplateInherit_FogClipDistance)
        result.fogClipDist = lightingTemplate.fogClipDist;
    if (inheritFlags & LightingTemplateInherit_FogPower)
        result.fogPower = lightingTemplate.fogPower;
    return result;
}
