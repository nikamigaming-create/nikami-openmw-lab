#ifndef COMPONENTS_L10N_ICUSTUB_H
#define COMPONENTS_L10N_ICUSTUB_H

#include <cstdint>
#include <string>
#include <string_view>

struct UParseError
{
    char16_t preContext[16]{};
    char16_t postContext[16]{};
};

namespace icu
{
    class StringPiece
    {
    public:
        StringPiece(const char* data, std::int32_t size)
            : mData(data)
            , mSize(size)
        {
        }

        const char* data() const { return mData; }
        std::int32_t size() const { return mSize; }

    private:
        const char* mData;
        std::int32_t mSize;
    };

    class UnicodeString
    {
    public:
        UnicodeString() = default;
        UnicodeString(const char* value)
            : mValue(value != nullptr ? value : "")
        {
        }
        UnicodeString(const std::string& value)
            : mValue(value)
        {
        }
        UnicodeString(std::string_view value)
            : mValue(value)
        {
        }
        UnicodeString(const char16_t*) {}

        static UnicodeString fromUTF8(const char* value) { return UnicodeString(value); }
        static UnicodeString fromUTF8(StringPiece value)
        {
            return UnicodeString(std::string(value.data(), static_cast<std::size_t>(value.size())));
        }

        void toUTF8String(std::string& out) const { out += mValue; }

    private:
        std::string mValue;

        friend class MessageFormat;
    };

    class Locale
    {
    public:
        Locale(const char* language = "")
            : mName(language != nullptr ? language : "")
            , mLanguage(mName)
        {
        }
        Locale(const char* language, const char* country)
            : mName(language != nullptr ? language : "")
            , mLanguage(mName)
            , mCountry(country != nullptr ? country : "")
        {
            if (!mCountry.empty())
            {
                mName += "_";
                mName += mCountry;
            }
        }

        static const Locale& getEnglish()
        {
            static const Locale english("en");
            return english;
        }

        const char* getName() const { return mName.c_str(); }
        const char* getLanguage() const { return mLanguage.c_str(); }
        const char* getCountry() const { return mCountry.c_str(); }
        const char* getVariant() const { return ""; }
        void getDisplayName(const Locale&, UnicodeString& out) const { out = UnicodeString(mName.c_str()); }

        friend bool operator==(const Locale& left, const Locale& right) { return left.mName == right.mName; }

    private:
        std::string mName;
        std::string mLanguage;
        std::string mCountry;
    };

    class Formattable
    {
    public:
        Formattable() = default;
        Formattable(const UnicodeString&) {}
        Formattable(const char*) {}
        Formattable(const std::string&) {}
        Formattable(std::string_view) {}
        Formattable(double) {}
        Formattable(int) {}
    };

    class ErrorCode
    {
    public:
        bool isFailure() const { return false; }
        bool isSuccess() const { return true; }
        const char* errorName() const { return "U_ZERO_ERROR"; }
    };

    class MessageFormat
    {
    public:
        MessageFormat() = default;
        MessageFormat(const UnicodeString& pattern, const Locale&, UParseError&, ErrorCode&)
            : mPattern(pattern.mValue)
        {
        }

        void format(const UnicodeString*, const Formattable*, std::int32_t, UnicodeString& result, ErrorCode&) const
        {
            result = UnicodeString(mPattern.c_str());
        }

    private:
        std::string mPattern;
    };
}

#endif
