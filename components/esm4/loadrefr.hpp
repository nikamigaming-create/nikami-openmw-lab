/*
  Copyright (C) 2015-2016, 2018, 2020-2021 cc9cii

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  cc9cii cc9c@iinet.net.au

  Much of the information on the data structures are based on the information
  from Tes4Mod:Mod_File_Format and Tes5Mod:File_Formats but also refined by
  trial & error.  See http://en.uesp.net/wiki for details.

*/
#ifndef ESM4_REFR_H
#define ESM4_REFR_H

#include <array>
#include <cstdint>

#include "reference.hpp" // EnableParent

#include <components/esm/defs.hpp>
#include <components/esm/position.hpp>
#include <components/esm/refid.hpp>

namespace ESM4
{
    class Reader;
    class Writer;

    enum MapMarkerType : std::uint8_t
    {
        Map_None = 0x00,
        Map_City = 0x01,
        Map_Settlement = 0x02,
        Map_Encampment = 0x03,
        Map_NaturalLandmark = 0x04,
        Map_Cave = 0x05,
        Map_Factory = 0x06,
        Map_Monument = 0x07,
        Map_Military = 0x08,
        Map_Office = 0x09,
        Map_TownRuins = 0x0A,
        Map_UrbanRuins = 0x0B,
        Map_SewerRuins = 0x0C,
        Map_Metro = 0x0D,
        Map_Vault = 0x0E,
    };

    enum MapMarkerFlags : std::uint8_t
    {
        MapMarker_Visible = 0x01,
        MapMarker_CanTravel = 0x02,
        MapMarker_ShowAllHidden = 0x04,
    };

    struct TeleportDest
    {
        ESM::FormId destDoor;
        ESM::Position destPos;
        std::uint32_t flags = 0; // 0x01 no alarm (only in TES5)
        ESM::FormId transitionInterior;
    };

    struct RadioStationData
    {
        float rangeRadius = 0.f;
        // 0 radius, 1 everywhere, 2 worldspace and linked int, 3 linked int, 4 current cell only
        std::uint32_t broadcastRange = 0;
        float staticPercentage = 0.f;
        ESM::FormId posReference{}; // only used if broadcastRange == 0
    };

    struct Primitive
    {
        enum Type : std::uint32_t
        {
            None = 0,
            Box = 1,
            Sphere = 2,
            PortalBox = 3,
        };

        std::array<float, 3> mBounds{};
        std::array<float, 4> mColor{};
        std::uint32_t mType = None;
    };

    struct Reference
    {
        ESM::FormId mId; // from the header

        ESM::RefId mParent; // cell FormId, from the loading sequence
                            // NOTE: for exterior cells it will be the dummy cell FormId

        std::uint32_t mFlags; // from the header, see enum type RecordFlag for details

        std::string mEditorId;
        std::string mFullName;
        ESM::FormId mBaseObj;

        ESM::Position mPos;
        float mScale = 1.0f;
        ESM::FormId mOwner;
        ESM::FormId mGlobal;
        std::int32_t mFactionRank = -1;

        bool mIsMapMarker = false;
        std::uint8_t mMapMarkerFlags = 0;
        std::uint8_t mMapMarkerType = Map_None;

        EnableParent mEsp;

        std::int32_t mCount = 1; // only if > 1

        ESM::FormId mAudioLocation;

        RadioStationData mRadio;

        TeleportDest mDoor;
        // XLOC is optional. References without it are ordinary unlocked objects.
        // Leaving these fields indeterminate made unlocked ESM4 doors randomly
        // return OpenMW's LockedDoor failure action instead of their XTEL action.
        bool mIsLocked = false;
        std::int8_t mLockLevel = 0;
        ESM::FormId mKey;

        ESM::FormId mTargetRef;

        // XPRM is the authored collision primitive used by model-less activators such as Fallout trigger volumes.
        Primitive mPrimitive;
        bool mHasPrimitive = false;

        // Fallout patrol routes are linked REFR chains. XLKR points at the next marker, XPRD stores the
        // marker's authored wait time, and an empty XPPA marks a patrol-idle/script point.
        ESM::FormId mLinkedReference;
        float mPatrolIdleTime = 0.f;
        bool mHasPatrolIdleTime = false;
        bool mIsPatrolIdleScriptMarker = false;

        void load(ESM4::Reader& reader);
        // void save(ESM4::Writer& writer) const;

        void blank();

        static constexpr ESM::RecNameInts sRecordId = ESM::REC_REFR4;
    };
}

#endif // ESM4_REFR_H
