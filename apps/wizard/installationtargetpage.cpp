#include "installationtargetpage.hpp"

#include <string>

#include <QDebug>
#include <QFileDialog>
#include <QMessageBox>

#include <components/files/configurationmanager.hpp>
<<<<<<< HEAD
#include <components/files/qtconversion.hpp>
=======
#include <components/files/conversion.hpp>
>>>>>>> origin/main
#include <components/misc/scalableicon.hpp>

#include "mainwizard.hpp"

Wizard::InstallationTargetPage::InstallationTargetPage(QWidget* parent, const Files::ConfigurationManager& cfg)
    : QWizardPage(parent)
    , mCfgMgr(cfg)
{
    setupUi(this);
    connect(browseButton, &QPushButton::clicked, this, &InstallationTargetPage::browseButtonClicked);

    folderIcon->setIcon(Misc::ScalableIcon::load(":folder"));

<<<<<<< HEAD
    registerField(QStringLiteral("installation.path*"), targetLineEdit);
=======
    registerField(QLatin1String("installation.path*"), targetLineEdit);
>>>>>>> origin/main
}

void Wizard::InstallationTargetPage::initializePage()
{
<<<<<<< HEAD
    const QDir dir(Files::pathToQString(mCfgMgr.getUserDataPath() / "basedata"));
=======
    QString path(QFile::decodeName(Files::pathToUnicodeString(mCfgMgr.getUserDataPath()).c_str()));
    path.append(QDir::separator() + QLatin1String("basedata"));

    QDir dir(path);
>>>>>>> origin/main
    targetLineEdit->setText(QDir::toNativeSeparators(dir.absolutePath()));
}

bool Wizard::InstallationTargetPage::validatePage()
{
    const QString path(field(QStringLiteral("installation.path")).toString());

    qDebug() << "Validating path: " << path;

    if (!QFile::exists(path))
    {
        QDir dir;

        if (!dir.mkpath(path))
        {
<<<<<<< HEAD
            QMessageBox msgBox(this);
=======
            QMessageBox msgBox;
>>>>>>> origin/main
            msgBox.setWindowTitle(tr("Error creating destination"));
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setStandardButtons(QMessageBox::Ok);
            msgBox.setText(
                tr("<html><head/><body><p><b>Could not create the destination directory</b></p>"
                   "<p>Please make sure you have the right permissions "
                   "and try again, or specify a different location.</p></body></html>"));
            msgBox.exec();
            return false;
        }
    }

    const QFileInfo info(path);

    if (!info.isWritable())
    {
<<<<<<< HEAD
        QMessageBox msgBox(this);
=======
        QMessageBox msgBox;
>>>>>>> origin/main
        msgBox.setWindowTitle(tr("Insufficient permissions"));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.setText(
            tr("<html><head/><body><p><b>Could not write to the destination directory</b></p>"
               "<p>Please make sure you have the right permissions "
               "and try again, or specify a different location.</p></body></html>"));
        msgBox.exec();
        return false;
    }

<<<<<<< HEAD
    if (MainWizard::findFiles(QStringLiteral("Morrowind"), path))
    {
        QMessageBox msgBox(this);
=======
    if (mWizard->findFiles(QLatin1String("Morrowind"), path))
    {
        QMessageBox msgBox;
>>>>>>> origin/main
        msgBox.setWindowTitle(tr("Destination not empty"));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.setText(
            tr("<html><head/><body><p><b>The destination directory is not empty</b></p>"
               "<p>An existing Morrowind installation is present in the specified location.</p>"
               "<p>Please specify a different location, or go back and select the location as an existing "
               "installation.</p></body></html>"));
        msgBox.exec();
        return false;
    }

    return true;
}

void Wizard::InstallationTargetPage::browseButtonClicked()
{
<<<<<<< HEAD
    const QString selectedPath = QFileDialog::getExistingDirectory(this, tr("Select where to install Morrowind"),
=======
    QString selectedPath = QFileDialog::getExistingDirectory(this, tr("Select where to install Morrowind"),
>>>>>>> origin/main
        QDir::homePath(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    qDebug() << selectedPath;
    const QFileInfo info(selectedPath);
    if (info.exists() && info.isWritable())
        targetLineEdit->setText(info.absoluteFilePath());
}

int Wizard::InstallationTargetPage::nextId() const
{
    return MainWizard::Page_LanguageSelection;
}
