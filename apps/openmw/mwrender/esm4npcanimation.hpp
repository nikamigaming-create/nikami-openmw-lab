#ifndef GAME_RENDER_ESM4NPCANIMATION_H
#define GAME_RENDER_ESM4NPCANIMATION_H

#include "animation.hpp"

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

    private:
        struct LiveHeadSurfaceAuthoringTarget
        {
            osg::ref_ptr<osg::MatrixTransform> node;
            std::string prefix;
            std::string model;
            osg::Vec3f defaultOffset;
            osg::Vec3f defaultRotationDegrees;
            osg::Vec3f pivot;
            bool defaultPivotMode = false;
        };

        std::vector<LiveHeadSurfaceAuthoringTarget> mLiveHeadSurfaceAuthoringTargets;
        std::string mLiveHeadSurfaceAuthoringContent;
        unsigned int mLiveHeadSurfaceAuthoringTick = 0;
        std::string mLiveRuntimeActorKitFingerprint;
        unsigned int mLiveRuntimeActorKitTick = 0;
        unsigned int mLiveRuntimeActorKitGeneration = 0;

        void applyLiveHeadSurfaceAuthoring();
        void applyLiveRuntimeActorKitSelectors();
        bool rebuildLiveRuntimeActorKitParts(const ESM4::Npc& traits, unsigned int generation, std::string_view fingerprint);

        osg::ref_ptr<osg::Node> insertPart(
            std::string_view model, const osg::Vec4f* tint = nullptr, std::string_view diffuseTexture = {});
        osg::ref_ptr<osg::Node> insertAttachedPart(std::string_view model, std::string_view preferredBone);

        // Works for FO3/FONV/TES5
        unsigned int insertHeadParts(const ESM4::Npc& traits, const std::vector<ESM::FormId>& partIds,
            std::set<uint32_t>& usedHeadPartTypes, uint32_t coveredBodySlots = 0,
            unsigned int* visibleHairGeometry = nullptr);

        void updateParts();
        void updatePartsTES4(const ESM4::Npc& traits);
        void updatePartsFONV(const ESM4::Npc& traits);
        void updatePartsTES5(const ESM4::Npc& traits);

        osg::Vec3f runAnimation(float duration) override;
    };
}

#endif // GAME_RENDER_ESM4NPCANIMATION_H
