#ifndef GAME_RENDER_FONVPACKAGEANIMATION_H
#define GAME_RENDER_FONVPACKAGEANIMATION_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace MWRender
{
    /// Return the retail KF candidates associated with the active humanoid package procedure.
    /// Asset availability remains a runtime VFS concern; keeping this policy pure makes the
    /// package-to-animation contract independently auditable.
    [[nodiscard]] inline std::vector<std::string_view> getFonvPackageProcedureAnimationCandidates(
        std::int32_t packageType, std::string_view furnitureModel)
    {
        static constexpr std::array<std::string_view, 8> chairTransitions = {
            "meshes/characters/_male/idleanims/chair_forwardenter.kf",
            "meshes/characters/_male/idleanims/chair_forwardexit.kf",
            "meshes/characters/_male/idleanims/chair_backenter.kf",
            "meshes/characters/_male/idleanims/chair_backexit.kf",
            "meshes/characters/_male/idleanims/chair_leftenter.kf",
            "meshes/characters/_male/idleanims/chair_leftexit.kf",
            "meshes/characters/_male/idleanims/chair_rightenter.kf",
            "meshes/characters/_male/idleanims/chair_rightexit.kf",
        };

        std::string lowerFurniture(furnitureModel);
        for (char& c : lowerFurniture)
        {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c + ('a' - 'A'));
        }

        const bool usesTableSeat = lowerFurniture.find("dinerbooth") != std::string::npos
            || lowerFurniture.find("table") != std::string::npos;
        const bool usesChairSeat = lowerFurniture.find("chair") != std::string::npos || usesTableSeat;

        std::vector<std::string_view> result;
        const auto addChairTransitions = [&]() {
            result.insert(result.end(), chairTransitions.begin(), chairTransitions.end());
        };

        switch (packageType)
        {
            case 3: // Eat
                if (usesChairSeat)
                    addChairTransitions();
                result.emplace_back(usesTableSeat
                        ? "meshes/characters/_male/idleanims/sittablechaireata.kf"
                        : "meshes/characters/_male/idleanims/sitchaireata.kf");
                result.emplace_back(usesChairSeat
                        ? "meshes/characters/_male/idleanims/dynamicidle_chairsit.kf"
                        : "meshes/characters/_male/idleanims/dynamicidle_sit.kf");
                break;
            case 4: // Sleep
                result.emplace_back("meshes/characters/_male/idleanims/dynamicidle_sleep.kf");
                break;
            case 6: // Travel-to-ref, used by scheduled chair packages.
            case 8: // Use item at / furniture.
                if (!furnitureModel.empty())
                {
                    if (usesChairSeat)
                        addChairTransitions();
                    result.emplace_back("meshes/characters/_male/idleanims/sitchairlistena.kf");
                    result.emplace_back("meshes/characters/_male/idleanims/sitchairtalktoplayera.kf");
                    result.emplace_back(usesChairSeat
                            ? "meshes/characters/_male/idleanims/dynamicidle_chairsit.kf"
                            : "meshes/characters/_male/idleanims/dynamicidle_sit.kf");
                }
                break;
            default:
                break;
        }

        return result;
    }
}

#endif // GAME_RENDER_FONVPACKAGEANIMATION_H
