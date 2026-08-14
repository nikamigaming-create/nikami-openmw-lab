#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUIRENDERMANAGER_H
#define OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUIRENDERMANAGER_H

#include <MyGUI_RenderManager.h>

#include <osg/ref_ptr>

<<<<<<< HEAD
=======
#include <set>

>>>>>>> origin/main
namespace Resource
{
    class ImageManager;
}

namespace Shader
{
    class ShaderManager;
}

namespace osgViewer
{
    class Viewer;
}

namespace osg
{
    class Group;
    class Camera;
    class RenderInfo;
    class StateSet;
<<<<<<< HEAD
=======
    class Program;
>>>>>>> origin/main
}

namespace MyGUIPlatform
{

    class Drawable;
    class OSGTexture;
<<<<<<< HEAD

    class RenderManager : public MyGUI::RenderManager, public MyGUI::IRenderTarget
    {
        osg::ref_ptr<osgViewer::Viewer> mViewer;
        osg::ref_ptr<osg::Group> mSceneRoot;
        osg::ref_ptr<Drawable> mDrawable;
        Resource::ImageManager* mImageManager;

        MyGUI::IntSize mViewSize;
        bool mUpdate;
        MyGUI::VertexColourType mVertexFormat;
        MyGUI::RenderTargetInfo mInfo;
=======
//## VR_PATCH BEGIN
// Make a separate class inherit IRenderTarget and handle the inject state
    class GUICamera;

    class StateInjectableRenderTarget : public MyGUI::IRenderTarget
    {
    public:
        StateInjectableRenderTarget() = default;
        ~StateInjectableRenderTarget() = default;

        /** specify a StateSet to inject for rendering. The StateSet will be used by future doRender calls until you
         * reset it to nullptr again. */
        void setInjectState(osg::StateSet* stateSet);

    protected:
        osg::StateSet* mInjectState{ nullptr };
    };

    class RenderManager : public MyGUI::RenderManager
    {
        osg::ref_ptr<osgViewer::Viewer> mViewer;
        osg::ref_ptr<osg::StateSet> mGuiStateSet;
        osg::ref_ptr<osg::Group> mSceneRoot;
        Resource::ImageManager* mImageManager;
        MyGUI::IntSize mViewSize;

        MyGUI::VertexColourType mVertexFormat;
>>>>>>> origin/main

        std::map<std::string, OSGTexture> mTextures;

        bool mIsInitialise;
<<<<<<< HEAD

        osg::ref_ptr<osg::Camera> mGuiRoot;

        float mInvScalingFactor;

        osg::StateSet* mInjectState;

    public:
=======
        bool mUseMissingTextureFallback;

        float mInvScalingFactor;

    public:
//## VR_PATCH END
>>>>>>> origin/main
        RenderManager(osgViewer::Viewer* viewer, osg::Group* sceneroot, Resource::ImageManager* imageManager,
            float scalingFactor);
        virtual ~RenderManager();

        void initialise();
        void shutdown();

        void enableShaders(Shader::ShaderManager& shaderManager);

<<<<<<< HEAD
=======
        /// Replace absent MyGUI-only images with generated, readable placeholders.
        /// Existing images, including malformed ones, remain on the normal loader path.
        void setUseMissingTextureFallback(bool enabled) { mUseMissingTextureFallback = enabled; }
        bool useMissingTextureFallback() const { return mUseMissingTextureFallback; }

>>>>>>> origin/main
        static RenderManager& getInstance() { return *getInstancePtr(); }
        static RenderManager* getInstancePtr()
        {
            return static_cast<RenderManager*>(MyGUI::RenderManager::getInstancePtr());
        }

        bool checkTexture(MyGUI::ITexture* texture) override;

        /** @see RenderManager::getViewSize */
        const MyGUI::IntSize& getViewSize() const override { return mViewSize; }

        /** @see RenderManager::getVertexFormat */
        MyGUI::VertexColourType getVertexFormat() const override { return mVertexFormat; }

        /** @see RenderManager::isFormatSupported */
        bool isFormatSupported(MyGUI::PixelFormat format, MyGUI::TextureUsage usage) override;

        /** @see RenderManager::createVertexBuffer */
        MyGUI::IVertexBuffer* createVertexBuffer() override;
        /** @see RenderManager::destroyVertexBuffer */
        void destroyVertexBuffer(MyGUI::IVertexBuffer* buffer) override;

        /** @see RenderManager::createTexture */
        MyGUI::ITexture* createTexture(const std::string& name) override;
        /** @see RenderManager::destroyTexture */
        void destroyTexture(MyGUI::ITexture* texture) override;
        /** @see RenderManager::getTexture */
        MyGUI::ITexture* getTexture(const std::string& name) override;

        // Called by the update traversal
        void update();

<<<<<<< HEAD
        // Called by the cull traversal
        /** @see IRenderTarget::begin */
        void begin() override;
        /** @see IRenderTarget::end */
        void end() override;
        /** @see IRenderTarget::doRender */
        void doRender(MyGUI::IVertexBuffer* buffer, MyGUI::ITexture* texture, size_t count) override;

        /** specify a StateSet to inject for rendering. The StateSet will be used by future doRender calls until you
         * reset it to nullptr again. */
        void setInjectState(osg::StateSet* stateSet);

        /** @see IRenderTarget::getInfo */
        const MyGUI::RenderTargetInfo& getInfo() const override { return mInfo; }
=======
//## VR_PATCH BEGIN
>>>>>>> origin/main

        void setViewSize(int width, int height) override;

        void registerShader(const std::string& shaderName, const std::string& vertexProgramFile,
            const std::string& fragmentProgramFile) override;

<<<<<<< HEAD
        /*internal:*/

        void collectDrawCalls();
=======
        osg::ref_ptr<osg::Camera> createGUICamera(int order, std::string layerFilter);
//## VR_PATCH END
>>>>>>> origin/main
    };

}

#endif
