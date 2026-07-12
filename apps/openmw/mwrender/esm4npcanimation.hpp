#ifndef GAME_RENDER_ESM4NPCANIMATION_H
#define GAME_RENDER_ESM4NPCANIMATION_H

#include "animation.hpp"

#include <osg/MatrixTransform>

#include <string>
#include <string_view>
#include <vector>

namespace ESM4
{
    struct Npc;
}

namespace MWRender
{
    class ESM4NpcAnimation : public Animation
    {
    public:
        ESM4NpcAnimation(
            const MWWorld::Ptr& ptr, osg::ref_ptr<osg::Group> parentNode, Resource::ResourceSystem* resourceSystem);
        osg::Vec3f runAnimation(float duration) override;
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

        osg::ref_ptr<osg::Node> insertPart(
            std::string_view model, const osg::Vec4f* tint = nullptr, std::string_view diffuseTexture = {},
            std::string_view preferredBone = {});
        osg::ref_ptr<osg::Node> insertAttachedPart(std::string_view model, std::string_view preferredBone);

        // Works for FO3/FONV/TES5
        unsigned int insertHeadParts(const ESM4::Npc& traits, const std::vector<ESM::FormId>& partIds,
            std::set<uint32_t>& usedHeadPartTypes, std::set<uint32_t>* attachedHeadPartTypes = nullptr);

        void updateParts();
        void updatePartsTES4(const ESM4::Npc& traits);
        void updatePartsFONV(const ESM4::Npc& traits);
        void updatePartsTES5(const ESM4::Npc& traits);
        void applyPostManualFalloutActorPose() override;
    };
}

#endif // GAME_RENDER_ESM4NPCANIMATION_H
