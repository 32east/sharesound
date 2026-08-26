# sharesound

**Sound for screen sharing in Firefox.** Firefox cannot capture audio when you share your screen, so
Discord streams from Firefox are silent. This puts the sound back.

No changes to your audio setup, no virtual audio driver, no patched Discord client.

---

## The problem

`getDisplayMedia()` is the browser API that hands a web app your screen. The spec lets the app ask for
audio too, and Discord does exactly that:

```
gDM.call    audio: {echoCancellation: false, noiseSuppression: false, autoGainControl: false}
            video: {1280x720@30}
gDM.result  audio: []          <- Firefox returns nothing
            video: [Primary Monitor]
```

Firefox has no implementation behind that request. Not a permission, not a setting — the code isn't
there. Searching a Firefox 153 binary for any display-media audio option comes up empty:

```
media.getdisplaymedia.enabled            present
media.getdisplaymedia.previews.enabled   present
systemAudio                              0 hits
selfBrowserSurface                       0 hits
DisplayMediaStreamOptions                0 hits
```

The upstream bug ([1541425](https://bugzilla.mozilla.org/show_bug.cgi?id=1541425)) has been open since
2019. Chrome and Edge implement it, which is why "share tab audio" works there.

## How this fixes it

Discord is not the problem — it asks for the audio and is ready to send it. Its screen-share peer
connection even reserves an audio slot that sits empty:

```
PC#2 (stream)  m=audio mid:0  recvonly   <- nothing to put here
               m=video mid:1  sendonly   <- your screen
```

So sharesound supplies what the browser should have: a small helper captures system audio through
WASAPI loopback and serves it as raw PCM on `127.0.0.1`, and a userscript attaches that as an audio
track on the stream Firefox returns. Discord picks the track up on its own and drops it into the slot:

```
PC#2 senders   ["audio:{335fbd2f-...}", "video:Primary Monitor"]
PC#2 stats     audio ssrc 292942600  bytes 8687346  pkts 53771
               video ssrc 3634280309 bytes 81822813 pkts 87824
```

Nothing is patched or reverse-engineered; the app receives an ordinary `MediaStream` with an ordinary
audio track.

```
 speakers/headphones mix
          |
          v
  WASAPI loopback  ──►  sharesound.exe  ──►  http://127.0.0.1:47823/pcm
                                                       |
                                          userscript reads the stream,
                                          feeds an AudioWorklet,
                                          adds the track to getDisplayMedia()
                                                       |
                                                       v
                                              Discord sends it
```

## Install

1. Install [Tampermonkey](https://addons.mozilla.org/firefox/addon/tampermonkey/) in Firefox.
2. Download a release (or build it — see below), then run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File install.ps1
   ```

   This copies the helper to `%LOCALAPPDATA%\sharesound`, starts it, registers it to run at logon, and
   opens `http://127.0.0.1:47823/`.
3. On that page, click **Install the userscript**.
4. Reload the Discord tab and start a screen share.

To remove: `powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall` (the userscript is deleted
separately, in Tampermonkey).

## Important: will callers hear themselves?

**Read this part.** Capture is a loopback of an output device, so by default it contains *everything*
playing on that device — including the voices Discord is playing to you. Your listeners would then hear
themselves a second later.

Whether that happens, and how to avoid it, depends on your machine. Run:

```powershell
sharesound-cli --doctor
```

| Your setup | What to do |
|---|---|
| Windows 11, or Windows 10 build 20348+ | The OS can capture per-process. Run with `--exclude firefox.exe` and callers are excluded automatically. Note this also drops audio playing in that browser's own tabs — use `--only game.exe` if you want just one app. |
| Default output is a virtual layer (FxSound, VoiceMeeter, VB-Cable) | Free isolation: in Discord, set **Output Device** to your real headphones instead of *Default*. Its audio then bypasses the device being captured. |
| Two or more real outputs (USB headset + HDMI, etc.) | Same idea: point Discord's output at the device you are *not* capturing. |
| Windows 10, a single output, no virtual layer | The call audio cannot be separated. Either accept the echo, upgrade to Windows 11, or install any virtual output device, make it the default, and point Discord at your real headphones. |

Verify the separation yourself — play a tone into one device and see whether the capture hears it:

```powershell
sharesound-cli --tone "Headset"    # into your headphones directly
sharesound-cli --tone "Speakers"   # into the default device
```

Measured on the setup this was built against, in the 997 Hz band of the captured stream:

| Tone played into | Level in capture |
|---|---|
| Headphones directly | −71.6 dB (noise floor) |
| Default device | −13.5 dB |

58 dB apart — full separation, so callers hear nothing of themselves.

## Capture modes

```powershell
sharesound.exe                          # whole system mix (default)
sharesound.exe --device "Speakers"      # loopback one output device by name
sharesound.exe --only game.exe          # only that process tree      (build 20348+)
sharesound.exe --exclude firefox.exe    # everything but that tree    (build 20348+)
sharesound.exe --port 47823
```

The mode can also be switched while running:
`http://127.0.0.1:47823/mode?set=only&name=game.exe`

## Diagnostics

```powershell
sharesound-cli --doctor    # what this machine can do about echo
sharesound-cli --list      # output devices and which processes are playing
sharesound-cli --tone NAME # 5-second test tone into one device
curl http://127.0.0.1:47823/health
```

## Build

Needs MSVC (Build Tools 2022) and the Windows SDK:

```powershell
cl /O2 /EHsc /std:c++17 sharesound.cpp /Fe:sharesound.exe ^
   /link ole32.lib mmdevapi.lib ws2_32.lib /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup
cl /O2 /EHsc /std:c++17 sharesound.cpp /Fe:sharesound-cli.exe ^
   /link ole32.lib mmdevapi.lib ws2_32.lib
```

Two binaries from one source: the GUI one runs windowless at logon, the console one prints diagnostics.

## Limits and honesty

- **This is a workaround.** The real fix is Firefox implementing display-media audio. Until then the
  sound has to come from outside the browser.
- Latency is about 150 ms (a 100 ms jitter buffer plus WASAPI). Fine for a stream; the video is delayed
  by roughly as much anyway.
- The audio track is attached when a share *starts*. If you start a share before the helper is running,
  restart the share.
- The bridge is plain HTTP on `127.0.0.1` and serves raw system audio to anything on the loopback
  interface that asks. It refuses nothing; treat it as you would any local audio device.
- Firefox-specific by nature. In Chrome and Edge, tick "share tab audio" instead — this isn't needed.
- Windows only. On Linux, PipeWire lets you route a monitor source into the browser directly.

## License

MIT.
