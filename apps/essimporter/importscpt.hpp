#ifndef OPENMW_ESSIMPORT_IMPORTSCPT_H
#define OPENMW_ESSIMPORT_IMPORTSCPT_H

#include "importscri.hpp"

#include <cstdint>

#include <components/esm/esmcommon.hpp>
#include <components/esm3/loadscpt.hpp>
<<<<<<< HEAD
#include <components/esm3/refnum.hpp>
=======
>>>>>>> origin/main

namespace ESM
{
    class ESMReader;
}

namespace ESSImport
{

    struct SCHD
    {
        ESM::NAME32 mName;
        std::uint32_t mNumShorts;
        std::uint32_t mNumLongs;
        std::uint32_t mNumFloats;
        std::uint32_t mScriptDataSize;
        std::uint32_t mStringTableSize;
    };

    // A running global script
    struct SCPT
    {
        SCHD mSCHD;

        // values of local variables
        SCRI mSCRI;

        bool mRunning;
<<<<<<< HEAD
        ESM::RefNum mRefNum; // Targeted reference
=======
        int32_t mRefNum; // Targeted reference, -1: no reference
>>>>>>> origin/main

        void load(ESM::ESMReader& esm);
    };

}

#endif
