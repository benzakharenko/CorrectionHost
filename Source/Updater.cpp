#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#else
 #include <signal.h>
 #include <unistd.h>
#endif

#include "Updater.h"

#include <juce_cryptography/juce_cryptography.h>

#ifndef CORRECTIONHOST_BUILD_NUMBER
 #define CORRECTIONHOST_BUILD_NUMBER 0
#endif

#ifndef CORRECTIONHOST_UPDATE_REPO
 #define CORRECTIONHOST_UPDATE_REPO ""
#endif

namespace
{
    constexpr double firstCheckDelaySeconds = 45.0;
    constexpr double checkIntervalSeconds   = 6.0 * 60.0 * 60.0;
    constexpr double retryAfterFailure      = 60.0 * 60.0;

    double nowSeconds()   { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

    int currentProcessId()
    {
       #if defined (_WIN32)
        return (int) GetCurrentProcessId();
       #else
        return (int) getpid();
       #endif
    }

   #if JUCE_WINDOWS
    const char* oldWindowsPrefix = "Correction Host.old-";
   #elif JUCE_MAC
    const char* oldMacBundleName = "Correction Host (previous version).app";
   #endif

    std::unique_ptr<juce::InputStream> openGitHub (const juce::String& url, int timeoutMs)
    {
        const auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                 .withExtraHeaders ("Accept: application/vnd.github+json\r\n"
                                                    "User-Agent: CorrectionHost-updater\r\n")
                                 .withConnectionTimeoutMs (timeoutMs)
                                 .withNumRedirectsToFollow (8);

        return juce::URL (url).createInputStream (options);
    }
}

//==============================================================================
bool Updater::isUpdatableBuild()            { return getBuildNumber() > 0 && getUpdateRepo().isNotEmpty() && getAssetName().isNotEmpty(); }
int Updater::getBuildNumber()               { return (int) CORRECTIONHOST_BUILD_NUMBER; }
juce::String Updater::getUpdateRepo()       { return juce::String (CORRECTIONHOST_UPDATE_REPO).trim(); }

juce::String Updater::getAssetName()
{
   #if JUCE_WINDOWS
    return "CorrectionHost-windows.zip";
   #elif JUCE_MAC
    return "CorrectionHost-macos.zip";
   #else
    return {};
   #endif
}

int Updater::parseBuildNumber (const juce::String& tag)
{
    const auto t = tag.trim();
    if (! t.startsWithIgnoreCase ("build-"))
        return 0;

    const auto digits = t.fromFirstOccurrenceOf ("-", false, false);
    return digits.containsOnly ("0123456789") && digits.isNotEmpty() ? digits.getIntValue() : 0;
}

juce::Result Updater::parseRelease (const juce::String& json, const juce::String& assetName, ReleaseInfo& info)
{
    juce::var root;
    const auto parsed = juce::JSON::parse (json, root);

    if (parsed.failed() || ! root.isObject())
        return juce::Result::fail ("GitHub sent something unexpected");

    if ((bool) root.getProperty ("draft", false) || (bool) root.getProperty ("prerelease", false))
        return juce::Result::fail ("latest release is a draft or pre-release");

    info = {};
    info.tag   = root.getProperty ("tag_name", {}).toString();
    info.build = parseBuildNumber (info.tag);

    if (info.build <= 0)
        return juce::Result::fail ("latest release has no build number (" + info.tag + ")");

    if (auto* assets = root.getProperty ("assets", {}).getArray())
    {
        for (auto& a : *assets)
        {
            if (a.getProperty ("name", {}).toString() != assetName)
                continue;

            info.assetUrl = a.getProperty ("browser_download_url", {}).toString();

            const auto digest = a.getProperty ("digest", {}).toString().trim();
            if (digest.startsWithIgnoreCase ("sha256:"))
                info.sha256 = digest.fromFirstOccurrenceOf (":", false, false).toLowerCase();
        }
    }

    if (! info.assetUrl.startsWith ("https://"))
        return juce::Result::fail ("latest release has no " + assetName);

    return juce::Result::ok();
}

//==============================================================================
void Updater::cleanUpAfterUpdate()
{
   #if JUCE_WINDOWS
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    for (const auto& f : exe.getParentDirectory().findChildFiles (juce::File::findFiles, false, juce::String (oldWindowsPrefix) + "*.exe"))
        f.deleteFile();   // fails harmlessly if the old copy is somehow still running
   #elif JUCE_MAC
    const auto app = juce::File::getSpecialLocation (juce::File::currentApplicationFile);
    const auto old = app.getSiblingFile (oldMacBundleName);
    if (old.isDirectory())
        old.deleteRecursively();
   #endif

    juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("CorrectionHostUpdate").deleteRecursively();
}

void Updater::waitForPreviousVersionToExit (const juce::String& commandLine, int timeoutMs)
{
    int pid = 0;

    for (const auto& token : juce::StringArray::fromTokens (commandLine, true))
        if (token.unquoted().startsWithIgnoreCase ("--updated-from="))
            pid = token.unquoted().fromFirstOccurrenceOf ("=", false, false).getIntValue();

    if (pid <= 0 || pid == currentProcessId())
        return;

   #if defined (_WIN32)
    if (auto h = OpenProcess (SYNCHRONIZE, FALSE, (DWORD) pid))
    {
        WaitForSingleObject (h, (DWORD) timeoutMs);
        CloseHandle (h);
    }
   #else
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (kill ((pid_t) pid, 0) == 0 && juce::Time::getMillisecondCounter() < deadline)
        juce::Thread::sleep (100);
   #endif
}

//==============================================================================
Updater::Updater (Options o)
    : juce::Thread ("Update check"),
      options (std::move (o)),
      enabled (options.repo.isNotEmpty() && options.currentBuild > 0 && getAssetName().isNotEmpty())
{
    workDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("CorrectionHostUpdate");

    if (enabled)
    {
        statusText = "build " + juce::String (options.currentBuild) + ", checking for updates soon";
        nextCheckTime = nowSeconds() + firstCheckDelaySeconds;
        startTimer (5000);
    }
    else
    {
        statusText = options.currentBuild > 0 ? "build " + juce::String (options.currentBuild) + " (updates off)"
                                              : "local build (doesn't update itself)";
    }
}

Updater::~Updater()
{
    stopTimer();
    stopThread (20000);
}

juce::String Updater::getStatusText() const
{
    const juce::ScopedLock sl (lock);
    return statusText;
}

void Updater::checkNow()
{
    const juce::ScopedLock sl (lock);
    if (enabled && (state == State::idle || state == State::failed))
        nextCheckTime = 0.0;
}

void Updater::checkIfStale (double maxAgeSeconds)
{
    if (lastCheckTime.load() > 0.0 && nowSeconds() - lastCheckTime.load() > maxAgeSeconds)
        checkNow();
}

void Updater::setStatus (State newState, const juce::String& text)
{
    {
        const juce::ScopedLock sl (lock);
        state = newState;
        statusText = text;
    }

    if (auto cb = options.onStatusChanged)
        juce::MessageManager::callAsync ([cb, text] { cb (text); });
}

//==============================================================================
void Updater::timerCallback()
{
    State s;
    {
        const juce::ScopedLock sl (lock);
        s = state;
    }

    if ((s == State::idle || s == State::failed) && nowSeconds() >= nextCheckTime && ! isThreadRunning())
    {
        nextCheckTime = nowSeconds() + checkIntervalSeconds;
        lastCheckTime = nowSeconds();
        setStatus (State::working, "build " + juce::String (options.currentBuild) + ", checking for updates...");
        startThread (juce::Thread::Priority::low);
        return;
    }

    if (s == State::ready)
    {
        const double quiet = options.secondsSinceAudible != nullptr ? options.secondsSinceAudible() : 1.0e9;

        if (quiet >= options.quietSecondsBeforeInstall)
            install();
    }
}

void Updater::run()
{
    const auto currentText = "build " + juce::String (options.currentBuild);

    juce::String json;
    {
        auto stream = openGitHub ("https://api.github.com/repos/" + options.repo + "/releases/latest", 15000);

        if (stream == nullptr)
        {
            nextCheckTime = nowSeconds() + retryAfterFailure;
            setStatus (State::failed, currentText + ", couldn't reach GitHub to check for updates (will retry)");
            return;
        }

        json = stream->readEntireStreamAsString();
    }

    ReleaseInfo info;
    const auto parsed = parseRelease (json, getAssetName(), info);

    if (parsed.failed())
    {
        nextCheckTime = nowSeconds() + retryAfterFailure;
        setStatus (State::failed, currentText + ", update check failed: " + parsed.getErrorMessage());
        return;
    }

    if (info.build <= options.currentBuild)
    {
        setStatus (State::idle, currentText + ", up to date");
        return;
    }

    setStatus (State::working, currentText + ", downloading build " + juce::String (info.build) + "...");

    juce::File payload;
    const auto result = downloadAndUnpack (info, payload);

    if (threadShouldExit())
        return;

    if (result.failed())
    {
        nextCheckTime = nowSeconds() + retryAfterFailure;
        setStatus (State::failed, currentText + ", update to build " + juce::String (info.build)
                                    + " failed: " + result.getErrorMessage() + " (will retry)");
        return;
    }

    {
        const juce::ScopedLock sl (lock);
        pending = info;
        pendingPayload = payload;
    }

    setStatus (State::ready, currentText + ", build " + juce::String (info.build)
                               + " downloaded; installs after 2 minutes without sound");
}

juce::Result Updater::downloadAndUnpack (const ReleaseInfo& info, juce::File& payloadOut)
{
    workDir.deleteRecursively();
    if (! workDir.createDirectory())
        return juce::Result::fail ("can't create " + workDir.getFullPathName());

    const auto zip = workDir.getChildFile (getAssetName());

    {
        auto in = openGitHub (info.assetUrl, 30000);
        if (in == nullptr)
            return juce::Result::fail ("download didn't start");

        juce::FileOutputStream out (zip);
        if (! out.openedOk())
            return juce::Result::fail ("can't write " + zip.getFullPathName());

        juce::HeapBlock<char> buffer (1 << 16);

        for (;;)
        {
            if (threadShouldExit())
                return juce::Result::fail ("cancelled");

            const auto n = in->read (buffer.getData(), 1 << 16);
            if (n <= 0)
                break;

            if (! out.write (buffer.getData(), (size_t) n))
                return juce::Result::fail ("disk write failed");
        }

        out.flush();
    }

    if (zip.getSize() < 1024)
        return juce::Result::fail ("download was empty");

    if (info.sha256.isNotEmpty())
    {
        const auto actual = juce::SHA256 (zip).toHexString().removeCharacters (" ").toLowerCase();
        if (actual != info.sha256)
            return juce::Result::fail ("download didn't match its checksum");
    }

    const auto unpacked = workDir.getChildFile ("unpacked");
    unpacked.createDirectory();

   #if JUCE_MAC
    // ditto keeps the bundle exactly as built (permissions, signature).
    juce::ChildProcess ditto;
    if (! ditto.start (juce::StringArray { "/usr/bin/ditto", "-x", "-k", zip.getFullPathName(), unpacked.getFullPathName() })
        || ! ditto.waitForProcessToFinish (120000) || ditto.getExitCode() != 0)
        return juce::Result::fail ("couldn't unpack the update");

    payloadOut = unpacked.getChildFile ("Correction Host.app");
    if (! payloadOut.getChildFile ("Contents/MacOS/Correction Host").existsAsFile())
        return juce::Result::fail ("the update doesn't contain Correction Host.app");
   #else
    juce::ZipFile archive (zip);
    const auto r = archive.uncompressTo (unpacked, true);
    if (r.failed())
        return juce::Result::fail ("couldn't unpack the update: " + r.getErrorMessage());

    payloadOut = unpacked.getChildFile ("Correction Host.exe");
    if (! payloadOut.existsAsFile())
        return juce::Result::fail ("the update doesn't contain Correction Host.exe");
   #endif

    return juce::Result::ok();
}

//==============================================================================
juce::Result Updater::swapInPlace (const juce::File& payload, juce::File& launchTarget)
{
   #if JUCE_WINDOWS
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    const auto old = exe.getSiblingFile (juce::String (oldWindowsPrefix) + juce::String (options.currentBuild) + ".exe");
    old.deleteFile();

    // A running .exe can't be overwritten, but it can be renamed.
    if (! exe.moveFileTo (old))
        return juce::Result::fail ("can't replace " + exe.getFullPathName() + " (is the folder read-only?)");

    if (! payload.copyFileTo (exe))
    {
        old.moveFileTo (exe);   // put the working version back
        return juce::Result::fail ("couldn't copy the new version into " + exe.getParentDirectory().getFullPathName());
    }

    launchTarget = exe;
    return juce::Result::ok();
   #elif JUCE_MAC
    const auto app = juce::File::getSpecialLocation (juce::File::currentApplicationFile);
    const auto old = app.getSiblingFile (oldMacBundleName);
    old.deleteRecursively();

    if (! app.moveFileTo (old))
        return juce::Result::fail ("can't replace " + app.getFullPathName()
                                   + ". Move Correction Host to /Applications (or ~/Applications) and it can update itself");

    if (! payload.moveFileTo (app))
    {
        old.moveFileTo (app);
        return juce::Result::fail ("couldn't move the new version into " + app.getParentDirectory().getFullPathName());
    }

    launchTarget = app;
    return juce::Result::ok();
   #else
    juce::ignoreUnused (payload, launchTarget);
    return juce::Result::fail ("not supported on this platform");
   #endif
}

void Updater::install()
{
    ReleaseInfo info;
    juce::File payload;
    {
        const juce::ScopedLock sl (lock);
        info = pending;
        payload = pendingPayload;
    }

    setStatus (State::installing, "installing build " + juce::String (info.build) + "...");

    if (options.beforeInstall != nullptr)
        options.beforeInstall();

    juce::File target;
    const auto swapped = swapInPlace (payload, target);

    if (swapped.failed())
    {
        nextCheckTime = nowSeconds() + retryAfterFailure;
        setStatus (State::failed, "build " + juce::String (options.currentBuild) + ", couldn't install build "
                                    + juce::String (info.build) + ": " + swapped.getErrorMessage());
        return;
    }

    juce::StringArray args;
    args.add ("--updated-from=" + juce::String (currentProcessId()));
    if (options.isWindowVisible != nullptr && ! options.isWindowVisible())
        args.add ("--minimised");

    bool launched = false;

   #if JUCE_MAC
    juce::StringArray command { "/usr/bin/open", "-n", target.getFullPathName(), "--args" };
    command.addArray (args);
    juce::ChildProcess opener;
    launched = opener.start (command) && opener.waitForProcessToFinish (15000) && opener.getExitCode() == 0;
   #else
    launched = target.startAsProcess (args.joinIntoString (" "));
   #endif

    if (! launched)
    {
        setStatus (State::failed, "build " + juce::String (info.build) + " is installed; restart Correction Host to use it");
        return;
    }

    // The new copy waits for this process to exit before it starts its audio.
    juce::JUCEApplicationBase::quit();
}
