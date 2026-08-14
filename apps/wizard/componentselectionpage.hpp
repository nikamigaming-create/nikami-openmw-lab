#ifndef COMPONENTSELECTIONPAGE_HPP
#define COMPONENTSELECTIONPAGE_HPP

#include "ui_componentselectionpage.h"

namespace Wizard
{
    class MainWizard;

    class ComponentSelectionPage : public QWizardPage, private Ui::ComponentSelectionPage
    {
        Q_OBJECT
    public:
        ComponentSelectionPage(QWidget* parent);

        int nextId() const override;
        bool validatePage() override;

    private slots:
<<<<<<< HEAD
        void updateButton();
=======
        void updateButton(QListWidgetItem* item);
>>>>>>> origin/main

    private:
        MainWizard* mWizard;

    protected:
        void initializePage() override;
    };

}

#endif // COMPONENTSELECTIONPAGE_HPP
