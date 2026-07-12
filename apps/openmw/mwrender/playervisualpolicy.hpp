#ifndef OPENMW_MWRENDER_PLAYERVISUALPOLICY_H
#define OPENMW_MWRENDER_PLAYERVISUALPOLICY_H

#include <string_view>

namespace MWRender
{
    struct ESM4PlayerVisualEquipmentPolicy
    {
        std::string_view mOutfit;
        std::string_view mHeadgear;
    };

    inline ESM4PlayerVisualEquipmentPolicy resolveESM4PlayerVisualEquipmentPolicy(const char* esm4Outfit,
        const char* legacyFalloutOutfit, const char* esm4Headgear, const char* legacyFalloutHeadgear,
        bool useLevelOneCourierProofDefault)
    {
        const auto nonEmpty = [](const char* value) {
            return value != nullptr && *value != '\0' ? std::string_view(value) : std::string_view{};
        };

        std::string_view outfit = nonEmpty(esm4Outfit);
        if (outfit.empty())
            outfit = nonEmpty(legacyFalloutOutfit);
        if (outfit.empty() && useLevelOneCourierProofDefault)
            outfit = "OutfitRepublican02";

        std::string_view headgear = nonEmpty(esm4Headgear);
        if (headgear.empty())
            headgear = nonEmpty(legacyFalloutHeadgear);

        return { outfit, headgear };
    }
}

#endif
