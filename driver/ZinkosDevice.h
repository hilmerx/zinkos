#ifndef ZINKOS_DEVICE_H
#define ZINKOS_DEVICE_H

// Device property constants
#define kZinkos_DeviceUID       "ZinkosDevice_UID"
#define kZinkos_ModelUID        "ZinkosModel_UID"
#define kZinkos_DeviceName      "Zinkos"
#define kZinkos_Manufacturer    "Zinkos Audio"
#define kZinkos_StreamName      "Zinkos Output"

// Audio format — stream format is Float32 (CoreAudio native), converted to S16LE in DoIOOperation
#define kZinkos_SampleRate      48000.0
#define kZinkos_ChannelCount    2
#define kZinkos_BitsPerChannel  32

// Latency (in frames)
#define kZinkos_DeviceLatency   480     // ~10ms at 48kHz
#define kZinkos_StreamLatency   240     // ~5ms (one packet)
#define kZinkos_SafetyOffset    240     // ~5ms

// IO buffer size range (in frames)
#define kZinkos_MinBufferFrames 128
#define kZinkos_MaxBufferFrames 1024

#endif // ZINKOS_DEVICE_H
