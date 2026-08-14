#include "existinginstallationpage.hpp"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>

#include <components/misc/scalableicon.hpp>

#include "mainwizard.hpp"

<<<<<<< HEAD
namespace
{
    bool versionIsOK(QWidget* parent, const QString& directoryName)
    {
        const QFileInfoList infoList = QDir(directoryName).entryInfoList({ "Morrowind.bsa" });
        if (infoList.size() != 1)
            return false;

        const qint64 actualFileSize = infoList.at(0).size();

        // Size of Morrowind.bsa in Steam and GOG editions.
        constexpr qint64 expectedFileSize = 310459500;
        if (actualFileSize == expectedFileSize)
            return true;

        QMessageBox msgBox(parent);
        msgBox.setWindowTitle(QObject::tr("Most recent Morrowind not detected"));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        msgBox.setDefaultButton(QMessageBox::No);
        msgBox.setText(
            QObject::tr("<br><b>There may be a more recent version of Morrowind available.</b><br><br>"
                        "Do you wish to continue anyway?<br>"));
        return msgBox.exec() == QMessageBox::Yes;
    }
}

=======
>>>>>>> origin/main
Wizard::ExistingInstallationPage::ExistingInstallationPage(QWidget* parent)
    : QWizardPage(parent)
{
    mWizard = qobject_cast<MainWizard*>(parent);

    setupUi(this);

    browseButton->setIcon(Misc::ScalableIcon::load(":folder"));
    connect(browseButton, &QPushButton::clicked, this, &ExistingInstallationPage::browseButtonClicked);

    // Add a placeholder item to the list of installations
<<<<<<< HEAD
    QListWidgetItem* emptyItem = new QListWidgetItem(tr("No existing installations detected"), installationsList);
    emptyItem->setFlags(Qt::NoItemFlags);

    connect(installationsList, &QListWidget::currentTextChanged, this, &ExistingInstallationPage::textChanged);
    connect(installationsList, &QListWidget::itemSelectionChanged, this, &ExistingInstallationPage::completeChanged);
=======
    QListWidgetItem* emptyItem = new QListWidgetItem(tr("No existing installations detected"));
    emptyItem->setFlags(Qt::NoItemFlags);

    browseButton->setIcon(Misc::ScalableIcon::load(":folder"));

    installationsList->insertItem(0, emptyItem);
>>>>>>> origin/main
}

void Wizard::ExistingInstallationPage::initializePage()
{
    // Add the available installation paths
    const QStringList paths(mWizard->mInstallations.keys());

    // Hide the default item if there are installations to choose from
    installationsList->item(0)->setHidden(!paths.isEmpty());

    for (const QString& path : paths)
    {
        if (installationsList->findItems(path, Qt::MatchExactly).isEmpty())
        {
<<<<<<< HEAD
            installationsList->addItem(path);
        }
    }
=======
            QListWidgetItem* item = new QListWidgetItem(path);
            installationsList->addItem(item);
        }
    }

    connect(installationsList, &QListWidget::currentTextChanged, this, &ExistingInstallationPage::textChanged);

    connect(installationsList, &QListWidget::itemSelectionChanged, this, &ExistingInstallationPage::completeChanged);
>>>>>>> origin/main
}

bool Wizard::ExistingInstallationPage::validatePage()
{
    // See if Morrowind.ini is detected, if not, ask the user
    // It can be missing entirely
    // Or failed to be detected due to the target being a symlink

<<<<<<< HEAD
    const QString path(field(QStringLiteral("installation.path")).toString());
    QString& iniPath = mWizard->mInstallations[path].iniPath;

    if (QFile::exists(iniPath))
        return true;

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Error detecting Morrowind configuration"));
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setStandardButtons(QMessageBox::Cancel);
    msgBox.setText(
        tr("<br><b>Could not find Morrowind.ini</b><br><br>"
           "The Wizard needs to update settings in this file.<br><br>"
           "Press \"Browse...\" to specify the location manually.<br>"));
=======
    QString path(field(QLatin1String("installation.path")).toString());

    if (!QFile::exists(mWizard->mInstallations[path].iniPath))
    {
        QMessageBox msgBox;
        msgBox.setWindowTitle(tr("Error detecting Morrowind configuration"));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setStandardButtons(QMessageBox::Cancel);
        msgBox.setText(
            QObject::tr("<br><b>Could not find Morrowind.ini</b><br><br>"
                        "The Wizard needs to update settings in this file.<br><br>"
                        "Press \"Browse...\" to specify the location manually.<br>"));

        QAbstractButton* browseButton2 = msgBox.addButton(QObject::tr("B&rowse..."), QMessageBox::ActionRole);
>>>>>>> origin/main

    QAbstractButton* browseIniButton = msgBox.addButton(tr("B&rowse..."), QMessageBox::ActionRole);

<<<<<<< HEAD
    msgBox.exec();

    if (msgBox.clickedButton() != browseIniButton)
        return false;
=======
        QString iniFile;
        if (msgBox.clickedButton() == browseButton2)
        {
            iniFile = QFileDialog::getOpenFileName(this, QObject::tr("Select configuration file"), QDir::currentPath(),
                QString(tr("Morrowind configuration file (*.ini)")));
        }

        if (iniFile.isEmpty())
        {
            return false; // Cancel was clicked;
        }
>>>>>>> origin/main

    const QString iniFile = QFileDialog::getOpenFileName(
        this, tr("Select configuration file"), QDir::currentPath(), tr("Morrowind configuration file (*.ini)"));

    // Cancel was clicked
    if (iniFile.isEmpty())
        return false;

    // A proper Morrowind.ini was selected, set it
    iniPath = QFileInfo(iniFile).absoluteFilePath();
    return true;
}

void Wizard::ExistingInstallationPage::browseButtonClicked()
{
<<<<<<< HEAD
    const QString selectedFile = QFileDialog::getOpenFileName(this, tr("Select Morrowind.esm (located in Data Files)"),
        QDir::currentPath(), tr("Morrowind master file (Morrowind.esm)"), nullptr, QFileDialog::DontResolveSymlinks);
=======
    QString selectedFile
        = QFileDialog::getOpenFileName(this, tr("Select Morrowind.esm (located in Data Files)"), QDir::currentPath(),
            QString(tr("Morrowind master file (Morrowind.esm)")), nullptr, QFileDialog::DontResolveSymlinks);
>>>>>>> origin/main

    if (selectedFile.isEmpty())
        return;

    const QString path(QDir::toNativeSeparators(QFileInfo(selectedFile).absolutePath()));
    if (!MainWizard::findFiles(QStringLiteral("Morrowind"), path))
    {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Error detecting Morrowind files"));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.setText(
            tr("<b>Morrowind.bsa</b> is missing!<br>"
               "Make sure your Morrowind installation is complete."));
        msgBox.exec();
        return;
    }

    if (!versionIsOK(this, path))
        return;

<<<<<<< HEAD
    const QList<QListWidgetItem*> items = installationsList->findItems(path, Qt::MatchExactly);

=======
    if (!mWizard->findFiles(QLatin1String("Morrowind"), info.absolutePath()))
    {
        QMessageBox msgBox;
        msgBox.setWindowTitle(tr("Error detecting Morrowind files"));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.setText(
            QObject::tr("<b>Morrowind.bsa</b> is missing!<br>"
                        "Make sure your Morrowind installation is complete."));
        msgBox.exec();
        return;
    }

    if (!versionIsOK(info.absolutePath()))
    {
        return;
    }

    QString path(QDir::toNativeSeparators(info.absolutePath()));
    QList<QListWidgetItem*> items = installationsList->findItems(path, Qt::MatchExactly);

>>>>>>> origin/main
    if (items.isEmpty())
    {
        // Path is not yet in the list, add it
        mWizard->addInstallation(path);

        // Hide the default item
        installationsList->item(0)->setHidden(true);

<<<<<<< HEAD
        QListWidgetItem* item = new QListWidgetItem(path, installationsList);
=======
        QListWidgetItem* item = new QListWidgetItem(path);
        installationsList->addItem(item);
>>>>>>> origin/main
        installationsList->setCurrentItem(item); // Select it too
    }
    else
    {
        installationsList->setCurrentItem(items.first());
    }

    // Update the button
    emit completeChanged();
}

void Wizard::ExistingInstallationPage::textChanged(const QString& text)
{
    // Set the installation path manually, as registerField doesn't work
    // Because it doesn't accept two widgets operating on a single field
    if (!text.isEmpty())
        mWizard->setField(QStringLiteral("installation.path"), text);
}

bool Wizard::ExistingInstallationPage::isComplete() const
{
<<<<<<< HEAD
    return installationsList->selectionModel()->hasSelection();
=======
    if (installationsList->selectionModel()->hasSelection())
    {
        return true;
    }
    else
    {
        return false;
    }
>>>>>>> origin/main
}

int Wizard::ExistingInstallationPage::nextId() const
{
    return MainWizard::Page_LanguageSelection;
}

bool Wizard::ExistingInstallationPage::versionIsOK(QString directoryName)
{
    QDir directory = QDir(directoryName);
    QFileInfoList infoList = directory.entryInfoList(QStringList(QString("Morrowind.bsa")));
    if (infoList.size() == 1)
    {
        qint64 actualFileSize = infoList.at(0).size();
        const qint64 expectedFileSize = 310459500; // Size of Morrowind.bsa in Steam and GOG editions.

        if (actualFileSize == expectedFileSize)
        {
            return true;
        }

        QMessageBox msgBox;
        msgBox.setWindowTitle(QObject::tr("Most recent Morrowind not detected"));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        msgBox.setDefaultButton(QMessageBox::No);
        msgBox.setText(
            QObject::tr("<br><b>There may be a more recent version of Morrowind available.</b><br><br>"
                        "Do you wish to continue anyway?<br>"));
        int ret = msgBox.exec();
        if (ret == QMessageBox::Yes)
        {
            return true;
        }

        return false;
    }

    return false;
}
