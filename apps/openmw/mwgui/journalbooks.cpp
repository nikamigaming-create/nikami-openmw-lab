#include "journalbooks.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

#include <components/misc/utf8stream.hpp>
#include <components/settings/values.hpp>

#include "textcolours.hpp"

namespace
{
    struct AddContent
    {
        std::shared_ptr<MWGui::BookTypesetter> mTypesetter;
        MWGui::BookTypesetter::Style* mBodyStyle;

<<<<<<< HEAD
        explicit AddContent(std::shared_ptr<MWGui::BookTypesetter> typesetter, MWGui::BookTypesetter::Style* bodyStyle)
=======
        explicit AddContent(MWGui::BookTypesetter::Ptr typesetter, MWGui::BookTypesetter::Style* bodyStyle)
>>>>>>> origin/main
            : mTypesetter(std::move(typesetter))
            , mBodyStyle(bodyStyle)
        {
        }
    };

    struct AddSpan : AddContent
    {
<<<<<<< HEAD
        explicit AddSpan(std::shared_ptr<MWGui::BookTypesetter> typesetter, MWGui::BookTypesetter::Style* bodyStyle)
=======
        explicit AddSpan(MWGui::BookTypesetter::Ptr typesetter, MWGui::BookTypesetter::Style* bodyStyle)
>>>>>>> origin/main
            : AddContent(std::move(typesetter), bodyStyle)
        {
        }

<<<<<<< HEAD
        void operator()(const MWDialogue::Topic* topic, size_t begin, size_t end)
=======
        void operator()(intptr_t topicId, size_t begin, size_t end)
>>>>>>> origin/main
        {
            MWGui::BookTypesetter::Style* style = mBodyStyle;

            const MWGui::TextColours& textColours = MWBase::Environment::get().getWindowManager()->getTextColours();
<<<<<<< HEAD
            if (topic)
                style = mTypesetter->createHotStyle(mBodyStyle, textColours.journalLink, textColours.journalLinkOver,
                    textColours.journalLinkPressed, MWGui::TypesetBook::InteractiveId(topic));
=======
            if (topicId)
                style = mTypesetter->createHotStyle(mBodyStyle, textColours.journalLink, textColours.journalLinkOver,
                    textColours.journalLinkPressed, topicId);
>>>>>>> origin/main

            mTypesetter->write(style, begin, end);
        }
    };

    struct AddEntry
    {
        std::shared_ptr<MWGui::BookTypesetter> mTypesetter;
        MWGui::BookTypesetter::Style* mBodyStyle;

<<<<<<< HEAD
        AddEntry(std::shared_ptr<MWGui::BookTypesetter> typesetter, MWGui::BookTypesetter::Style* bodyStyle)
=======
        AddEntry(MWGui::BookTypesetter::Ptr typesetter, MWGui::BookTypesetter::Style* bodyStyle)
>>>>>>> origin/main
            : mTypesetter(std::move(typesetter))
            , mBodyStyle(bodyStyle)
        {
        }

        void operator()(MWGui::JournalViewModel::Entry const& entry)
        {
            mTypesetter->addContent(entry.body());

            entry.visitSpans(AddSpan(mTypesetter, mBodyStyle));
        }
    };

    struct AddJournalEntry : AddEntry
    {
        bool mAddHeader;
        MWGui::BookTypesetter::Style* mHeaderStyle;

<<<<<<< HEAD
        explicit AddJournalEntry(std::shared_ptr<MWGui::BookTypesetter> typesetter,
            MWGui::BookTypesetter::Style* bodyStyle, MWGui::BookTypesetter::Style* headerStyle, bool addHeader)
=======
        explicit AddJournalEntry(MWGui::BookTypesetter::Ptr typesetter, MWGui::BookTypesetter::Style* bodyStyle,
            MWGui::BookTypesetter::Style* headerStyle, bool addHeader)
>>>>>>> origin/main
            : AddEntry(std::move(typesetter), bodyStyle)
            , mAddHeader(addHeader)
            , mHeaderStyle(headerStyle)
        {
        }

        void operator()(MWGui::JournalViewModel::JournalEntry const& entry)
        {
            if (mAddHeader)
            {
                mTypesetter->write(mHeaderStyle, entry.timestamp());
                mTypesetter->lineBreak();
            }

            AddEntry::operator()(entry);

            mTypesetter->sectionBreak(30);
        }
    };

    struct AddTopicEntry : AddEntry
    {
        const MWGui::TypesetBook::Content* mContentId;
        MWGui::BookTypesetter::Style* mHeaderStyle;

<<<<<<< HEAD
        explicit AddTopicEntry(std::shared_ptr<MWGui::BookTypesetter> typesetter,
            MWGui::BookTypesetter::Style* bodyStyle, MWGui::BookTypesetter::Style* headerStyle,
            const MWGui::TypesetBook::Content* contentId)
=======
        explicit AddTopicEntry(MWGui::BookTypesetter::Ptr typesetter, MWGui::BookTypesetter::Style* bodyStyle,
            MWGui::BookTypesetter::Style* headerStyle, intptr_t contentId)
>>>>>>> origin/main
            : AddEntry(std::move(typesetter), bodyStyle)
            , mContentId(contentId)
            , mHeaderStyle(headerStyle)
        {
        }

        void operator()(MWGui::JournalViewModel::TopicEntry const& entry)
        {
            mTypesetter->write(mBodyStyle, entry.source());
            mTypesetter->write(mBodyStyle, 0, 3); // begin

            AddEntry::operator()(entry);

            mTypesetter->selectContent(mContentId);
            mTypesetter->write(mBodyStyle, 2, 3); // end quote

            mTypesetter->sectionBreak(30);
        }
    };

    struct AddTopicName : AddContent
    {
<<<<<<< HEAD
        AddTopicName(std::shared_ptr<MWGui::BookTypesetter> typesetter, MWGui::BookTypesetter::Style* style)
=======
        AddTopicName(MWGui::BookTypesetter::Ptr typesetter, MWGui::BookTypesetter::Style* style)
>>>>>>> origin/main
            : AddContent(std::move(typesetter), style)
        {
        }

<<<<<<< HEAD
        void operator()(std::string_view topicName)
=======
        void operator()(MWGui::JournalViewModel::Utf8Span topicName)
>>>>>>> origin/main
        {
            mTypesetter->write(mBodyStyle, topicName);
            mTypesetter->sectionBreak();
        }
    };

    struct AddQuestName : AddContent
    {
<<<<<<< HEAD
        AddQuestName(std::shared_ptr<MWGui::BookTypesetter> typesetter, MWGui::BookTypesetter::Style* style)
=======
        AddQuestName(MWGui::BookTypesetter::Ptr typesetter, MWGui::BookTypesetter::Style* style)
>>>>>>> origin/main
            : AddContent(std::move(typesetter), style)
        {
        }

<<<<<<< HEAD
        void operator()(std::string_view topicName)
=======
        void operator()(MWGui::JournalViewModel::Utf8Span topicName)
>>>>>>> origin/main
        {
            mTypesetter->write(mBodyStyle, topicName);
            mTypesetter->sectionBreak();
        }
    };
}

namespace MWGui
{

<<<<<<< HEAD
    int getCyrillicIndexPageCount()
    {
        // For small font size split alphabet to two columns (2x15 characers), for big font size split it to three
        // colums (3x10 characters).
        return Settings::gui().mFontSize < 18 ? 2 : 3;
    }

    JournalBooks::JournalBooks(std::shared_ptr<JournalViewModel> model, ToUTF8::FromType encoding)
        : mModel(std::move(model))
        , mEncoding(encoding)
        , mIndexPagesCount(0)
    {
    }

    std::shared_ptr<TypesetBook> JournalBooks::createEmptyJournalBook()
    {
        std::shared_ptr<BookTypesetter> typesetter = createTypesetter();
=======
    MWGui::BookTypesetter::Utf8Span to_utf8_span(std::string_view text)
    {
        typedef MWGui::BookTypesetter::Utf8Point point;

        point begin = reinterpret_cast<point>(text.data());

        return MWGui::BookTypesetter::Utf8Span(begin, begin + text.length());
    }

    int getCyrillicIndexPageCount()
    {
        // For small font size split alphabet to two columns (2x15 characers), for big font size split it to three
        // colums (3x10 characters).
        return Settings::gui().mFontSize < 18 ? 2 : 3;
    }

    typedef TypesetBook::Ptr book;

    JournalBooks::JournalBooks(JournalViewModel::Ptr model, ToUTF8::FromType encoding)
        : mModel(std::move(model))
        , mEncoding(encoding)
        , mIndexPagesCount(0)
    {
    }

    book JournalBooks::createEmptyJournalBook()
    {
        BookTypesetter::Ptr typesetter = createTypesetter();
>>>>>>> origin/main

        BookTypesetter::Style* header = typesetter->createStyle({}, journalHeaderColour);
        BookTypesetter::Style* body = typesetter->createStyle({}, MyGUI::Colour::Black);

<<<<<<< HEAD
        typesetter->write(header, "You have no journal entries!");
        typesetter->lineBreak();
        typesetter->write(body, "You should have gone though the starting quest and got an initial quest.");
=======
        typesetter->write(header, to_utf8_span("You have no journal entries!"));
        typesetter->lineBreak();
        typesetter->write(
            body, to_utf8_span("You should have gone though the starting quest and got an initial quest."));
>>>>>>> origin/main

        return typesetter->complete();
    }

<<<<<<< HEAD
    std::shared_ptr<TypesetBook> JournalBooks::createJournalBook()
    {
        std::shared_ptr<BookTypesetter> typesetter = createTypesetter();
=======
    book JournalBooks::createJournalBook()
    {
        BookTypesetter::Ptr typesetter = createTypesetter();
>>>>>>> origin/main

        BookTypesetter::Style* header = typesetter->createStyle({}, journalHeaderColour);
        BookTypesetter::Style* body = typesetter->createStyle({}, MyGUI::Colour::Black);

        mModel->visitJournalEntries({}, AddJournalEntry(typesetter, body, header, true));

        return typesetter->complete();
    }

<<<<<<< HEAD
    std::shared_ptr<TypesetBook> JournalBooks::createTopicBook(const MWDialogue::Topic& topic)
    {
        std::shared_ptr<BookTypesetter> typesetter = createTypesetter();
=======
    book JournalBooks::createTopicBook(uintptr_t topicId)
    {
        BookTypesetter::Ptr typesetter = createTypesetter();
>>>>>>> origin/main

        BookTypesetter::Style* header = typesetter->createStyle({}, journalHeaderColour);
        BookTypesetter::Style* body = typesetter->createStyle({}, MyGUI::Colour::Black);

<<<<<<< HEAD
        mModel->visitTopicName(topic, AddTopicName(typesetter, header));

        const TypesetBook::Content* contentId = typesetter->addContent(", \"");

        mModel->visitTopicEntries(topic, AddTopicEntry(typesetter, body, header, contentId));
=======
        mModel->visitTopicName(topicId, AddTopicName(typesetter, header));

        intptr_t contentId = typesetter->addContent(to_utf8_span(", \""));

        mModel->visitTopicEntries(topicId, AddTopicEntry(typesetter, body, header, contentId));
>>>>>>> origin/main

        return typesetter->complete();
    }

<<<<<<< HEAD
    std::shared_ptr<TypesetBook> JournalBooks::createQuestBook(std::string_view questName)
    {
        std::shared_ptr<BookTypesetter> typesetter = createTypesetter();
=======
    book JournalBooks::createQuestBook(std::string_view questName)
    {
        BookTypesetter::Ptr typesetter = createTypesetter();
>>>>>>> origin/main

        BookTypesetter::Style* header = typesetter->createStyle({}, journalHeaderColour);
        BookTypesetter::Style* body = typesetter->createStyle({}, MyGUI::Colour::Black);

        AddQuestName addName(typesetter, header);
<<<<<<< HEAD
        addName(questName);
=======
        addName(to_utf8_span(questName));
>>>>>>> origin/main

        mModel->visitJournalEntries(questName, AddJournalEntry(typesetter, body, header, false));

        return typesetter->complete();
    }

<<<<<<< HEAD
    std::shared_ptr<TypesetBook> JournalBooks::createTopicIndexBook()
    {
        bool isRussian = (mEncoding == ToUTF8::WINDOWS_1251);

        std::shared_ptr<BookTypesetter> typesetter
            = isRussian ? createCyrillicJournalIndex() : createLatinJournalIndex();
=======
    book JournalBooks::createTopicIndexBook()
    {
        bool isRussian = (mEncoding == ToUTF8::WINDOWS_1251);

        BookTypesetter::Ptr typesetter = isRussian ? createCyrillicJournalIndex() : createLatinJournalIndex();
>>>>>>> origin/main

        return typesetter->complete();
    }

<<<<<<< HEAD
    std::shared_ptr<BookTypesetter> JournalBooks::createLatinJournalIndex()
    {
        std::shared_ptr<BookTypesetter> typesetter = BookTypesetter::create(92, 260);
=======
    BookTypesetter::Ptr JournalBooks::createLatinJournalIndex()
    {
        BookTypesetter::Ptr typesetter = BookTypesetter::create(92, 260);
>>>>>>> origin/main

        typesetter->setSectionAlignment(BookTypesetter::AlignCenter);

        // Latin journal index always has two columns for now.
        mIndexPagesCount = 2;

        char ch = 'A';
        std::string buffer;

        BookTypesetter::Style* body = typesetter->createStyle({}, MyGUI::Colour::Black);
        for (int i = 0; i < 26; ++i)
        {
            buffer = "( ";
            buffer += ch;
            buffer += " )";

            const MWGui::TextColours& textColours = MWBase::Environment::get().getWindowManager()->getTextColours();
            BookTypesetter::Style* style = typesetter->createHotStyle(body, textColours.journalTopic,
<<<<<<< HEAD
                textColours.journalTopicOver, textColours.journalTopicPressed, Utf8Stream::UnicodeChar(ch));
=======
                textColours.journalTopicOver, textColours.journalTopicPressed, (Utf8Stream::UnicodeChar)ch);
>>>>>>> origin/main

            if (i == 13)
                typesetter->sectionBreak();

<<<<<<< HEAD
            typesetter->write(style, buffer);
=======
            typesetter->write(style, to_utf8_span(buffer));
>>>>>>> origin/main
            typesetter->lineBreak();

            ch++;
        }

        return typesetter;
    }

<<<<<<< HEAD
    std::shared_ptr<BookTypesetter> JournalBooks::createCyrillicJournalIndex()
    {
        std::shared_ptr<BookTypesetter> typesetter = BookTypesetter::create(92, 260);
=======
    BookTypesetter::Ptr JournalBooks::createCyrillicJournalIndex()
    {
        BookTypesetter::Ptr typesetter = BookTypesetter::create(92, 260);
>>>>>>> origin/main

        typesetter->setSectionAlignment(BookTypesetter::AlignCenter);

        BookTypesetter::Style* body = typesetter->createStyle({}, MyGUI::Colour::Black);

        // for small font size split alphabet to two columns (2x15 characers), for big font size split it to three
        // colums (3x10 characters).
        mIndexPagesCount = getCyrillicIndexPageCount();
        int sectionBreak = 30 / mIndexPagesCount;

        unsigned char ch[3] = { 0xd0, 0x90, 0x00 }; // CYRILLIC CAPITAL A is a 0xd090 in UTF-8

        std::string buffer;

        for (int i = 0; i < 32; ++i)
        {
            buffer = "( ";
            buffer += ch[0];
            buffer += ch[1];
            buffer += " )";

            Utf8Stream stream(ch, ch + 2);
            Utf8Stream::UnicodeChar first = stream.peek();

            const MWGui::TextColours& textColours = MWBase::Environment::get().getWindowManager()->getTextColours();
            BookTypesetter::Style* style = typesetter->createHotStyle(
                body, textColours.journalTopic, textColours.journalTopicOver, textColours.journalTopicPressed, first);

            ch[1]++;

            // Words can not be started with these characters
            if (i == 26 || i == 28)
                continue;

            if (i % sectionBreak == 0)
                typesetter->sectionBreak();

<<<<<<< HEAD
            typesetter->write(style, buffer);
=======
            typesetter->write(style, to_utf8_span(buffer));
>>>>>>> origin/main
            typesetter->lineBreak();
        }

        return typesetter;
    }

<<<<<<< HEAD
    std::shared_ptr<BookTypesetter> JournalBooks::createTypesetter()
=======
    BookTypesetter::Ptr JournalBooks::createTypesetter()
>>>>>>> origin/main
    {
        // TODO: determine page size from layout...
        return BookTypesetter::create(240, 320);
    }

}
