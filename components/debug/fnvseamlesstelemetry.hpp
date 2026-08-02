#ifndef COMPONENTS_DEBUG_FNVSEAMLESSTELEMETRY_H
#define COMPONENTS_DEBUG_FNVSEAMLESSTELEMETRY_H

#include "debuglog.hpp"

#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>

namespace Debug::FNVSeamlessTelemetry
{
    inline constexpr std::string_view sSchema = "nikami-fnv-seamless-exterior-telemetry/v1";

    inline bool enabled()
    {
        const char* const value = std::getenv("OPENMW_FNV_SEAMLESS_TELEMETRY");
        if (value == nullptr)
            return false;

        const std::string_view text(value);
        return !text.empty() && text != "0" && text != "false" && text != "off" && text != "no";
    }

    inline unsigned int frameSampleInterval()
    {
        constexpr unsigned int defaultInterval = 60;
        const char* const value = std::getenv("OPENMW_FNV_SEAMLESS_TELEMETRY_FRAME_INTERVAL");
        if (value == nullptr || *value == '\0')
            return defaultInterval;

        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end == value || *end != '\0' || parsed > 3600)
            return defaultInterval;
        return static_cast<unsigned int>(parsed);
    }

    inline std::string jsonEscape(std::string_view value)
    {
        static constexpr char hex[] = "0123456789abcdef";

        std::string result;
        result.reserve(value.size());
        for (const char character : value)
        {
            const unsigned char byte = static_cast<unsigned char>(character);
            switch (byte)
            {
                case '"':
                    result += "\\\"";
                    break;
                case '\\':
                    result += "\\\\";
                    break;
                case '\b':
                    result += "\\b";
                    break;
                case '\f':
                    result += "\\f";
                    break;
                case '\n':
                    result += "\\n";
                    break;
                case '\r':
                    result += "\\r";
                    break;
                case '\t':
                    result += "\\t";
                    break;
                default:
                    if (byte < 0x20)
                    {
                        result += "\\u00";
                        result += hex[(byte >> 4) & 0x0f];
                        result += hex[byte & 0x0f];
                    }
                    else
                        result += character;
                    break;
            }
        }
        return result;
    }

    inline std::string eventPrefix(std::string_view event)
    {
        std::string result;
        result.reserve(sSchema.size() + event.size() + 32);
        result += "{\"schema\":\"";
        result.append(sSchema.data(), sSchema.size());
        result += "\",\"event\":\"";
        result += jsonEscape(event);
        result += "\"";
        return result;
    }

    class Event
    {
    public:
        explicit Event(std::string_view event)
            : mEnabled(enabled())
        {
            if (mEnabled)
                mPayload = eventPrefix(event);
        }

        Event& string(std::string_view name, std::string_view value)
        {
            if (mEnabled)
            {
                appendFieldName(name);
                mPayload += "\"";
                mPayload += jsonEscape(value);
                mPayload += "\"";
            }
            return *this;
        }

        Event& boolean(std::string_view name, bool value)
        {
            if (mEnabled)
            {
                appendFieldName(name);
                mPayload += value ? "true" : "false";
            }
            return *this;
        }

        template <class T, class = std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>>
        Event& integer(std::string_view name, T value)
        {
            if (mEnabled)
            {
                appendFieldName(name);
                mPayload += std::to_string(value);
            }
            return *this;
        }

        Event& number(std::string_view name, double value)
        {
            if (mEnabled)
            {
                appendFieldName(name);
                mPayload += std::to_string(value);
            }
            return *this;
        }

        void emit() const
        {
            if (mEnabled)
                Log(Debug::Info) << "FNV seamless telemetry: " << mPayload << "}";
        }

    private:
        void appendFieldName(std::string_view name)
        {
            mPayload += ",\"";
            mPayload += jsonEscape(name);
            mPayload += "\":";
        }

        bool mEnabled;
        std::string mPayload;
    };
}

#endif
