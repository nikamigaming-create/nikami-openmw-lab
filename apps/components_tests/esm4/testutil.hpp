#ifndef OPENMW_APPS_COMPONENTS_TESTS_ESM4_TESTUTIL_H
#define OPENMW_APPS_COMPONENTS_TESTS_ESM4_TESTUTIL_H

#include <components/esm4/reader.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace ESM4Test
{
    template <class T>
    void appendPod(std::string& output, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        output.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    inline void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        if (type.size() != 4 || data.size() > std::numeric_limits<std::uint16_t>::max())
            throw std::logic_error("invalid synthetic ESM4 subrecord");
        output.append(type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    inline void appendRecord(std::string& output, std::string_view type, std::uint32_t formId, std::string_view data)
    {
        if (type.size() != 4)
            throw std::logic_error("invalid synthetic ESM4 record");
        output.append(type);
        appendPod(output, static_cast<std::uint32_t>(data.size()));
        appendPod(output, std::uint32_t{ 0 });
        appendPod(output, formId);
        appendPod(output, std::uint32_t{ 0 });
        appendPod(output, std::uint16_t{ 0 });
        appendPod(output, std::uint16_t{ 0 });
        output.append(data);
    }

    inline std::unique_ptr<ESM4::Reader> makeReader(
        std::string_view recordType, std::string recordData, std::uint32_t modIndex = 2, float version = 1.34f)
    {
        std::string hedr;
        appendPod(hedr, version);
        appendPod(hedr, std::int32_t{ 2 });
        appendPod(hedr, std::uint32_t{ 0x800 });
        std::string headerData;
        appendSubRecord(headerData, "HEDR", hedr);

        std::string plugin;
        appendRecord(plugin, "TES4", 0, headerData);
        appendRecord(plugin, recordType, 0x123456, recordData);
        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), "synthetic.esm", nullptr, nullptr, true);
        reader->setModIndex(modIndex);
        if (!reader->getRecordHeader())
            throw std::logic_error("synthetic ESM4 record is missing");
        reader->getRecordData();
        return reader;
    }
}

#endif // OPENMW_APPS_COMPONENTS_TESTS_ESM4_TESTUTIL_H
