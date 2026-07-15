#ifndef GAME_RENDER_FALLOUTWEAPONANIMATION_H
#define GAME_RENDER_FALLOUTWEAPONANIMATION_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <osg/CopyOp>
#include <osg/ref_ptr>

#include <components/misc/strings/algorithm.hpp>
#include <components/sceneutil/controller.hpp>
#include <components/sceneutil/keyframe.hpp>

namespace MWRender
{
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
