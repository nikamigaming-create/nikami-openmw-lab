#ifndef OPENMW_APPS_OPENMW_MWRENDER_ACTORUTIL_H
#define OPENMW_APPS_OPENMW_MWRENDER_ACTORUTIL_H

<<<<<<< HEAD
#include <components/vfs/pathutil.hpp>

#include <string>
=======
#include <string>
#include <string_view>
>>>>>>> origin/main

namespace MWRender
{
    const std::string& getActorSkeleton(bool firstPerson, bool female, bool beast, bool werewolf);
<<<<<<< HEAD
    bool isDefaultActorSkeleton(VFS::Path::NormalizedView model);
=======
    bool isDefaultActorSkeleton(std::string_view model);
>>>>>>> origin/main
    std::string addSuffixBeforeExtension(const std::string& filename, const std::string& suffix);
}

#endif
