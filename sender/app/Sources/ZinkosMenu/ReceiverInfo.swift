import Foundation

/// Must match PROTO_VERSION in engine/src/config.rs
let senderProtoVersion: UInt8 = 1

struct ReceiverInfo: Identifiable, Hashable {
    let id: String          // unique service name
    let displayName: String // e.g. "Zinkos on living-room-pi"
    let host: String        // hostname from mDNS
    let port: UInt16
    let protoVersion: UInt8? // from mDNS TXT "proto=N"

    var protoMismatch: Bool {
        guard let rv = protoVersion else { return false }
        return rv != senderProtoVersion
    }
}
