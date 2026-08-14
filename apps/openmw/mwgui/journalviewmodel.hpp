#ifndef MWGUI_JOURNALVIEWMODEL_HPP
#define MWGUI_JOURNALVIEWMODEL_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <components/misc/utf8stream.hpp>
<<<<<<< HEAD

namespace MWDialogue
{
    class Topic;
}
=======
>>>>>>> origin/main

namespace MWGui
{
    /// View-Model for the journal GUI
    ///
    /// This interface defines an abstract data model suited
    ///  specifically to the needs of the journal GUI. It isolates
    /// the journal GUI from the implementation details of the
    /// game data store.
    struct JournalViewModel
    {
<<<<<<< HEAD
=======
        typedef std::shared_ptr<JournalViewModel> Ptr;

        typedef intptr_t QuestId;
        typedef intptr_t TopicId;
        typedef uint8_t const* Utf8Point;
        typedef std::pair<Utf8Point, Utf8Point> Utf8Span;

>>>>>>> origin/main
        /// The base interface for both journal entries and topics.
        struct Entry
        {
            /// returns the body text for the journal entry
            ///
            /// This function returns a borrowed reference to the body of the
            /// journal entry. The returned reference becomes invalid when the
            /// entry is destroyed.
<<<<<<< HEAD
            virtual std::string_view body() const = 0;
=======
            virtual Utf8Span body() const = 0;
>>>>>>> origin/main

            /// Visits each subset of text in the body, delivering the beginning
            /// and end of the span relative to the body, and a valid topic ID if
            /// the span represents a keyword, or zero if not.
<<<<<<< HEAD
            virtual void visitSpans(std::function<void(const MWDialogue::Topic*, size_t, size_t)> visitor) const = 0;
=======
            virtual void visitSpans(std::function<void(TopicId, size_t, size_t)> visitor) const = 0;
>>>>>>> origin/main

            virtual ~Entry() = default;
        };

        /// An interface to topic data.
        struct TopicEntry : Entry
        {
            /// Returns a pre-formatted span of UTF8 encoded text representing
            /// the name of the NPC this portion of dialog was heard from.
<<<<<<< HEAD
            virtual std::string_view source() const = 0;
=======
            virtual Utf8Span source() const = 0;
>>>>>>> origin/main

            virtual ~TopicEntry() = default;
        };

        /// An interface to journal data.
        struct JournalEntry : Entry
        {
            /// Returns a pre-formatted span of UTF8 encoded text representing
            /// the in-game date this entry was added to the journal.
<<<<<<< HEAD
            virtual std::string_view timestamp() const = 0;
=======
            virtual Utf8Span timestamp() const = 0;
>>>>>>> origin/main

            virtual ~JournalEntry() = default;
        };

        /// called prior to journal opening
        virtual void load() = 0;

        /// called prior to journal closing
        virtual void unload() = 0;

        /// returns true if their are no journal entries to display
        virtual bool isEmpty() const = 0;

        /// walks the active and optionally completed, quests providing the name and completed status
        virtual void visitQuestNames(bool activeOnly, std::function<void(std::string_view, bool)> visitor) const = 0;

        /// walks over the journal entries related to all quests with the given name
        /// If \a questName is empty, simply visits all journal entries
        virtual void visitJournalEntries(
            std::string_view questName, std::function<void(JournalEntry const&)> visitor) const = 0;

        /// provides the name of the topic specified by its id
<<<<<<< HEAD
        virtual void visitTopicName(
            const MWDialogue::Topic& topic, std::function<void(std::string_view)> visitor) const = 0;
=======
        virtual void visitTopicName(TopicId topicId, std::function<void(Utf8Span)> visitor) const = 0;
>>>>>>> origin/main

        /// walks over the topics whose names start with the character
        virtual void visitTopicNamesStartingWith(
            Utf8Stream::UnicodeChar character, std::function<void(std::string_view)> visitor) const = 0;

        /// walks over the topic entries for the topic specified by its identifier
<<<<<<< HEAD
        virtual void visitTopicEntries(
            const MWDialogue::Topic& topic, std::function<void(TopicEntry const&)> visitor) const = 0;

        // create an instance of the default journal view model implementation
        static std::shared_ptr<JournalViewModel> create();
=======
        virtual void visitTopicEntries(TopicId topicId, std::function<void(TopicEntry const&)> visitor) const = 0;

        // create an instance of the default journal view model implementation
        static Ptr create();
>>>>>>> origin/main

        virtual ~JournalViewModel() = default;
    };
}

#endif // MWGUI_JOURNALVIEWMODEL_HPP
