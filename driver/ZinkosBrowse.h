#ifndef ZINKOS_BROWSE_H
#define ZINKOS_BROWSE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start Bonjour browsing for _zinkos._udp services.
// If manual IP is configured (non-default), publishes device immediately and skips browsing.
void ZinkosBrowse_Start(void);

// Stop browsing and clean up the browse thread.
void ZinkosBrowse_Stop(void);

// Publish/unpublish the device to CoreAudio (calls PropertiesChanged on plugin object).
void ZinkosBrowse_PublishDevice(void);
void ZinkosBrowse_UnpublishDevice(void);

// Returns true if the device is currently published (visible to CoreAudio).
bool ZinkosBrowse_IsDevicePublished(void);

#ifdef __cplusplus
}
#endif

#endif // ZINKOS_BROWSE_H
