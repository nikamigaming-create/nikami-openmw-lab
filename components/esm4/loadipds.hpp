#ifndef OPENMW_ESM4_IMPACTDATASET_H
#define OPENMW_ESM4_IMPACTDATASET_H

#include <array>
#include <cstdint>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    struct ImpactDataSet
    {
        enum Material : std::size_t
        {
            Stone = 0,
            Dirt,
            Grass,
            Glass,
            Metal,
            Wood,
            Organic,
            Cloth,
            Water,
            HollowMetal,
            OrganicBug,
            OrganicGlow,
            MaterialCount
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::array<ESM::FormId, MaterialCount> mImpacts{};

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_IPDS4;
    };
}

#endif
