#ifndef OPENMW_LUAUI_RESOURCES
#define OPENMW_LUAUI_RESOURCES

#include <memory>
<<<<<<< HEAD
=======
#include <string>
>>>>>>> origin/main
#include <unordered_map>
#include <vector>

#include <osg/Vec2f>

<<<<<<< HEAD
#include <components/vfs/pathutil.hpp>

=======
>>>>>>> origin/main
namespace VFS
{
    class Manager;
}

namespace LuaUi
{
    struct TextureData
    {
<<<<<<< HEAD
        VFS::Path::Normalized mPath;
=======
        std::string mPath;
>>>>>>> origin/main
        osg::Vec2f mOffset;
        osg::Vec2f mSize;
    };

    // will have more/different data when automated atlasing is supported
    using TextureResource = TextureData;

    class ResourceManager
    {
    public:
<<<<<<< HEAD
        std::shared_ptr<TextureResource> registerTexture(TextureData data)
        {
            TextureResources& list = mTextures[data.mPath];
            list.push_back(std::make_shared<TextureResource>(std::move(data)));
            return list.back();
        }

        void clear() { mTextures.clear(); }

    private:
        using TextureResources = std::vector<std::shared_ptr<TextureResource>>;
        std::unordered_map<VFS::Path::Normalized, TextureResources, VFS::Path::Hash> mTextures;
=======
        std::shared_ptr<TextureResource> registerTexture(TextureData data);
        void clear();

    private:
        using TextureResources = std::vector<std::shared_ptr<TextureResource>>;
        std::unordered_map<std::string, TextureResources> mTextures;
>>>>>>> origin/main
    };
}

#endif // OPENMW_LUAUI_LAYERS
