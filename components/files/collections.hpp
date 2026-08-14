#ifndef COMPONENTS_FILES_COLLECTION_HPP
#define COMPONENTS_FILES_COLLECTION_HPP

#include <filesystem>

#include "multidircollection.hpp"

namespace Files
{
    class Collections
    {
    public:
        Collections();

        ///< Directories are listed with increasing priority.
        Collections(const Files::PathContainer& directories);

<<<<<<< HEAD
        ///< Return a file collection for the given extension.
        const MultiDirCollection& getCollection(std::string_view extension) const;

        std::filesystem::path getPath(std::string_view file) const;
=======
        ///< Return a file collection for the given extension. Extension must contain the
        /// leading dot and must be all lower-case.
        const MultiDirCollection& getCollection(const std::string& extension) const;

        std::filesystem::path getPath(const std::string& file) const;
>>>>>>> origin/main
        ///< Return full path (including filename) of \a file.
        ///
        /// If the file does not exist in any of the collection's
        /// directories, an exception is thrown. \a file must include the
        /// extension.

<<<<<<< HEAD
        bool doesExist(std::string_view file) const;
=======
        bool doesExist(const std::string& file) const;
>>>>>>> origin/main
        ///< \return Does a file with the given name exist?

        const Files::PathContainer& getPaths() const;

    private:
<<<<<<< HEAD
        Files::PathContainer mDirectories;

        mutable std::map<std::string, MultiDirCollection, Misc::StringUtils::CiComp> mCollections;
=======
        typedef std::map<std::string, MultiDirCollection> MultiDirCollectionContainer;
        Files::PathContainer mDirectories;

        mutable MultiDirCollectionContainer mCollections;
>>>>>>> origin/main
    };
}

#endif
