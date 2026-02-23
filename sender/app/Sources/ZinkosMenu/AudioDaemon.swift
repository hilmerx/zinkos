import Foundation

struct AudioDaemon {
    /// Validate an IP or hostname is reachable (resolves + UDP sendto succeeds)
    static func isReachable(_ host: String) -> Bool {
        let trimmed = host.trimmingCharacters(in: .whitespaces)
        if trimmed.isEmpty || trimmed == "0.0.0.0" { return false }

        // Resolve hostname (or parse IP) via getaddrinfo
        var hints = addrinfo()
        hints.ai_family = AF_INET // IPv4 only
        hints.ai_socktype = SOCK_DGRAM

        var result: UnsafeMutablePointer<addrinfo>?
        let status = getaddrinfo(trimmed, "4010", &hints, &result)
        guard status == 0, let info = result else { return false }
        defer { freeaddrinfo(info) }

        // Try a quick UDP send
        let sock = socket(AF_INET, SOCK_DGRAM, 0)
        guard sock >= 0 else { return false }
        defer { close(sock) }

        var tv = timeval(tv_sec: 0, tv_usec: 200_000)
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        let testData: [UInt8] = [0]
        let sendResult = testData.withUnsafeBufferPointer { buf in
            sendto(sock, buf.baseAddress, 1, 0, info.pointee.ai_addr, info.pointee.ai_addrlen)
        }

        return sendResult >= 0
    }

    /// Restart coreaudiod only (config already saved separately).
    static func restartCoreAudio() {
        let script = """
        do shell script "killall coreaudiod" with administrator privileges
        """
        guard let appleScript = NSAppleScript(source: script) else { return }
        var error: NSDictionary?
        appleScript.executeAndReturnError(&error)
        if let error = error {
            print("coreaudiod restart failed: \(error)")
        }
    }
}
