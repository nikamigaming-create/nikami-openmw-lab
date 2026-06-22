#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUITEXTURE_H
#define OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUITEXTURE_H

#include <MyGUI_ITexture.h>

#include <components/widgets/myguicompat.hpp>

#include <osg/ref_ptr>

namespace osg
{
    class Image;
    class Texture2D;
    class StateSet;
}

namespace Resource
{
    class ImageManager;
}

namespace MyGUIPlatform
{

    class OSGTexture final : public MyGUI::ITexture
    {
        std::string mName;
        Resource::ImageManager* mImageManager;

        osg::ref_ptr<osg::Image> mLockedImage;
        osg::ref_ptr<osg::Texture2D> mTexture;
        osg::ref_ptr<osg::StateSet> mInjectState;
        MyGUI::PixelFormat mFormat;
        MyGUI::TextureUsage mUsage;
        size_t mNumElemBytes;

        int mWidth;
        int mHeight;

    public:
        OSGTexture(const std::string& name, Resource::ImageManager* imageManager);
        OSGTexture(osg::Texture2D* texture, osg::StateSet* injectState = nullptr);
        ~OSGTexture() override;

        osg::StateSet* getInjectState() { return mInjectState; }

        const std::string& getName() const override { return mName; }

        void createManual(int width, int height, MyGUI::TextureUsage usage, MyGUI::PixelFormat format) override;
        void loadFromFile(const std::string& fname) override;
        void saveToFile(const std::string& fname) override;

        void destroy() override;

        void* lock(MyGUI::TextureUsage access) override;
        void unlock() override;
        bool isLocked() OPENMW_MYGUI_TEXTURE_CONST override { return mLockedImage.valid(); }

        int getWidth() OPENMW_MYGUI_TEXTURE_CONST override { return mWidth; }
        int getHeight() OPENMW_MYGUI_TEXTURE_CONST override { return mHeight; }

        MyGUI::PixelFormat getFormat() OPENMW_MYGUI_TEXTURE_CONST override { return mFormat; }
        MyGUI::TextureUsage getUsage() OPENMW_MYGUI_TEXTURE_CONST override { return mUsage; }
        size_t getNumElemBytes() OPENMW_MYGUI_TEXTURE_CONST override { return mNumElemBytes; }

        MyGUI::IRenderTarget* getRenderTarget() override;

        void setShader(const std::string& shaderName) OPENMW_MYGUI_SET_SHADER_OVERRIDE;

        /*internal:*/
        osg::Texture2D* getTexture() const { return mTexture.get(); }
    };

}

#endif
