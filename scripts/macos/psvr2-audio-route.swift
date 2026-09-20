import CoreAudio
import Foundation

enum Command: String {
    case getDefault = "get-default"
    case routePSVR2 = "route-psvr2"
    case setDefault = "set-default"
}

func stringProperty(_ objectID: AudioObjectID, _ selector: AudioObjectPropertySelector) -> String? {
    var address = AudioObjectPropertyAddress(
        mSelector: selector,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var value: CFString?
    var size = UInt32(MemoryLayout<CFString?>.stride)
    let status = withUnsafeMutablePointer(to: &value) { pointer in
        AudioObjectGetPropertyData(objectID, &address, 0, nil, &size, pointer)
    }
    guard status == noErr, let value else { return nil }
    return value as String
}

func allDevices() -> [AudioDeviceID] {
    var address = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyDevices,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var size: UInt32 = 0
    let system = AudioObjectID(kAudioObjectSystemObject)
    guard AudioObjectGetPropertyDataSize(system, &address, 0, nil, &size) == noErr else { return [] }
    var devices = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.stride)
    guard AudioObjectGetPropertyData(system, &address, 0, nil, &size, &devices) == noErr else { return [] }
    return devices
}

func getDefault(_ selector: AudioObjectPropertySelector) -> AudioDeviceID? {
    var address = AudioObjectPropertyAddress(
        mSelector: selector,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var value = AudioDeviceID(0)
    var size = UInt32(MemoryLayout<AudioDeviceID>.stride)
    let system = AudioObjectID(kAudioObjectSystemObject)
    guard AudioObjectGetPropertyData(system, &address, 0, nil, &size, &value) == noErr else { return nil }
    return value
}

func setDefault(_ id: AudioDeviceID, selector: AudioObjectPropertySelector) -> OSStatus {
    var address = AudioObjectPropertyAddress(
        mSelector: selector,
        mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain
    )
    var value = id
    let system = AudioObjectID(kAudioObjectSystemObject)
    return AudioObjectSetPropertyData(
        system,
        &address,
        0,
        nil,
        UInt32(MemoryLayout<AudioDeviceID>.stride),
        &value
    )
}

func findPSVR2() -> AudioDeviceID? {
    for id in allDevices() {
        guard let name = stringProperty(id, kAudioObjectPropertyName) else { continue }
        let normalized = name.lowercased().replacingOccurrences(of: " ", with: "")
        if normalized.contains("psvr2") { return id }
    }
    return nil
}

guard CommandLine.arguments.count >= 2, let command = Command(rawValue: CommandLine.arguments[1]) else {
    fputs("usage: psvr2-audio-route get-default | route-psvr2 | set-default <device-id>\n", stderr)
    exit(2)
}

switch command {
case .getDefault:
    guard let id = getDefault(kAudioHardwarePropertyDefaultOutputDevice) else { exit(1) }
    let name = stringProperty(id, kAudioObjectPropertyName) ?? "<unknown>"
    print("\(id)\t\(name)")
case .routePSVR2:
    guard let id = findPSVR2() else {
        fputs("PS VR2 CoreAudio output device not found\n", stderr)
        exit(1)
    }
    let s1 = setDefault(id, selector: kAudioHardwarePropertyDefaultOutputDevice)
    let s2 = setDefault(id, selector: kAudioHardwarePropertyDefaultSystemOutputDevice)
    guard s1 == noErr && s2 == noErr else {
        fputs("Failed to set PS VR2 as default output: \(s1), \(s2)\n", stderr)
        exit(1)
    }
    let name = stringProperty(id, kAudioObjectPropertyName) ?? "<unknown>"
    print("\(id)\t\(name)")
case .setDefault:
    guard CommandLine.arguments.count >= 3, let raw = UInt32(CommandLine.arguments[2]) else {
        fputs("set-default requires a numeric AudioDeviceID\n", stderr)
        exit(2)
    }
    let id = AudioDeviceID(raw)
    let s1 = setDefault(id, selector: kAudioHardwarePropertyDefaultOutputDevice)
    let s2 = setDefault(id, selector: kAudioHardwarePropertyDefaultSystemOutputDevice)
    guard s1 == noErr && s2 == noErr else {
        fputs("Failed to restore default output: \(s1), \(s2)\n", stderr)
        exit(1)
    }
    let name = stringProperty(id, kAudioObjectPropertyName) ?? "<unknown>"
    print("\(id)\t\(name)")
}
