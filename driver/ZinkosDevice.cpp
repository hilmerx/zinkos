//
// ZinkosDevice.cpp — Property dispatch for Plugin, Device, and Stream objects.
//
// CoreAudio queries properties by (objectID, selector, scope, element).
// We handle three objects: Plugin (ID 1), Device (ID 2), Stream (ID 3).
//

#include "ZinkosPlugin.h"
#include "ZinkosDevice.h"
#include <CoreAudio/AudioServerPlugIn.h>
#include <os/log.h>

static os_log_t sDevLog = os_log_create("com.zinkos.driver", "device");

// ============================================================
// Helper macros
// ============================================================

#define RETURN_SIZE(type) do { *outDataSize = sizeof(type); return kAudioHardwareNoError; } while(0)

static inline OSStatus WriteUInt32(void* outData, UInt32* outDataSize, UInt32 value) {
    *(UInt32*)outData = value;
    *outDataSize = sizeof(UInt32);
    return kAudioHardwareNoError;
}

static inline OSStatus WriteFloat64(void* outData, UInt32* outDataSize, Float64 value) {
    *(Float64*)outData = value;
    *outDataSize = sizeof(Float64);
    return kAudioHardwareNoError;
}

static inline OSStatus WriteCFString(void* outData, UInt32* outDataSize, const char* str) {
    CFStringRef cfStr = CFStringCreateWithCString(NULL, str, kCFStringEncodingUTF8);
    *(CFStringRef*)outData = cfStr;
    *outDataSize = sizeof(CFStringRef);
    return kAudioHardwareNoError;
}

// ============================================================
// Plugin properties (objectID == kAudioObjectPlugInObject, which is 1)
// ============================================================

static bool IsPluginProperty(AudioObjectID inObjectID, AudioObjectPropertySelector sel) {
    if (inObjectID != kAudioObjectPlugInObject) return false;
    switch (sel) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
            return true;
        default:
            return false;
    }
}

static OSStatus GetPluginPropertySize(AudioObjectPropertySelector sel, UInt32* outDataSize) {
    switch (sel) {
        case kAudioObjectPropertyBaseClass:     RETURN_SIZE(AudioClassID);
        case kAudioObjectPropertyClass:         RETURN_SIZE(AudioClassID);
        case kAudioObjectPropertyOwner:         RETURN_SIZE(AudioObjectID);
        case kAudioObjectPropertyManufacturer:  RETURN_SIZE(CFStringRef);
        case kAudioObjectPropertyOwnedObjects:  RETURN_SIZE(AudioObjectID); // owns 1 device
        case kAudioPlugInPropertyDeviceList:    RETURN_SIZE(AudioObjectID);
        case kAudioPlugInPropertyTranslateUIDToDevice: RETURN_SIZE(AudioObjectID);
        case kAudioPlugInPropertyResourceBundle: RETURN_SIZE(CFStringRef);
        default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus GetPluginProperty(AudioObjectPropertySelector sel, UInt32 inDataSize, UInt32* outDataSize, void* outData, UInt32 inQualifierDataSize, const void* inQualifierData) {
    switch (sel) {
        case kAudioObjectPropertyBaseClass:
            return WriteUInt32(outData, outDataSize, kAudioObjectClassID);
        case kAudioObjectPropertyClass:
            return WriteUInt32(outData, outDataSize, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner:
            return WriteUInt32(outData, outDataSize, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer:
            return WriteCFString(outData, outDataSize, kZinkos_Manufacturer);
        case kAudioObjectPropertyOwnedObjects:
            return WriteUInt32(outData, outDataSize, kObjectID_Device);
        case kAudioPlugInPropertyDeviceList:
            return WriteUInt32(outData, outDataSize, kObjectID_Device);
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            // Qualifier is a CFStringRef UID
            if (inQualifierDataSize == sizeof(CFStringRef)) {
                CFStringRef uid = *(CFStringRef*)inQualifierData;
                CFStringRef ourUID = CFStringCreateWithCString(NULL, kZinkos_DeviceUID, kCFStringEncodingUTF8);
                AudioObjectID result = kAudioObjectUnknown;
                if (CFEqual(uid, ourUID)) {
                    result = kObjectID_Device;
                }
                CFRelease(ourUID);
                return WriteUInt32(outData, outDataSize, result);
            }
            return WriteUInt32(outData, outDataSize, kAudioObjectUnknown);
        }
        case kAudioPlugInPropertyResourceBundle:
            return WriteCFString(outData, outDataSize, "");
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

// ============================================================
// Device properties (objectID == kObjectID_Device)
// ============================================================

static bool IsDeviceProperty(AudioObjectID inObjectID, AudioObjectPropertySelector sel) {
    if (inObjectID != kObjectID_Device) return false;
    switch (sel) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
        case kAudioDevicePropertyRelatedDevices:
        case 'ctrl':  // kAudioObjectPropertyControlList — empty, no controls
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
            return true;
        default:
            return false;
    }
}

static OSStatus GetDevicePropertySize(AudioObjectPropertySelector sel, const AudioObjectPropertyAddress* inAddress, UInt32* outDataSize) {
    switch (sel) {
        case kAudioObjectPropertyBaseClass:     RETURN_SIZE(AudioClassID);
        case kAudioObjectPropertyClass:         RETURN_SIZE(AudioClassID);
        case kAudioObjectPropertyOwner:         RETURN_SIZE(AudioObjectID);
        case kAudioObjectPropertyName:          RETURN_SIZE(CFStringRef);
        case kAudioObjectPropertyManufacturer:  RETURN_SIZE(CFStringRef);
        case kAudioDevicePropertyDeviceUID:     RETURN_SIZE(CFStringRef);
        case kAudioDevicePropertyModelUID:      RETURN_SIZE(CFStringRef);
        case kAudioDevicePropertyTransportType: RETURN_SIZE(UInt32);
        case kAudioDevicePropertyDeviceIsAlive: RETURN_SIZE(UInt32);
        case kAudioDevicePropertyDeviceIsRunning: RETURN_SIZE(UInt32);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: RETURN_SIZE(UInt32);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: RETURN_SIZE(UInt32);
        case kAudioObjectPropertyOwnedObjects:
            // Owns 1 stream (output only)
            if (inAddress->mScope == kAudioObjectPropertyScopeOutput || inAddress->mScope == kAudioObjectPropertyScopeGlobal) {
                *outDataSize = sizeof(AudioObjectID);
            } else {
                *outDataSize = 0;
            }
            return kAudioHardwareNoError;
        case kAudioDevicePropertyStreams:
            if (inAddress->mScope == kAudioObjectPropertyScopeOutput || inAddress->mScope == kAudioObjectPropertyScopeGlobal) {
                *outDataSize = sizeof(AudioObjectID);
            } else {
                *outDataSize = 0;
            }
            return kAudioHardwareNoError;
        case kAudioDevicePropertyRelatedDevices: RETURN_SIZE(AudioObjectID);
        case 'ctrl':  // control list — empty
            *outDataSize = 0;
            return kAudioHardwareNoError;
        case kAudioDevicePropertyLatency:       RETURN_SIZE(UInt32);
        case kAudioDevicePropertySafetyOffset:  RETURN_SIZE(UInt32);
        case kAudioDevicePropertyNominalSampleRate: RETURN_SIZE(Float64);
        case kAudioDevicePropertyAvailableNominalSampleRates:
            *outDataSize = sizeof(AudioValueRange);
            return kAudioHardwareNoError;
        case kAudioDevicePropertyClockDomain:   RETURN_SIZE(UInt32);
        case kAudioDevicePropertyZeroTimeStampPeriod: RETURN_SIZE(UInt32);
        case kAudioDevicePropertyIsHidden:      RETURN_SIZE(UInt32);
        case kAudioDevicePropertyPreferredChannelsForStereo:
            *outDataSize = 2 * sizeof(UInt32);
            return kAudioHardwareNoError;
        case kAudioDevicePropertyPreferredChannelLayout:
            *outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions) + kZinkos_ChannelCount * sizeof(AudioChannelDescription);
            return kAudioHardwareNoError;
        default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus GetDeviceProperty(AudioObjectPropertySelector sel, const AudioObjectPropertyAddress* inAddress, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    switch (sel) {
        case kAudioObjectPropertyBaseClass:
            return WriteUInt32(outData, outDataSize, kAudioObjectClassID);
        case kAudioObjectPropertyClass:
            return WriteUInt32(outData, outDataSize, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner:
            return WriteUInt32(outData, outDataSize, kAudioObjectPlugInObject);
        case kAudioObjectPropertyName:
            return WriteCFString(outData, outDataSize, kZinkos_DeviceName);
        case kAudioObjectPropertyManufacturer:
            return WriteCFString(outData, outDataSize, kZinkos_Manufacturer);
        case kAudioDevicePropertyDeviceUID:
            return WriteCFString(outData, outDataSize, kZinkos_DeviceUID);
        case kAudioDevicePropertyModelUID:
            return WriteCFString(outData, outDataSize, kZinkos_ModelUID);
        case kAudioDevicePropertyTransportType:
            return WriteUInt32(outData, outDataSize, kAudioDeviceTransportTypeVirtual);
        case kAudioDevicePropertyDeviceIsAlive:
            return WriteUInt32(outData, outDataSize, 1);
        case kAudioDevicePropertyDeviceIsRunning:
            return WriteUInt32(outData, outDataSize, gDriverState.ioRunning ? 1 : 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            // Only for output scope — we're an output-only device
            if (inAddress->mScope == kAudioObjectPropertyScopeOutput) {
                return WriteUInt32(outData, outDataSize, 1);
            }
            return WriteUInt32(outData, outDataSize, 0);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            if (inAddress->mScope == kAudioObjectPropertyScopeOutput) {
                return WriteUInt32(outData, outDataSize, 1);
            }
            return WriteUInt32(outData, outDataSize, 0);
        case kAudioObjectPropertyOwnedObjects:
            if (inAddress->mScope == kAudioObjectPropertyScopeOutput || inAddress->mScope == kAudioObjectPropertyScopeGlobal) {
                return WriteUInt32(outData, outDataSize, kObjectID_Stream);
            }
            *outDataSize = 0;
            return kAudioHardwareNoError;
        case kAudioDevicePropertyStreams:
            // Only output stream
            if (inAddress->mScope == kAudioObjectPropertyScopeOutput || inAddress->mScope == kAudioObjectPropertyScopeGlobal) {
                return WriteUInt32(outData, outDataSize, kObjectID_Stream);
            }
            *outDataSize = 0;
            return kAudioHardwareNoError;
        case kAudioDevicePropertyRelatedDevices:
            return WriteUInt32(outData, outDataSize, kObjectID_Device);
        case 'ctrl':  // control list — empty
            *outDataSize = 0;
            return kAudioHardwareNoError;
        case kAudioDevicePropertyLatency:
            return WriteUInt32(outData, outDataSize, kZinkos_DeviceLatency);
        case kAudioDevicePropertySafetyOffset:
            return WriteUInt32(outData, outDataSize, kZinkos_SafetyOffset);
        case kAudioDevicePropertyNominalSampleRate:
            return WriteFloat64(outData, outDataSize, kZinkos_SampleRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            AudioValueRange range = { kZinkos_SampleRate, kZinkos_SampleRate };
            memcpy(outData, &range, sizeof(range));
            *outDataSize = sizeof(range);
            return kAudioHardwareNoError;
        }
        case kAudioDevicePropertyClockDomain:
            return WriteUInt32(outData, outDataSize, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod:
            // How many frames between zero timestamps. Using 256 as IO cycle size.
            return WriteUInt32(outData, outDataSize, 256);
        case kAudioDevicePropertyIsHidden:
            return WriteUInt32(outData, outDataSize, 0); // visible in Sound preferences
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            UInt32 channels[2] = { 1, 2 };
            memcpy(outData, channels, sizeof(channels));
            *outDataSize = sizeof(channels);
            return kAudioHardwareNoError;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            AudioChannelLayout* layout = (AudioChannelLayout*)outData;
            layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            layout->mChannelBitmap = 0;
            layout->mNumberChannelDescriptions = kZinkos_ChannelCount;
            layout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
            layout->mChannelDescriptions[0].mChannelFlags = 0;
            layout->mChannelDescriptions[0].mCoordinates[0] = 0;
            layout->mChannelDescriptions[0].mCoordinates[1] = 0;
            layout->mChannelDescriptions[0].mCoordinates[2] = 0;
            layout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            layout->mChannelDescriptions[1].mChannelFlags = 0;
            layout->mChannelDescriptions[1].mCoordinates[0] = 0;
            layout->mChannelDescriptions[1].mCoordinates[1] = 0;
            layout->mChannelDescriptions[1].mCoordinates[2] = 0;
            *outDataSize = offsetof(AudioChannelLayout, mChannelDescriptions) + kZinkos_ChannelCount * sizeof(AudioChannelDescription);
            return kAudioHardwareNoError;
        }
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

// ============================================================
// Stream properties (objectID == kObjectID_Stream)
// ============================================================

static bool IsStreamProperty(AudioObjectID inObjectID, AudioObjectPropertySelector sel) {
    if (inObjectID != kObjectID_Stream) return false;
    switch (sel) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyName:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
        default:
            return false;
    }
}

static AudioStreamBasicDescription MakeStreamFormat() {
    AudioStreamBasicDescription desc = {};
    desc.mSampleRate = kZinkos_SampleRate;
    desc.mFormatID = kAudioFormatLinearPCM;
    desc.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    desc.mBitsPerChannel = kZinkos_BitsPerChannel;
    desc.mChannelsPerFrame = kZinkos_ChannelCount;
    desc.mFramesPerPacket = 1;
    desc.mBytesPerFrame = kZinkos_ChannelCount * (kZinkos_BitsPerChannel / 8);
    desc.mBytesPerPacket = desc.mBytesPerFrame;
    return desc;
}

static OSStatus GetStreamPropertySize(AudioObjectPropertySelector sel, UInt32* outDataSize) {
    switch (sel) {
        case kAudioObjectPropertyBaseClass:     RETURN_SIZE(AudioClassID);
        case kAudioObjectPropertyClass:         RETURN_SIZE(AudioClassID);
        case kAudioObjectPropertyOwner:         RETURN_SIZE(AudioObjectID);
        case kAudioObjectPropertyOwnedObjects:
            *outDataSize = 0; // streams own nothing
            return kAudioHardwareNoError;
        case kAudioObjectPropertyName:          RETURN_SIZE(CFStringRef);
        case kAudioStreamPropertyIsActive:      RETURN_SIZE(UInt32);
        case kAudioStreamPropertyDirection:     RETURN_SIZE(UInt32);
        case kAudioStreamPropertyTerminalType:  RETURN_SIZE(UInt32);
        case kAudioStreamPropertyStartingChannel: RETURN_SIZE(UInt32);
        case kAudioStreamPropertyLatency:       RETURN_SIZE(UInt32);
        case kAudioStreamPropertyVirtualFormat: RETURN_SIZE(AudioStreamBasicDescription);
        case kAudioStreamPropertyPhysicalFormat: RETURN_SIZE(AudioStreamBasicDescription);
        case kAudioStreamPropertyAvailableVirtualFormats:
            *outDataSize = sizeof(AudioStreamRangedDescription);
            return kAudioHardwareNoError;
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outDataSize = sizeof(AudioStreamRangedDescription);
            return kAudioHardwareNoError;
        default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus GetStreamProperty(AudioObjectPropertySelector sel, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    switch (sel) {
        case kAudioObjectPropertyBaseClass:
            return WriteUInt32(outData, outDataSize, kAudioObjectClassID);
        case kAudioObjectPropertyClass:
            return WriteUInt32(outData, outDataSize, kAudioStreamClassID);
        case kAudioObjectPropertyOwner:
            return WriteUInt32(outData, outDataSize, kObjectID_Device);
        case kAudioObjectPropertyOwnedObjects:
            *outDataSize = 0; // streams own nothing
            return kAudioHardwareNoError;
        case kAudioObjectPropertyName:
            return WriteCFString(outData, outDataSize, kZinkos_StreamName);
        case kAudioStreamPropertyIsActive:
            return WriteUInt32(outData, outDataSize, 1);
        case kAudioStreamPropertyDirection:
            return WriteUInt32(outData, outDataSize, 0); // 0 = output, 1 = input
        case kAudioStreamPropertyTerminalType:
            return WriteUInt32(outData, outDataSize, kAudioStreamTerminalTypeLine);
        case kAudioStreamPropertyStartingChannel:
            return WriteUInt32(outData, outDataSize, 1);
        case kAudioStreamPropertyLatency:
            return WriteUInt32(outData, outDataSize, kZinkos_StreamLatency);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            AudioStreamBasicDescription desc = MakeStreamFormat();
            memcpy(outData, &desc, sizeof(desc));
            *outDataSize = sizeof(desc);
            return kAudioHardwareNoError;
        }
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription ranged = {};
            ranged.mFormat = MakeStreamFormat();
            ranged.mSampleRateRange.mMinimum = kZinkos_SampleRate;
            ranged.mSampleRateRange.mMaximum = kZinkos_SampleRate;
            memcpy(outData, &ranged, sizeof(ranged));
            *outDataSize = sizeof(ranged);
            return kAudioHardwareNoError;
        }
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

// ============================================================
// Public dispatch functions
// ============================================================

OSStatus ZinkosDevice_HasProperty(
    AudioServerPlugInDriverRef /*inDriver*/,
    AudioObjectID inObjectID,
    pid_t /*inClientProcessID*/,
    const AudioObjectPropertyAddress* inAddress)
{
    if (IsPluginProperty(inObjectID, inAddress->mSelector)) return kAudioHardwareNoError;
    if (IsDeviceProperty(inObjectID, inAddress->mSelector)) return kAudioHardwareNoError;
    if (IsStreamProperty(inObjectID, inAddress->mSelector)) return kAudioHardwareNoError;
    // Log unknown property queries — helps diagnose what the host expects
    os_log(sDevLog, "HasProperty: UNKNOWN obj=%u sel='%{public}.4s' scope='%{public}.4s'",
           (unsigned)inObjectID,
           (const char*)&inAddress->mSelector,
           (const char*)&inAddress->mScope);
    return kAudioHardwareUnknownPropertyError;
}

OSStatus ZinkosDevice_IsPropertySettable(
    AudioServerPlugInDriverRef /*inDriver*/,
    AudioObjectID /*inObjectID*/,
    pid_t /*inClientProcessID*/,
    const AudioObjectPropertyAddress* /*inAddress*/,
    Boolean* outIsSettable)
{
    // Nothing is settable for now
    *outIsSettable = false;
    return kAudioHardwareNoError;
}

OSStatus ZinkosDevice_GetPropertyDataSize(
    AudioServerPlugInDriverRef /*inDriver*/,
    AudioObjectID inObjectID,
    pid_t /*inClientProcessID*/,
    const AudioObjectPropertyAddress* inAddress,
    UInt32 /*inQualifierDataSize*/,
    const void* /*inQualifierData*/,
    UInt32* outDataSize)
{
    OSStatus result;
    if (inObjectID == kAudioObjectPlugInObject)
        result = GetPluginPropertySize(inAddress->mSelector, outDataSize);
    else if (inObjectID == kObjectID_Device)
        result = GetDevicePropertySize(inAddress->mSelector, inAddress, outDataSize);
    else if (inObjectID == kObjectID_Stream)
        result = GetStreamPropertySize(inAddress->mSelector, outDataSize);
    else
        result = kAudioHardwareUnknownPropertyError;

    if (result != kAudioHardwareNoError) {
        os_log_error(sDevLog, "GetPropertyDataSize: FAILED obj=%u sel='%{public}.4s' scope='%{public}.4s' err=%d",
                     (unsigned)inObjectID,
                     (const char*)&inAddress->mSelector,
                     (const char*)&inAddress->mScope,
                     (int)result);
    }
    return result;
}

OSStatus ZinkosDevice_GetPropertyData(
    AudioServerPlugInDriverRef /*inDriver*/,
    AudioObjectID inObjectID,
    pid_t /*inClientProcessID*/,
    const AudioObjectPropertyAddress* inAddress,
    UInt32 inQualifierDataSize,
    const void* inQualifierData,
    UInt32 inDataSize,
    UInt32* outDataSize,
    void* outData)
{
    OSStatus result;
    if (inObjectID == kAudioObjectPlugInObject)
        result = GetPluginProperty(inAddress->mSelector, inDataSize, outDataSize, outData, inQualifierDataSize, inQualifierData);
    else if (inObjectID == kObjectID_Device)
        result = GetDeviceProperty(inAddress->mSelector, inAddress, inDataSize, outDataSize, outData);
    else if (inObjectID == kObjectID_Stream)
        result = GetStreamProperty(inAddress->mSelector, inDataSize, outDataSize, outData);
    else
        result = kAudioHardwareUnknownPropertyError;

    if (result != kAudioHardwareNoError) {
        os_log_error(sDevLog, "GetPropertyData: FAILED obj=%u sel='%{public}.4s' scope='%{public}.4s' err=%d",
                     (unsigned)inObjectID,
                     (const char*)&inAddress->mSelector,
                     (const char*)&inAddress->mScope,
                     (int)result);
    }
    return result;
}
