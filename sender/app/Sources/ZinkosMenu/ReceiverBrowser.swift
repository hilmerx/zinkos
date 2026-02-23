import Foundation
import Network

@MainActor
final class ReceiverBrowser: ObservableObject {
    @Published var receivers: [ReceiverInfo] = []

    private var browser: NWBrowser?

    func start() {
        let params = NWParameters()
        params.includePeerToPeer = true

        let descriptor = NWBrowser.Descriptor.bonjourWithTXTRecord(
            type: "_zinkos._udp",
            domain: nil
        )
        let browser = NWBrowser(for: descriptor, using: params)

        browser.browseResultsChangedHandler = { [weak self] results, _ in
            Task { @MainActor in
                self?.handleResults(results)
            }
        }

        browser.stateUpdateHandler = { state in
            if case .failed(let error) = state {
                print("Browser failed: \(error)")
            }
        }

        browser.start(queue: .main)
        self.browser = browser
    }

    func stop() {
        browser?.cancel()
        browser = nil
    }

    private func handleResults(_ results: Set<NWBrowser.Result>) {
        var found: [ReceiverInfo] = []
        for result in results {
            guard case .service(let name, _, _, _) = result.endpoint else {
                continue
            }

            // Extract the mDNS hostname from the service name.
            // Avahi uses "%h" which is the machine hostname.
            // The service name "Zinkos on <hostname>" → hostname is after "Zinkos on "
            var hostname = name
            if let range = name.range(of: "Zinkos on ") {
                hostname = String(name[range.upperBound...])
            }
            // The .local mDNS hostname for resolution
            let mdnsHost = "\(hostname).local"

            // Parse TXT record for proto version
            var protoVersion: UInt8? = nil
            if case .bonjour(let txt) = result.metadata {
                let dict = txt.dictionary
                if let val = dict["proto"], let n = UInt8(val) {
                    protoVersion = n
                }
            }

            let info = ReceiverInfo(
                id: name,
                displayName: name,
                host: mdnsHost,
                port: 4010,
                protoVersion: protoVersion
            )
            found.append(info)
        }
        receivers = found.sorted { $0.displayName < $1.displayName }
    }
}
