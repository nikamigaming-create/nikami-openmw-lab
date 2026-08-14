#ifndef CSV_TOOLS_SEARCHBOX_H
#define CSV_TOOLS_SEARCHBOX_H

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QWidget>

class QGridLayout;

namespace CSMTools
{
    class Search;
}

namespace CSVTools
{
    class SearchBox : public QWidget
    {
        Q_OBJECT

        QStackedWidget mInput;
        QLineEdit mText;
        QComboBox mRecordState;
        QCheckBox mCaseSensitive;
        QPushButton mSearch;
        QGridLayout* mLayout;
        QComboBox mMode;
        bool mSearchEnabled;
<<<<<<< HEAD
        bool mAllowReplace{ false };
=======
>>>>>>> origin/main
        QStackedWidget mReplaceInput;
        QLineEdit mReplaceText;
        QLabel mReplacePlaceholder;
        QPushButton mReplace;

    private:
<<<<<<< HEAD
        int mSearchResultCount = 0;

        void updateSearchButtons();
=======
        void updateSearchButton();
>>>>>>> origin/main

    public:
        SearchBox(QWidget* parent = nullptr);

<<<<<<< HEAD
        void setEditLock(bool locked);

        void setSearchMode(bool enabled);

        void setSearchResultCount(int resultCount);

=======
        void setSearchMode(bool enabled);

>>>>>>> origin/main
        CSMTools::Search getSearch() const;

        std::string getReplaceText() const;

<<<<<<< HEAD
=======
        void setEditLock(bool locked);

>>>>>>> origin/main
        void focus();

    private slots:

        void modeSelected(int index);

        void textChanged(const QString& text);

        void startSearch(bool checked = true);

        void replaceAll(bool checked);

    signals:

        void startSearch(const CSMTools::Search& search);

        void replaceAll();
    };
}

#endif
