#ifndef OPENMW_MWCLASS_FNVAI_PACKAGE_H
#define OPENMW_MWCLASS_FNVAI_PACKAGE_H

namespace MWWorld
{
    class Ptr;
}

namespace MWClass
{
    // Re-evaluate the authored Fallout package list for a live NPC. This is
    // the runtime contract behind the GECK EvaluatePackage/evp command.
    bool requestFnvAiPackageEvaluation(const MWWorld::Ptr& ptr);
}

#endif
