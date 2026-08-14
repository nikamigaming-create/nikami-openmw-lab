#include "languageselectionpage.hpp"

#include <components/misc/scalableicon.hpp>

#include "mainwizard.hpp"

Wizard::LanguageSelectionPage::LanguageSelectionPage(QWidget* parent)
    : QWizardPage(parent)
{
    mWizard = qobject_cast<MainWizard*>(parent);

    setupUi(this);

    flagIcon->setIcon(Misc::ScalableIcon::load(":preferences-desktop-locale"));
<<<<<<< HEAD

    registerField(QStringLiteral("installation.language"), languageComboBox, "currentData", "currentDataChanged");

    const QList<std::pair<QString, QString>> languages = { { tr("English"), QStringLiteral("English") },
        { tr("French"), QStringLiteral("French") }, { tr("German"), QStringLiteral("German") },
        { tr("Italian"), QStringLiteral("Italian") }, { tr("Polish"), QStringLiteral("Polish") },
        { tr("Russian"), QStringLiteral("Russian") }, { tr("Spanish"), QStringLiteral("Spanish") } };

    for (const auto& [localizedName, name] : languages)
    {
        languageComboBox->addItem(localizedName, name);
=======

    registerField(QLatin1String("installation.language"), languageComboBox, "currentData", "currentDataChanged");
}

void Wizard::LanguageSelectionPage::initializePage()
{
    QVector<std::pair<QString, QString>> languages = { { "English", tr("English") }, { "French", tr("French") },
        { "German", tr("German") }, { "Italian", tr("Italian") }, { "Polish", tr("Polish") },
        { "Russian", tr("Russian") }, { "Spanish", tr("Spanish") } };

    for (auto lang : languages)
    {
        languageComboBox->addItem(lang.second, lang.first);
>>>>>>> origin/main
    }
}

int Wizard::LanguageSelectionPage::nextId() const
{
<<<<<<< HEAD
    if (!field(QStringLiteral("installation.retailDisc")).toBool())
    {
        const QString path(field(QStringLiteral("installation.path")).toString());
        if (!path.isEmpty())
        {
            const MainWizard::Installation& installation = mWizard->mInstallations[path];
            if (installation.hasMorrowind && installation.hasTribunal && installation.hasBloodmoon)
                return MainWizard::Page_Import;
=======
    if (field(QLatin1String("installation.retailDisc")).toBool() == true)
    {
        return MainWizard::Page_ComponentSelection;
    }
    else
    {
        QString path(field(QLatin1String("installation.path")).toString());

        if (path.isEmpty())
            return MainWizard::Page_ComponentSelection;

        // Check if we have to install something
        if (mWizard->mInstallations[path].hasMorrowind == true && mWizard->mInstallations[path].hasTribunal == true
            && mWizard->mInstallations[path].hasBloodmoon == true)
        {
            return MainWizard::Page_Import;
        }
        else
        {
            return MainWizard::Page_ComponentSelection;
>>>>>>> origin/main
        }
    }

    return MainWizard::Page_ComponentSelection;
}
