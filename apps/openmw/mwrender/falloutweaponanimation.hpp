#ifndef GAME_RENDER_FALLOUTWEAPONANIMATION_H
#define GAME_RENDER_FALLOUTWEAPONANIMATION_H

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <osg/CopyOp>
#include <osg/ref_ptr>

#include <components/misc/strings/algorithm.hpp>
#include <components/sceneutil/controller.hpp>
#include <components/sceneutil/keyframe.hpp>

namespace MWRender
{
    enum class FonvWeaponAction : std::uint8_t
    {
        PrimaryAttack,
        Equip,
        Reload,
        Jam,
        Unequip,
    };

    enum class FonvWeaponActionProgress : std::uint8_t
    {
        Running,
        Completed,
        Interrupted,
    };

    struct FonvWeaponActionSource
    {
        FonvWeaponAction mAction;
        std::string mPath;
        std::string_view mSemanticGroup;
        bool mRequired = true;
    };

    inline constexpr std::uint32_t FonvPowerArmorGeneralFlag = 0x0020;

    struct FonvRetailHolsterContract
    {
        std::uint8_t mAnimationType = 0xff;
        std::uint32_t mSourceForm = 0;
        std::uint32_t mEvaluatedSlot = 0;
        std::uint32_t mEvaluatedState = 0;
        std::string_view mFrameName;
        std::string_view mParentName;
        std::array<std::uint32_t, 9> mRotationBits{};
        std::array<std::uint32_t, 3> mTranslationBits{};
        std::uint32_t mScaleBits = 0;
        std::string_view mCaptureSequence;
    };

    // These are the unmodified IEEE-754 words emitted by the xNVSE retail oracle. A weapon receives a contract only
    // through its authored TESObjectWEAP::EWeaponType byte; uncaptured families fail closed instead of borrowing a
    // nearby-looking transform.
    inline constexpr std::array<FonvRetailHolsterContract, 3> FonvRetailHolsterContracts{ {
        { 3, 0x000e3778, 5, 0, "Weapon", "Bip01 Pelvis",
            { 3209060608, 3206663839, 3137485292, 3206659353, 1061569791, 1023527333, 3164042662,
                1020489499, 3212828405 },
            { 1071143469, 3223221091, 3245862991 }, 1065353218,
            "ringo-sidearm-idle-smoke" },
        { 5, 0x0007ea24, 5, 0, "Weapon", "Bip01 Spine2",
            { 3210826934, 3203525720, 1026989424, 3189668151, 1045015346, 3212302068, 1055242061,
                3210471966, 3195822950 },
            { 1100293858, 3239811472, 3235687431 }, 1065353217,
            "sunny-smiles-idle-smoke" },
        { 6, 0x000e9c3b, 5, 0, "Weapon", "Bip01 Spine2",
            { 1065318487, 1027137190, 3174802633, 3175280776, 1025547984, 3212804936, 3174115547,
                1065323204, 1026102809 },
            { 1088343032, 1074830907, 1068977381 }, 1065353218,
            "trooper-automatic-holster-capture-20260717" },
    } };

    inline const FonvRetailHolsterContract* getFonvRetailHolsterContract(std::uint8_t animationType)
    {
        const auto found = std::find_if(FonvRetailHolsterContracts.begin(), FonvRetailHolsterContracts.end(),
            [animationType](const FonvRetailHolsterContract& contract) {
                return contract.mAnimationType == animationType;
            });
        return found == FonvRetailHolsterContracts.end() ? nullptr : &*found;
    }

    template <std::size_t Size>
    constexpr std::array<float, Size> decodeFonvRetailFloatBits(
        const std::array<std::uint32_t, Size>& bits)
    {
        std::array<float, Size> result{};
        for (std::size_t index = 0; index < Size; ++index)
            result[index] = std::bit_cast<float>(bits[index]);
        return result;
    }

    inline bool hasFonvPowerArmorGeneralFlag(std::uint32_t generalFlags)
    {
        return (generalFlags & FonvPowerArmorGeneralFlag) != 0;
    }

    inline constexpr bool shouldUseFonvWeaponAnimationFamily(bool weaponPresent, bool weaponDrawn)
    {
        return weaponPresent && weaponDrawn;
    }

    enum class FonvAnimationFamilySelection : std::uint8_t
    {
        Missing,
        Generic,
        PowerArmor,
        GenericFallback,
    };

    struct FonvAnimationFamilyResolution
    {
        std::string mPath;
        FonvAnimationFamilySelection mSelection = FonvAnimationFamilySelection::Missing;
    };

    class FonvAnimationSemanticSnapshot
    {
    public:
        template <class HasAnimation>
        FonvAnimationSemanticSnapshot(
            std::initializer_list<std::string_view> semantics, HasAnimation&& hasAnimation)
        {
            for (const std::string_view semantic : semantics)
            {
                if (hasAnimation(semantic))
                    mPresent.emplace_back(semantic);
            }
        }

        bool wasPresent(std::string_view semantic) const
        {
            return std::any_of(mPresent.begin(), mPresent.end(), [semantic](const std::string& present) {
                return present == semantic;
            });
        }

    private:
        std::vector<std::string> mPresent;
    };

    inline std::string getFonvPowerArmorAnimationKf(std::string_view genericPath)
    {
        if (genericPath.empty())
            return {};

        const std::size_t slash = genericPath.find_last_of("/\\");
        const std::size_t filename = slash == std::string_view::npos ? 0 : slash + 1;
        if (Misc::StringUtils::ciStartsWith(genericPath.substr(filename), "pa"))
            return std::string(genericPath);

        std::string result(genericPath);
        result.insert(filename, "pa");
        return result;
    }

    inline std::string_view getFonvAnimationFamilySelectionName(FonvAnimationFamilySelection selection)
    {
        switch (selection)
        {
            case FonvAnimationFamilySelection::Generic:
                return "generic";
            case FonvAnimationFamilySelection::PowerArmor:
                return "power-armor";
            case FonvAnimationFamilySelection::GenericFallback:
                return "generic-fallback";
            case FonvAnimationFamilySelection::Missing:
                return "missing";
        }
        return "missing";
    }

    template <class Exists>
    FonvAnimationFamilyResolution resolveFonvAnimationFamily(
        const std::vector<std::string>& genericCandidates, bool powerArmor, Exists&& exists)
    {
        if (powerArmor)
        {
            // Retail resolves the family before it resolves candidate precedence. Scan every PA sibling first so a
            // generic weapon-direction source cannot beat a later PA-neutral source (for example 2hrforward versus
            // pamtforward).
            for (const std::string& generic : genericCandidates)
            {
                const std::string powerArmorPath = getFonvPowerArmorAnimationKf(generic);
                if (!powerArmorPath.empty() && exists(powerArmorPath))
                {
                    return { powerArmorPath, FonvAnimationFamilySelection::PowerArmor };
                }
            }
        }

        for (const std::string& generic : genericCandidates)
        {
            if (!generic.empty() && exists(generic))
            {
                return { generic, powerArmor ? FonvAnimationFamilySelection::GenericFallback
                                             : FonvAnimationFamilySelection::Generic };
            }
        }
        return {};
    }

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

    inline std::optional<FonvWeaponActionSource> getFonvWeaponActionSource(
        std::uint8_t animationType, std::uint8_t reloadAnimation, FonvWeaponAction action)
    {
        const std::string_view prefix = getFonvWeaponAnimationPrefix(animationType);
        if (prefix.empty())
            return std::nullopt;

        if (action == FonvWeaponAction::Equip)
            return FonvWeaponActionSource{ action, getFonvWeaponAnimationKf(animationType, "equip"), "equip", true };
        if (action == FonvWeaponAction::Unequip)
        {
            return FonvWeaponActionSource{
                action, getFonvWeaponAnimationKf(animationType, "unequip"), "unequip", true };
        }

        if (action == FonvWeaponAction::Reload || action == FonvWeaponAction::Jam)
        {
            if (animationType < 3 || animationType > 9)
                return std::nullopt;
            const std::optional<char> letter = getFonvWeaponReloadAnimationLetter(reloadAnimation);
            if (!letter)
                return std::nullopt;
            const bool jam = action == FonvWeaponAction::Jam;
            const std::string suffix = std::string(jam ? "jam" : "reload") + *letter;
            return FonvWeaponActionSource{ action, getFonvWeaponAnimationKf(animationType, suffix),
                jam ? std::string_view("jam") : std::string_view("reload"), !jam };
        }

        std::string_view suffix;
        std::string_view group;
        switch (animationType)
        {
            case 0: // HandToHand
            case 1: // OneHandMelee
            case 2: // TwoHandMelee
                suffix = "attackright_a";
                group = "attack2";
                break;
            case 3: // OneHandPistol
            case 4: // OneHandPistolEnergy
            case 5: // TwoHandRifle
            case 7: // TwoHandRifleEnergy
            case 9: // TwoHandLauncher
                suffix = "attack3";
                group = "attack3";
                break;
            case 6: // TwoHandAutomatic
            case 8: // TwoHandHandle
                suffix = "attackloop";
                group = "attack1";
                break;
            case 10: // OneHandGrenade
            case 13: // OneHandThrown
                suffix = "attackthrow";
                group = "attack1";
                break;
            case 11: // OneHandMine
            case 12: // OneHandLandMine
                suffix = "placemine";
                group = "attack1";
                break;
            default:
                return std::nullopt;
        }
        return FonvWeaponActionSource{ action, getFonvWeaponAnimationKf(animationType, suffix), group, true };
    }

    inline std::vector<FonvWeaponActionSource> getFonvWeaponActionManifest(
        std::uint8_t animationType, std::uint8_t reloadAnimation)
    {
        static constexpr std::array<FonvWeaponAction, 5> actions{ FonvWeaponAction::PrimaryAttack,
            FonvWeaponAction::Equip, FonvWeaponAction::Reload, FonvWeaponAction::Jam,
            FonvWeaponAction::Unequip };

        std::vector<FonvWeaponActionSource> result;
        result.reserve(actions.size());
        for (FonvWeaponAction action : actions)
        {
            if (std::optional<FonvWeaponActionSource> source
                = getFonvWeaponActionSource(animationType, reloadAnimation, action))
            {
                result.push_back(std::move(*source));
            }
        }
        return result;
    }

    inline bool matchesFonvWeaponActionSource(const FonvWeaponActionSource& expected,
        std::string_view selectedGroup, std::string_view selectedPath, std::string_view resolvedPath)
    {
        return selectedGroup == expected.mSemanticGroup && !resolvedPath.empty() && selectedPath == resolvedPath;
    }

    inline FonvWeaponActionProgress getFonvWeaponActionProgress(bool stateExists, float completion)
    {
        if (!stateExists)
            return FonvWeaponActionProgress::Interrupted;
        return completion < 1.f ? FonvWeaponActionProgress::Running : FonvWeaponActionProgress::Completed;
    }

    inline bool canAdvanceFonvWeaponState(bool knockedOut, bool knockedDown, bool recovering)
    {
        return !knockedOut && !knockedDown && !recovering;
    }

    inline bool shouldSynthesizeFonvSemanticAlias(bool falloutActorContext, std::string_view semanticGroup)
    {
        return falloutActorContext || !semanticGroup.empty();
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
