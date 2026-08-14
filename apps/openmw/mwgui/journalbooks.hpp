#ifndef MWGUI_JOURNALBOOKS_HPP
#define MWGUI_JOURNALBOOKS_HPP

#include "bookpage.hpp"
#include "journalviewmodel.hpp"

#include <components/toutf8/toutf8.hpp>

namespace MWGui
{
<<<<<<< HEAD
=======
    MWGui::BookTypesetter::Utf8Span to_utf8_span(std::string_view text);
>>>>>>> origin/main
    int getCyrillicIndexPageCount();

    const MyGUI::Colour journalHeaderColour = MyGUI::Colour(0.60f, 0.00f, 0.00f);

    struct JournalBooks
    {
        std::shared_ptr<JournalViewModel> mModel;

<<<<<<< HEAD
        JournalBooks(std::shared_ptr<JournalViewModel> model, ToUTF8::FromType encoding);

        std::shared_ptr<TypesetBook> createEmptyJournalBook();
        std::shared_ptr<TypesetBook> createJournalBook();
        std::shared_ptr<TypesetBook> createTopicBook(const MWDialogue::Topic& topic);
        std::shared_ptr<TypesetBook> createQuestBook(std::string_view questName);
        std::shared_ptr<TypesetBook> createTopicIndexBook();
=======
        JournalBooks(JournalViewModel::Ptr model, ToUTF8::FromType encoding);

        Book createEmptyJournalBook();
        Book createJournalBook();
        Book createTopicBook(uintptr_t topicId);
        Book createQuestBook(std::string_view questName);
        Book createTopicIndexBook();
>>>>>>> origin/main

        ToUTF8::FromType mEncoding;
        int mIndexPagesCount;

    private:
<<<<<<< HEAD
        std::shared_ptr<BookTypesetter> createTypesetter();
        std::shared_ptr<BookTypesetter> createLatinJournalIndex();
        std::shared_ptr<BookTypesetter> createCyrillicJournalIndex();
=======
        BookTypesetter::Ptr createTypesetter();
        BookTypesetter::Ptr createLatinJournalIndex();
        BookTypesetter::Ptr createCyrillicJournalIndex();
>>>>>>> origin/main
    };
}

#endif // MWGUI_JOURNALBOOKS_HPP
