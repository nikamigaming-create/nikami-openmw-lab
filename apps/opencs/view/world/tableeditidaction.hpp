#ifndef CSVWORLD_TABLEEDITIDACTION_HPP
#define CSVWORLD_TABLEEDITIDACTION_HPP

#include <QAction>
#include <QString>

#include <utility>

#include "../../model/world/columnbase.hpp"
#include "../../model/world/universalid.hpp"

class QTableView;

<<<<<<< HEAD
namespace CSMWorld
{
    class Data;
}

=======
>>>>>>> origin/main
namespace CSVWorld
{
    class TableEditIdAction : public QAction
    {
        const QTableView& mTable;
<<<<<<< HEAD
        CSMWorld::Data& mData;
        CSMWorld::UniversalId mCurrentId;

    public:
        TableEditIdAction(const QTableView& table, CSMWorld::Data& data, QWidget* parent = nullptr);

        bool setCell(int row, int column);

        CSMWorld::UniversalId getCurrentId() const;
=======
        CSMWorld::UniversalId mCurrentId;

        typedef std::pair<CSMWorld::ColumnBase::Display, QString> CellData;
        CellData getCellData(int row, int column) const;

    public:
        TableEditIdAction(const QTableView& table, QWidget* parent = nullptr);

        void setCell(int row, int column);

        CSMWorld::UniversalId getCurrentId() const;
        bool isValidIdCell(int row, int column) const;
>>>>>>> origin/main
    };
}

#endif
