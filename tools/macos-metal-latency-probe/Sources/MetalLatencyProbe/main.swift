import AppKit
import CoreGraphics
import CoreVideo
import Foundation
import Metal
import QuartzCore

private enum PresentMode: String {
    case immediate
    case timed
    case metal1
    case metal2

    var usesMetalDisplayLink: Bool {
        self == .metal1 || self == .metal2
    }

    var requestedFrameLatency: Float? {
        switch self {
        case .metal1: return 1.0
        case .metal2: return 2.0
        default: return nil
        }
    }
}

private struct Options {
    var mode: PresentMode = .immediate
    var displayName = "PS VR2"
    var drawableCount = 3
    var presentOffsetUs = 0.0
    var cpuDelayMs = 0.0
    var gpuBurnPasses = 0
    var waitCompleted = false
    var displaySync = true
    var framebufferOnly = true
    var frameLimit = 0
    var refreshOverrideHz: Double? = nil
    var tracePath: String? = nil

    static func usage() -> String {
        """
        metal-latency-probe [options]

          --mode immediate|timed|metal1|metal2   Presentation strategy (default: immediate)
          --display-name NAME                    NSScreen name (default: PS VR2; falls back to 4000 px wide)
          --drawable-count 2|3                   CAMetalLayer maximumDrawableCount (default: 3)
          --present-offset-us N                   timed mode: add N microseconds to CVDisplayLink output time
          --cpu-delay-ms N                       sleep after drawable acquisition before encoding/commit
          --gpu-burn-passes N                    synthetic compute passes before the visible clear
          --wait-completed                       call waitUntilCompleted() after commit
          --vsync                                set CAMetalLayer.displaySyncEnabled = true (default)
          --no-vsync                             set CAMetalLayer.displaySyncEnabled = false
          --framebuffer-only                     set CAMetalLayer.framebufferOnly = true (default)
          --no-framebuffer-only                  set CAMetalLayer.framebufferOnly = false
          --frames N                             stop after N submitted frames (0 = run until Ctrl-C/Quit)
          --refresh-hz N                         override requested CAMetalDisplayLink frame-rate range
          --trace PATH                           CSV path (default: /tmp/metal_latency_probe_<pid>.csv)
          --help

        Modes:
          immediate  CVDisplayLink-paced; commandBuffer.present(drawable)
          timed      CVDisplayLink-paced; commandBuffer.present(drawable, atTime: output + offset)
          metal1     CAMetalDisplayLink; preferredFrameLatency = 1; drawable.present()
          metal2     CAMetalDisplayLink; preferredFrameLatency = 2; drawable.present()
        """
    }

    static func parse(_ args: [String]) throws -> Options {
        var o = Options()
        var i = 1
        func value(_ flag: String) throws -> String {
            guard i + 1 < args.count else { throw ProbeError.argument("Missing value for \(flag)") }
            i += 1
            return args[i]
        }
        while i < args.count {
            let arg = args[i]
            switch arg {
            case "--mode":
                let raw = try value(arg)
                guard let m = PresentMode(rawValue: raw) else { throw ProbeError.argument("Unknown mode: \(raw)") }
                o.mode = m
            case "--display-name": o.displayName = try value(arg)
            case "--drawable-count": o.drawableCount = Int(try value(arg)) ?? 0
            case "--present-offset-us": o.presentOffsetUs = Double(try value(arg)) ?? .nan
            case "--cpu-delay-ms": o.cpuDelayMs = Double(try value(arg)) ?? .nan
            case "--gpu-burn-passes": o.gpuBurnPasses = Int(try value(arg)) ?? -1
            case "--frames": o.frameLimit = Int(try value(arg)) ?? -1
            case "--refresh-hz": o.refreshOverrideHz = Double(try value(arg))
            case "--trace": o.tracePath = try value(arg)
            case "--wait-completed": o.waitCompleted = true
            case "--vsync": o.displaySync = true
            case "--no-vsync": o.displaySync = false
            case "--framebuffer-only": o.framebufferOnly = true
            case "--no-framebuffer-only": o.framebufferOnly = false
            case "--help", "-h":
                print(usage())
                exit(0)
            default: throw ProbeError.argument("Unknown argument: \(arg)")
            }
            i += 1
        }
        guard o.drawableCount == 2 || o.drawableCount == 3 else {
            throw ProbeError.argument("--drawable-count must be 2 or 3")
        }
        guard o.presentOffsetUs.isFinite else { throw ProbeError.argument("Invalid --present-offset-us") }
        guard o.cpuDelayMs.isFinite && o.cpuDelayMs >= 0 else { throw ProbeError.argument("Invalid --cpu-delay-ms") }
        guard o.gpuBurnPasses >= 0 else { throw ProbeError.argument("Invalid --gpu-burn-passes") }
        guard o.frameLimit >= 0 else { throw ProbeError.argument("Invalid --frames") }
        if let hz = o.refreshOverrideHz, (!hz.isFinite || hz <= 1) {
            throw ProbeError.argument("Invalid --refresh-hz")
        }
        return o
    }
}

private enum ProbeError: Error, CustomStringConvertible {
    case argument(String)
    case runtime(String)

    var description: String {
        switch self {
        case .argument(let s), .runtime(let s): return s
        }
    }
}

private final class FrameRecord {
    let sequence: UInt64
    let mode: String
    let refreshHz: Double
    let drawableCount: Int
    let displaySync: Bool
    let framebufferOnly: Bool
    let cpuDelayMs: Double
    let gpuBurnPasses: Int
    let waitCompleted: Bool
    let callbackTime: Double
    let cvNow: Double?
    let cvOutput: Double?
    let metalTargetDeadline: Double?
    let metalTargetPresentation: Double?
    let drawableID: UInt64
    let nextDrawableStart: Double?
    let nextDrawableEnd: Double?
    let encodeStart: Double
    var encodeEnd: Double
    var commitTime: Double
    let requestedPresentTime: Double?

    var gpuStart: Double?
    var gpuEnd: Double?
    var completedTime: Double?
    var presentedHandlerTime: Double?
    var presentedTime: Double?

    init(
        sequence: UInt64,
        mode: String,
        refreshHz: Double,
        drawableCount: Int,
        displaySync: Bool,
        framebufferOnly: Bool,
        cpuDelayMs: Double,
        gpuBurnPasses: Int,
        waitCompleted: Bool,
        callbackTime: Double,
        cvNow: Double?,
        cvOutput: Double?,
        metalTargetDeadline: Double?,
        metalTargetPresentation: Double?,
        drawableID: UInt64,
        nextDrawableStart: Double?,
        nextDrawableEnd: Double?,
        encodeStart: Double,
        encodeEnd: Double,
        commitTime: Double,
        requestedPresentTime: Double?
    ) {
        self.sequence = sequence
        self.mode = mode
        self.refreshHz = refreshHz
        self.drawableCount = drawableCount
        self.displaySync = displaySync
        self.framebufferOnly = framebufferOnly
        self.cpuDelayMs = cpuDelayMs
        self.gpuBurnPasses = gpuBurnPasses
        self.waitCompleted = waitCompleted
        self.callbackTime = callbackTime
        self.cvNow = cvNow
        self.cvOutput = cvOutput
        self.metalTargetDeadline = metalTargetDeadline
        self.metalTargetPresentation = metalTargetPresentation
        self.drawableID = drawableID
        self.nextDrawableStart = nextDrawableStart
        self.nextDrawableEnd = nextDrawableEnd
        self.encodeStart = encodeStart
        self.encodeEnd = encodeEnd
        self.commitTime = commitTime
        self.requestedPresentTime = requestedPresentTime
    }
}

private final class CSVLogger {
    private let queue = DispatchQueue(label: "metal-latency-probe.csv")
    private var pending: [UInt64: FrameRecord] = [:]
    private let handle: FileHandle
    let path: String

    private static let header = [
        "sequence", "mode", "refresh_hz", "drawable_count", "display_sync", "framebuffer_only", "cpu_delay_ms", "gpu_burn_passes", "wait_completed",
        "callback_time_s", "cv_now_s", "cv_output_s", "metal_target_deadline_s", "metal_target_presentation_s",
        "drawable_id", "next_drawable_start_s", "next_drawable_end_s", "next_drawable_wait_ms",
        "encode_start_s", "encode_end_s", "commit_time_s", "requested_present_time_s",
        "gpu_start_s", "gpu_end_s", "gpu_duration_ms", "command_completed_s",
        "presented_handler_s", "presented_time_s", "callback_to_presented_ms", "commit_to_presented_ms",
        "gpu_end_to_presented_ms", "requested_to_presented_ms", "metal_target_to_presented_ms"
    ].joined(separator: ",") + "\n"

    init(path: String) throws {
        self.path = path
        FileManager.default.createFile(atPath: path, contents: nil)
        guard let h = FileHandle(forWritingAtPath: path) else {
            throw ProbeError.runtime("Could not open trace file: \(path)")
        }
        handle = h
        try handle.write(contentsOf: Data(Self.header.utf8))
    }

    deinit {
        try? handle.close()
    }

    func submit(_ record: FrameRecord) {
        queue.async { self.pending[record.sequence] = record }
    }

    func completed(sequence: UInt64, commandBuffer: MTLCommandBuffer) {
        let now = CACurrentMediaTime()
        let gpuStart = commandBuffer.gpuStartTime
        let gpuEnd = commandBuffer.gpuEndTime
        queue.async {
            guard let r = self.pending[sequence] else { return }
            r.gpuStart = gpuStart > 0 ? gpuStart : nil
            r.gpuEnd = gpuEnd > 0 ? gpuEnd : nil
            r.completedTime = now
            self.maybeWrite(sequence)
        }
    }

    func presented(sequence: UInt64, drawable: MTLDrawable) {
        let handlerTime = CACurrentMediaTime()
        let presented = drawable.presentedTime
        queue.async {
            guard let r = self.pending[sequence] else { return }
            r.presentedHandlerTime = handlerTime
            r.presentedTime = presented > 0 ? presented : nil
            self.maybeWrite(sequence)
        }
    }

    private func fmt(_ value: Double?) -> String {
        guard let value, value.isFinite else { return "" }
        return String(format: "%.9f", value)
    }

    private func deltaMs(_ a: Double?, _ b: Double?) -> Double? {
        guard let a, let b else { return nil }
        return (a - b) * 1000.0
    }

    private func maybeWrite(_ sequence: UInt64) {
        guard let r = pending[sequence], r.completedTime != nil, r.presentedHandlerTime != nil else { return }
        let nextWait = deltaMs(r.nextDrawableEnd, r.nextDrawableStart)
        let gpuDuration = deltaMs(r.gpuEnd, r.gpuStart)
        let callbackToPresented = deltaMs(r.presentedTime, r.callbackTime)
        let commitToPresented = deltaMs(r.presentedTime, r.commitTime)
        let gpuEndToPresented = deltaMs(r.presentedTime, r.gpuEnd)
        let requestedToPresented = deltaMs(r.presentedTime, r.requestedPresentTime)
        let targetToPresented = deltaMs(r.presentedTime, r.metalTargetPresentation)

        let fields: [String] = [
            "\(r.sequence)", r.mode, fmt(r.refreshHz), "\(r.drawableCount)", r.displaySync ? "1" : "0",
            r.framebufferOnly ? "1" : "0", fmt(r.cpuDelayMs), "\(r.gpuBurnPasses)", r.waitCompleted ? "1" : "0",
            fmt(r.callbackTime), fmt(r.cvNow), fmt(r.cvOutput), fmt(r.metalTargetDeadline), fmt(r.metalTargetPresentation),
            "\(r.drawableID)", fmt(r.nextDrawableStart), fmt(r.nextDrawableEnd), fmt(nextWait),
            fmt(r.encodeStart), fmt(r.encodeEnd), fmt(r.commitTime), fmt(r.requestedPresentTime),
            fmt(r.gpuStart), fmt(r.gpuEnd), fmt(gpuDuration), fmt(r.completedTime), fmt(r.presentedHandlerTime),
            fmt(r.presentedTime), fmt(callbackToPresented), fmt(commitToPresented), fmt(gpuEndToPresented),
            fmt(requestedToPresented), fmt(targetToPresented)
        ]
        let line = fields.joined(separator: ",") + "\n"
        try? handle.write(contentsOf: Data(line.utf8))
        pending.removeValue(forKey: sequence)
    }

    func close() {
        queue.sync {
            try? handle.synchronize()
            try? handle.close()
        }
    }
}

private final class Renderer: NSObject, CAMetalDisplayLinkDelegate {
    let options: Options
    let device: MTLDevice
    let layer: CAMetalLayer
    let queue: MTLCommandQueue
    let logger: CSVLogger
    let refreshHz: Double

    private var computePipeline: MTLComputePipelineState?
    private var burnBuffer: MTLBuffer?
    private var sequence: UInt64 = 0
    private var cvDisplayLink: CVDisplayLink?
    private var metalDisplayLink: CAMetalDisplayLink?
    private var stopping = false

    init(options: Options, layer: CAMetalLayer, displayID: CGDirectDisplayID, refreshHz: Double) throws {
        self.options = options
        guard let device = layer.device ?? MTLCreateSystemDefaultDevice() else {
            throw ProbeError.runtime("No Metal device")
        }
        self.device = device
        self.layer = layer
        guard let commandQueue = device.makeCommandQueue() else {
            throw ProbeError.runtime("Could not create Metal command queue")
        }
        self.queue = commandQueue
        self.refreshHz = refreshHz

        let trace = options.tracePath ?? "/tmp/metal_latency_probe_\(ProcessInfo.processInfo.processIdentifier).csv"
        self.logger = try CSVLogger(path: trace)
        super.init()

        if options.gpuBurnPasses > 0 {
            try setupGpuBurn()
        }

        if options.mode.usesMetalDisplayLink {
            let link = CAMetalDisplayLink(metalLayer: layer)
            link.delegate = self
            link.preferredFrameLatency = options.mode.requestedFrameLatency!
            let requestedHz = Float(options.refreshOverrideHz ?? refreshHz)
            if requestedHz > 1 {
                link.preferredFrameRateRange = CAFrameRateRange(minimum: requestedHz, maximum: requestedHz, preferred: requestedHz)
            }
            metalDisplayLink = link
        } else {
            var link: CVDisplayLink?
            let rc = CVDisplayLinkCreateWithCGDisplay(displayID, &link)
            guard rc == kCVReturnSuccess, let link else {
                throw ProbeError.runtime("CVDisplayLinkCreateWithCGDisplay failed: \(rc)")
            }
            cvDisplayLink = link
            CVDisplayLinkSetOutputCallback(link, cvOutputCallback, Unmanaged.passUnretained(self).toOpaque())
        }
    }

    private func setupGpuBurn() throws {
        let source = """
        #include <metal_stdlib>
        using namespace metal;
        kernel void burn(device uint *values [[buffer(0)]], constant uint &seed [[buffer(1)]], uint gid [[thread_position_in_grid]]) {
            uint x = values[gid] ^ seed ^ gid;
            for (uint i = 0; i < 64; ++i) {
                x = x * 1664525u + 1013904223u;
                x ^= (x << 13);
                x ^= (x >> 17);
                x ^= (x << 5);
            }
            values[gid] = x;
        }
        """
        let library = try device.makeLibrary(source: source, options: nil)
        guard let function = library.makeFunction(name: "burn") else {
            throw ProbeError.runtime("Could not create burn compute function")
        }
        computePipeline = try device.makeComputePipelineState(function: function)
        let count = 262_144
        burnBuffer = device.makeBuffer(length: count * MemoryLayout<UInt32>.stride, options: .storageModeShared)
        if burnBuffer == nil { throw ProbeError.runtime("Could not allocate GPU burn buffer") }
    }

    func start() throws {
        print("Metal latency probe")
        print("  mode: \(options.mode.rawValue)")
        print("  refresh: \(String(format: "%.3f", refreshHz)) Hz")
        print("  drawable count: \(layer.maximumDrawableCount)")
        print("  display sync: \(layer.displaySyncEnabled ? "on" : "off")")
        print("  framebuffer only: \(layer.framebufferOnly ? "yes" : "no")")
        print("  CPU delay: \(options.cpuDelayMs) ms")
        print("  GPU burn passes: \(options.gpuBurnPasses)")
        print("  trace: \(logger.path)")

        if let link = metalDisplayLink {
            link.add(to: .main, forMode: .common)
            link.isPaused = false
        } else if let link = cvDisplayLink {
            let rc = CVDisplayLinkStart(link)
            if rc != kCVReturnSuccess { throw ProbeError.runtime("CVDisplayLinkStart failed: \(rc)") }
        }
    }

    func stop() {
        if stopping { return }
        stopping = true
        if let link = cvDisplayLink, CVDisplayLinkIsRunning(link) {
        }
        metalDisplayLink?.invalidate()
        logger.close()
    }

    fileprivate func onCVDisplayLink(nowHostTime: UInt64, outputHostTime: UInt64) {
        guard !stopping else { return }
        let frequency = CVGetHostClockFrequency()
        let now = nowHostTime > 0 ? Double(nowHostTime) / frequency : nil
        let output = outputHostTime > 0 ? Double(outputHostTime) / frequency : nil
        renderCV(callbackTime: CACurrentMediaTime(), cvNow: now, cvOutput: output)
    }

    private func renderCV(callbackTime: Double, cvNow: Double?, cvOutput: Double?) {
        let acquireStart = CACurrentMediaTime()
        guard let drawable = layer.nextDrawable() else { return }
        let acquireEnd = CACurrentMediaTime()
        var requested: Double? = nil
        if options.mode == .timed, let cvOutput {
            requested = cvOutput + options.presentOffsetUs / 1_000_000.0
        }
        encodeAndSubmit(
            drawable: drawable,
            callbackTime: callbackTime,
            cvNow: cvNow,
            cvOutput: cvOutput,
            metalDeadline: nil,
            metalPresentation: nil,
            acquireStart: acquireStart,
            acquireEnd: acquireEnd,
            requestedPresent: requested,
            metalDisplayLinkMode: false
        )
    }

    func metalDisplayLink(_ link: CAMetalDisplayLink, needsUpdate update: CAMetalDisplayLink.Update) {
        guard !stopping else { return }
        encodeAndSubmit(
            drawable: update.drawable,
            callbackTime: CACurrentMediaTime(),
            cvNow: nil,
            cvOutput: nil,
            metalDeadline: update.targetTimestamp,
            metalPresentation: update.targetPresentationTimestamp,
            acquireStart: nil,
            acquireEnd: nil,
            requestedPresent: nil,
            metalDisplayLinkMode: true
        )
    }

    private func encodeAndSubmit(
        drawable: CAMetalDrawable,
        callbackTime: Double,
        cvNow: Double?,
        cvOutput: Double?,
        metalDeadline: Double?,
        metalPresentation: Double?,
        acquireStart: Double?,
        acquireEnd: Double?,
        requestedPresent: Double?,
        metalDisplayLinkMode: Bool
    ) {
        if options.cpuDelayMs > 0 {
            Thread.sleep(forTimeInterval: options.cpuDelayMs / 1000.0)
        }

        sequence += 1
        let seq = sequence
        let encodeStart = CACurrentMediaTime()
        guard let commandBuffer = queue.makeCommandBuffer() else { return }
        commandBuffer.label = "latency-probe-\(seq)"

        if options.gpuBurnPasses > 0, let pipeline = computePipeline, let buffer = burnBuffer {
            let threads = buffer.length / MemoryLayout<UInt32>.stride
            for pass in 0..<options.gpuBurnPasses {
                guard let encoder = commandBuffer.makeComputeCommandEncoder() else { break }
                encoder.setComputePipelineState(pipeline)
                encoder.setBuffer(buffer, offset: 0, index: 0)
                var seed = UInt32(truncatingIfNeeded: seq &+ UInt64(pass))
                encoder.setBytes(&seed, length: MemoryLayout<UInt32>.stride, index: 1)
                let width = min(pipeline.maxTotalThreadsPerThreadgroup, 256)
                encoder.dispatchThreads(MTLSize(width: threads, height: 1, depth: 1),
                                        threadsPerThreadgroup: MTLSize(width: width, height: 1, depth: 1))
                encoder.endEncoding()
            }
        }

        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = drawable.texture
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = .store
        let white = (seq & 1) == 0
        let level = white ? 1.0 : 0.0
        pass.colorAttachments[0].clearColor = MTLClearColor(red: level, green: level, blue: level, alpha: 1.0)
        if let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) {
            encoder.endEncoding()
        }

        let encodeEnd = CACurrentMediaTime()

        drawable.addPresentedHandler { [weak self] d in
            self?.logger.presented(sequence: seq, drawable: d)
        }
        commandBuffer.addCompletedHandler { [weak self] cb in
            self?.logger.completed(sequence: seq, commandBuffer: cb)
        }

        if metalDisplayLinkMode {
            // CAMetalDisplayLink requires ordinary present(), not a timed present API.
        } else if options.mode == .timed, let requestedPresent {
            commandBuffer.present(drawable, atTime: requestedPresent)
        } else {
            commandBuffer.present(drawable)
        }

        let commitTime = CACurrentMediaTime()
        let record = FrameRecord(
            sequence: seq,
            mode: options.mode.rawValue,
            refreshHz: refreshHz,
            drawableCount: layer.maximumDrawableCount,
            displaySync: layer.displaySyncEnabled,
            framebufferOnly: layer.framebufferOnly,
            cpuDelayMs: options.cpuDelayMs,
            gpuBurnPasses: options.gpuBurnPasses,
            waitCompleted: options.waitCompleted,
            callbackTime: callbackTime,
            cvNow: cvNow,
            cvOutput: cvOutput,
            metalTargetDeadline: metalDeadline,
            metalTargetPresentation: metalPresentation,
            drawableID: UInt64(drawable.drawableID),
            nextDrawableStart: acquireStart,
            nextDrawableEnd: acquireEnd,
            encodeStart: encodeStart,
            encodeEnd: encodeEnd,
            commitTime: commitTime,
            requestedPresentTime: requestedPresent
        )
        logger.submit(record)

        commandBuffer.commit()
        if metalDisplayLinkMode {
            drawable.present()
        }
        if options.waitCompleted {
            commandBuffer.waitUntilCompleted()
        }

        if options.frameLimit > 0, seq >= UInt64(options.frameLimit) {
            stopping = true
            if let link = cvDisplayLink, CVDisplayLinkIsRunning(link) { CVDisplayLinkStop(link) }
            metalDisplayLink?.isPaused = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                NSApp.terminate(nil)
            }
        }
    }
}

nonisolated(unsafe) private let cvOutputCallback: CVDisplayLinkOutputCallback = { _, inNow, inOutput, _, _, context in
    guard let context else { return kCVReturnError }
    let renderer = Unmanaged<Renderer>.fromOpaque(context).takeUnretainedValue()
    renderer.onCVDisplayLink(nowHostTime: inNow.pointee.hostTime, outputHostTime: inOutput.pointee.hostTime)
    return kCVReturnSuccess
}

@MainActor
private final class AppDelegate: NSObject, NSApplicationDelegate {
    private let options: Options
    private var renderer: Renderer?
    private var window: NSWindow?

    init(options: Options) {
        self.options = options
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        do {
            try setup()
        } catch {
            fputs("metal-latency-probe: \(error)\n", stderr)
            NSApp.terminate(nil)
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        renderer?.stop()
    }

    private func setup() throws {
        guard let screen = findTargetScreen(name: options.displayName) else {
            throw ProbeError.runtime("Could not find display '\(options.displayName)' or 4000-pixel-wide fallback")
        }
        guard let displayID = displayID(for: screen) else {
            throw ProbeError.runtime("Could not get CGDirectDisplayID for target screen")
        }
        guard let device = MTLCreateSystemDefaultDevice() else {
            throw ProbeError.runtime("No Metal device")
        }

        let pixelWidth = CGDisplayPixelsWide(displayID)
        let pixelHeight = CGDisplayPixelsHigh(displayID)
        let displayModeHz = CGDisplayCopyDisplayMode(displayID)?.refreshRate ?? 0
        let refreshHz = options.refreshOverrideHz ?? (displayModeHz > 1 ? displayModeHz : 120.0)

        let metalLayer = CAMetalLayer()
        metalLayer.device = device
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = options.framebufferOnly
        metalLayer.maximumDrawableCount = options.drawableCount
        metalLayer.displaySyncEnabled = options.displaySync
        metalLayer.presentsWithTransaction = false
        metalLayer.isOpaque = true
        metalLayer.contentsScale = screen.backingScaleFactor
        metalLayer.drawableSize = CGSize(width: pixelWidth, height: pixelHeight)

        let view = NSView(frame: screen.frame)
        view.wantsLayer = true
        view.layer = metalLayer

        let window = NSWindow(
            contentRect: screen.frame,
            styleMask: .borderless,
            backing: .buffered,
            defer: false,
            screen: screen
        )
        window.contentView = view
        window.backgroundColor = .black
        window.hasShadow = false
        window.hidesOnDeactivate = false
        window.ignoresMouseEvents = true
        window.level = .mainMenu + 1
        window.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]
        window.setFrame(screen.frame, display: true)
        window.orderFrontRegardless()
        self.window = window

        CATransaction.flush()
        print("Target display: \(screen.localizedName) \(pixelWidth)x\(pixelHeight), CG refresh \(String(format: "%.3f", displayModeHz)) Hz")

        let renderer = try Renderer(options: options, layer: metalLayer, displayID: displayID, refreshHz: refreshHz)
        self.renderer = renderer
        try renderer.start()
    }

    private func displayID(for screen: NSScreen) -> CGDirectDisplayID? {
        guard let n = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else { return nil }
        return CGDirectDisplayID(n.uint32Value)
    }

    private func findTargetScreen(name: String) -> NSScreen? {
        if let exact = NSScreen.screens.first(where: { $0.localizedName.caseInsensitiveCompare(name) == .orderedSame }) {
            return exact
        }
        for screen in NSScreen.screens {
            if let id = displayID(for: screen), CGDisplayPixelsWide(id) == 4000 {
                return screen
            }
        }
        return nil
    }
}

private let options: Options
do {
    options = try Options.parse(CommandLine.arguments)
} catch {
    fputs("metal-latency-probe: \(error)\n\n\(Options.usage())\n", stderr)
    exit(2)
}

let app = NSApplication.shared
app.setActivationPolicy(.regular)
private let delegate = AppDelegate(options: options)
app.delegate = delegate
app.run()
