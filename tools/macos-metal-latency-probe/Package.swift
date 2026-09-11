// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "MetalLatencyProbe",
    platforms: [
        .macOS(.v14)
    ],
    products: [
        .executable(name: "metal-latency-probe", targets: ["MetalLatencyProbe"])
    ],
    targets: [
        .executableTarget(
            name: "MetalLatencyProbe",
            path: "Sources/MetalLatencyProbe",
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("CoreGraphics"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("Metal"),
                .linkedFramework("QuartzCore")
            ]
        )
    ]
)
