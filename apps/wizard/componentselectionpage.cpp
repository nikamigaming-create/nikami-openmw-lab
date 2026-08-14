#include "componentselectionpage.hpp"

#include <QMessageBox>
#include <QPushButton>

#include "mainwizard.hpp"

Wizard::ComponentSelectionPage::ComponentSelectionPage(QWidget* parent)
    : QWizardPage(parent)
{
    mWizard = qobject_cast<MainWizard*>(parent);

    setupUi(this);

    setCommitPage(true);
    setButtonText(QWizard::CommitButton, tr("&Install"));

<<<<<<< HEAD
    registerField(QStringLiteral("installation.installMorrowind"), morrowindCheckbox);
    registerField(QStringLiteral("installation.installTribunal"), tribunalCheckbox);
    registerField(QStringLiteral("installation.installBloodmoon"), bloodmoonCheckbox);

    connect(morrowindCheckbox, &QCheckBox::toggled, this, &ComponentSelectionPage::updateButton);
    connect(tribunalCheckbox, &QCheckBox::toggled, this, &ComponentSelectionPage::updateButton);
    connect(bloodmoonCheckbox, &QCheckBox::toggled, this, &ComponentSelectionPage::updateButton);
}

void Wizard::ComponentSelectionPage::updateButton()
{
    if (field(QStringLiteral("installation.retailDisc")).toBool())
        return;

    if (!morrowindCheckbox->isChecked() && !tribunalCheckbox->isChecked() && !bloodmoonCheckbox->isChecked())
=======
    registerField(QLatin1String("installation.components"), componentsList);

    connect(componentsList, &ComponentListWidget::itemChanged, this, &ComponentSelectionPage::updateButton);
}

void Wizard::ComponentSelectionPage::updateButton(QListWidgetItem*)
{
    if (field(QLatin1String("installation.retailDisc")).toBool() == true)
        return; // Morrowind is always checked here

    bool unchecked = true;

    for (int i = 0; i < componentsList->count(); ++i)
    {
        QListWidgetItem* item = componentsList->item(i);

        if (!item)
            continue;

        if (item->checkState() == Qt::Checked)
        {
            unchecked = false;
        }
    }

    if (unchecked)
>>>>>>> origin/main
    {
        setCommitPage(false);
        setButtonText(QWizard::NextButton, tr("&Skip"));
    }
    else
    {
        setCommitPage(true);
    }
}

void Wizard::ComponentSelectionPage::initializePage()
{
    const bool retailDisc = field(QStringLiteral("installation.retailDisc")).toBool();

<<<<<<< HEAD
    bool hasMorrowind = false;
    bool hasTribunal = false;
    bool hasBloodmoon = false;
    if (!retailDisc)
    {
        const QString path = field(QStringLiteral("installation.path")).toString();
        const MainWizard::Installation& installation = mWizard->mInstallations[path];
        hasMorrowind = installation.hasMorrowind;
        hasTribunal = installation.hasTribunal;
        hasBloodmoon = installation.hasBloodmoon;
=======
    QString path(field(QLatin1String("installation.path")).toString());

    QListWidgetItem* morrowindItem = new QListWidgetItem(QLatin1String("Morrowind"));
    QListWidgetItem* tribunalItem = new QListWidgetItem(QLatin1String("Tribunal"));
    QListWidgetItem* bloodmoonItem = new QListWidgetItem(QLatin1String("Bloodmoon"));

    if (field(QLatin1String("installation.retailDisc")).toBool() == true)
    {
        morrowindItem->setFlags((morrowindItem->flags() & ~Qt::ItemIsEnabled) | Qt::ItemIsUserCheckable);
        morrowindItem->setData(Qt::CheckStateRole, Qt::Checked);
        componentsList->addItem(morrowindItem);

        tribunalItem->setFlags(tribunalItem->flags() | Qt::ItemIsUserCheckable);
        tribunalItem->setData(Qt::CheckStateRole, Qt::Checked);
        componentsList->addItem(tribunalItem);

        bloodmoonItem->setFlags(bloodmoonItem->flags() | Qt::ItemIsUserCheckable);
        bloodmoonItem->setData(Qt::CheckStateRole, Qt::Checked);
        componentsList->addItem(bloodmoonItem);
    }
    else
    {

        if (mWizard->mInstallations[path].hasMorrowind)
        {
            morrowindItem->setText(tr("Morrowind\t\t(installed)"));
            morrowindItem->setFlags((morrowindItem->flags() & ~Qt::ItemIsEnabled) | Qt::ItemIsUserCheckable);
            morrowindItem->setData(Qt::CheckStateRole, Qt::Unchecked);
        }
        else
        {
            morrowindItem->setText(tr("Morrowind"));
            morrowindItem->setData(Qt::CheckStateRole, Qt::Checked);
        }

        componentsList->addItem(morrowindItem);

        if (mWizard->mInstallations[path].hasTribunal)
        {
            tribunalItem->setText(tr("Tribunal\t\t(installed)"));
            tribunalItem->setFlags((tribunalItem->flags() & ~Qt::ItemIsEnabled) | Qt::ItemIsUserCheckable);
            tribunalItem->setData(Qt::CheckStateRole, Qt::Unchecked);
        }
        else
        {
            tribunalItem->setText(tr("Tribunal"));
            tribunalItem->setData(Qt::CheckStateRole, Qt::Checked);
        }

        componentsList->addItem(tribunalItem);

        if (mWizard->mInstallations[path].hasBloodmoon)
        {
            bloodmoonItem->setText(tr("Bloodmoon\t\t(installed)"));
            bloodmoonItem->setFlags((bloodmoonItem->flags() & ~Qt::ItemIsEnabled) | Qt::ItemIsUserCheckable);
            bloodmoonItem->setData(Qt::CheckStateRole, Qt::Unchecked);
        }
        else
        {
            bloodmoonItem->setText(tr("Bloodmoon"));
            bloodmoonItem->setData(Qt::CheckStateRole, Qt::Checked);
        }

        componentsList->addItem(bloodmoonItem);
>>>>>>> origin/main
    }

    morrowindCheckbox->setText(hasMorrowind ? tr("Morrowind\t\t(installed)") : tr("Morrowind"));
    morrowindCheckbox->setChecked(!hasMorrowind);
    morrowindCheckbox->setEnabled(!hasMorrowind && !retailDisc);
    tribunalCheckbox->setText(hasTribunal ? tr("Tribunal\t\t(installed)") : tr("Tribunal"));
    tribunalCheckbox->setChecked(!hasTribunal);
    tribunalCheckbox->setEnabled(!hasTribunal);
    bloodmoonCheckbox->setText(hasBloodmoon ? tr("Bloodmoon\t\t(installed)") : tr("Bloodmoon"));
    bloodmoonCheckbox->setChecked(!hasBloodmoon);
    bloodmoonCheckbox->setEnabled(!hasBloodmoon);
}

bool Wizard::ComponentSelectionPage::validatePage()
{
    if (field(QStringLiteral("installation.retailDisc")).toBool())
        return true;

<<<<<<< HEAD
    const QString path = field(QStringLiteral("installation.path")).toString();
    MainWizard::Installation& installation = mWizard->mInstallations[path];

    bool installingTribunal = field(QStringLiteral("installation.installTribunal")).toBool();
    bool installingBloodmoon = field(QStringLiteral("installation.installBloodmoon")).toBool();

    if (installingTribunal && !installingBloodmoon && installation.hasBloodmoon)
    {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("About to install Tribunal after Bloodmoon"));
        msgBox.setIcon(QMessageBox::Information);
        msgBox.setStandardButtons(QMessageBox::Cancel);
        msgBox.setText(
            tr("<html><head/><body><p><b>You are about to install Tribunal</b></p>"
               "<p>Bloodmoon is already installed on your computer.</p>"
               "<p>However, it is recommended that you install Tribunal before Bloodmoon.</p>"
               "<p>Would you like to re-install Bloodmoon?</p></body></html>"));

        QAbstractButton* reinstallButton = msgBox.addButton(tr("Re-install &Bloodmoon"), QMessageBox::ActionRole);
        msgBox.exec();

        if (msgBox.clickedButton() == reinstallButton)
        {
            installation.hasBloodmoon = false;
            bloodmoonCheckbox->setText(tr("Bloodmoon"));
            bloodmoonCheckbox->setChecked(true);
            bloodmoonCheckbox->setEnabled(true);
=======
    if (field(QLatin1String("installation.retailDisc")).toBool() == false)
    {
        if (components.contains(QLatin1String("Tribunal")) && !components.contains(QLatin1String("Bloodmoon")))
        {
            if (mWizard->mInstallations[path].hasBloodmoon)
            {
                QMessageBox msgBox;
                msgBox.setWindowTitle(tr("About to install Tribunal after Bloodmoon"));
                msgBox.setIcon(QMessageBox::Information);
                msgBox.setStandardButtons(QMessageBox::Cancel);
                msgBox.setText(
                    tr("<html><head/><body><p><b>You are about to install Tribunal</b></p>"
                       "<p>Bloodmoon is already installed on your computer.</p>"
                       "<p>However, it is recommended that you install Tribunal before Bloodmoon.</p>"
                       "<p>Would you like to re-install Bloodmoon?</p></body></html>"));

                QAbstractButton* reinstallButton
                    = msgBox.addButton(tr("Re-install &Bloodmoon"), QMessageBox::ActionRole);
                msgBox.exec();

                if (msgBox.clickedButton() == reinstallButton)
                {
                    // Force reinstallation
                    mWizard->mInstallations[path].hasBloodmoon = false;
                    QList<QListWidgetItem*> items
                        = componentsList->findItems(QLatin1String("Bloodmoon"), Qt::MatchStartsWith);

                    for (QListWidgetItem* item : items)
                    {
                        item->setText(QLatin1String("Bloodmoon"));
                        item->setCheckState(Qt::Checked);
                    }

                    return true;
                }
            }
>>>>>>> origin/main
        }
    }

    return true;
}

int Wizard::ComponentSelectionPage::nextId() const
{
#ifdef OPENMW_USE_UNSHIELD
    if (isCommitPage())
<<<<<<< HEAD
        return MainWizard::Page_Installation;
#endif
    return MainWizard::Page_Import;
=======
    {
        return MainWizard::Page_Installation;
    }
    else
    {
        return MainWizard::Page_Import;
    }
#else
    return MainWizard::Page_Import;
#endif
>>>>>>> origin/main
}
