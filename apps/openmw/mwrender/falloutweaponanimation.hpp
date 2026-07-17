#ifndef GAME_RENDER_FALLOUTWEAPONANIMATION_H
#define GAME_RENDER_FALLOUTWEAPONANIMATION_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <osg/CopyOp>
#include <osg/ref_ptr>

#include <components/misc/strings/algorithm.hpp>
#include <components/sceneutil/controller.hpp>
#include <components/sceneutil/keyframe.hpp>

namespace MWRender
{
    struct FonvWeaponActionSource
    {
        std::string mPath;
        std::string_view mSemanticGroup;
        bool mRequired = true;
    };

    inline std::string_view getFonvWeaponAnimationPrefix(std::uint8_t animationType)
    {
        // TESObjectWEAP::EWeaponType, verified against xNVSE's runtime layout and retail sequence telemetry.
        // In particular, TwoHandAutomatic is the 2ha family; TwoHandMelee is 2hm, not 2ha.
        switch (animationType)
        {
            case 0:
                return "h2h";
            case 1:
                return "1hm";
            case 2:
                return "2hm";
            case 3:
            case 4:
                return "1hp";
            case 5:
            case 7:
                return "2hr";
            case 6:
                return "2ha";
            case 8:
                return "2hh";
            case 9:
                return "2hl";
            case 10:
            case 13:
                return "1gt";
            case 11:
                return "1md";
            case 12:
                return "1lm";
            default:
                return {};
        }
    }

    inline std::string getFonvWeaponAnimationKf(std::uint8_t animationType, std::string_view suffix)
    {
        const std::string_view prefix = getFonvWeaponAnimationPrefix(animationType);
        if (prefix.empty())
            return {};
        return "meshes/characters/_male/" + std::string(prefix) + std::string(suffix) + ".kf";
    }

    inline std::optional<char> getFonvWeaponReloadAnimationLetter(std::uint8_t rawReloadAnimation)
    {
        // TESObjectWEAP::ReloadAnim is non-contiguous alphabetically: A..S, W, X, Y, Z. Values 19..22 must not
        // be interpreted as T..W (for example the .357 revolver authors selector 20, which is ReloadX).
        if (rawReloadAnimation <= 18)
            return static_cast<char>('a' + rawReloadAnimation);
        switch (rawReloadAnimation)
        {
            case 19:
                return 'w';
            case 20:
                return 'x';
            case 21:
                return 'y';
            case 22:
                return 'z';
            default:
                return std::nullopt;
        }
    }

    inline std::vector<FonvWeaponActionSource> getFonvWeaponActionManifest(
        std::uint8_t animationType, std::uint8_t reloadAnimation)
    {
        const std::string_view prefix = getFonvWeaponAnimationPrefix(animationType);
        if (prefix.empty())
            return {};

        std::string_view primarySuffix;
        std::string_view primaryGroup;
        switch (animationType)
        {
            case 0: // HandToHand
            case 1: // OneHandMelee
            case 2: // TwoHandMelee
                primarySuffix = "attackright_a";
                primaryGroup = "attack2";
                break;
            case 3: // OneHandPistol
            case 4: // OneHandPistolEnergy
            case 5: // TwoHandRifle
            case 7: // TwoHandRifleEnergy
            case 9: // TwoHandLauncher
                primarySuffix = "attack3";
                primaryGroup = "attack3";
                break;
            case 6: // TwoHandAutomatic
            case 8: // TwoHandHandle
                primarySuffix = "attackloop";
                primaryGroup = "attack1";
                break;
            case 10: // OneHandGrenade
            case 13: // OneHandThrown
                primarySuffix = "attackthrow";
                primaryGroup = "attack1";
                break;
            case 11: // OneHandMine
            case 12: // OneHandLandMine
                primarySuffix = "placemine";
                primaryGroup = "attack1";
                break;
            default:
                return {};
        }

        std::vector<FonvWeaponActionSource> result;
        result.reserve(5);
        result.push_back({ getFonvWeaponAnimationKf(animationType, primarySuffix), primaryGroup, true });
        result.push_back({ getFonvWeaponAnimationKf(animationType, "equip"), "equip", true });

        // Only firearm/launcher families consume the WEAP.DNAM reload selector. Jam uses the same retail suffix.
        if (animationType >= 3 && animationType <= 9)
        {
            if (const std::optional<char> letter = getFonvWeaponReloadAnimationLetter(reloadAnimation))
            {
                const std::string suffix(1, *letter);
                result.push_back({ getFonvWeaponAnimationKf(animationType, "reload" + suffix), "reload", true });
                result.push_back({ getFonvWeaponAnimationKf(animationType, "jam" + suffix), "jam", false });
            }
        }

        result.push_back({ getFonvWeaponAnimationKf(animationType, "unequip"), "unequip", true });
        return result;
    }

    inline std::optional<unsigned int> getFonvWeaponHandGripIndex(std::uint8_t rawHandGrip)
    {
        // TESObjectWEAP::EHandGrip: Default is 0xff and authored HandGrip_1..6 are 0xe6..0xeb.
        if (rawHandGrip == 0xff)
            return 0;
        if (rawHandGrip >= 0xe6 && rawHandGrip <= 0xeb)
            return static_cast<unsigned int>(rawHandGrip - 0xe6) + 1;
        return std::nullopt;
    }

    inline std::string getFonvWeaponHandGripKf(std::uint8_t animationType, std::uint8_t rawHandGrip)
    {
        const std::optional<unsigned int> grip = getFonvWeaponHandGripIndex(rawHandGrip);
        const std::string_view prefix = getFonvWeaponAnimationPrefix(animationType);
        if (!grip || *grip == 0 || prefix.empty())
            return {};
        return "meshes/characters/_male/" + std::string(prefix) + "handgrip" + std::to_string(*grip) + ".kf";
    }

    inline osg::ref_ptr<SceneUtil::KeyframeHolder> mergeFonvWeaponControllerOverlay(
        const SceneUtil::KeyframeHolder& base, const SceneUtil::KeyframeHolder& overlay)
    {
        osg::ref_ptr<SceneUtil::KeyframeHolder> merged
            = new SceneUtil::KeyframeHolder(base, osg::CopyOp::SHALLOW_COPY);
        for (const auto& [overlayName, overlayController] : overlay.mKeyframeControllers)
        {
            auto existing = std::find_if(merged->mKeyframeControllers.begin(), merged->mKeyframeControllers.end(),
                [&](const auto& entry) { return Misc::StringUtils::ciEqual(entry.first, overlayName); });
            if (existing != merged->mKeyframeControllers.end())
                merged->mKeyframeControllers.erase(existing);
            merged->mKeyframeControllers.emplace(overlayName, overlayController);
        }
        return merged;
    }

    namespace FalloutWeaponAnimationDetail
    {
        class FixedControllerSource final : public SceneUtil::ControllerSource
        {
        public:
            explicit FixedControllerSource(float value)
                : mValue(value)
            {
            }

            float getValue(osg::NodeVisitor*) override { return mValue; }

        private:
            float mValue;
        };
    }

    inline std::optional<SceneUtil::KeyframeController::KfTransform> sampleFonvWeaponAttachmentEndpoint(
        const SceneUtil::KeyframeHolder& keyframes, std::string_view targetName = "Weapon")
    {
        const auto found = std::find_if(keyframes.mKeyframeControllers.begin(), keyframes.mKeyframeControllers.end(),
            [&](const auto& entry) { return Misc::StringUtils::ciEqual(entry.first, targetName); });
        if (found == keyframes.mKeyframeControllers.end() || found->second == nullptr)
            return std::nullopt;

        const std::shared_ptr<SceneUtil::ControllerFunction> function = found->second->getFunction();
        if (function == nullptr || !std::isfinite(function->getMaximum()))
            return std::nullopt;

        // Sample the authored stop time directly. Removing the controller function avoids applying frequency/phase
        // twice when the fixed source already contains the controller-space endpoint.
        osg::ref_ptr<SceneUtil::KeyframeController> sampled
            = osg::clone(found->second.get(), osg::CopyOp::SHALLOW_COPY);
        sampled->setFunction(std::shared_ptr<SceneUtil::ControllerFunction>());
        sampled->setSource(
            std::make_shared<FalloutWeaponAnimationDetail::FixedControllerSource>(function->getMaximum()));
        SceneUtil::KeyframeController::KfTransform transform = sampled->getCurrentTransformation(nullptr);
        if (!transform.mTranslation && !transform.mRotation && !transform.mScale)
            return std::nullopt;
        return transform;
    }
}

#endif // GAME_RENDER_FALLOUTWEAPONANIMATION_H
