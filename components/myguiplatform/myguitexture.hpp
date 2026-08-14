#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUITEXTURE_H
#define OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUITEXTURE_H

#include <MyGUI_ITexture.h>

#include <osg/ref_ptr>

<<<<<<< HEAD
namespace osg
{
    class Image;
    class Texture2D;
=======
#include <string_view>

namespace osg
{
    class Image;
    class Texture;
>>>>>>> origin/main
    class StateSet;
}

namespace Resource
{
    class ImageManager;
}

namespace MyGUIPlatform
{

<<<<<<< HEAD
=======
    /// Create a procedural image for an absent MyGUI resource. Callers must first verify that the requested VFS path
    /// does not exist; existing or malformed assets must remain on the regular image loader path.
    osg::ref_ptr<osg::Image> createMissingTextureFallback(std::string_view name);

>>>>>>> origin/main
    class OSGTexture final : public MyGUI::ITexture
    {
        std::string mName;
        Resource::ImageManager* mImageManager;
<<<<<<< HEAD

        osg::ref_ptr<osg::Image> mLockedImage;
        osg::ref_ptr<osg::Texture2D> mTexture;
=======
        bool mUseMissingTextureFallback;

        osg::ref_ptr<osg::Image> mLockedImage;
//## VR_PATCH BEGIN
// Texture2D -> Texture for multiview compatibility
// VR-TODO: Again, why am i not using texture views for this? Upstream is and I merged multiview upstream!
        osg::ref_ptr<osg::Texture> mTexture;
//## VR_PATCH END
>>>>>>> origin/main
        osg::ref_ptr<osg::StateSet> mInjectState;
        MyGUI::PixelFormat mFormat;
        MyGUI::TextureUsage mUsage;
        size_t mNumElemBytes;

        int mWidth;
        int mHeight;

    public:
<<<<<<< HEAD
        OSGTexture(const std::string& name, Resource::ImageManager* imageManager);
        OSGTexture(osg::Texture2D* texture, osg::StateSet* injectState = nullptr);
=======
        OSGTexture(
            const std::string& name, Resource::ImageManager* imageManager, bool useMissingTextureFallback = false);
//## VR_PATCH BEGIN
// Texture2D -> Texture
        OSGTexture(osg::Texture* texture, osg::StateSet* injectState = nullptr);
//## VR_PATCH END
>>>>>>> origin/main
        ~OSGTexture() override;

        osg::StateSet* getInjectState() { return mInjectState; }

        const std::string& getName() const override { return mName; }

        void createManual(int width, int height, MyGUI::TextureUsage usage, MyGUI::PixelFormat format) override;
        void loadFromFile(const std::string& fname) override;
        void saveToFile(const std::string& fname) override;

        void destroy() override;

        void* lock(MyGUI::TextureUsage access) override;
        void unlock() override;
        bool isLocked() const override { return mLockedImage.valid(); }

        int getWidth() const override { return mWidth; }
        int getHeight() const override { return mHeight; }

        MyGUI::PixelFormat getFormat() const override { return mFormat; }
        MyGUI::TextureUsage getUsage() const override { return mUsage; }
        size_t getNumElemBytes() const override { return mNumElemBytes; }

        MyGUI::IRenderTarget* getRenderTarget() override;

        void setShader(const std::string& shaderName) override;

        /*internal:*/
<<<<<<< HEAD
        osg::Texture2D* getTexture() const { return mTexture.get(); }
=======
//## VR_PATCH BEGIN
        osg::Texture* getTexture() const { return mTexture.get(); }
//## VR_PATCH END
>>>>>>> origin/main
    };

}

#endif
