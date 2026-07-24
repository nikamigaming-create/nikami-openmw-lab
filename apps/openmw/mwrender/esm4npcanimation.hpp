#ifndef GAME_RENDER_ESM4NPCANIMATION_H
#define GAME_RENDER_ESM4NPCANIMATION_H

#include "animation.hpp"

#include <components/nifosg/matrixtransform.hpp>

#include <osg/MatrixTransform>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace ESM4
{
    struct Npc;
    struct Race;
    struct Weapon;
}

namespace MWRender
{
    class ESM4NpcAnimation : public Animation
    {
    public:
        struct WeaponAttachmentState
        {
            bool mApplied = false;
            bool mAttached = false;
            bool mVisible = false;
            std::string mFrameName;
            std::string mParentName;
            std::array<float, 9> mRotation{};
            std::array<float, 3> mTranslation{};
            float mScale = 0.f;
        };

        ESM4NpcAnimation(
            const MWWorld::Ptr& ptr, osg::ref_ptr<osg::Group> parentNode, Resource::ResourceSystem* resourceSystem);
        osg::Vec3f runAnimation(float duration) override;
        bool getWeaponsShown() const override { return mFalloutWeaponsShown; }
        void showWeapons(bool showWeapon) override;
        bool prepareFalloutWeaponAnimation(
            std::uint8_t animationType, std::uint8_t reloadAnimation, FonvWeaponAction action) override;
        bool setFalloutAnimatedObject(std::string_view model, std::string_view activeGroup) override;
        bool setWeaponHolsterAttachment(std::string_view frameName, std::string_view parentName,
            const std::array<float, 9>& rotation, const std::array<float, 3>& translation, float scale);
        WeaponAttachmentState getWeaponHolsterAttachmentState() const;
        bool supportsProceduralHumanoidLocomotion() const;
        bool applyProceduralHumanoidLocomotion(std::string_view group, float elapsed);

    private:
        struct ProceduralPoseBone
        {
            osg::ref_ptr<osg::MatrixTransform> mNode;
            osg::Matrix mRootRelative;
        };

        std::vector<ProceduralPoseBone> mFo4ProceduralPoseBones;
        bool mFo4ProceduralPoseInitialized = false;
        std::string mFo4ProceduralGroup;
        bool mFo4ProceduralAdvancedLogged = false;
        float mSkyrimAuthoredAnimationElapsed = 0.f;
        bool mSkyrimAuthoredAnimationLogged = false;

        osg::ref_ptr<osg::Node> mFalloutWeaponPart;
        osg::ref_ptr<NifOsg::MatrixTransform> mFalloutWeaponHolsterFrame;
        std::string mFalloutWeaponDrawBone = "Weapon";
        std::string mFalloutWeaponHolsterBone;
        bool mFalloutWeaponsShown = false;
        const ESM4::Weapon* mFalloutActionWeapon = nullptr;
        osg::ref_ptr<osg::Node> mFalloutAnimatedObjectPart;
        std::string mFalloutAnimatedObjectModel;
        std::string mFalloutAnimatedObjectGroup;

        osg::ref_ptr<osg::Node> insertPart(
            std::string_view model, const osg::Vec4f* tint = nullptr, std::string_view diffuseTexture = {},
            std::string_view preferredBone = {}, const float* colorRemappingIndex = nullptr,
            bool applyTes4RigidHeadBasis = true);
        osg::ref_ptr<osg::Node> insertAttachedPart(std::string_view model, std::string_view preferredBone,
            std::string* authoredParent = nullptr);

        // Works for FO3/FONV/TES5
        unsigned int insertHeadParts(const ESM4::Npc& traits, const std::vector<ESM::FormId>& partIds,
            std::set<uint32_t>& usedHeadPartTypes, std::set<uint32_t>* attachedHeadPartTypes = nullptr,
            unsigned int* attachedRequestedPartCount = nullptr, const ESM4::Race* faceGenRace = nullptr,
            bool faceGenFemale = false);

        void updateParts();
        bool applyRetailWeaponHolsterContract(const ESM4::Weapon& weapon);
        bool refreshFalloutWeaponPart();
        void updatePartsTES4(const ESM4::Npc& traits);
        void updatePartsFONV(const ESM4::Npc& traits);
        void updatePartsTES5(const ESM4::Npc& traits);
        void applyPostManualFalloutActorPose() override;
    };
}

#endif // GAME_RENDER_ESM4NPCANIMATION_H
