#ifndef MWRENDER_CHARACTERPREVIEW_H
#define MWRENDER_CHARACTERPREVIEW_H

#include <memory>
<<<<<<< HEAD
#include <osg/ref_ptr>

#include <osg/PositionAttitudeTransform>

#include <components/esm3/loadnpc.hpp>

#include <components/resource/resourcesystem.hpp>
=======
#include <string>
#include <osg/ref_ptr>

#include <osg/PositionAttitudeTransform>
>>>>>>> origin/main

#include <components/esm3/loadnpc.hpp>
#include <components/esm4/loadnpc.hpp>

#include <components/resource/resourcesystem.hpp>

#include "../mwworld/livecellref.hpp"
#include "../mwworld/ptr.hpp"

namespace osg
{
    class Texture2D;
    class Camera;
    class Group;
    class Viewport;
    class StateSet;
}

namespace MWRender
{

    class NpcAnimation;
<<<<<<< HEAD
=======
    class Animation;
>>>>>>> origin/main
    class DrawOnceCallback;
    class CharacterPreviewRTTNode;

    class CharacterPreview
    {
    public:
        CharacterPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character,
            int sizeX, int sizeY, const osg::Vec3f& position, const osg::Vec3f& lookAt);
        virtual ~CharacterPreview();

        int getTextureWidth() const;
        int getTextureHeight() const;

        void redraw();

        void rebuild();
<<<<<<< HEAD
=======
        void updateLive(double simulationTime = 0.0);
>>>>>>> origin/main

        osg::ref_ptr<osg::Texture2D> getTexture();
        /// Get the osg::StateSet required to render the texture correctly, if any.
        osg::StateSet* getTextureStateSet() { return mTextureStateSet; }

    private:
        CharacterPreview(const CharacterPreview&);
        CharacterPreview& operator=(const CharacterPreview&);

    protected:
        virtual bool renderHeadOnly() { return false; }
        void setBlendMode();
<<<<<<< HEAD
        virtual void onSetup();
=======
        void setRedrawSimulationTime(double simulationTime);
        virtual void onSetup();
        virtual osg::ref_ptr<Animation> createAnimation();
>>>>>>> origin/main

        osg::ref_ptr<osg::Group> mParent;
        Resource::ResourceSystem* mResourceSystem;
        osg::ref_ptr<osg::StateSet> mTextureStateSet;
        osg::ref_ptr<DrawOnceCallback> mDrawOnceCallback;
        osg::ref_ptr<CharacterPreviewRTTNode> mRTTNode;

        osg::Vec3f mPosition;
        osg::Vec3f mLookAt;

        MWWorld::Ptr mCharacter;

<<<<<<< HEAD
        osg::ref_ptr<MWRender::NpcAnimation> mAnimation;
=======
        osg::ref_ptr<MWRender::Animation> mAnimation;
>>>>>>> origin/main
        osg::ref_ptr<osg::PositionAttitudeTransform> mNode;
        std::string mCurrentAnimGroup;

        int mSizeX;
        int mSizeY;
    };

    class InventoryPreview : public CharacterPreview
    {
    public:
<<<<<<< HEAD
        InventoryPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character);

=======
        enum class ViewMode
        {
            Front,
            Profile,
            Top,
        };

        InventoryPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character,
            ViewMode viewMode = ViewMode::Front);

>>>>>>> origin/main
        void updatePtr(const MWWorld::Ptr& ptr);

        void update(); // Render preview again, e.g. after changed equipment
        void setViewport(int sizeX, int sizeY);

        int getSlotSelected(int posX, int posY);

    protected:
        osg::ref_ptr<osg::Viewport> mViewport;
<<<<<<< HEAD

=======
        std::unique_ptr<MWWorld::LiveCellRef<ESM4::Npc>> mFalloutPreviewRef;
        ViewMode mViewMode;

        osg::ref_ptr<Animation> createAnimation() override;
>>>>>>> origin/main
        void onSetup() override;
    };

    class UpdateCameraCallback;

    class RaceSelectionPreview : public CharacterPreview
    {
        ESM::NPC mBase;
        MWWorld::LiveCellRef<ESM::NPC> mRef;

    protected:
        bool renderHeadOnly() override { return true; }
        void onSetup() override;

    public:
        RaceSelectionPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem);
        virtual ~RaceSelectionPreview();

        void setAngle(float angleRadians);

        const ESM::NPC& getPrototype() const { return mBase; }

        void setPrototype(const ESM::NPC& proto);

    private:
        osg::ref_ptr<UpdateCameraCallback> mUpdateCameraCallback;

        float mPitchRadians;
<<<<<<< HEAD
=======
    };

    class FalloutActorPreview : public CharacterPreview
    {
    public:
        enum class ViewMode
        {
            Front,
            FrontLeft,
            FrontRight,
            Left,
            Right,
            Top,
            Back,
            IsoNW,
            IsoSW,
        };

        FalloutActorPreview(osg::Group* parent, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character,
            ViewMode viewMode, float cameraDistanceMultiplier = 1.f, std::string profileOverride = {},
            osg::Vec3f editorRotation = osg::Vec3f(), float editorScale = 1.f);

    protected:
        osg::ref_ptr<Animation> createAnimation() override;
        void onSetup() override;

    private:
        ViewMode mViewMode;
        float mCameraDistanceMultiplier;
        std::string mProfileOverride;
        osg::Vec3f mEditorRotation;
        float mEditorScale;
>>>>>>> origin/main
    };

}

#endif
