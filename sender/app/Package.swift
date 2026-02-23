// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "Zinkos",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "Zinkos",
            path: "Sources/ZinkosMenu",
            resources: [.copy("Resources")]
        )
    ]
)
