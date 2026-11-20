# JUCE's ASIO device reopens at the sample rate it had *before* whenever the driver asks it
# to reset - which is exactly what a multi-client driver does when another app (your DAW)
# changes the rate. Reopening at the old rate forces the interface back to it.
#
# This patch makes it follow the driver's current rate instead. It's applied to the JUCE
# copy that CMake downloads into the build folder, is idempotent, and handles CRLF/LF files.
#
# Usage: include() with CORRECTIONHOST_JUCE_SOURCE set to the JUCE source directory.

set(_asio_file "${CORRECTIONHOST_JUCE_SOURCE}/modules/juce_audio_devices/native/juce_ASIO_windows.cpp")

if(NOT EXISTS "${_asio_file}")
    message(WARNING "CorrectionHost: ${_asio_file} not found; ASIO sample-rate patch not applied.")
    return()
endif()

file(READ "${_asio_file}" _src)

string(FIND "${_src}" "CorrectionHost: follow the driver" _already)
if(NOT _already EQUAL -1)
    message(STATUS "CorrectionHost: JUCE ASIO sample-rate patch already applied")
    return()
endif()

set(_old [=[            open (BigInteger (currentChansIn), BigInteger (currentChansOut),
                  currentSampleRate, currentBlockSizeSamples);]=])

set(_new [=[            // CorrectionHost: follow the driver's current rate after a reset (multi-client drivers)
            {
                const auto driverRate = getSampleRate();
                open (BigInteger (currentChansIn), BigInteger (currentChansOut),
                      driverRate > 0 ? driverRate : currentSampleRate, currentBlockSizeSamples);
            }]=])

string(FIND "${_src}" "${_old}" _pos)

if(_pos EQUAL -1)
    # Windows checkouts usually have CRLF line endings.
    string(REPLACE "\n" "\r\n" _old "${_old}")
    string(REPLACE "\n" "\r\n" _new "${_new}")
    string(FIND "${_src}" "${_old}" _pos)
endif()

if(_pos EQUAL -1)
    message(WARNING "CorrectionHost: couldn't find the code to patch in juce_ASIO_windows.cpp "
                    "(JUCE version changed?). ASIO may force the old sample rate after a driver reset.")
    return()
endif()

string(REPLACE "${_old}" "${_new}" _src "${_src}")
file(WRITE "${_asio_file}" "${_src}")
message(STATUS "CorrectionHost: applied JUCE ASIO sample-rate patch")
