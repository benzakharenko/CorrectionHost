#include "CaptureSource.h"
#include "JuceDeviceCaptureSource.h"

#if JUCE_WINDOWS
 #include "LoopbackCaptureSource.h"
#endif

std::vector<std::unique_ptr<CaptureSource>> createPlatformCaptureSources()
{
    std::vector<std::unique_ptr<CaptureSource>> sources;

   #if JUCE_WINDOWS
    sources.push_back (std::make_unique<LoopbackCaptureSource>());   // default on Windows
   #endif

    sources.push_back (std::make_unique<JuceDeviceCaptureSource>());
    return sources;
}
