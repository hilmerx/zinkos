//
// ZinkosPlugin.cpp — AudioServerPlugIn entry point and COM vtable
//
// This is the thin C++ shim. All real logic lives in the Rust engine.
// Implements: Initialize, CreateDevice, Teardown, IOOperation, GetZeroTimeStamp.
//

#include "ZinkosPlugin.h"
#include <mach/mach_time.h>
#include <os/log.h>

// Logger
static os_log_t sLog = os_log_create("com.zinkos.driver", "plugin");

// Global singleton state
ZinkosDriverState gDriverState = {};

// Forward declarations for vtable functions
static HRESULT ZinkosQueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
static ULONG ZinkosAddRef(void* inDriver);
static ULONG ZinkosRelease(void* inDriver);
static OSStatus ZinkosInitialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus ZinkosCreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID);
static OSStatus ZinkosDestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus ZinkosAddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus ZinkosRemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus ZinkosPerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
static OSStatus ZinkosAbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);

static Boolean ZinkosHasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus ZinkosIsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus ZinkosGetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus ZinkosGetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus ZinkosSetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData);

static OSStatus ZinkosStartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus ZinkosStopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus ZinkosGetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
static OSStatus ZinkosWillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outIsInput);
static OSStatus ZinkosBeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);
static OSStatus ZinkosDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);
static OSStatus ZinkosEndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);

// The vtable
static AudioServerPlugInDriverInterface gDriverVtable = {
    // IUnknown
    NULL, // _reserved
    ZinkosQueryInterface,
    ZinkosAddRef,
    ZinkosRelease,
    // AudioServerPlugInDriverInterface
    ZinkosInitialize,
    ZinkosCreateDevice,
    ZinkosDestroyDevice,
    ZinkosAddDeviceClient,
    ZinkosRemoveDeviceClient,
    ZinkosPerformDeviceConfigurationChange,
    ZinkosAbortDeviceConfigurationChange,
    ZinkosHasProperty,
    ZinkosIsPropertySettable,
    ZinkosGetPropertyDataSize,
    ZinkosGetPropertyData,
    ZinkosSetPropertyData,
    ZinkosStartIO,
    ZinkosStopIO,
    ZinkosGetZeroTimeStamp,
    ZinkosWillDoIOOperation,
    ZinkosBeginIOOperation,
    ZinkosDoIOOperation,
    ZinkosEndIOOperation,
};

static AudioServerPlugInDriverInterface* gDriverVtablePtr = &gDriverVtable;

// ============================================================
// Factory function — the only exported symbol
// ============================================================

extern "C" void* ZinkosPlugIn_Create(CFAllocatorRef /*allocator*/, CFUUIDRef requestedTypeUUID)
{
    // Check that CoreAudio is asking for the right type
    // Apple-defined kAudioServerPlugInTypeUUID: 443ABAB8-E7B3-491A-B985-BEB9187030DB
    CFUUIDRef audioServerPlugInTypeUUID = CFUUIDGetConstantUUIDWithBytes(
        NULL,
        0x44, 0x3A, 0xBA, 0xB8, 0xE7, 0xB3, 0x49, 0x1A,
        0xB9, 0x85, 0xBE, 0xB9, 0x18, 0x70, 0x30, 0xDB);

    if (!CFEqual(requestedTypeUUID, audioServerPlugInTypeUUID)) {
        return NULL;
    }

    gDriverState.refCount = 1;
    os_log(sLog, "ZinkosPlugIn_Create: created driver instance");
    return &gDriverVtablePtr;
}

// ============================================================
// IUnknown
// ============================================================

static HRESULT ZinkosQueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface)
{
    os_log(sLog, "ZinkosQueryInterface: called");
    CFUUIDRef iUnknownUUID = CFUUIDGetConstantUUIDWithBytes(
        NULL,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);

    // Accept any interface — the host only ever asks for IUnknown or
    // AudioServerPlugInDriverInterface (version may vary across macOS releases).
    (void)iUnknownUUID;
    (void)inUUID;
    gDriverState.refCount++;
    *outInterface = inDriver;
    os_log(sLog, "ZinkosQueryInterface: OK");
    return S_OK;
}

static ULONG ZinkosAddRef(void* /*inDriver*/)
{
    os_log(sLog, "ZinkosAddRef: refCount=%u", gDriverState.refCount + 1);
    return ++gDriverState.refCount;
}

static ULONG ZinkosRelease(void* /*inDriver*/)
{
    if (gDriverState.refCount > 0) {
        gDriverState.refCount--;
    }
    os_log(sLog, "ZinkosRelease: refCount=%u", gDriverState.refCount);
    return gDriverState.refCount;
}

// ============================================================
// Lifecycle
// ============================================================

static OSStatus ZinkosInitialize(AudioServerPlugInDriverRef /*inDriver*/, AudioServerPlugInHostRef inHost)
{
    os_log(sLog, "ZinkosInitialize: ENTERED");
    gDriverState.host = inHost;
    gDriverState.engine = nullptr;

    // Compute ticks per frame for GetZeroTimeStamp
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    // ticks per second = 1e9 * timebase.denom / timebase.numer
    // ticks per frame = ticks_per_second / sample_rate
    double ticksPerSecond = 1e9 * (double)timebase.denom / (double)timebase.numer;
    gDriverState.ticksPerFrame = (UInt64)(ticksPerSecond / 48000.0);

    // Engine creation deferred to StartIO — Initialize runs in sandboxed
    // driver service context where network operations may be restricted.

    os_log(sLog, "ZinkosInitialize: success, ticksPerFrame=%llu", gDriverState.ticksPerFrame);
    return kAudioHardwareNoError;
}

static OSStatus ZinkosCreateDevice(AudioServerPlugInDriverRef /*inDriver*/, CFDictionaryRef /*inDescription*/, const AudioServerPlugInClientInfo* /*inClientInfo*/, AudioObjectID* outDeviceObjectID)
{
    // We don't support dynamic device creation — our device is always present
    *outDeviceObjectID = kObjectID_Device;
    return kAudioHardwareNoError;
}

static OSStatus ZinkosDestroyDevice(AudioServerPlugInDriverRef /*inDriver*/, AudioObjectID /*inDeviceObjectID*/)
{
    return kAudioHardwareNoError;
}

static OSStatus ZinkosAddDeviceClient(AudioServerPlugInDriverRef /*inDriver*/, AudioObjectID /*inDeviceObjectID*/, const AudioServerPlugInClientInfo* /*inClientInfo*/)
{
    return kAudioHardwareNoError;
}

static OSStatus ZinkosRemoveDeviceClient(AudioServerPlugInDriverRef /*inDriver*/, AudioObjectID /*inDeviceObjectID*/, const AudioServerPlugInClientInfo* /*inClientInfo*/)
{
    return kAudioHardwareNoError;
}

static OSStatus ZinkosPerformDeviceConfigurationChange(AudioServerPlugInDriverRef /*inDriver*/, AudioObjectID /*inDeviceObjectID*/, UInt64 /*inChangeAction*/, void* /*inChangeInfo*/)
{
    return kAudioHardwareNoError;
}

static OSStatus ZinkosAbortDeviceConfigurationChange(AudioServerPlugInDriverRef /*inDriver*/, AudioObjectID /*inDeviceObjectID*/, UInt64 /*inChangeAction*/, void* /*inChangeInfo*/)
{
    return kAudioHardwareNoError;
}

// ============================================================
// IO Operations
// ============================================================

static OSStatus ZinkosStartIO(AudioServerPlugInDriverRef /*inDriver*/, AudioObjectID /*inDeviceObjectID*/, UInt32 /*inClientID*/)
{
    if (gDriverState.ioRunning) {
        return kAudioHardwareNoError;
    }

    // Lazily create engine on first StartIO (deferred from Initialize)
    if (!gDriverState.engine) {
        gDriverState.engine = zinkos_engine_create("192.168.1.87", 4010);
        if (!gDriverState.engine) {
            os_log_error(sLog, "ZinkosStartIO: failed to create engine");
            return kAudioHardwareUnspecifiedError;
        }
        os_log(sLog, "ZinkosStartIO: engine created");
    }

    int32_t ret = zinkos_engine_start(gDriverState.engine);
    if (ret != 0) {
        os_log_error(sLog, "ZinkosStartIO: engine start failed");
        return kAudioHardwareUnspecifiedError;
    }

    gDriverState.anchorHostTime = mach_absolute_time();
    gDriverState.anchorSampleTime = 0;
    gDriverState.ioRunning = true;

    os_log(sLog, "ZinkosStartIO: streaming started");
    return kAudioHardwareNoError;
}

static OSStatus ZinkosStopIO(AudioServerPlugInDriverRef /*inDriver*/, AudioObjectID /*inDeviceObjectID*/, UInt32 /*inClientID*/)
{
    if (!gDriverState.ioRunning) {
        return kAudioHardwareNoError;
    }

    if (gDriverState.engine) {
        zinkos_engine_stop(gDriverState.engine);
    }

    gDriverState.ioRunning = false;
    os_log(sLog, "ZinkosStopIO: streaming stopped");
    return kAudioHardwareNoError;
}

static OSStatus ZinkosGetZeroTimeStamp(AudioServerPlugInDriverRef /*inDriver*/, AudioObjectID /*inDeviceObjectID*/, UInt32 /*inClientID*/, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed)
{
    // Synthesize timestamps based on anchor time.
    // CoreAudio uses this to synchronize its IO calls.
    UInt64 currentHostTime = mach_absolute_time();
    UInt64 elapsed = currentHostTime - gDriverState.anchorHostTime;
    UInt64 elapsedFrames = elapsed / gDriverState.ticksPerFrame;

    // Round down to the nearest IO cycle boundary (typically 512 frames)
    // Using 256 frames as a reasonable IO cycle size
    const UInt64 ioCycleFrames = 256;
    UInt64 cycleCount = elapsedFrames / ioCycleFrames;

    *outSampleTime = (Float64)(cycleCount * ioCycleFrames);
    *outHostTime = gDriverState.anchorHostTime + (cycleCount * ioCycleFrames * gDriverState.ticksPerFrame);
    *outSeed = 1; // changes only on config changes

    return kAudioHardwareNoError;
}

static OSStatus ZinkosWillDoIOOperation(AudioServerPlugInDriverRef /*inDriver*/, AudioObjectID /*inDeviceObjectID*/, UInt32 /*inClientID*/, UInt32 inOperationID, Boolean* outWillDo, Boolean* outIsInput)
{
    // We only support WriteMix (output, mixing into our buffer)
    *outIsInput = false;
    *outWillDo = (inOperationID == kAudioServerPlugInIOOperationWriteMix);
    return kAudioHardwareNoError;
}

static OSStatus ZinkosBeginIOOperation(AudioServerPlugInDriverRef /*inDriver*/, AudioObjectID /*inDeviceObjectID*/, UInt32 /*inClientID*/, UInt32 /*inOperationID*/, UInt32 /*inIOBufferFrameSize*/, const AudioServerPlugInIOCycleInfo* /*inIOCycleInfo*/)
{
    return kAudioHardwareNoError;
}

static OSStatus ZinkosDoIOOperation(AudioServerPlugInDriverRef /*inDriver*/, AudioObjectID /*inDeviceObjectID*/, AudioObjectID /*inStreamObjectID*/, UInt32 /*inClientID*/, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* /*inIOCycleInfo*/, void* ioMainBuffer, void* /*ioSecondaryBuffer*/)
{
    if (inOperationID != kAudioServerPlugInIOOperationWriteMix) {
        return kAudioHardwareNoError;
    }

    if (!gDriverState.engine || !gDriverState.ioRunning) {
        return kAudioHardwareNoError;
    }

    // CoreAudio delivers Float32 interleaved samples despite our S16LE declaration.
    // Convert Float32 → S16LE in place, then hand off to engine.
    Float32* floatBuf = (Float32*)ioMainBuffer;
    int16_t* intBuf = (int16_t*)ioMainBuffer; // safe: int16 is smaller than float32

    UInt32 sampleCount = inIOBufferFrameSize * 2; // stereo
    for (UInt32 i = 0; i < sampleCount; i++) {
        Float32 sample = floatBuf[i];
        // Clamp to [-1, 1]
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        intBuf[i] = (int16_t)(sample * 32767.0f);
    }

    // Write to ring buffer — RT-safe call
    zinkos_engine_write_frames(gDriverState.engine, intBuf, inIOBufferFrameSize);

    return kAudioHardwareNoError;
}

static OSStatus ZinkosEndIOOperation(AudioServerPlugInDriverRef /*inDriver*/, AudioObjectID /*inDeviceObjectID*/, UInt32 /*inClientID*/, UInt32 /*inOperationID*/, UInt32 /*inIOBufferFrameSize*/, const AudioServerPlugInIOCycleInfo* /*inIOCycleInfo*/)
{
    return kAudioHardwareNoError;
}

// ============================================================
// Property dispatch — delegates to ZinkosDevice.cpp
// ============================================================

static Boolean ZinkosHasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
    return ZinkosDevice_HasProperty(inDriver, inObjectID, inClientProcessID, inAddress) == kAudioHardwareNoError;
}

static OSStatus ZinkosIsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
    return ZinkosDevice_IsPropertySettable(inDriver, inObjectID, inClientProcessID, inAddress, outIsSettable);
}

static OSStatus ZinkosGetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
    return ZinkosDevice_GetPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, outDataSize);
}

static OSStatus ZinkosGetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    return ZinkosDevice_GetPropertyData(inDriver, inObjectID, inClientProcessID, inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
}

static OSStatus ZinkosSetPropertyData(AudioServerPlugInDriverRef /*inDriver*/, AudioObjectID /*inObjectID*/, pid_t /*inClientProcessID*/, const AudioObjectPropertyAddress* /*inAddress*/, UInt32 /*inQualifierDataSize*/, const void* /*inQualifierData*/, UInt32 /*inDataSize*/, const void* /*inData*/)
{
    // No settable properties for now
    return kAudioHardwareUnsupportedOperationError;
}
