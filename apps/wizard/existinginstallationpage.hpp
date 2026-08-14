#ifndef EXISTINGINSTALLATIONPAGE_HPP
#define EXISTINGINSTALLATIONPAGE_HPP

#include "ui_existinginstallationpage.h"

namespace Wizard
{
    class MainWizard;

    class ExistingInstallationPage : public QWizardPage, private Ui::ExistingInstallationPage
    {
        Q_OBJECT
    public:
        ExistingInstallationPage(QWidget* parent);

        int nextId() const override;
        bool isComplete() const override;
        bool validatePage() override;

    private slots:
<<<<<<< HEAD
        void browseButtonClicked();
=======
        void on_browseButton_clicked();
>>>>>>> origin/main
        void textChanged(const QString& text);

    private:
        MainWizard* mWizard;
<<<<<<< HEAD
=======

        bool versionIsOK(QString directoryName);
>>>>>>> origin/main

    protected:
        void initializePage() override;
    };

}

#endif // EXISTINGINSTALLATIONPAGE_HPP
