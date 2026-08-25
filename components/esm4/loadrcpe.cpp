#include "loadrcpe.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "falloutformat.hpp"
#include "reader.hpp"

namespace
{
    constexpr std::uint32_t sEditorId = ESM::fourCC("EDID");
    constexpr std::uint32_t sFullName = ESM::fourCC("FULL");
    constexpr std::uint32_t sCondition = ESM::fourCC("CTDA");
    constexpr std::uint32_t sData = ESM::fourCC("DATA");
    constexpr std::uint32_t sIngredient = ESM::fourCC("RCIL");
    constexpr std::uint32_t sOutput = ESM::fourCC("RCOD");
    constexpr std::uint32_t sQuantity = ESM::fourCC("RCQY");

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
            fail(ESM::printName(header.typeId) + " size " + std::to_string(header.dataSize) + ", expected "
                + std::to_string(expected));
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
        if (reader.subRecordHeader().dataSize == ESM4::Fallout::kEmptySubrecordBytes)
            fail("zero-sized " + std::string(field));
        if (!reader.getZString(value))
            fail("could not read " + std::string(field));
    }

    ESM4::TargetCondition readCondition(ESM4::Reader& reader)
    {
        requireSize(reader.subRecordHeader(), static_cast<std::uint32_t>(sizeof(ESM4::TargetCondition)));
        ESM4::TargetCondition condition;
        if (!ESM4::loadTargetCondition(reader, condition))
            fail("could not read CTDA");
        return condition;
    }

    ESM::FormId readFormId(ESM4::Reader& reader, std::string_view field, bool nullable)
    {
        std::uint32_t raw = ESM4::Fallout::kRecipeNullFormId;
        if (!reader.getExact(raw))
            fail("could not read " + std::string(field) + " FormID");

        if (raw == ESM4::Fallout::kRecipeNullFormId)
        {
            if (!nullable)
                fail("could not read nonzero " + std::string(field) + " FormID");
            return {};
        }

        ESM::FormId value = ESM::FormId::fromUint32(raw);
        reader.adjustFormId(value);
        return value;
    }

    ESM::FormId readRequiredFormId(ESM4::Reader& reader, std::string_view field)
    {
        return readFormId(reader, field, false);
    }

    ESM::FormId readNullableFormId(ESM4::Reader& reader, std::string_view field)
    {
        return readFormId(reader, field, true);
    }
}

void ESM4::Recipe::load(Reader& reader)
{
    if (!Fallout::isNewVegasVersion(reader.esmVersion()))
        fail("unsupported ESM4 version");

    Recipe value;
    value.mId = reader.getFormIdFromHeader();
    value.mFlags = reader.hdr().record.flags;

    Phase phase = Phase::Start;
    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& header = reader.subRecordHeader();
        switch (header.typeId)
        {
            case sEditorId:
                if (phase != Phase::Start)
                    fail("EDID is duplicated or out of order");
                readZString(reader, value.mEditorId, "EDID");
                phase = Phase::EditorId;
                break;
            case sFullName:
                if (phase != Phase::EditorId)
                    fail("FULL is missing, duplicated, or out of order");
                readZString(reader, value.mFullName, "FULL");
                phase = Phase::Header;
                break;
            case sCondition:
                if (phase != Phase::Header)
                    fail("CTDA is out of order");
                value.mConditions.push_back(readCondition(reader));
                break;
            case sData:
                if (phase != Phase::Header)
                    fail("DATA is missing, duplicated, or out of order");
                requireSize(header, Fallout::kRecipeDataBytes);
                readExact(reader, value.mData.mRequiredSkill, "DATA required skill");
                readExact(reader, value.mData.mRequiredSkillLevel, "DATA required skill level");
                value.mData.mCategory = readNullableFormId(reader, "DATA category");
                value.mData.mSubCategory = readRequiredFormId(reader, "DATA subcategory");
                phase = Phase::Data;
                break;
            case sIngredient:
                if (phase != Phase::Data && phase != Phase::IngredientQuantity)
                    fail("RCIL is out of order or its preceding quantity is missing");
                requireSize(header, Fallout::kRecipeItemBytes);
                value.mIngredients.push_back({ .mItem = readRequiredFormId(reader, "RCIL item") });
                phase = Phase::IngredientItem;
                break;
            case sOutput:
                if (phase != Phase::IngredientQuantity && phase != Phase::OutputQuantity)
                    fail("RCOD is out of order or its preceding quantity is missing");
                requireSize(header, Fallout::kRecipeItemBytes);
                value.mOutputs.push_back({ .mItem = readRequiredFormId(reader, "RCOD item") });
                phase = Phase::OutputItem;
                break;
            case sQuantity:
                requireSize(header, Fallout::kRecipeQuantityBytes);
                if (phase == Phase::IngredientItem)
                {
                    readExact(reader, value.mIngredients.back().mQuantity, "ingredient RCQY");
                    if (value.mIngredients.back().mQuantity < Fallout::kRecipeMinimumQuantity)
                        fail("ingredient RCQY is zero");
                    phase = Phase::IngredientQuantity;
                }
                else if (phase == Phase::OutputItem)
                {
                    readExact(reader, value.mOutputs.back().mQuantity, "output RCQY");
                    if (value.mOutputs.back().mQuantity < Fallout::kRecipeMinimumQuantity)
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
