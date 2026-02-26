//
// ZinkosBrowse.cpp — Bonjour browsing for _zinkos._udp receivers.
//
// Runs a background thread that uses DNSServiceBrowse to discover receivers
// on the local network. When a receiver is found, resolves its address and
// publishes the Zinkos device to CoreAudio. When the receiver disappears
// (and IO is not running), unpublishes the device.
//

#include "ZinkosBrowse.h"
#include "ZinkosPlugin.h"
#include <dns_sd.h>
#include <os/log.h>
#include <pthread.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <sys/select.h>
#include <arpa/inet.h>
#include <mach/mach_time.h>

static os_log_t sBrowseLog = os_log_create("com.zinkos.driver", "browse");

static const char* kServiceType = "_zinkos._udp";
static const uint64_t kUnpublishGraceNs = 3ULL * 1000000000ULL; // 3 seconds

// Browse thread state
static pthread_t sBrowseThread;
static bool sBrowseThreadRunning = false;
static int sStopPipe[2] = { -1, -1 };  // pipe to signal thread shutdown
static DNSServiceRef sBrowseRef = NULL;
static uint64_t sUnpublishDeadlineNs = 0; // 0 = no pending unpublish (browse thread only)

// Cached timebase for mach_absolute_time → nanoseconds conversion
static mach_timebase_info_data_t sTimebase = {};

static uint64_t NowNs()
{
    if (sTimebase.denom == 0) mach_timebase_info(&sTimebase);
    return mach_absolute_time() * sTimebase.numer / sTimebase.denom;
}

// Forward declarations
static void* BrowseThreadFunc(void* arg);
static void DNSSD_API BrowseCallback(
    DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex,
    DNSServiceErrorType errorCode, const char* serviceName,
    const char* regtype, const char* replyDomain, void* context);
static void DNSSD_API ResolveCallback(
    DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex,
    DNSServiceErrorType errorCode, const char* fullname,
    const char* hosttarget, uint16_t port, uint16_t txtLen,
    const unsigned char* txtRecord, void* context);
static void DNSSD_API AddrInfoCallback(
    DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex,
    DNSServiceErrorType errorCode, const char* hostname,
    const struct sockaddr* address, uint32_t ttl, void* context);

// Notify CoreAudio that the device list changed
static void NotifyDeviceListChanged()
{
    if (!gDriverState.host) return;

    AudioObjectPropertyAddress addrs[2] = {
        { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
        { kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
    };

    gDriverState.host->PropertiesChanged(gDriverState.host, kAudioObjectPlugInObject, 2, addrs);
}

// ============================================================
// Public API
// ============================================================

void ZinkosBrowse_PublishDevice()
{
    if (__c11_atomic_load((_Atomic bool*)&gDriverState.devicePublished, __ATOMIC_ACQUIRE)) {
        return;  // already published
    }

    os_log(sBrowseLog, "Publishing device to CoreAudio");
    __c11_atomic_store((_Atomic bool*)&gDriverState.devicePublished, true, __ATOMIC_RELEASE);
    NotifyDeviceListChanged();
}

void ZinkosBrowse_UnpublishDevice()
{
    if (!__c11_atomic_load((_Atomic bool*)&gDriverState.devicePublished, __ATOMIC_ACQUIRE)) {
        return;  // already unpublished
    }

    os_log(sBrowseLog, "Unpublishing device from CoreAudio");
    __c11_atomic_store((_Atomic bool*)&gDriverState.devicePublished, false, __ATOMIC_RELEASE);
    NotifyDeviceListChanged();
}

bool ZinkosBrowse_IsDevicePublished()
{
    return __c11_atomic_load((_Atomic bool*)&gDriverState.devicePublished, __ATOMIC_ACQUIRE);
}

void ZinkosBrowse_Start()
{
    // Check if manual IP is configured (non-default)
    if (strcmp(gDriverState.targetIP, "0.0.0.0") != 0 &&
        gDriverState.targetIP[0] != '\0') {
        gDriverState.manualIPConfigured = true;
        os_log(sBrowseLog, "Manual IP configured (%{public}s), publishing immediately — skipping Bonjour browse",
               gDriverState.targetIP);
        ZinkosBrowse_PublishDevice();
        return;
    }

    gDriverState.manualIPConfigured = false;
    os_log(sBrowseLog, "No manual IP, starting Bonjour browse for %{public}s", kServiceType);

    // Create stop pipe
    if (pipe(sStopPipe) != 0) {
        os_log_error(sBrowseLog, "Failed to create stop pipe");
        return;
    }

    // Start browse
    DNSServiceErrorType err = DNSServiceBrowse(
        &sBrowseRef,
        0,                       // flags
        kDNSServiceInterfaceIndexAny,
        kServiceType,
        NULL,                    // domain (default)
        BrowseCallback,
        NULL);                   // context

    if (err != kDNSServiceErr_NoError) {
        os_log_error(sBrowseLog, "DNSServiceBrowse failed: %d", err);
        close(sStopPipe[0]);
        close(sStopPipe[1]);
        sStopPipe[0] = sStopPipe[1] = -1;
        return;
    }

    // Launch browse thread
    sBrowseThreadRunning = true;
    if (pthread_create(&sBrowseThread, NULL, BrowseThreadFunc, NULL) != 0) {
        os_log_error(sBrowseLog, "Failed to create browse thread");
        DNSServiceRefDeallocate(sBrowseRef);
        sBrowseRef = NULL;
        close(sStopPipe[0]);
        close(sStopPipe[1]);
        sStopPipe[0] = sStopPipe[1] = -1;
        sBrowseThreadRunning = false;
    }
}

void ZinkosBrowse_Stop()
{
    if (!sBrowseThreadRunning) return;

    os_log(sBrowseLog, "Stopping Bonjour browse");

    // Signal the thread to stop
    if (sStopPipe[1] >= 0) {
        char c = 1;
        write(sStopPipe[1], &c, 1);
    }

    // Wait for thread to finish
    pthread_join(sBrowseThread, NULL);
    sBrowseThreadRunning = false;

    // Clean up
    if (sBrowseRef) {
        DNSServiceRefDeallocate(sBrowseRef);
        sBrowseRef = NULL;
    }
    if (sStopPipe[0] >= 0) { close(sStopPipe[0]); sStopPipe[0] = -1; }
    if (sStopPipe[1] >= 0) { close(sStopPipe[1]); sStopPipe[1] = -1; }
}

// ============================================================
// Browse thread
// ============================================================

// Attempt to (re-)establish a DNSServiceBrowse connection.
// Returns the DNS-SD socket fd on success, -1 on failure.
// On success, sBrowseRef is set to the new browse ref.
static int EstablishBrowse()
{
    if (sBrowseRef) {
        DNSServiceRefDeallocate(sBrowseRef);
        sBrowseRef = NULL;
    }

    DNSServiceErrorType err = DNSServiceBrowse(
        &sBrowseRef, 0, kDNSServiceInterfaceIndexAny,
        kServiceType, NULL, BrowseCallback, NULL);

    if (err != kDNSServiceErr_NoError) {
        os_log_error(sBrowseLog, "DNSServiceBrowse failed: %d", err);
        sBrowseRef = NULL;
        return -1;
    }

    return DNSServiceRefSockFD(sBrowseRef);
}

static void* BrowseThreadFunc(void* /*arg*/)
{
    int dnsFd = DNSServiceRefSockFD(sBrowseRef);
    if (dnsFd < 0) {
        os_log_error(sBrowseLog, "DNSServiceRefSockFD returned invalid fd");
        return NULL;
    }

    os_log(sBrowseLog, "Browse thread started");

    while (true) {
        // Check if grace period expired
        if (sUnpublishDeadlineNs != 0 && NowNs() >= sUnpublishDeadlineNs) {
            os_log(sBrowseLog, "Grace period expired, unpublishing device");
            sUnpublishDeadlineNs = 0;
            ZinkosBrowse_UnpublishDevice();
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(dnsFd, &readfds);
        FD_SET(sStopPipe[0], &readfds);
        int maxFd = (dnsFd > sStopPipe[0]) ? dnsFd : sStopPipe[0];

        // Use timeout if an unpublish is pending, otherwise block indefinitely
        struct timeval* tvPtr = NULL;
        struct timeval tv;
        if (sUnpublishDeadlineNs != 0) {
            uint64_t now = NowNs();
            uint64_t remainNs = (sUnpublishDeadlineNs > now) ? (sUnpublishDeadlineNs - now) : 0;
            tv.tv_sec = (long)(remainNs / 1000000000ULL);
            tv.tv_usec = (int)((remainNs % 1000000000ULL) / 1000);
            tvPtr = &tv;
        }

        int ret = select(maxFd + 1, &readfds, NULL, NULL, tvPtr);
        if (ret < 0) {
            if (errno == EINTR) continue;
            os_log_error(sBrowseLog, "select() failed: %d", errno);
            break;
        }
        if (ret == 0) continue; // timeout — loop back to check deadline

        // Check for stop signal
        if (FD_ISSET(sStopPipe[0], &readfds)) {
            os_log(sBrowseLog, "Browse thread received stop signal");
            break;
        }

        // Process DNS-SD events
        if (FD_ISSET(dnsFd, &readfds)) {
            DNSServiceErrorType err = DNSServiceProcessResult(sBrowseRef);
            if (err != kDNSServiceErr_NoError) {
                // Connection to mDNSResponder lost (daemon restart, network change, etc.)
                // Unpublish the device — the receiver is effectively gone.
                os_log_error(sBrowseLog, "DNS-SD connection lost (err=%d), unpublishing device", err);
                __c11_atomic_store((_Atomic bool*)&gDriverState.discoveredAvailable, false, __ATOMIC_RELEASE);
                ZinkosBrowse_UnpublishDevice();

                // Try to reconnect with backoff (mDNSResponder may need time to restart)
                bool reconnected = false;
                for (int attempt = 0; attempt < 10; attempt++) {
                    // Check stop signal between retries
                    fd_set stopfds;
                    FD_ZERO(&stopfds);
                    FD_SET(sStopPipe[0], &stopfds);
                    struct timeval tv = { 2, 0 };  // 2 second backoff
                    if (select(sStopPipe[0] + 1, &stopfds, NULL, NULL, &tv) > 0) {
                        os_log(sBrowseLog, "Stop signal during reconnect");
                        goto exit_thread;
                    }

                    dnsFd = EstablishBrowse();
                    if (dnsFd >= 0) {
                        os_log(sBrowseLog, "Reconnected to mDNSResponder (attempt %d)", attempt + 1);
                        reconnected = true;
                        break;
                    }
                    os_log(sBrowseLog, "Reconnect attempt %d failed", attempt + 1);
                }

                if (!reconnected) {
                    os_log_error(sBrowseLog, "Failed to reconnect after 10 attempts, giving up");
                    break;
                }
            }
        }
    }

exit_thread:
    os_log(sBrowseLog, "Browse thread exiting");
    return NULL;
}

// ============================================================
// DNS-SD callback chain: Browse -> Resolve -> AddrInfo
// ============================================================

static void DNSSD_API BrowseCallback(
    DNSServiceRef /*sdRef*/, DNSServiceFlags flags, uint32_t interfaceIndex,
    DNSServiceErrorType errorCode, const char* serviceName,
    const char* regtype, const char* replyDomain, void* /*context*/)
{
    if (errorCode != kDNSServiceErr_NoError) {
        os_log_error(sBrowseLog, "BrowseCallback error: %d", errorCode);
        return;
    }

    if (flags & kDNSServiceFlagsAdd) {
        os_log(sBrowseLog, "Found receiver: %{public}s on interface %u", serviceName, interfaceIndex);

        // Cancel any pending unpublish — receiver is back
        if (sUnpublishDeadlineNs != 0) {
            os_log(sBrowseLog, "Cancelling pending unpublish — receiver reappeared");
            sUnpublishDeadlineNs = 0;
        }

        // Resolve to get hostname and port
        DNSServiceRef resolveRef = NULL;
        DNSServiceErrorType err = DNSServiceResolve(
            &resolveRef,
            0,
            interfaceIndex,
            serviceName,
            regtype,
            replyDomain,
            ResolveCallback,
            NULL);

        if (err == kDNSServiceErr_NoError) {
            // Process the resolve synchronously — single-shot
            DNSServiceProcessResult(resolveRef);
            DNSServiceRefDeallocate(resolveRef);
        } else {
            os_log_error(sBrowseLog, "DNSServiceResolve failed: %d", err);
        }
    } else {
        // Service removed — schedule unpublish after grace period.
        // Brief WiFi hiccups cause remove+add flaps; the grace period
        // avoids yanking the device out from under the user.
        os_log(sBrowseLog, "Receiver disappeared: %{public}s — unpublish in 3s", serviceName);
        __c11_atomic_store((_Atomic bool*)&gDriverState.discoveredAvailable, false, __ATOMIC_RELEASE);
        sUnpublishDeadlineNs = NowNs() + kUnpublishGraceNs;
    }
}

static void DNSSD_API ResolveCallback(
    DNSServiceRef /*sdRef*/, DNSServiceFlags /*flags*/, uint32_t interfaceIndex,
    DNSServiceErrorType errorCode, const char* /*fullname*/,
    const char* hosttarget, uint16_t port, uint16_t /*txtLen*/,
    const unsigned char* /*txtRecord*/, void* /*context*/)
{
    if (errorCode != kDNSServiceErr_NoError) {
        os_log_error(sBrowseLog, "ResolveCallback error: %d", errorCode);
        return;
    }

    // Store port (network byte order -> host byte order)
    gDriverState.discoveredPort = ntohs(port);
    os_log(sBrowseLog, "Resolved: host=%{public}s port=%u", hosttarget, gDriverState.discoveredPort);

    // Get the IP address
    DNSServiceRef addrRef = NULL;
    DNSServiceErrorType err = DNSServiceGetAddrInfo(
        &addrRef,
        0,
        interfaceIndex,
        kDNSServiceProtocol_IPv4,
        hosttarget,
        AddrInfoCallback,
        NULL);

    if (err == kDNSServiceErr_NoError) {
        DNSServiceProcessResult(addrRef);
        DNSServiceRefDeallocate(addrRef);
    } else {
        os_log_error(sBrowseLog, "DNSServiceGetAddrInfo failed: %d", err);
    }
}

static void DNSSD_API AddrInfoCallback(
    DNSServiceRef /*sdRef*/, DNSServiceFlags /*flags*/, uint32_t /*interfaceIndex*/,
    DNSServiceErrorType errorCode, const char* /*hostname*/,
    const struct sockaddr* address, uint32_t /*ttl*/, void* /*context*/)
{
    if (errorCode != kDNSServiceErr_NoError) {
        os_log_error(sBrowseLog, "AddrInfoCallback error: %d", errorCode);
        return;
    }

    if (address->sa_family != AF_INET) {
        return;  // only handle IPv4
    }

    const struct sockaddr_in* addr4 = (const struct sockaddr_in*)address;
    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr4->sin_addr, ipStr, sizeof(ipStr));

    // Store discovered IP
    strlcpy(gDriverState.discoveredIP, ipStr, sizeof(gDriverState.discoveredIP));

    // Mark as available (release so other threads see IP/port writes)
    __c11_atomic_store((_Atomic bool*)&gDriverState.discoveredAvailable, true, __ATOMIC_RELEASE);

    os_log(sBrowseLog, "Discovered receiver at %{public}s:%u", gDriverState.discoveredIP, gDriverState.discoveredPort);

    // Publish the device so it appears in Sound settings
    ZinkosBrowse_PublishDevice();
}
