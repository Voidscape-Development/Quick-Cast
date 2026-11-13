# Quick Cast

Quick Cast is an OBS Studio plugin for bringing remote guests into a broadcast. You invite someone with a link, they
open it in a browser, and they show up in a dock inside OBS where one click puts them on screen.

Guest video and audio are received natively inside OBS over WebRTC. There is no browser source and no separate
window to capture. Signaling and the guest-side web app are currently provided by [VDO.Ninja](https://vdo.ninja);
the plugin is structured so a first-party service can replace it later.

> **Status:** early development (0.1). Windows is the primary target. Linux builds and is used for automated
> testing; macOS is built in CI but untested.

## Using it

1. Open the **Quick Cast** dock. It appears on the right the first time the plugin loads; afterwards it lives under
   **Docks → Quick Cast**.
2. Click **Invite guest…**, type the guest's name, and send them the link that was copied to your clipboard.
3. Click **Start show**. When the guest opens their link, their row turns green.
4. Click **Add to scene** on their row.

Each guest gets a personal link tied to a seat. The link keeps working from one show to the next, and the guest's OBS
source stays bound to that seat, so a guest who drops and rejoins comes back in the same place in your scenes.

Other things the dock can do:

- **Copy open invite link** (under ⋯) gives you one link for anyone. Each person who joins with it is added to
  the list under the name they type.
- The ⋯ menu on a guest renames them, mutes them in OBS, refreshes their video, disconnects them, or removes them.
- Guest sources are ordinary OBS sources named `Guest: <name>`. You can crop, filter, and arrange them like any
  other source, and add them to more scenes with **Add → Quick Cast Guest**.

### What guests see and hear (the return feed)

While at least one guest is connected, Quick Cast sends them your program output and one OBS audio track so they can
follow the show. Two things to know:

- Guests see your **program output** by default. Under Settings → Return feed you can change the **main feed** to
  any source or scene, for example your camera.
- You can also turn on a **second feed** (the program or any other source). Guests see it as a separate tile next
  to the main one. A typical co-streaming setup is main feed = your camera, second feed = the program or a screen
  capture. The second feed is video only; your audio travels with the main feed. Each feed has its own resolution
  and bitrate.
- Guests hear **audio track 6** by default (configurable). Quick Cast automatically removes guest sources from that
  track, so nobody hears a delayed echo of themselves. Guests hear each other directly, peer to peer.
- The return feed has its own lightweight encoder (x264 720p, 1.5 Mbps by default), separate from your stream and
  recording encoders. It only runs while a guest is watching. While it runs, OBS treats an output as active, so
  video settings are locked just as they are while streaming.

### Settings

**⋯ → Settings…** in the dock:

| Tab | What is there |
| --- | --- |
| Show | Your display name, the show name and password that every invite link is built from, a button to generate new ones if a link leaks, and "start the show when OBS starts". |
| Guests | Video quality requested from guests (1.5 to 8 Mbps), preferred codec (H.264, VP8 or VP9), hardware decoding, and whether new guests are fitted to the canvas. |
| Return feed | On/off, the main feed and optional second feed (program or a chosen source such as your camera), audio track, resolution, and bitrate. |
| Network | An optional TURN relay for networks that block direct connections. |

Settings are stored in the plugin's config directory (`plugin_config/quick-cast/settings.json`) and are shared by all
scene collections.

## Known limitations

- No TURN relay is configured out of the box. Guests' browsers bring VDO.Ninja's relays with them, which covers
  most networks, but a host behind a very restrictive firewall needs their own TURN server (Settings → Network).
- Guest video can be H.264, VP8 or VP9. AV1 is not accepted yet.
- Hardware decoding (D3D11VA on Windows, VideoToolbox on macOS, VAAPI on Linux) is used when the graphics card
  supports the codec, with an automatic switch to software decoding when it does not. VP8 is always decoded in
  software. Decoded frames are copied back from the GPU rather than kept there.
- Each of your two feeds runs its own encoder, and each is sent to every guest, so a second feed roughly doubles
  the return-feed CPU and upload cost.
- One camera per guest. A guest who shares their screen from the web app appears as a second entry in the list.
- libobs cannot request a keyframe on demand, so the return feed uses a one-second keyframe interval. A guest who
  joins waits up to a second for the first picture.
- Removing a guest with **Disconnect** relies on Quick Cast holding the director role in the room. If someone else
  has already claimed the same show name, guests still connect but cannot be disconnected remotely.

## Building

Quick Cast uses the standard [OBS plugin template](https://github.com/obsproject/obs-plugintemplate) build system,
so the template's [build instructions](https://github.com/obsproject/obs-plugintemplate/wiki) apply.

```sh
# Windows (Visual Studio 2022)
cmake --preset windows-x64
cmake --build --preset windows-x64

# Ubuntu 24.04
sudo apt install libavcodec-dev libavutil-dev libssl-dev
cmake --preset ubuntu-x86_64
cmake --build --preset ubuntu-x86_64
```

Dependencies beyond the template's:

- **libdatachannel** (WebRTC and the signaling WebSocket) is fetched by CMake at configure time and linked
  statically, so the plugin does not depend on the copy OBS ships.
- **FFmpeg** (`libavcodec`, `libavutil`) for decoding. On Windows and macOS it comes from obs-deps and OBS provides
  the runtime libraries.
- **Mbed TLS** (Windows, macOS; from obs-deps) or **OpenSSL** (Linux).

### Tests

```sh
cmake --preset ubuntu-x86_64 -DENABLE_TESTS=ON
cmake --build --preset ubuntu-x86_64
ctest --test-dir build_x86_64 --output-on-failure
```

`-DQUICK_CAST_BUILD_PLUGIN=OFF` builds only the core library and its tests, without the plugin module.

`ENABLE_TESTS` also builds `quick-cast-probe`, a command-line host that joins a show without OBS and prints what it
receives. It is handy for checking interoperability against a real browser guest:

```sh
./build_x86_64/tests/quick-cast-probe --room my_test_room --password secret --seat alice:Alice
```

## How it is put together

```
src/
  core/                 No OBS or Qt dependencies. Unit tested.
    session/            GuestService: the interface between Quick Cast and a guest service.
    vdo/                The VDO.Ninja implementation of GuestService: signaling, room/director
                        logic, ID hashing and SDP encryption, one peer connection per direction.
    rtp/                Receive path: reordering and NACK, H.264/VP8 depacketizing, RTCP
                        feedback (receiver reports, REMB, PLI), sender-report based A/V sync.
    util/               Crypto wrappers, serial executor, logging.
  media/                FFmpeg video and audio decoders.
  obs/                  OBS integration: guest source, per-guest decode thread, return-feed
                        output and encoders, scene helpers, settings, and the controller.
  ui/                   The dock and settings dialog (Qt).
tests/                  GoogleTest suites for the core.
tools/                  quick-cast-probe.
```

Everything above `core/session/guest-service.hpp` is service agnostic. Replacing VDO.Ninja with another service means
writing one new `GuestService` implementation; the OBS sources, return feed, and UI do not change.

## Acknowledgements

Quick Cast talks to the public [VDO.Ninja](https://vdo.ninja) service created by Steve Seguin. Quick Cast is an
independent project and is not affiliated with or endorsed by VDO.Ninja. If you rely on it, consider supporting the
VDO.Ninja project.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).
