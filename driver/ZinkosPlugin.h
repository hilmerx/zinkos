#ifndef ZINKOS_PLUGIN_H
#define ZINKOS_PLUGIN_H

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdatomic.h>
#include "zinkos_engine.h"

// Object IDs
// Note: kObjectID_PlugIn is assigned by the host (always 1)
#define kObjectID_Device  2
#define kObjectID_Stream  3
#define kObjectID_Volume  4
#define kObjectID_Mute    5

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

    // Volume control
    Float32 volumeScalar;     // 0.0 – 1.0
    bool muted;

    // Config (loaded from com.zinkos.driver plist)
    char targetIP[256];       // receiver IP address
    uint16_t targetPort;      // receiver UDP port
    uint32_t latencyOffsetMs; // user-configurable latency offset
    uint32_t framesPerPacket; // frames per UDP packet (default 240 = 5ms @ 48kHz)

    // Bonjour discovery state
    _Atomic bool devicePublished;      // is device visible to CoreAudio?
    bool manualIPConfigured;           // plist has a real (non-default) IP?
    char discoveredIP[256];            // resolved IP from Bonjour
    uint16_t discoveredPort;           // port from Bonjour
    _Atomic bool discoveredAvailable;  // set by browse thread after resolve
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
