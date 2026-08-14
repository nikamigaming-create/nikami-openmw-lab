#ifndef SETTINGSPAGE_H
#define SETTINGSPAGE_H

#include <QCompleter>
#include <QStringListModel>
<<<<<<< HEAD

#include <components/files/configurationmanager.hpp>
=======
>>>>>>> origin/main

#include "ui_settingspage.h"

namespace Config
{
    class GameSettings;
}

namespace Launcher
{
    class SettingsPage : public QWidget, private Ui::SettingsPage
    {
        Q_OBJECT

    public:
<<<<<<< HEAD
        explicit SettingsPage(const Files::ConfigurationManager& configurationManager,
            Config::GameSettings& gameSettings, QWidget* parent = nullptr);
=======
        explicit SettingsPage(Config::GameSettings& gameSettings, QWidget* parent = nullptr);
>>>>>>> origin/main

        bool loadSettings();
        void saveSettings();

    public slots:
        void slotLoadedCellsChanged(QStringList cellNames);

    private slots:
        void on_skipMenuCheckBox_stateChanged(int state);
        void on_runScriptAfterStartupBrowseButton_clicked();
        void slotAnimSourcesToggled(bool checked);
        void slotPostProcessToggled(bool checked);
        void slotSkyBlendingToggled(bool checked);
        void slotShadowDistLimitToggled(bool checked);
        void slotDistantLandToggled(bool checked);
        void slotControllerMenusToggled(bool checked);
<<<<<<< HEAD
        void slotOpenFile(QTreeWidgetItem* item);

    private:
        void populateLoadedConfigs();

        const Files::ConfigurationManager& mCfgMgr;

        Config::GameSettings& mGameSettings;
        QCompleter mCellNameCompleter;
        QStringListModel mCellNameCompleterModel;

=======

    private:
        Config::GameSettings& mGameSettings;
        QCompleter mCellNameCompleter;
        QStringListModel mCellNameCompleterModel;

>>>>>>> origin/main
        /**
         * Load the cells associated with the given content files for use in autocomplete
         * @param filePaths the file paths of the content files to be examined
         */
        void loadCellsForAutocomplete(QStringList filePaths);
    };
}
#endif
