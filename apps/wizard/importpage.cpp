#include "importpage.hpp"

#include "mainwizard.hpp"

Wizard::ImportPage::ImportPage(QWidget* parent)
    : QWizardPage(parent)
{
    mWizard = qobject_cast<MainWizard*>(parent);

    setupUi(this);

<<<<<<< HEAD
    registerField(QStringLiteral("installation.import-settings"), importCheckBox);
    registerField(QStringLiteral("installation.import-addons"), addonsCheckBox);
    registerField(QStringLiteral("installation.import-fonts"), fontsCheckBox);
=======
    registerField(QLatin1String("installation.import-settings"), importCheckBox);
    registerField(QLatin1String("installation.import-addons"), addonsCheckBox);
    registerField(QLatin1String("installation.import-fonts"), fontsCheckBox);
>>>>>>> origin/main
}

int Wizard::ImportPage::nextId() const
{
    return MainWizard::Page_Conclusion;
}
