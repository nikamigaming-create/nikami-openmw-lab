#include "translation.hpp"

#include <fstream>
<<<<<<< HEAD

#include <components/misc/pathhelpers.hpp>
=======
>>>>>>> origin/main

namespace Translation
{
    Storage::Storage()
        : mEncoder(nullptr)
    {
    }

    void Storage::loadTranslationData(const Files::Collections& dataFileCollections, std::string_view esmFileName)
    {
<<<<<<< HEAD
        std::string_view esmNameNoExtension = Misc::stemFile(esmFileName);
=======
        std::string esmNameNoExtension(Misc::StringUtils::lowerCase(esmFileName));
        // changing the extension
        size_t dotPos = esmNameNoExtension.rfind('.');
        if (dotPos != std::string::npos)
            esmNameNoExtension.resize(dotPos);
>>>>>>> origin/main

        loadData(mCellNamesTranslations, esmNameNoExtension, "cel", dataFileCollections);
        loadData(mPhraseForms, esmNameNoExtension, "top", dataFileCollections);
        loadData(mKeywords, esmNameNoExtension, "mrk", dataFileCollections);
    }

<<<<<<< HEAD
    void Storage::loadData(ContainerType& container, std::string_view fileNameNoExtension, std::string_view extension,
        const Files::Collections& dataFileCollections)
=======
    void Storage::loadData(ContainerType& container, const std::string& fileNameNoExtension,
        const std::string& extension, const Files::Collections& dataFileCollections)
>>>>>>> origin/main
    {
        std::string fileName(fileNameNoExtension);
        fileName += '.';
        fileName += extension;

<<<<<<< HEAD
        const Files::MultiDirCollection& collection = dataFileCollections.getCollection(extension);
        if (collection.doesExist(fileName))
        {
            std::ifstream stream(collection.getPath(fileName));
=======
        if (dataFileCollections.getCollection(extension).doesExist(fileName))
        {
            std::ifstream stream(dataFileCollections.getCollection(extension).getPath(fileName).c_str());
>>>>>>> origin/main

            if (!stream.is_open())
                throw std::runtime_error("failed to open translation file: " + fileName);

            loadDataFromStream(container, stream);
        }
    }

    void Storage::loadDataFromStream(ContainerType& container, std::istream& stream)
    {
        std::string line;
        while (!stream.eof() && !stream.fail())
        {
            std::getline(stream, line);
            if (!line.empty() && *line.rbegin() == '\r')
                line.resize(line.size() - 1);

            if (!line.empty())
            {
                const std::string_view utf8 = mEncoder->getUtf8(line);

                size_t tabPos = utf8.find('\t');
                if (tabPos != std::string::npos && tabPos > 0 && tabPos < utf8.size() - 1)
                {
                    const std::string_view key = utf8.substr(0, tabPos);
                    const std::string_view value = utf8.substr(tabPos + 1);

                    if (!key.empty() && !value.empty())
                        container.emplace(key, value);
                }
            }
        }
    }

    std::string_view Storage::translateCellName(std::string_view cellName) const
    {
        auto entry = mCellNamesTranslations.find(cellName);

        if (entry == mCellNamesTranslations.end())
            return cellName;

        return entry->second;
    }

<<<<<<< HEAD
    std::string_view Storage::topicStandardForm(std::string_view phrase) const
    {
=======
    std::string_view Storage::topicID(std::string_view phrase) const
    {
        std::string_view result = topicStandardForm(phrase);

        // seeking for the topic ID
        auto topicIDIterator = mTopicIDs.find(result);

        if (topicIDIterator != mTopicIDs.end())
            result = topicIDIterator->second;

        return result;
    }

    std::string_view Storage::topicStandardForm(std::string_view phrase) const
    {
>>>>>>> origin/main
        auto phraseFormsIterator = mPhraseForms.find(phrase);

        if (phraseFormsIterator != mPhraseForms.end())
            return phraseFormsIterator->second;
        else
            return phrase;
    }

    std::string_view Storage::topicKeyword(std::string_view phrase) const
    {
        auto entry = mKeywords.find(phrase);

        if (entry == mKeywords.end())
            return phrase;

        return entry->second;
    }

    void Storage::addPhraseForm(std::string_view phrase, std::string_view topicId)
    {
        mPhraseForms.emplace(phrase, topicId);
    }

    void Storage::setEncoder(ToUTF8::Utf8Encoder* encoder)
    {
        mEncoder = encoder;
    }
<<<<<<< HEAD
=======

    bool Storage::hasTranslation() const
    {
        return !mCellNamesTranslations.empty() || !mTopicIDs.empty() || !mPhraseForms.empty();
    }
>>>>>>> origin/main
}
