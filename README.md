[[_TOC_]]

OpenMW
======

OpenMW is an open-source open-world RPG game engine that supports playing Morrowind by Bethesda Softworks. You need to own the game for OpenMW to play Morrowind.

OpenMW also comes with OpenMW-CS, a replacement for Bethesda's Construction Set.

* Version: 0.50.0
* License: GPLv3 (see [LICENSE](https://gitlab.com/OpenMW/openmw/-/raw/master/LICENSE) for more information)
* Website: https://www.openmw.org
* IRC: #openmw on irc.libera.chat
* Discord: https://discord.gg/bWuqq2e


Font Licenses:
* DejaVuLGCSansMono.ttf: custom (see [files/data/fonts/DejaVuFontLicense.txt](https://gitlab.com/OpenMW/openmw/-/raw/master/files/data/fonts/DejaVuFontLicense.txt) for more information)
* DemonicLetters.ttf: SIL Open Font License (see [files/data/fonts/DemonicLettersFontLicense.txt](https://gitlab.com/OpenMW/openmw/-/raw/master/files/data/fonts/DemonicLettersFontLicense.txt) for more information)
* MysticCards.ttf: SIL Open Font License (see [files/data/fonts/MysticCardsFontLicense.txt](https://gitlab.com/OpenMW/openmw/-/raw/master/files/data/fonts/MysticCardsFontLicense.txt) for more information)

VR
--
[Read the docs](https://openmw-vr.readthedocs.io/en/latest/index.html) to get started with playing VR

Playing Fallout: New Vegas (experimental)
------------------------------------------

Windows prereleases provide two installers:

* `OpenMW-Flat-*-win64.exe` for normal monitor, keyboard, mouse, or controller play.
* `OpenMW-VR-*-win64.exe` for OpenXR headsets and VR controllers.

Use the Flat installer unless you specifically want to play in VR.

Launching a game
----------------

The installer variant selects the display mode; the launcher Content List selects the game:

1. Start `openmw-launcher.exe` from the installation folder and open **Data Files**.
2. Select **New Content List** and name it for the game, for example `Morrowind` or `Fallout New Vegas`.
3. Add that game's data directory, archives, and master/plugin files to the new Content List.
4. Select the desired Content List before pressing **Play**. The Flat launcher starts `openmw.exe`; the VR launcher
   starts `openmw_vr.exe`.
5. Directly starting `openmw.exe` or `openmw_vr.exe` later uses the Content List most recently selected and saved by
   the launcher.

For Morrowind, add the folder containing `Morrowind.esm`, enable `Morrowind.esm`, and enable `Morrowind.bsa`. If
installed, load `Tribunal.esm` and `Bloodmoon.esm` after the base game with their matching archives. Morrowind is the
stable, upstream-supported game path.

Fallout: New Vegas uses the experimental setup below. The engine can identify `Oblivion.esm`, `Fallout3.esm`,
`Skyrim.esm`/`SkyrimVR.esm`, `Fallout4.esm`, and `Starfield.esm` for ongoing parser and world-viewer development, but
this release does not claim those games are playable yet.

This fork contains work-in-progress Fallout: New Vegas support. It does not include the game or any Bethesda assets;
you must own and install Fallout: New Vegas separately. Expect incomplete gameplay and keep saves made by this build
separate from saves you care about.

Engine contributors can follow the corpus-derived command and event status in
[the FNV retail script coverage report](docs/fnv-obscript-coverage.md). A cell
rendering successfully does not mean its quests, dialogue, AI, combat, or
inventory scripts are complete.

1. Download the appropriate Windows installer from this repository's
   [Releases](https://github.com/nikamigaming-create/nikami-openmw-lab/releases) page and install it to a new folder.
2. Install Fallout: New Vegas with Steam or GOG and run the original launcher once. A typical Steam data folder is
   `C:\Program Files (x86)\Steam\steamapps\common\Fallout New Vegas\Data`.
3. Run `openmw-launcher.exe` from the installation folder. On **Data Files**, add the Fallout: New Vegas `Data` folder.
4. Enable `FalloutNV.esm`. For a new game, enable owned DLC after the base game. For a native `.fos` save, the Content
   List must instead match the save's embedded master table exactly; the preflight error reports the first mismatched
   index without modifying the session. Master order can vary between saves, especially for `GunRunnersArsenal.esm`
   and the Courier's Stash packs, so do not assume a generic DLC order when loading an existing save.
5. Enable the matching `Fallout - *.bsa` archives shown by the launcher, including meshes, textures, sounds, and
   voices. Enable DLC archives only for DLC you own and enabled in step 4.
6. Save the Content List, select it, and press **Play**. You can launch the same configured Content List later with
   `openmw.exe` for Flat or `openmw_vr.exe` for VR.

The experimental FNV main menu is not yet a reliable launch path. To load an existing save directly, start the engine
with `--load-savegame "C:\path\to\Save Name.fos"` after saving the matching FNV Content List in the launcher. The engine
preflights the native save before replacing the current session and stops with a precise error if the master order or
required compatibility state does not match.

If the launcher does not list `FalloutNV.esm`, verify that the selected directory is the game's `Data` directory,
not the directory containing it. Do not copy Fallout files into this repository or attach them to bug reports.

Current Status
--------------

The main quests in Morrowind, Tribunal and Bloodmoon are all completable. Some issues with side quests are to be expected (but rare). Check the [bug tracker](https://gitlab.com/OpenMW/openmw/-/issues/?milestone_title=openmw-1.0) for a list of issues we need to resolve before the "1.0" release. Even before the "1.0" release however, OpenMW boasts some new [features](https://wiki.openmw.org/index.php?title=Features), such as improved graphics and user interfaces.

Pre-existing modifications created for the original Morrowind engine can be hit-and-miss. The OpenMW script compiler performs more thorough error-checking than Morrowind does, meaning that a mod created for Morrowind may not necessarily run in OpenMW. Some mods also rely on quirky behaviour or engine bugs in order to work. We are considering such compatibility issues on a case-by-case basis - in some cases adding a workaround to OpenMW may be feasible, in other cases fixing the mod will be the only option. If you know of any mods that work or don't work, feel free to add them to the [Mod status](https://wiki.openmw.org/index.php?title=Mod_status) wiki page.

Getting Started
---------------

* [Official forums](https://forum.openmw.org/)
* [Installation instructions](https://openmw.readthedocs.io/en/latest/manuals/installation/index.html)
* [Build from source](https://wiki.openmw.org/index.php?title=Development_Environment_Setup)
* [Testing the game](https://wiki.openmw.org/index.php?title=Testing)
* [How to contribute](https://wiki.openmw.org/index.php?title=Contribution_Wanted)
* [Report a bug](https://gitlab.com/OpenMW/openmw/issues) - read the [guidelines](https://wiki.openmw.org/index.php?title=Bug_Reporting_Guidelines) before submitting your first bug!
* [Known issues](https://gitlab.com/OpenMW/openmw/issues?label_name%5B%5D=Bug)

The data path
-------------

The data path tells OpenMW where to find your Morrowind files. If you run the launcher, OpenMW should be able to pick up the location of these files on its own, if both Morrowind and OpenMW are installed properly (installing Morrowind under WINE is considered a proper install).

Command line options
--------------------

    Syntax: openmw <options>
    Allowed options:
      --help                                print help message
      --version                             print version information and quit
      --data arg (=data)                    set data directories (later directories
                                            have higher priority)
      --data-local arg                      set local data directory (highest
                                            priority)
      --fallback-archive arg (=fallback-archive)
                                            set fallback BSA archives (later
                                            archives have higher priority)
      --resources arg (=resources)          set resources directory
      --start arg                           set initial cell
      --content arg                         content file(s): esm/esp, or
                                            omwgame/omwaddon
      --no-sound [=arg(=1)] (=0)            disable all sounds
      --script-verbose [=arg(=1)] (=0)      verbose script output
      --script-all [=arg(=1)] (=0)          compile all scripts (excluding dialogue
                                            scripts) at startup
      --script-all-dialogue [=arg(=1)] (=0) compile all dialogue scripts at startup
      --script-console [=arg(=1)] (=0)      enable console-only script
                                            functionality
      --script-run arg                      select a file containing a list of
                                            console commands that is executed on
                                            startup
      --script-warn [=arg(=1)] (=1)         handling of warnings when compiling
                                            scripts
                                            0 - ignore warning
                                            1 - show warning but consider script as
                                            correctly compiled anyway
                                            2 - treat warnings as errors
      --load-savegame arg                   load a save game file on game startup
                                            (specify an absolute filename or a
                                            filename relative to the current
                                            working directory)
      --skip-menu [=arg(=1)] (=0)           skip main menu on game startup
      --new-game [=arg(=1)] (=0)            run new game sequence (ignored if
                                            skip-menu=0)
      --encoding arg (=win1252)             Character encoding used in OpenMW game
                                            messages:

                                            win1250 - Central and Eastern European
                                            such as Polish, Czech, Slovak,
                                            Hungarian, Slovene, Bosnian, Croatian,
                                            Serbian (Latin script), Romanian and
                                            Albanian languages

                                            win1251 - Cyrillic alphabet such as
                                            Russian, Bulgarian, Serbian Cyrillic
                                            and other languages

                                            win1252 - Western European (Latin)
                                            alphabet, used by default
      --fallback arg                        fallback values
      --no-grab                             Don't grab mouse cursor
      --export-fonts [=arg(=1)] (=0)        Export Morrowind .fnt fonts to PNG
                                            image and XML file in current directory
      --activate-dist arg (=-1)             activation distance override
      --random-seed arg (=<impl defined>)   seed value for random number generator
