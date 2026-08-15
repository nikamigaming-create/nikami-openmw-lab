#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUIRENDERMANAGER_H
#define OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUIRENDERMANAGER_H

#include <MyGUI_RenderManager.h>

#include <osg/ref_ptr>

#include <set>

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
    class Program;
}

namespace MyGUIPlatform
{

    class Drawable;
    class OSGTexture;
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

        std::map<std::string, OSGTexture> mTextures;

        bool mIsInitialise;
        bool mUseMissingTextureFallback;

        float mInvScalingFactor;
        // Layers rendered to an in-world device are deliberately omitted from
        // the normal full-screen GUI camera while their filtered RTT camera
        // continues to draw them.
        std::set<std::string> mSuppressedGuiLayers;
        // A physical in-world device owns the complete GUI frame while it is
        // raised.  Its filtered RTT camera still renders the selected layer,
        // but the unfiltered, fullscreen GUI camera must draw nothing.
        bool mSuppressUnfilteredGui = false;

    public:
//## VR_PATCH END
        RenderManager(osgViewer::Viewer* viewer, osg::Group* sceneroot, Resource::ImageManager* imageManager,
            float scalingFactor);
        virtual ~RenderManager();

        void initialise();
        void shutdown();

        void enableShaders(Shader::ShaderManager& shaderManager);

        /// Replace absent MyGUI-only images with generated, readable placeholders.
        /// Existing images, including malformed ones, remain on the normal loader path.
        void setUseMissingTextureFallback(bool enabled) { mUseMissingTextureFallback = enabled; }
        bool useMissingTextureFallback() const { return mUseMissingTextureFallback; }

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

        void setSuppressedGuiLayers(std::set<std::string> layers) { mSuppressedGuiLayers = std::move(layers); }
        void setSuppressUnfilteredGui(bool suppressed) { mSuppressUnfilteredGui = suppressed; }
        bool suppressUnfilteredGui() const { return mSuppressUnfilteredGui; }
        bool isGuiLayerSuppressed(const std::string& layer) const
        {
            return mSuppressedGuiLayers.find(layer) != mSuppressedGuiLayers.end();
        }

//## VR_PATCH BEGIN

        void setViewSize(int width, int height) override;

        void registerShader(const std::string& shaderName, const std::string& vertexProgramFile,
            const std::string& fragmentProgramFile) override;

        osg::ref_ptr<osg::Camera> createGUICamera(int order, std::string layerFilter);
        /// Crop a filtered camera to a widget's live screen-space rectangle
        /// while retaining the physical framebuffer viewport. This lets an
        /// in-world display consume a normal MyGUI window without resizing or
        /// mutating that window's persisted desktop layout.
        void setGUICameraContentRect(osg::Camera* camera, const MyGUI::IntCoord& rect);
        /// Set the physical output resolution of a filtered GUI camera while
        /// retaining its logical MyGUI content rectangle.  In-world displays
        /// use this to downscale an authored desktop canvas into a texture.
        void setGUICameraRenderTargetSize(osg::Camera* camera, int width, int height);
//## VR_PATCH END
    };

}

#endif
