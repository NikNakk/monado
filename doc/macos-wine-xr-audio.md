# macOS Wine XR audio routing

This document describes the audio-routing design for Windows XR applications
running under Wine on the macOS Monado port.

## Goals

- Route application audio to the audio device associated with the active HMD.
- Do not change the macOS system default audio device.
- Do not hard-code PS VR2 in the generic Wine/OpenXR/OpenVR path.
- Preserve ordinary Windows audio semantics for applications using WASAPI,
  XAudio, DirectSound, Unity, Unreal, etc.
- Allow HMD-specific discovery to live near the HMD/native platform layer while
  keeping Wine endpoint selection generic.
- Restore any temporary Wine-prefix selection when the application exits.
- Leave room for microphone routing and OpenXR audio-device extensions later.

## Current temporary implementation

The first working implementation changes the macOS default CoreAudio output to
PS VR2 while the Wine game runs, then restores the previous default. This proves
the routing requirement but has undesirable global side effects.

It should remain a fallback/debug mode only.

## Wine behaviour we should use

Wine's mmdevapi/winecfg path already supports pinning its default render endpoint
to a specific physical endpoint.

Winecfg stores the selected endpoint IDs under:

    HKCU\Software\Wine\Drivers\<audio-driver>\DefaultOutput
    HKCU\Software\Wine\Drivers\<audio-driver>\DefaultVoiceOutput

The audio driver name is read from DEVPKEY_Device_Driver. On macOS the physical
winecoreaudio endpoints originate from CoreAudio physical devices, whose driver
device identifiers are CoreAudio UIDs.

This means the macOS system default does not need to change.

## Staged architecture

### Stage 1: Wine-prefix physical endpoint pinning

A small Windows helper runs inside the same Wine prefix:

    wine-hmd-audio.exe list
    wine-hmd-audio.exe select <friendly-name-substring>
    wine-hmd-audio.exe restore <driver> <old-output> <old-voice-output>

It enumerates IMMDevice render endpoints using Wine itself, reads
DEVPKEY_Device_Driver and IMMDevice::GetId(), updates the same registry values
as winecfg, and returns enough state for the launcher to restore the prefix.

Advantages:

- no macOS system-default changes;
- no Wine fork required;
- uses Wine's own endpoint IDs rather than reconstructing them on the host;
- applies to ordinary Windows audio APIs automatically.

Limitation:

- the selected defaults are prefix-global, not process-local. Concurrent apps in
  the same prefix could observe the temporary setting.

### Stage 2: generic HMD -> CoreAudio device discovery

The native macOS side should provide an HMD audio hint containing, in priority
order:

1. CoreAudio device UID;
2. USB identity / device ancestry sufficient to locate the CoreAudio device;
3. stable model-specific matching hints;
4. user-configured CoreAudio UID/name override.

The generic Wine launcher should not contain PS VR2 matching logic.

Proposed representation:

    struct xrt_audio_device_hint {
        const char *output_uid;
        const char *input_uid;
        uint16_t usb_vid;
        uint16_t usb_pid;
        const char *name_hint;
    };

This does not need to be part of xrt_device permanently; an IPC/session metadata
object may be a better fit once the design is proven.

For PS VR2 the initial implementation can obtain the CoreAudio output associated
with the headset and expose its UID. Future HMD drivers can supply equivalent
metadata without changing Wine-facing code.

### Stage 3: UID -> Wine endpoint mapping

Prefer exact CoreAudio UID mapping rather than friendly-name matching.

Wine already keeps a mapping below:

    HKCU\Software\Wine\Drivers\<audio-driver>\devices\0,<CoreAudio UID>

for render devices. The Windows helper can use Wine/MMDevice enumeration plus
this mapping to choose the correct Windows endpoint.

Friendly-name matching exists only as an initial compatibility path.

### Stage 4: runtime/session integration

The native Monado service exposes the selected HMD audio hint to the Wine client
when the XR connection is established.

Suggested bridge metadata:

    audio.output_uid
    audio.input_uid
    audio.output_name
    audio.input_name

The Wine launcher/client resolves that metadata before starting the application
and pins the Wine render endpoint.

Longer term this could happen inside the Wine bridge itself instead of a shell
launcher, allowing non-launcher users to get the same behaviour.

### Stage 5: OpenXR audio-device extension

Implement XR_OCULUS_audio_device_guid where useful.

On Windows this extension returns the Windows MMDevice ID selected for headset
audio. Once the Wine endpoint corresponding to the HMD is known, the Wine OpenXR
runtime can return that endpoint ID directly.

This allows applications which explicitly ask OpenXR for the HMD audio device to
get the same endpoint that ordinary Windows audio APIs use.

### Stage 6: capture / microphone

Apply the same scheme to:

    DefaultInput
    DefaultVoiceInput

only when the active HMD exposes a microphone and the user has not explicitly
overridden capture routing.

Capture should be independent from render selection.

## Configuration

Proposed controls:

    MONADO_WINE_AUDIO_MODE=auto
        auto              Prefer Wine endpoint pinning, fall back safely.
        wine-endpoint     Require Wine physical endpoint pinning.
        system-default    Legacy temporary macOS-default implementation.
        unchanged         Do not alter audio routing.

    MONADO_WINE_AUDIO_DEVICE_UID=<CoreAudio UID>
        Explicit output device override.

    MONADO_WINE_AUDIO_DEVICE_MATCH=<string>
        Temporary/fallback friendly-name match.

    MONADO_WINE_AUDIO_CAPTURE_UID=<CoreAudio UID>
        Explicit capture device override.

The generic default should eventually be auto. During development keep the
existing system-default mechanism available so regressions are easy to isolate.

## Failure behaviour

Audio routing must never prevent an XR application from launching unless
MONADO_WINE_AUDIO_MODE=wine-endpoint was explicitly requested.

For auto mode:

1. try exact UID;
2. try HMD-provided fallback identity;
3. optionally fall back to the current Wine default;
4. log the selected path.

Do not silently change the macOS global default in auto mode once Wine endpoint
pinning is considered stable.

## Testing

At minimum test:

- Underture / OpenComposite, normal Windows audio;
- a Unity OpenXR application;
- an Unreal OpenXR application;
- application with an explicit audio-device picker;
- game with no audio-device picker;
- HMD disconnected before launch;
- HMD disconnected during playback;
- two Wine applications in the same prefix;
- a second HMD/audio interface;
- headset microphone routing.

For every test verify:

- macOS system default remains unchanged;
- selected Wine endpoint is the HMD endpoint;
- audio reaches the headset;
- selection is restored on process exit;
- XR rendering/pacing is unaffected.

## Current implementation status

Implemented on macos-wine-openvr-legacy-unity:

- `tests/windows/wine_hmd_audio.cpp`
  - enumerates Wine MMDevice render endpoints;
  - selects by friendly-name substring;
  - writes Wine's DefaultOutput and DefaultVoiceOutput values;
  - returns previous values and can restore them.
- `scripts/macos/build-wine-hmd-audio-helper.zsh`
  - builds the helper using MinGW.

Next implementation steps:

1. integrate helper into run-wine-openvr-game.zsh behind an experimental mode;
2. add CoreAudio UID inventory/discovery;
3. select by exact CoreAudio UID rather than friendly name;
4. move HMD audio identity into Monado session metadata;
5. implement XR_OCULUS_audio_device_guid.
