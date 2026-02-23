import Foundation

struct ReceiverInfo: Identifiable, Hashable {
    let id: String          // unique service name
    let displayName: String // e.g. "Zinkos on living-room-pi"
    let host: String        // hostname from mDNS
    let port: UInt16
}
