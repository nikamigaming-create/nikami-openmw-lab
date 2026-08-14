#ifndef OPENMW_COMPONENTS_ESM4_LOADRCPE_H
#define OPENMW_COMPONENTS_ESM4_LOADRCPE_H

#include <cstdint>
#include <string>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

#include "script.hpp"

namespace ESM4
{
    class Reader;

    // Fallout: New Vegas recipe records. This is authored data only; crafting
    // transactions, menus, workbench activation, and condition evaluation are
    // separate runtime contracts.
    struct Recipe
    {
        struct Data
        {
            std::int32_t mRequiredSkill = -1;
            std::uint32_t mRequiredSkillLevel = 0;
            ESM::FormId mCategory;
            ESM::FormId mSubCategory;
        };

        struct Item
        {
            ESM::FormId mItem;
            std::uint32_t mQuantity = 0;
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::string mFullName;
        std::vector<TargetCondition> mConditions;
        Data mData;
        std::vector<Item> mIngredients;
        std::vector<Item> mOutputs;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::REC_RCPE4;
    };
}

#endif
