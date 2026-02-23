import SwiftUI

@main
struct ZinkosMenuApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var browser = ReceiverBrowser()

    var body: some Scene {
        WindowGroup {
            MenuView(browser: browser)
                .task { browser.start() }
        }
        .windowResizability(.contentSize)
        .defaultSize(width: 300, height: 400)
    }
}

class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApplication.shared.setActivationPolicy(.regular)

        // Set logo as dock/Cmd+Tab icon with rounded corners
        if let logoURL = Bundle.module.url(forResource: "logo", withExtension: "png", subdirectory: "Resources"),
           let original = NSImage(contentsOf: logoURL) {
            let iconSize = NSSize(width: 512, height: 512)
            let rounded = NSImage(size: iconSize)
            rounded.lockFocus()
            let rect = NSRect(origin: .zero, size: iconSize)
            let path = NSBezierPath(roundedRect: rect, xRadius: 100, yRadius: 100)
            path.addClip()
            NSColor.white.setFill()
            rect.fill()

            // Center logo preserving aspect ratio
            let logoAspect = original.size.width / original.size.height
            let drawWidth = iconSize.width * 0.7
            let drawHeight = drawWidth / logoAspect
            let drawX = (iconSize.width - drawWidth) / 2
            let drawY = (iconSize.height - drawHeight) / 2
            let drawRect = NSRect(x: drawX, y: drawY, width: drawWidth, height: drawHeight)
            original.draw(in: drawRect,
                         from: NSRect(origin: .zero, size: original.size),
                         operation: .sourceOver, fraction: 1.0)
            rounded.unlockFocus()
            NSApplication.shared.applicationIconImage = rounded
        }

        // Title + center + bring to front
        DispatchQueue.main.async {
            if let window = NSApplication.shared.windows.first {
                window.title = "ZINKOS"
                window.center()
                window.makeKeyAndOrderFront(nil)
            }
            NSApplication.shared.activate(ignoringOtherApps: true)
        }
    }
}
