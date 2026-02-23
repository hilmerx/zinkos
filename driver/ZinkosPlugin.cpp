//
// ZinkosPlugin.cpp — AudioServerPlugIn entry point and COM vtable
//
// This is the thin C++ shim. All real logic lives in the Rust engine.
// Implements: Initialize, CreateDevice, Teardown, IOOperation, GetZeroTimeStamp.
//

#include "ZinkosPlugin.h"
#include <cmath>
#include <cstdio>
#include <mach/mach_time.h>
#include <os/log.h>

// Logger
static os_log_t sLog = os_log_create("com.zinkos.driver", "plugin");

// Global singleton state
ZinkosDriverState gDriverState = {};

// ============================================================
// Config loading from /Library/Preferences/com.zinkos.driver.plist
// ============================================================

static const char* kConfigAppID = "com.zinkos.driver";
static const char* kDefaultIP = "0.0.0.0";
static const uint16_t kDefaultPort = 4010;
static const uint32_t kDefaultLatencyOffsetMs = 0;
static const uint32_t kDefaultFramesPerPacket = 240;
static const char* kVolumeStatePath = "/Library/Preferences/Audio/com.zinkos.volume";

static uint64_t sLastPersistTime = 0;

static void PersistVolumeState()
{
    // Throttle writes — slider sends dozens of updates per second.
    // Only write to disk at most every 500ms to avoid stalling coreaudiod.
    uint64_t now = mach_absolute_time();
    mach_timebase_info_data_t tbi;
    mach_timebase_info(&tbi);
    uint64_t elapsedNs = (now - sLastPersistTime) * tbi.numer / tbi.denom;
    if (sLastPersistTime != 0 && elapsedNs < 500000000ULL) {
        return;
    }
    sLastPersistTime = now;

    FILE* f = fopen(kVolumeStatePath, "w");
    if (f) {
        fprintf(f, "%.6f\n%d\n", gDriverState.volumeScalar, gDriverState.muted ? 1 : 0);
        fclose(f);
    }
}

static void LoadVolumeState()
{
    gDriverState.volumeScalar = 0.5f;  // safe default — never blast full volume
    gDriverState.muted = false;
    FILE* f = fopen(kVolumeStatePath, "r");
    if (f) {
        float v = 0.5f;
        int m = 0;
        if (fscanf(f, "%f\n%d", &v, &m) >= 1) {
            if (v >= 0.0f && v <= 1.0f) gDriverState.volumeScalar = v;
            gDriverState.muted = (m != 0);
        }
        fclose(f);
    }
}

static void LoadConfig()
{
    // Read plist file directly from /Library/Preferences/ — the driver runs
    // as _coreaudiod in a sandboxed service helper and cannot see user-domain
    // preferences. CLI/app write here with: sudo defaults write /Library/Preferences/com.zinkos.driver ...
    static const char* kPlistPath = "/Library/Preferences/com.zinkos.driver.plist";

    strlcpy(gDriverState.targetIP, kDefaultIP, sizeof(gDriverState.targetIP));
    gDriverState.targetPort = kDefaultPort;
    gDriverState.latencyOffsetMs = kDefaultLatencyOffsetMs;
    gDriverState.framesPerPacket = kDefaultFramesPerPacket;

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, (const UInt8*)kPlistPath, strlen(kPlistPath), false);
    if (url) {
        CFReadStreamRef stream = CFReadStreamCreateWithFile(kCFAllocatorDefault, url);
        CFRelease(url);

        if (stream) {
            if (CFReadStreamOpen(stream)) {
                CFPropertyListRef plist = CFPropertyListCreateWithStream(
                    kCFAllocatorDefault, stream, 0, kCFPropertyListImmutable, NULL, NULL);
                CFReadStreamClose(stream);

                if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
                    CFDictionaryRef dict = (CFDictionaryRef)plist;

                    // ReceiverIP
                    CFStringRef ipVal = (CFStringRef)CFDictionaryGetValue(dict, CFSTR("ReceiverIP"));
                    if (ipVal && CFGetTypeID(ipVal) == CFStringGetTypeID()) {
                        CFStringGetCString(ipVal, gDriverState.targetIP, sizeof(gDriverState.targetIP), kCFStringEncodingUTF8);
                    }

                    // ReceiverPort
                    CFNumberRef portVal = (CFNumberRef)CFDictionaryGetValue(dict, CFSTR("ReceiverPort"));
                    if (portVal && CFGetTypeID(portVal) == CFNumberGetTypeID()) {
                        int64_t portNum = 0;
                        CFNumberGetValue(portVal, kCFNumberSInt64Type, &portNum);
                        if (portNum > 0 && portNum <= 65535) {
                            gDriverState.targetPort = (uint16_t)portNum;
                        }
                    }

                    // LatencyOffsetMs
                    CFNumberRef latencyVal = (CFNumberRef)CFDictionaryGetValue(dict, CFSTR("LatencyOffsetMs"));
                    if (latencyVal && CFGetTypeID(latencyVal) == CFNumberGetTypeID()) {
                        int64_t latencyNum = 0;
                        CFNumberGetValue(latencyVal, kCFNumberSInt64Type, &latencyNum);
                        if (latencyNum >= 0) {
                            gDriverState.latencyOffsetMs = (uint32_t)latencyNum;
                        }
                    }

                    // FramesPerPacket
                    CFNumberRef fppVal = (CFNumberRef)CFDictionaryGetValue(dict, CFSTR("FramesPerPacket"));
                    if (fppVal && CFGetTypeID(fppVal) == CFNumberGetTypeID()) {
                        int64_t fppNum = 0;
                        CFNumberGetValue(fppVal, kCFNumberSInt64Type, &fppNum);
                        if (fppNum >= 48 && fppNum <= 4800) {
                            gDriverState.framesPerPacket = (uint32_t)fppNum;
                        }
                    }
                }
                if (plist) CFRelease(plist);
            }
            CFRelease(stream);
        }
    }

    // Volume/mute state (persisted via plain file — CFPreferences blocked in coreaudiod)
    LoadVolumeState();

    os_log(sLog, "LoadConfig: IP=%{public}s port=%u latencyOffset=%ums fpp=%u volume=%.2f muted=%d",
           gDriverState.targetIP, gDriverState.targetPort, gDriverState.latencyOffsetMs,
           gDriverState.framesPerPacket, gDriverState.volumeScalar, gDriverState.muted);
}

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
    LoadConfig();

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

    // Re-read config on every StartIO so plist changes take effect without
    // needing to reload the entire driver.
    LoadConfig();

    // Tear down old engine if one exists (config may have changed)
    if (gDriverState.engine) {
        zinkos_engine_stop(gDriverState.engine);
        zinkos_engine_destroy(gDriverState.engine);
        gDriverState.engine = nullptr;
    }

    // Skip engine creation if no valid IP is configured — the driver still
    // shows up as a device, IO callbacks still run (writing silence), but
    // nothing is sent on the network. This prevents coreaudiod hangs.
    if (strcmp(gDriverState.targetIP, "0.0.0.0") == 0 ||
        gDriverState.targetIP[0] == '\0') {
        os_log(sLog, "ZinkosStartIO: no receiver IP configured, running without network");
    } else {
        gDriverState.engine = zinkos_engine_create(gDriverState.targetIP, gDriverState.targetPort, gDriverState.framesPerPacket);
        if (!gDriverState.engine) {
            os_log_error(sLog, "ZinkosStartIO: failed to create engine for %{public}s:%u — running without network",
                         gDriverState.targetIP, gDriverState.targetPort);
        } else {
            int32_t ret = zinkos_engine_start(gDriverState.engine);
            if (ret != 0) {
                os_log_error(sLog, "ZinkosStartIO: engine start failed — running without network");
                zinkos_engine_destroy(gDriverState.engine);
                gDriverState.engine = nullptr;
            } else {
                os_log(sLog, "ZinkosStartIO: streaming to %{public}s:%u",
                       gDriverState.targetIP, gDriverState.targetPort);
            }
        }
    }

    gDriverState.anchorHostTime = mach_absolute_time();
    gDriverState.anchorSampleTime = 0;
    gDriverState.ioRunning = true;

    os_log(sLog, "ZinkosStartIO: IO started (engine=%s)", gDriverState.engine ? "active" : "none");
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

    // Force-save final volume (throttle may have skipped the last update)
    sLastPersistTime = 0;
    PersistVolumeState();

    os_log(sLog, "ZinkosStopIO: streaming stopped");
    return kAudioHardwareNoError;
}

static OSStatus ZinkosGetZeroTimeStamp(AudioServerPlugInDriverRef /*inDriver*/, AudioObjectID /*inDeviceObjectID*/, UInt32 /*inClientID*/, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed)
{
    if (!gDriverState.ioRunning) {
        return kAudioHardwareNotRunningError;
    }

    // Synthesize timestamps based on anchor time set at StartIO.
    UInt64 currentHostTime = mach_absolute_time();
    UInt64 elapsed = currentHostTime - gDriverState.anchorHostTime;
    UInt64 elapsedFrames = elapsed / gDriverState.ticksPerFrame;

    // Round down to the nearest zero-timestamp period boundary
    const UInt64 ztsPeriod = 128;
    UInt64 cycleCount = elapsedFrames / ztsPeriod;

    *outSampleTime = (Float64)(cycleCount * ztsPeriod);
    *outHostTime = gDriverState.anchorHostTime + (cycleCount * ztsPeriod * gDriverState.ticksPerFrame);
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

    // Stream format is Float32 (CoreAudio native). Apply volume, convert to S16LE for engine.
    Float32* floatBuf = (Float32*)ioMainBuffer;
    int16_t* intBuf = (int16_t*)ioMainBuffer; // safe: int16 is smaller than float32

    Float32 scalar = gDriverState.volumeScalar;
    Float32 gain = gDriverState.muted ? 0.0f : (scalar * scalar); // quadratic curve
    UInt32 sampleCount = inIOBufferFrameSize * 2; // stereo
    for (UInt32 i = 0; i < sampleCount; i++) {
        Float32 sample = floatBuf[i] * gain;
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

static OSStatus ZinkosSetPropertyData(AudioServerPlugInDriverRef /*inDriver*/, AudioObjectID inObjectID, pid_t /*inClientProcessID*/, const AudioObjectPropertyAddress* inAddress, UInt32 /*inQualifierDataSize*/, const void* /*inQualifierData*/, UInt32 inDataSize, const void* inData)
{
    // Volume scalar
    if (inObjectID == kObjectID_Volume && inAddress->mSelector == kAudioLevelControlPropertyScalarValue) {
        if (inDataSize >= sizeof(Float32)) {
            Float32 val = *(const Float32*)inData;
            if (val < 0.0f) val = 0.0f;
            if (val > 1.0f) val = 1.0f;
            gDriverState.volumeScalar = val;
            PersistVolumeState();
        }
        return kAudioHardwareNoError;
    }
    // Volume dB
    if (inObjectID == kObjectID_Volume && inAddress->mSelector == kAudioLevelControlPropertyDecibelValue) {
        if (inDataSize >= sizeof(Float32)) {
            Float32 dB = *(const Float32*)inData;
            if (dB <= -96.0f) gDriverState.volumeScalar = 0.0f;
            else if (dB >= 0.0f) gDriverState.volumeScalar = 1.0f;
            else gDriverState.volumeScalar = powf(10.0f, dB / 20.0f);
            PersistVolumeState();
        }
        return kAudioHardwareNoError;
    }
    // Mute
    if (inObjectID == kObjectID_Mute && inAddress->mSelector == kAudioBooleanControlPropertyValue) {
        if (inDataSize >= sizeof(UInt32)) {
            gDriverState.muted = (*(const UInt32*)inData) != 0;
            PersistVolumeState();
        }
        return kAudioHardwareNoError;
    }
    return kAudioHardwareUnsupportedOperationError;
}
