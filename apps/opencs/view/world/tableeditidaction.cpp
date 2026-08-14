#include "tableeditidaction.hpp"

#include <QTableView>

<<<<<<< HEAD
#include <components/esm/refid.hpp>

#include "../../model/world/data.hpp"
#include "../../model/world/idtable.hpp"
#include "../../model/world/tablemimedata.hpp"

CSVWorld::TableEditIdAction::TableEditIdAction(const QTableView& table, CSMWorld::Data& data, QWidget* parent)
    : QAction(parent)
    , mTable(table)
    , mData(data)
=======
#include <memory>
#include <type_traits>

#include "../../model/world/tablemimedata.hpp"

#include <apps/opencs/model/world/columnbase.hpp>
#include <apps/opencs/model/world/universalid.hpp>

CSVWorld::TableEditIdAction::CellData CSVWorld::TableEditIdAction::getCellData(int row, int column) const
{
    QModelIndex index = mTable.model()->index(row, column);
    if (index.isValid())
    {
        QVariant display = mTable.model()->data(index, CSMWorld::ColumnBase::Role_Display);
        QString value = mTable.model()->data(index).toString();
        return std::make_pair(static_cast<CSMWorld::ColumnBase::Display>(display.toInt()), value);
    }
    return std::make_pair(CSMWorld::ColumnBase::Display_None, "");
}

CSVWorld::TableEditIdAction::TableEditIdAction(const QTableView& table, QWidget* parent)
    : QAction(parent)
    , mTable(table)
>>>>>>> origin/main
    , mCurrentId(CSMWorld::UniversalId::Type_None)
{
}

<<<<<<< HEAD
bool CSVWorld::TableEditIdAction::setCell(int row, int column)
{
    QModelIndex index = mTable.model()->index(row, column);
    if (!index.isValid())
        return false;

    QVariant displayVar = index.data(CSMWorld::ColumnBase::Role_Display);
    const auto display = static_cast<CSMWorld::ColumnBase::Display>(displayVar.toInt());
    const QString name = index.data(Qt::DisplayRole).toString();

    if (!CSMWorld::ColumnBase::isId(display) || name.isEmpty())
        return false;

    CSMWorld::UniversalId::Type idType = CSMWorld::TableMimeData::convertEnums(display);
    if (idType == CSMWorld::UniversalId::Type_None)
        return false;

    mCurrentId = CSMWorld::UniversalId(idType, name.toStdString());
    if (mCurrentId.getClass() == CSMWorld::UniversalId::Class_Resource)
        return false;

    if (mCurrentId.getClass() == CSMWorld::UniversalId::Class_RefRecord)
        idType = CSMWorld::UniversalId::Type_Referenceable;

    CSMWorld::IdTable* idModel = dynamic_cast<CSMWorld::IdTable*>(mData.getTableModel(idType));
    if (idModel && !idModel->getModelIndex(mCurrentId.getId(), 0).isValid())
        return false;

    setText("Edit '" + name + "'");
    return true;
=======
void CSVWorld::TableEditIdAction::setCell(int row, int column)
{
    CellData data = getCellData(row, column);
    CSMWorld::UniversalId::Type idType = CSMWorld::TableMimeData::convertEnums(data.first);

    if (idType != CSMWorld::UniversalId::Type_None)
    {
        mCurrentId = CSMWorld::UniversalId(idType, data.second.toUtf8().constData());
        setText("Edit '" + data.second + "'");
    }
>>>>>>> origin/main
}

CSMWorld::UniversalId CSVWorld::TableEditIdAction::getCurrentId() const
{
    return mCurrentId;
}
<<<<<<< HEAD
=======

bool CSVWorld::TableEditIdAction::isValidIdCell(int row, int column) const
{
    CellData data = getCellData(row, column);
    CSMWorld::UniversalId::Type idType = CSMWorld::TableMimeData::convertEnums(data.first);
    return CSMWorld::ColumnBase::isId(data.first) && idType != CSMWorld::UniversalId::Type_None
        && !data.second.isEmpty();
}
>>>>>>> origin/main
