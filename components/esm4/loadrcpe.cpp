#include "loadrcpe.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "reader.hpp"

namespace
{
    enum class Phase
    {
        Start,
        EditorId,
        Header,
        Data,
        IngredientItem,
        IngredientQuantity,
        OutputItem,
        OutputQuantity,
    };

    [[noreturn]] void fail(std::string_view message)
    {
        throw std::runtime_error("ESM4::Recipe::load - " + std::string(message));
    }

    void requireSize(const ESM4::SubRecordHeader& header, std::uint32_t expected)
    {
        if (header.dataSize != expected)
        {
            fail("unsupported " + ESM::printName(header.typeId) + " size " + std::to_string(header.dataSize)
                + ", expected " + std::to_string(expected));
        }
    }

    template <class T>
    void readExact(ESM4::Reader& reader, T& value, std::string_view field)
    {
        if (!reader.getExact(value))
            fail("could not read " + std::string(field));
    }

    void readZString(ESM4::Reader& reader, std::string& value, std::string_view field)
    {
        if (reader.subRecordHeader().dataSize == 0)
            fail("zero-sized " + std::string(field));
        if (!reader.getZString(value))
            fail("could not read " + std::string(field));
    }

    ESM4::TargetCondition readCondition(ESM4::Reader& reader)
    {
        requireSize(reader.subRecordHeader(), 28);
        ESM4::TargetCondition condition;
        if (!ESM4::loadTargetCondition(reader, condition))
            fail("could not read CTDA");
        return condition;
    }

    ESM::FormId readRequiredFormId(ESM4::Reader& reader, std::string_view field)
    {
        ESM::FormId value;
        if (!reader.getFormId(value) || value.isZeroOrUnset())
            fail("could not read nonzero " + std::string(field) + " FormID");
        return value;
    }

    ESM::FormId readNullableFormId(ESM4::Reader& reader, std::string_view field)
    {
        ESM::FormId value;
        if (!reader.getFormId(value))
            fail("could not read " + std::string(field) + " FormID");
        return value;
    }
}

void ESM4::Recipe::load(Reader& reader)
{
    // Frozen English Ultimate Edition corpus (10 official masters):
    // 291 physical/winning-live RCPE records (FalloutNV 105, Dead Money 77,
    // Honest Hearts 32, Old World Blues 29, Lonesome Road 30, GRA 18);
    // EDID/FULL/DATA 291 each;
    // CTDA 156x28; RCIL 651x4; RCOD 336x4; RCQY 987x4.
    // Every record is exactly EDID,FULL,CTDA*,DATA,(RCIL,RCQY)+,
    // (RCOD,RCQY)+. None is deleted, compressed, or overridden.
    Recipe value;
    value.mId = reader.getFormIdFromHeader();
    value.mFlags = reader.hdr().record.flags;

    Phase phase = Phase::Start;
    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& header = reader.subRecordHeader();
        switch (header.typeId)
        {
            case ESM::fourCC("EDID"):
                if (phase != Phase::Start)
                    fail("EDID is duplicated or out of order");
                readZString(reader, value.mEditorId, "EDID");
                phase = Phase::EditorId;
                break;
            case ESM::fourCC("FULL"):
                if (phase != Phase::EditorId)
                    fail("FULL is missing, duplicated, or out of order");
                readZString(reader, value.mFullName, "FULL");
                phase = Phase::Header;
                break;
            case ESM::fourCC("CTDA"):
                if (phase != Phase::Header)
                    fail("CTDA is out of order");
                value.mConditions.push_back(readCondition(reader));
                break;
            case ESM::fourCC("DATA"):
                if (phase != Phase::Header)
                    fail("DATA is missing, duplicated, or out of order");
                requireSize(header, 16);
                readExact(reader, value.mData.mRequiredSkill, "DATA required skill");
                readExact(reader, value.mData.mRequiredSkillLevel, "DATA required skill level");
                value.mData.mCategory = readNullableFormId(reader, "DATA category");
                value.mData.mSubCategory = readRequiredFormId(reader, "DATA subcategory");
                phase = Phase::Data;
                break;
            case ESM::fourCC("RCIL"):
            {
                if (phase != Phase::Data && phase != Phase::IngredientQuantity)
                    fail("RCIL is out of order or its preceding quantity is missing");
                requireSize(header, 4);
                value.mIngredients.push_back({ .mItem = readRequiredFormId(reader, "RCIL item") });
                phase = Phase::IngredientItem;
                break;
            }
            case ESM::fourCC("RCOD"):
            {
                if (phase != Phase::IngredientQuantity && phase != Phase::OutputQuantity)
                    fail("RCOD is out of order or its preceding quantity is missing");
                requireSize(header, 4);
                value.mOutputs.push_back({ .mItem = readRequiredFormId(reader, "RCOD item") });
                phase = Phase::OutputItem;
                break;
            }
            case ESM::fourCC("RCQY"):
                requireSize(header, 4);
                if (phase == Phase::IngredientItem)
                {
                    readExact(reader, value.mIngredients.back().mQuantity, "ingredient RCQY");
                    if (value.mIngredients.back().mQuantity == 0)
                        fail("ingredient RCQY is zero");
                    phase = Phase::IngredientQuantity;
                }
                else if (phase == Phase::OutputItem)
                {
                    readExact(reader, value.mOutputs.back().mQuantity, "output RCQY");
                    if (value.mOutputs.back().mQuantity == 0)
                        fail("output RCQY is zero");
                    phase = Phase::OutputQuantity;
                }
                else
                    fail("RCQY appears without an RCIL or RCOD item");
                break;
            default:
                fail("unknown or out-of-order Fallout New Vegas subrecord " + ESM::printName(header.typeId));
        }
    }

    if (phase != Phase::OutputQuantity || value.mIngredients.empty() || value.mOutputs.empty())
        fail("record is incomplete or has no ingredient/output pair");

    *this = std::move(value);
}
