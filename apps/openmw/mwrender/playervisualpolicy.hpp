#ifndef OPENMW_MWRENDER_PLAYERVISUALPOLICY_H
#define OPENMW_MWRENDER_PLAYERVISUALPOLICY_H

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace MWRender
{
    inline constexpr float getFalloutFlatPlayerVisualYawOffset()
    {
        // FO3/FNV NPC meshes already share the gameplay-forward basis. Only TES4 NPC rigs need a quarter-turn.
        return 0.f;
    }

    struct ESM4PlayerVisualEquipmentPolicy
    {
        std::string_view mOutfit;
        std::string_view mHeadgear;
        std::string_view mWeapon;
    };

    inline ESM4PlayerVisualEquipmentPolicy resolveESM4PlayerVisualEquipmentPolicy(const char* esm4Outfit,
        const char* legacyFalloutOutfit, const char* esm4Headgear, const char* legacyFalloutHeadgear,
        const char* esm4Weapon, const char* legacyFalloutWeapon, bool useLevelOneCourierProofDefault)
    {
        const auto nonEmpty = [](const char* value) {
            return value != nullptr && *value != '\0' ? std::string_view(value) : std::string_view{};
        };

        std::string_view outfit = nonEmpty(esm4Outfit);
        if (outfit.empty())
            outfit = nonEmpty(legacyFalloutOutfit);
        if (outfit.empty() && useLevelOneCourierProofDefault)
            outfit = "VaultSuit21";

        std::string_view headgear = nonEmpty(esm4Headgear);
        if (headgear.empty())
            headgear = nonEmpty(legacyFalloutHeadgear);
        if (headgear.empty() && useLevelOneCourierProofDefault)
            headgear = "CowboyHat02";

        std::string_view weapon = nonEmpty(esm4Weapon);
        if (weapon.empty())
            weapon = nonEmpty(legacyFalloutWeapon);
        if (weapon.empty() && useLevelOneCourierProofDefault)
            weapon = "WeapNVVarmintRifle";

        return { outfit, headgear, weapon };
    }

    inline std::string normalizeFalloutFirstPersonBipedModel(std::string_view bipedModel)
    {
        if (bipedModel.empty())
            return {};
        std::string result(bipedModel);
        std::replace(result.begin(), result.end(), '\\', '/');
        std::string lowered(result);
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (!lowered.starts_with("meshes/"))
        {
            result.insert(0, "meshes/");
            lowered.insert(0, "meshes/");
        }
        if (!lowered.ends_with(".nif"))
            return {};
        if (!lowered.ends_with("1st.nif"))
            result.insert(result.size() - 4, "1st");
        return result;
    }

    inline bool isFalloutPipBoyGloveFirstPersonModel(bool pipBoyWorn, std::string_view firstPersonModel)
    {
        std::string lowered(firstPersonModel);
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        return pipBoyWorn && lowered.find("pipboy") != std::string::npos
            && lowered.find("glove") != std::string::npos;
    }

    inline std::string_view selectFalloutWeaponViewModel(
        std::string_view worldModel, std::string_view firstPersonModel, bool firstPerson)
    {
        if (firstPerson && !firstPersonModel.empty())
            return firstPersonModel;
        return worldModel;
    }

}

#endif
