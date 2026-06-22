#include "myguidatamanager.hpp"

#include <stdexcept>
#include <string>

#include <MyGUI_DataFileStream.h>

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

    void DataManager::setResourcePath(VFS::Path::NormalizedView path)
    {
        mResourcePath = path;
    }

    VFS::Path::NormalizedView DataManager::getResourcePath() const
    {
        return mResourcePath;
    }

    DataManager::DataManager(VFS::Path::NormalizedView resourcePath, const VFS::Manager* vfs)
        : mResourcePath(resourcePath)
        , mVfs(vfs)
    {
    }

    MyGUI::IDataStream* DataManager::getData(const std::string& name) OPENMW_MYGUI_DATA_MANAGER_CONST
    {
        VFS::Path::Normalized path(mResourcePath);
        path /= name;
        return new DataStream(mVfs->get(path));
    }

    void DataManager::freeData(MyGUI::IDataStream* data)
    {
        delete data;
    }

    bool DataManager::isDataExist(const std::string& name) OPENMW_MYGUI_DATA_MANAGER_CONST
    {
        VFS::Path::Normalized path(mResourcePath);
        path /= name;
        return mVfs->exists(path);
    }

    const MyGUI::VectorString& DataManager::getDataListNames(const std::string& /*pattern*/) OPENMW_MYGUI_DATA_MANAGER_CONST
    {
        throw std::runtime_error("DataManager::getDataListNames is not implemented - VFS is used");
    }

    OPENMW_MYGUI_DATA_PATH_RETURN DataManager::getDataPath(const std::string& name) OPENMW_MYGUI_DATA_MANAGER_CONST
    {
        VFS::Path::Normalized path(mResourcePath);
        path /= name;
        if (!mVfs->exists(path))
        {
#if MYGUI_VERSION < MYGUI_DEFINE_VERSION(3, 4, 3)
            mDataPath.clear();
            return mDataPath;
#else
            return {};
#endif
        }

#if MYGUI_VERSION < MYGUI_DEFINE_VERSION(3, 4, 3)
        mDataPath = path;
        return mDataPath;
#else
        return path;
#endif
    }

}
