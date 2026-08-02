#include <components/debug/fnvseamlesstelemetry.hpp>

#include <gtest/gtest.h>

#include <string>

namespace
{
    TEST(FnvSeamlessTelemetry, EscapesJsonControlCharacters)
    {
        const std::string value = std::string("quote\" slash\\ newline\n tab\t") + static_cast<char>(1);

        EXPECT_EQ(Debug::FNVSeamlessTelemetry::jsonEscape(value), "quote\\\" slash\\\\ newline\\n tab\\t\\u0001");
    }

    TEST(FnvSeamlessTelemetry, IncludesTheVersionedSchemaInEachEventPrefix)
    {
        EXPECT_EQ(Debug::FNVSeamlessTelemetry::eventPrefix("grid-change-begin"),
            "{\"schema\":\"nikami-fnv-seamless-exterior-telemetry/v1\",\"event\":\"grid-change-begin\"");
    }
}
