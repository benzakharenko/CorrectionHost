#include <juce_gui_extra/juce_gui_extra.h>

#include "AppIcon.h"
#include "HostEngine.h"
#include "MainComponent.h"
#include "Updater.h"

//==============================================================================
class CorrectionHostApplication;

namespace
{
    /** Pixel size for the tray icon: Windows wants its small-icon size at the current
        display scale (16 px at 100 %, 24 px at 150 %...), macOS scales a larger image down. */
    int trayIconPixels()
    {
       #if JUCE_WINDOWS
        double scale = 1.0;
        if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            scale = display->scale;
        return juce::roundToInt (16.0 * scale);
       #else
        return 64;
       #endif
    }
}

//==============================================================================
class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow (const juce::String& name, HostEngine& engine, juce::PropertiesFile& props, std::function<bool()> trayAvailable)
        : DocumentWindow (name,
                          juce::LookAndFeel::getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
                          DocumentWindow::closeButton | DocumentWindow::minimiseButton),
          hasTray (std::move (trayAvailable))
    {
        setUsingNativeTitleBar (true);
        content = new MainComponent (engine, props);
        setContentOwned (content, true);
        setResizable (true, false);
        setResizeLimits (640, 740, 1600, 1400);
        centreWithSize (getWidth(), getHeight());
    }

    void minimisationStateChanged (bool isNowMinimised) override
    {
        DocumentWindow::minimisationStateChanged (isNowMinimised);

        if (! isNowMinimised || ! content->shouldMinimiseToTray() || ! hasTray || ! hasTray())
            return;

        // Hide once the minimise has finished, so the window leaves the taskbar and only the
        // tray icon remains. Clicking the tray icon restores it.
        juce::Component::SafePointer<MainWindow> safeThis (this);
        juce::MessageManager::callAsync ([safeThis]
        {
            if (safeThis != nullptr && safeThis->isMinimised())
            {
                safeThis->content->saveState();
                safeThis->setVisible (false);
            }
        });
    }

    void closeButtonPressed() override
    {
        if (content->shouldCloseToTray() && hasTray && hasTray())
        {
            content->saveState();
            setVisible (false);
        }
        else
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    }

    MainComponent& getMainComponent()   { return *content; }

private:
    MainComponent* content = nullptr;   // owned by the window
    std::function<bool()> hasTray;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

//==============================================================================
class TrayIcon final : public juce::SystemTrayIconComponent,
                       private juce::Timer
{
public:
    struct Actions
    {
        std::function<void()> showWindow;
        std::function<bool()> isBypassed;
        std::function<void (bool)> setBypassed;
        std::function<void()> quit;
        std::function<void()> checkForUpdates;
        std::function<bool()> canUpdate;
    };

    explicit TrayIcon (Actions a) : actions (std::move (a))
    {
        refreshIcon (true);
        statePoller.owner = this;
        statePoller.startTimer (500);
    }

    /** Swaps to the grey icon while bypassed, and redraws if the display scale changed. */
    void refreshIcon (bool force = false)
    {
        const bool bypassed = actions.isBypassed != nullptr && actions.isBypassed();
        const int pixels = trayIconPixels();

        if (! force && bypassed == shownBypassed && pixels == shownPixels)
            return;

        shownBypassed = bypassed;
        shownPixels = pixels;

        setIconImage (AppIcon::render (pixels, bypassed ? AppIcon::Style::bypassed : AppIcon::Style::colour),
                      AppIcon::render (pixels, AppIcon::Style::monochrome));
        setIconTooltip (bypassed ? "Correction Host (correction bypassed)" : "Correction Host");
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
       #if JUCE_WINDOWS
        if (e.mods.isLeftButtonDown() && ! e.mods.isPopupMenu())
        {
            actions.showWindow();
            return;
        }
       #else
        juce::ignoreUnused (e);
       #endif

        // Bring the app forward first so the menu behaves, then open it a moment later.
        juce::Process::makeForegroundProcess();
        startTimer (50);
    }

private:
    void timerCallback() override
    {
        stopTimer();

        juce::PopupMenu menu;
        menu.addItem ("Show window", [this] { actions.showWindow(); });
        menu.addItem ("Bypass correction", true, actions.isBypassed(),
                      [this] { actions.setBypassed (! actions.isBypassed()); });
        menu.addSeparator();
        if (actions.canUpdate != nullptr && actions.canUpdate())
            menu.addItem ("Check for updates", [this] { actions.checkForUpdates(); });
        menu.addItem ("Quit", [this] { actions.quit(); });

       #if JUCE_MAC
        showDropdownMenu (menu);
       #else
        menu.showMenuAsync (juce::PopupMenu::Options());
       #endif
    }

    struct StatePoller final : juce::Timer
    {
        TrayIcon* owner = nullptr;
        void timerCallback() override   { owner->refreshIcon(); }
    };

    Actions actions;
    StatePoller statePoller;
    bool shownBypassed = false;
    int shownPixels = 0;
};

//==============================================================================
class CorrectionHostApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }

    bool moreThanOneInstanceAllowed() override
    {
        // After a self-update the new copy is started by the old one: let the old copy exit
        // first so this one takes over the single-instance lock (and the audio devices).
        Updater::waitForPreviousVersionToExit (getCommandLineParameters());
        return false;
    }

    void initialise (const juce::String& commandLine) override
    {
        juce::PropertiesFile::Options options;
        options.applicationName     = "Correction Host";
        options.filenameSuffix      = ".settings";
        options.folderName          = "CorrectionHost";
        options.osxLibrarySubFolder = "Application Support";
        options.storageFormat       = juce::PropertiesFile::storeAsXML;
        appProperties.setStorageParameters (options);

        Updater::cleanUpAfterUpdate();

        engine = std::make_unique<HostEngine>();

        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *engine, *appProperties.getUserSettings(),
                                                   [this] { return tray != nullptr; });

       #if JUCE_WINDOWS || JUCE_MAC
        TrayIcon::Actions actions;
        actions.showWindow  = [this] { showWindow(); };
        actions.isBypassed  = [this] { return engine->isBypassed(); };
        actions.setBypassed = [this] (bool b) { if (mainWindow != nullptr) mainWindow->getMainComponent().setBypassed (b); };
        actions.quit        = [this] { systemRequestedQuit(); };
        actions.checkForUpdates = [this] { if (updater != nullptr) updater->checkNow(); };
        actions.canUpdate   = [this] { return updater != nullptr && updater->isEnabled(); };
        tray = std::make_unique<TrayIcon> (std::move (actions));
       #endif

        Updater::Options updaterOptions;
        updaterOptions.repo                = Updater::getUpdateRepo();
        updaterOptions.currentBuild        = Updater::getBuildNumber();
        updaterOptions.secondsSinceAudible = [this] { return engine->getSecondsSinceAudible(); };
        updaterOptions.beforeInstall       = [this] { mainWindow->getMainComponent().saveState(); };
        updaterOptions.isWindowVisible     = [this] { return mainWindow->isVisible() && ! mainWindow->isMinimised(); };
        updaterOptions.onStatusChanged     = [this] (const juce::String& text)
        {
            if (mainWindow != nullptr)
                mainWindow->getMainComponent().setVersionText (text);
        };
        updater = std::make_unique<Updater> (std::move (updaterOptions));
        mainWindow->getMainComponent().setVersionText (updater->getStatusText());

        const bool startHidden = commandLine.containsIgnoreCase ("--minimised")
                              || commandLine.containsIgnoreCase ("--minimized");

        if (! startHidden || tray == nullptr)
            showWindow();
    }

    void shutdown() override
    {
        if (mainWindow != nullptr)
            mainWindow->getMainComponent().saveState();

        updater.reset();
        tray.reset();
        mainWindow.reset();   // closes plugin editors before the plugin goes away
        engine.reset();
        appProperties.closeFiles();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override
    {
        showWindow();

        // Opening the app again (Start menu, Finder) is a good moment to look for updates.
        if (updater != nullptr)
            updater->checkIfStale (10.0 * 60.0);
    }

    void showWindow()
    {
        if (mainWindow != nullptr)
        {
            mainWindow->setVisible (true);
            mainWindow->setMinimised (false);
            mainWindow->toFront (true);
        }
    }

private:
    juce::ApplicationProperties appProperties;
    std::unique_ptr<HostEngine> engine;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<TrayIcon> tray;
    std::unique_ptr<Updater> updater;
};

START_JUCE_APPLICATION (CorrectionHostApplication)
