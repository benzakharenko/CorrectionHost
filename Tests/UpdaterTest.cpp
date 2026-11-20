// Checks the self-updater's parsing of GitHub's "latest release" JSON and its build numbers.
#include <juce_core/juce_core.h>
#include "Updater.h"

namespace
{
    int failures = 0;

    void check (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";
        if (! ok) ++failures;
    }

    // Shape of https://api.github.com/repos/OWNER/REPO/releases/latest (trimmed).
    const char* releaseJson = R"({
      "url": "https://api.github.com/repos/someone/CorrectionHost/releases/1",
      "tag_name": "build-42",
      "name": "Build 42",
      "draft": false,
      "prerelease": false,
      "assets": [
        { "name": "CorrectionHost-macos.zip",
          "digest": "sha256:AAAABBBBCCCCDDDDEEEEFFFF0000111122223333444455556666777788889999",
          "browser_download_url": "https://github.com/someone/CorrectionHost/releases/download/build-42/CorrectionHost-macos.zip" },
        { "name": "CorrectionHost-windows.zip",
          "digest": "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
          "browser_download_url": "https://github.com/someone/CorrectionHost/releases/download/build-42/CorrectionHost-windows.zip" }
      ]
    })";
}

int main (int argc, char** argv)
{
    // Optional: parse a real GitHub response saved to a file, e.g.
    //   curl -s https://api.github.com/repos/OWNER/REPO/releases/latest > latest.json
    //   UpdaterTest latest.json
    if (argc > 1)
    {
        for (auto name : { "CorrectionHost-windows.zip", "CorrectionHost-macos.zip" })
        {
            Updater::ReleaseInfo info;
            const auto r = Updater::parseRelease (juce::File (juce::String (argv[1])).loadFileAsString(), name, info);
            std::cout << name << ": " << (r.wasOk() ? juce::String ("ok") : r.getErrorMessage())
                      << ", build " << info.build << ", sha256 " << info.sha256 << "\n  " << info.assetUrl << "\n";
        }
        return 0;
    }

    std::cout << "Build numbers\n";
    check (Updater::parseBuildNumber ("build-42") == 42, "build-42 -> 42");
    check (Updater::parseBuildNumber (" build-7 ") == 7, "whitespace tolerated");
    check (Updater::parseBuildNumber ("v1.2.3") == 0, "other tag styles are ignored");
    check (Updater::parseBuildNumber ("build-") == 0, "empty number rejected");
    check (Updater::parseBuildNumber ("build-12a") == 0, "junk rejected");

    std::cout << "Release JSON\n";
    {
        Updater::ReleaseInfo info;
        const auto r = Updater::parseRelease (releaseJson, "CorrectionHost-windows.zip", info);
        check (r.wasOk(), "windows asset parsed: " + r.getErrorMessage());
        check (info.build == 42, "build number from tag");
        check (info.assetUrl.endsWith ("/build-42/CorrectionHost-windows.zip"), "windows download URL");
        check (info.sha256 == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "windows digest");
    }
    {
        Updater::ReleaseInfo info;
        const auto r = Updater::parseRelease (releaseJson, "CorrectionHost-macos.zip", info);
        check (r.wasOk() && info.sha256 == juce::String ("AAAABBBBCCCCDDDDEEEEFFFF0000111122223333444455556666777788889999").toLowerCase(),
               "mac asset parsed, digest lower-cased");
    }
    {
        Updater::ReleaseInfo info;
        check (Updater::parseRelease (releaseJson, "CorrectionHost-linux.zip", info).failed(), "missing asset is an error");
        check (Updater::parseRelease ("{\"message\":\"Not Found\"}", "CorrectionHost-windows.zip", info).failed(), "404 body is an error");
        check (Updater::parseRelease ("<html>", "CorrectionHost-windows.zip", info).failed(), "non-JSON is an error");

        juce::String draft (releaseJson);
        draft = draft.replace ("\"draft\": false", "\"draft\": true");
        check (Updater::parseRelease (draft, "CorrectionHost-windows.zip", info).failed(), "drafts are ignored");

        juce::String http (releaseJson);
        http = http.replace ("https://github.com", "http://github.com");
        check (Updater::parseRelease (http, "CorrectionHost-windows.zip", info).failed(), "non-HTTPS download refused");

        juce::String noDigest (releaseJson);
        noDigest = noDigest.replace ("\"digest\": \"sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",", "");
        check (Updater::parseRelease (noDigest, "CorrectionHost-windows.zip", info).wasOk() && info.sha256.isEmpty(),
               "release without a digest still parses");
    }

    std::cout << "This build\n";
    check (Updater::getBuildNumber() == 0, "test build is a local build (0)");
    check (! Updater::isUpdatableBuild(), "local builds don't update themselves");

    std::cout << "Waiting for the previous version\n";
    {
        const auto start = juce::Time::getMillisecondCounter();
        Updater::waitForPreviousVersionToExit ("--minimised --updated-from=999999", 3000);
        check (juce::Time::getMillisecondCounter() - start < 1000, "returns at once when that process is gone");

        const auto start2 = juce::Time::getMillisecondCounter();
        Updater::waitForPreviousVersionToExit ("--minimised", 3000);
        check (juce::Time::getMillisecondCounter() - start2 < 100, "no-op without the flag");
    }

    std::cout << (failures == 0 ? "ALL PASSED\n" : "FAILURES\n");
    return failures == 0 ? 0 : 1;
}
