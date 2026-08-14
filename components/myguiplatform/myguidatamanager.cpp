#include "myguidatamanager.hpp"

#include <stdexcept>
#include <string>

#include <MyGUI_DataFileStream.h>

<<<<<<< HEAD
=======
#include <components/files/conversion.hpp>
>>>>>>> origin/main
#include <components/vfs/manager.hpp>

namespace
{
    class DataStream final : public MyGUI::DataStream
    {
    public:
        explicit DataStream(std::unique_ptr<std::istream>&& stream)
            : MyGUI::DataStream(stream.get())
            , mOwnedStream(std::move(stream))
        {
        }

    private:
        std::unique_ptr<std::istream> mOwnedStream;
    };
}

namespace MyGUIPlatform
{

<<<<<<< HEAD
    void DataManager::setResourcePath(VFS::Path::NormalizedView path)
=======
    void DataManager::setResourcePath(const std::filesystem::path& path)
>>>>>>> origin/main
    {
        mResourcePath = path;
    }

<<<<<<< HEAD
    VFS::Path::NormalizedView DataManager::getResourcePath() const
    {
        return mResourcePath;
    }

    DataManager::DataManager(VFS::Path::NormalizedView resourcePath, const VFS::Manager* vfs)
=======
    DataManager::DataManager(const std::string& resourcePath, const VFS::Manager* vfs)
>>>>>>> origin/main
        : mResourcePath(resourcePath)
        , mVfs(vfs)
    {
    }

    MyGUI::IDataStream* DataManager::getData(const std::string& name) const
    {
<<<<<<< HEAD
        VFS::Path::Normalized path(mResourcePath);
        path /= name;
        return new DataStream(mVfs->get(path));
=======
        return new DataStream(mVfs->get(Files::pathToUnicodeString(mResourcePath / name)));
>>>>>>> origin/main
    }

    void DataManager::freeData(MyGUI::IDataStream* data)
    {
        delete data;
    }

    bool DataManager::isDataExist(const std::string& name) const
    {
<<<<<<< HEAD
        VFS::Path::Normalized path(mResourcePath);
        path /= name;
        return mVfs->exists(path);
=======
        return mVfs->exists(Files::pathToUnicodeString(mResourcePath / name));
>>>>>>> origin/main
    }

    const MyGUI::VectorString& DataManager::getDataListNames(const std::string& /*pattern*/) const
    {
        throw std::runtime_error("DataManager::getDataListNames is not implemented - VFS is used");
    }

    std::string DataManager::getDataPath(const std::string& name) const
    {
<<<<<<< HEAD
        VFS::Path::Normalized path(mResourcePath);
        path /= name;
        if (!mVfs->exists(path))
            return {};

        return path;
=======
        if (name.empty())
        {
            return Files::pathToUnicodeString(mResourcePath);
        }

        if (!isDataExist(name))
            return {};

        return Files::pathToUnicodeString(mResourcePath / name);
>>>>>>> origin/main
    }

}
