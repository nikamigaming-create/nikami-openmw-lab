#include "image.hpp"

#include <cmath>

#include <MyGUI_RenderManager.h>

#include "resources.hpp"

namespace LuaUi
{
    void LuaTileRect::_setAlign(const MyGUI::IntSize& /*oldSize*/)
    {
        mCoord.set(0, 0, mCroppedParent->getWidth(), mCroppedParent->getHeight());
        mTileSize = mSetTileSize;

        // zero tilesize stands for not tiling
        if (mTileSize.width == 0)
            mTileSize.width = mCoord.width;
        if (mTileSize.height == 0)
            mTileSize.height = mCoord.height;

        // mCoord could be zero, prevent division by 0
        // use arbitrary large numbers to prevent performance issues
        if (mTileSize.width <= 0)
            mTileSize.width = 1e7;
        if (mTileSize.height <= 0)
            mTileSize.height = 1e7;

        MyGUI::TileRect::_updateView();
    }

    void LuaImage::initialize()
    {
        changeWidgetSkin("LuaImage");
        mTileRect = dynamic_cast<LuaTileRect*>(getSubWidgetMain());
        mRotatingSkin = nullptr;
        WidgetExtension::initialize();
    }

    void LuaImage::updateProperties()
    {
        const float rotation = propertyValue("rotation", 0.f);
        const bool shouldRotate = std::abs(rotation) > 1e-6f;
        const bool isRotating = mRotatingSkin != nullptr;
        if (shouldRotate != isRotating)
        {
            changeWidgetSkin(shouldRotate ? "LuaRotatingImage" : "LuaImage");
            mTileRect = dynamic_cast<LuaTileRect*>(getSubWidgetMain());
            mRotatingSkin = dynamic_cast<MyGUI::RotatingSkin*>(getSubWidgetMain());
        }

        deleteAllItems();
        TextureResource* resource = propertyValue<TextureResource*>("resource", nullptr);
        MyGUI::IntCoord atlasCoord;
        if (resource)
        {
            atlasCoord
                = MyGUI::IntCoord(static_cast<int>(resource->mOffset.x()), static_cast<int>(resource->mOffset.y()),
                    static_cast<int>(resource->mSize.x()), static_cast<int>(resource->mSize.y()));
            setImageTexture(resource->mPath);
        }

        bool tileH = propertyValue("tileH", false);
        bool tileV = propertyValue("tileV", false);

        MyGUI::ITexture* texture = MyGUI::RenderManager::getInstance().getTexture(_getTextureName());
        MyGUI::IntSize textureSize;
        if (texture != nullptr)
            textureSize = MyGUI::IntSize(texture->getWidth(), texture->getHeight());

        if (atlasCoord.width == 0)
            atlasCoord.width = textureSize.width;
        if (atlasCoord.height == 0)
            atlasCoord.height = textureSize.height;

        if (mTileRect != nullptr)
            mTileRect->updateSize(MyGUI::IntSize(tileH ? atlasCoord.width : 0, tileV ? atlasCoord.height : 0));
        setImageTile(atlasCoord.size());
        setImageCoord(atlasCoord);

        if (mRotatingSkin != nullptr)
        {
            mRotatingSkin->setCenter(propertyValue(
                "rotationCenter", MyGUI::IntPoint(getWidth() / 2, getHeight() / 2)));
            mRotatingSkin->setAngle(rotation);
        }

        setColour(propertyValue("color", MyGUI::Colour(1, 1, 1, 1)));

        WidgetExtension::updateProperties();
    }
}
