#ifndef OPENMW_COMPONENTS_ESM4_LOADRCCT_H
#define OPENMW_COMPONENTS_ESM4_LOADRCCT_H

#include <cstdint>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    // Fallout: New Vegas recipe-category metadata. DATA is an opaque byte:
    // the official corpus authors values outside the one currently named bit,
    // so preserving the complete value is required.
    struct RecipeCategory
    {
        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::string mFullName;
        std::uint8_t mData = 0;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::REC_RCCT4;
    };
}

#endif
