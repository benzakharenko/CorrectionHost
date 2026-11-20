#pragma once

#include <atomic>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

/**
    Keeps the app up to date from the GitHub releases of the repo it was built from.

    Builds made by the release workflow carry a build number and the repo name
    (CORRECTIONHOST_BUILD_NUMBER / CORRECTIONHOST_UPDATE_REPO). Local builds have build
    number 0 and never update themselves, so a development copy is left alone.

    Flow: check the latest release a little after launch and every few hours; if it is
    newer, download the asset for this platform in the background, verify its SHA-256
    against the digest GitHub publishes, unpack it, then wait until the audio has been
    silent for a while before swapping the app in place and relaunching.
*/
class Updater final : private juce::Thread,
                      private juce::Timer
{
public:
    struct Options
    {
        juce::String repo;                    // "owner/name"
        int currentBuild = 0;
        double quietSecondsBeforeInstall = 120.0;
        std::function<double()> secondsSinceAudible;   // from the engine
        std::function<void()> beforeInstall;           // save settings etc.
        std::function<bool()> isWindowVisible;         // relaunch hidden if it was hidden
        std::function<void (const juce::String&)> onStatusChanged;
    };

    explicit Updater (Options);
    ~Updater() override;

    bool isEnabled() const noexcept          { return enabled; }
    juce::String getStatusText() const;

    /** Checks now instead of waiting for the next scheduled check. */
    void checkNow();

    /** Checks now if the last check was more than this long ago (e.g. when the app is
        opened again from the Start menu or Finder). */
    void checkIfStale (double maxAgeSeconds);

    //==============================================================================
    /** True if this build was made with update support (non-zero build number and a repo). */
    static bool isUpdatableBuild();
    static int  getBuildNumber();
    static juce::String getUpdateRepo();

    /** Name of the release asset for this platform. */
    static juce::String getAssetName();

    /** Removes leftovers of the previous version after an update. Call at startup. */
    static void cleanUpAfterUpdate();

    /** Parses "--updated-from=<pid>" and waits (up to timeoutMs) for that process to exit,
        so the new version can take over the single-instance lock. */
    static void waitForPreviousVersionToExit (const juce::String& commandLine, int timeoutMs = 20000);

    //==============================================================================
    /** Exposed for tests: picks the build number from a tag like "build-123". */
    static int parseBuildNumber (const juce::String& tag);

    struct ReleaseInfo
    {
        int build = 0;
        juce::String tag, assetUrl, sha256;   // sha256 may be empty if GitHub didn't publish one
    };

    /** Exposed for tests: reads the GitHub "latest release" JSON. */
    static juce::Result parseRelease (const juce::String& json, const juce::String& assetName, ReleaseInfo&);

private:
    enum class State { idle, working, ready, installing, failed };

    void run() override;
    void timerCallback() override;

    void setStatus (State, const juce::String& text);
    juce::Result downloadAndUnpack (const ReleaseInfo&, juce::File& payloadOut);
    void install();
    juce::Result swapInPlace (const juce::File& payload, juce::File& launchTarget);

    Options options;
    const bool enabled;

    mutable juce::CriticalSection lock;
    State state = State::idle;
    juce::String statusText;
    ReleaseInfo pending;
    juce::File pendingPayload;
    juce::File workDir;

    std::atomic<double> nextCheckTime { 0.0 };
    std::atomic<double> lastCheckTime { 0.0 };   // Time::getMillisecondCounterHiRes() based, seconds

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Updater)
};
