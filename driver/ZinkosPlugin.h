#ifndef ZINKOS_PLUGIN_H
#define ZINKOS_PLUGIN_H

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include "zinkos_engine.h"

// Object IDs
// Note: kObjectID_PlugIn is assigned by the host (always 1)
#define kObjectID_Device  2
#define kObjectID_Stream  3

// Plugin driver interface — the one function we export
extern "C" void* ZinkosPlugIn_Create(CFAllocatorRef allocator, CFUUIDRef requestedTypeUUID);

// Internal driver state
struct ZinkosDriverState {
    // Reference count (COM-style)
    UInt32 refCount;

    // Host interface — provided by CoreAudio
    AudioServerPlugInHostRef host;

    // Engine handle
    ZinkosEngine* engine;

    // IO state
    bool ioRunning;
    UInt64 anchorHostTime;    // mach_absolute_time at StartIO
    UInt64 anchorSampleTime;  // sample counter at StartIO
    UInt64 ticksPerFrame;     // host ticks per audio frame (for GetZeroTimeStamp)
};

// Global driver state (singleton — CoreAudio loads one instance)
extern ZinkosDriverState gDriverState;

// Device property dispatch (implemented in ZinkosDevice.cpp)
OSStatus ZinkosDevice_GetPropertyDataSize(
    AudioServerPlugInDriverRef inDriver,
    AudioObjectID inObjectID,
    pid_t inClientProcessID,
    const AudioObjectPropertyAddress* inAddress,
    UInt32 inQualifierDataSize,
    const void* inQualifierData,
    UInt32* outDataSize);

OSStatus ZinkosDevice_GetPropertyData(
    AudioServerPlugInDriverRef inDriver,
    AudioObjectID inObjectID,
    pid_t inClientProcessID,
    const AudioObjectPropertyAddress* inAddress,
    UInt32 inQualifierDataSize,
    const void* inQualifierData,
    UInt32 inDataSize,
    UInt32* outDataSize,
    void* outData);

OSStatus ZinkosDevice_HasProperty(
    AudioServerPlugInDriverRef inDriver,
    AudioObjectID inObjectID,
    pid_t inClientProcessID,
    const AudioObjectPropertyAddress* inAddress);

OSStatus ZinkosDevice_IsPropertySettable(
    AudioServerPlugInDriverRef inDriver,
    AudioObjectID inObjectID,
    pid_t inClientProcessID,
    const AudioObjectPropertyAddress* inAddress,
    Boolean* outIsSettable);

#endif // ZINKOS_PLUGIN_H
