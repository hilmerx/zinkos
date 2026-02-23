import Foundation

/// Single source of truth: /Library/Preferences/com.zinkos.driver.plist
/// Read by: driver (direct file read), CLI (defaults read), app (this code)
/// Written by: CLI (sudo defaults write), app (via AppleScript with admin)
struct PlistConfig {
    private static let plistPath = "/Library/Preferences/com.zinkos.driver.plist"

    /// Read the plist dict from disk
    private static func readDict() -> NSDictionary? {
        NSDictionary(contentsOfFile: plistPath)
    }

    static var currentIP: String? {
        readDict()?["ReceiverIP"] as? String
    }

    static var currentPort: UInt16 {
        let val = readDict()?["ReceiverPort"] as? Int ?? 0
        return val > 0 ? UInt16(val) : 4010
    }

    static var latencyOffset: UInt32 {
        let val = readDict()?["LatencyOffsetMs"] as? Int ?? 0
        return val >= 0 ? UInt32(val) : 0
    }

    /// Write all settings via AppleScript (triggers admin password prompt).
    /// This writes to /Library/Preferences/ which the driver can read.
    static func saveSettings(ip: String, port: UInt16, latency: UInt32) {
        var cmds: [String] = []
        if !ip.isEmpty {
            cmds.append("defaults write /Library/Preferences/com.zinkos.driver ReceiverIP -string '\(ip)'")
        }
        cmds.append("defaults write /Library/Preferences/com.zinkos.driver ReceiverPort -int \(port)")
        cmds.append("defaults write /Library/Preferences/com.zinkos.driver LatencyOffsetMs -int \(latency)")

        let joined = cmds.joined(separator: " && ")
        let script = "do shell script \"\(joined)\" with administrator privileges"
        guard let appleScript = NSAppleScript(source: script) else { return }
        var error: NSDictionary?
        appleScript.executeAndReturnError(&error)
        if let error = error {
            print("Failed to write config: \(error)")
        }
    }
}
