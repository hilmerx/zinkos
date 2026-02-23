import SwiftUI

struct MenuView: View {
    @ObservedObject var browser: ReceiverBrowser
    @State private var selectedId: String?
    @State private var manualIP: String = ""
    @State private var portText: String = ""
    @State private var latencyText: String = ""
    @State private var dirty = false
    @State private var statusMessage: String?
    @State private var errorMessage: String?
    @State private var saving = false

    private var currentIP: String? { PlistConfig.currentIP }
    private var currentPort: UInt16 { PlistConfig.currentPort }
    private var currentLatency: UInt32 { PlistConfig.latencyOffset }

    private var isValidIP: Bool {
        let val = manualIP.trimmingCharacters(in: .whitespaces)
        if val.isEmpty { return true } // empty is ok (clears config)
        // Accept IPv4 addresses (192.168.1.x) or hostnames (foo.local)
        let ipParts = val.split(separator: ".")
        let isIPv4 = ipParts.count == 4 && ipParts.allSatisfy { UInt8($0) != nil }
        let isHostname = val.contains(".") && val.allSatisfy { $0.isLetter || $0.isNumber || $0 == "." || $0 == "-" }
        return isIPv4 || isHostname
    }

    private var isValidPort: Bool {
        guard let port = UInt16(portText) else { return false }
        return port > 0
    }

    private var isValidLatency: Bool {
        UInt32(latencyText) != nil
    }

    private var canSave: Bool {
        isValidIP && isValidPort && isValidLatency
    }

    var body: some View {
        VStack(spacing: 0) {
            // Header
            LogoImage()
                .frame(height: 160)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 24)
                .background(.white)

            // Status bar
            if let msg = statusMessage {
                HStack(spacing: 6) {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                        .font(.caption)
                    Text(msg)
                        .font(.system(.caption, design: .monospaced))
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 8)
                .background(Color.green.opacity(0.1))
            } else if let ip = currentIP, !dirty {
                HStack(spacing: 6) {
                    Circle()
                        .fill(.green)
                        .frame(width: 8, height: 8)
                    Text("\(ip):\(currentPort)")
                        .font(.system(.caption, design: .monospaced))
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 8)
                .background(Color.green.opacity(0.1))
            }

            if let err = errorMessage {
                HStack(spacing: 6) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(.red)
                        .font(.caption)
                    Text(err)
                        .font(.caption)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 8)
                .background(Color.red.opacity(0.1))
            }

            Divider()

            // Receiver list
            VStack(alignment: .leading, spacing: 0) {
                Text("RECEIVERS")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(.secondary)
                    .tracking(1)
                    .padding(.horizontal, 16)
                    .padding(.top, 12)
                    .padding(.bottom, 8)

                if browser.receivers.isEmpty {
                    HStack(spacing: 8) {
                        ProgressView().controlSize(.small)
                        Text("Scanning network...")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 20)
                } else {
                    ForEach(browser.receivers) { receiver in
                        ReceiverRow(
                            receiver: receiver,
                            isSelected: isSelected(receiver),
                            isResolving: false
                        ) {
                            selectReceiver(receiver)
                        }
                    }
                }
            }

            Divider()
                .padding(.top, 4)

            // Settings
            VStack(alignment: .leading, spacing: 10) {
                Text("SETTINGS")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(.secondary)
                    .tracking(1)

                HStack {
                    Text("IP")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .frame(width: 50, alignment: .leading)
                    TextField("192.168.1.x", text: $manualIP)
                        .textFieldStyle(.roundedBorder)
                        .font(.system(.body, design: .monospaced))
                        .foregroundColor(isValidIP ? .primary : .red)
                        .onChange(of: manualIP) { _ in dirty = true; statusMessage = nil }
                }

                HStack {
                    Text("Port")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .frame(width: 50, alignment: .leading)
                    TextField("4010", text: $portText)
                        .textFieldStyle(.roundedBorder)
                        .font(.system(.body, design: .monospaced))
                        .foregroundColor(isValidPort ? .primary : .red)
                        .onChange(of: portText) { _ in dirty = true; statusMessage = nil }
                }

                HStack {
                    Text("Latency")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .frame(width: 50, alignment: .leading)
                    TextField("0", text: $latencyText)
                        .textFieldStyle(.roundedBorder)
                        .font(.system(.body, design: .monospaced))
                        .foregroundColor(isValidLatency ? .primary : .red)
                        .onChange(of: latencyText) { _ in dirty = true; statusMessage = nil }
                    Text("ms")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Button {
                    saveAndReload()
                } label: {
                    HStack {
                        if saving {
                            ProgressView().controlSize(.small)
                        } else {
                            Image(systemName: "arrow.clockwise")
                        }
                        Text(saving ? "Checking..." : (dirty ? "Save & Reload" : "Reload"))
                    }
                    .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .disabled(!canSave || saving)
            }
            .padding(16)

            // Version footer
            Text("v\(AppDelegate.appVersion)")
                .font(.system(size: 10, design: .monospaced))
                .foregroundStyle(.tertiary)
                .frame(maxWidth: .infinity)
                .padding(.bottom, 8)
        }
        .frame(width: 300)
        .background(.white)
        .environment(\.colorScheme, .light)
        .onAppear { loadCurrentValues() }
        .onChange(of: browser.receivers) { _ in matchSavedReceiver() }
    }

    private func loadCurrentValues() {
        manualIP = currentIP ?? ""
        portText = "\(currentPort)"
        latencyText = "\(currentLatency)"
        dirty = false
        statusMessage = nil
    }

    private func matchSavedReceiver() {
        // Auto-select if a discovered receiver matches the saved config
        guard selectedId == nil, !manualIP.isEmpty else { return }
        if let match = browser.receivers.first(where: { $0.host == manualIP }) {
            selectedId = match.id
        }
    }

    private func isSelected(_ receiver: ReceiverInfo) -> Bool {
        selectedId == receiver.id
    }

    private func selectReceiver(_ receiver: ReceiverInfo) {
        selectedId = receiver.id
        // Use the mDNS hostname — the engine resolves it at connect time,
        // so DHCP IP changes are handled automatically.
        manualIP = receiver.host
        portText = "\(receiver.port)"
        dirty = true
        statusMessage = nil
    }

    private func saveAndReload() {
        let ip = manualIP.trimmingCharacters(in: .whitespaces)
        let port = UInt16(portText) ?? 4010
        let latency = UInt32(latencyText) ?? 0

        errorMessage = nil
        saving = true

        // Validate IP reachability on background thread
        DispatchQueue.global(qos: .userInitiated).async {
            let reachable = ip.isEmpty || AudioDaemon.isReachable(ip)

            DispatchQueue.main.async {
                saving = false

                guard reachable else {
                    errorMessage = "Cannot reach \(ip) — check the IP and network"
                    return
                }

                PlistConfig.saveSettings(ip: ip, port: port, latency: latency)
                AudioDaemon.restartCoreAudio()
                dirty = false
                statusMessage = "Saved — reloading"
                errorMessage = nil

                DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
                    statusMessage = nil
                }
            }
        }
    }
}

struct ReceiverRow: View {
    let receiver: ReceiverInfo
    let isSelected: Bool
    let isResolving: Bool
    let onSelect: () -> Void

    var body: some View {
        Button(action: onSelect) {
            HStack(spacing: 12) {
                Image(systemName: isSelected ? "hifispeaker.fill" : "hifispeaker")
                    .font(.title3)
                    .foregroundStyle(isSelected ? .blue : .primary)
                    .frame(width: 24)

                VStack(alignment: .leading, spacing: 2) {
                    Text(receiver.displayName)
                        .font(.callout.weight(.medium))
                    if receiver.protoMismatch {
                        Text("Protocol mismatch — update receiver")
                            .font(.caption)
                            .foregroundStyle(.red)
                    } else if receiver.protoVersion != nil {
                        Text("\(receiver.host) · proto v\(receiver.protoVersion!)")
                            .font(.caption)
                            .foregroundStyle(.green)
                    } else {
                        Text(receiver.host)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }

                Spacer()

                if receiver.protoMismatch {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                } else if isResolving {
                    ProgressView().controlSize(.small)
                } else if isSelected {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                }
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 10)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .background(isSelected ? Color.blue.opacity(0.08) : Color.clear)
    }
}

struct LogoImage: View {
    var body: some View {
        if let url = Bundle.module.url(forResource: "logo", withExtension: "png", subdirectory: "Resources"),
           let nsImage = NSImage(contentsOf: url) {
            Image(nsImage: nsImage)
                .resizable()
                .aspectRatio(contentMode: .fit)
        }
    }
}
