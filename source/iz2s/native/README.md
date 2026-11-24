# IZtwiNS2 for Nintendo Switch 2

IZtwiNS2 is a standalone Twitch client for Nintendo Switch 2. Catalog browsing, chat and playback connect directly from the console.

## Install

- **Native IZ2S application (0.11.0).** Import the supplied distribution in the IZ2S SD application library or install its verified files to `sd:/iz2s/modules/iztwins2/`. IZ2S opens `launcher.iza` directly. That file includes all SPIR-V shaders and tile art. The content-addressed `.izdata` file contains the ARM64 player and its embedded fonts. Press **B** from a main section to return to IZ2S. The native app stores its favorites and Twitch device session in its own SD module directory.
- **NRO (0.11.0).** Copy the standalone NRO to `sd:/switch/IZtwiNS2-NRO.nro` and refresh the IZ2S app list. The NRO can use the bundled return entry. Its preferences live in `sd:/switch/IZtwiNS2/`.

## Touch controls

- Tap a section, card, search field, filter, or page button to open it. Swipe the catalog to move between groups of cards. Hold a card to select it and tap **Channel**, **Favorite**, or **Remove** along the bottom if available.
- Tap the video to show or hide the playback controls. Tap **Pause**, **Chat**, **Message**, or **Settings** directly. Tap a quality, caption, or text-size row to change it.
- Tap or drag the timeline on a recorded video. The player shows the target time while you drag and seeks when you lift your finger.
- Swipe down in chat to read older messages. Swipe up or tap **Live** to return to current messages. Tap **Write a message** to bring up the system keyboard when signed in.
- Tap **Back** to close a panel or return to browsing. Tap **Exit** from a main section to return to IZ2S.

## Browse and sign in

- Use the D-pad or left stick to select cards. From the leftmost card, press **Left** to focus the sidebar. On the first row, press **Up** to focus Search, Open link, and Sign in. Press **A** to open the focused item and **B** to go back.
- On the main screen **L/R** switch between Home, Trends, Following, Continue, Favorites, and Recent. **+** refreshes the current online feed, including search results and channel pages. Trends ranks categories by live viewers.
- **Y** searches channels, categories, and recorded broadcasts as a guest. On the results screen, **L/R** select All, Live, Channels, Categories, or Videos. **ZL** opens the selected channel, then **L/R** switch between Live, Videos, and Clips. **ZR** opens a Twitch channel name, Twitch video link, or Twitch clip link from a main tab.
- Below the last row, press **Down** to focus Previous page or Next page where available. In a channel’s Clips tab, press **Y** to filter titles as you browse the pages.
- Press **−** on a channel card to add or remove a favorite. Favorites refreshes live and offline status while open. Open an offline favorite to see its channel page.
- **Continue** lists unfinished recordings, with a progress bar on each card. Press **A** to resume or **−** to remove a bookmark.
- **X** on a main tab opens sign-in. Scan the code or open [Twitch Activate](https://www.twitch.tv/activate) and sign in on Twitch's website. Choose **Remember** to store a Twitch refresh token on the SD card, or **This session only** for a temporary sign-in. Signing out clears the saved session. Sign in to browse Following and send chat messages.

## Watch a video

Press **A** or a direction to show the playback controls. Move left or right to focus a control, then press **A** to select it. Press **B** to close the active panel or leave playback. **Settings** opens quality, closed captions and the independent caption and chat text sizes. On live streams, select **Chat** to open or hide the side panel. Press **Right** from the last player control to read chat history, use **Up/Down** to scroll, and press **A** to jump to live. Select **Message** to type when signed in. Twitch confirms delivery. If a message is refused or delivery is interrupted, the same control opens the saved draft.

For recorded broadcasts, the transport buttons rewind or advance 30 seconds. Press **Up** from the control row to focus the timeline, tap left or right to move five seconds, or hold to accelerate. Press **A** to seek or **B** to cancel. The player keeps the previous frame visible while it loads the new position or quality.

Playback supports Twitch HLS with MPEG-TS and fragmented MP4 segments. Auto starts near 360p and can fall back to a lower rendition when a live stream disconnects. Manual choices are populated from the media available and capped at compatible H.264 renditions up to 720p. Quality changes reload playback in the background. Retry restores the requested seek position or the last decoded position. Recorded videos decode from the preceding HLS segment and skip to the chosen time when segment timestamps are available. Live streams reload near the live edge and clips restart. Seeking is available for recorded videos with a complete HLS index.

Captions use the default WebVTT track in the HLS playlist or the first advertised track when no default is marked. Embedded CEA-608 CC1 is available as a fallback. The SD card retains the caption and quality preferences. Mute applies for the current app session.

Following uses Twitch Helix. Public browsing, search, and playback use Twitch's web APIs. Live chat reads over Twitch’s secure IRC WebSocket, including as a guest. Signed-in messages use the Helix Send Chat Message endpoint. Twitch can change the web APIs independently of this app.

## Text and languages

The interface is in English. Stream and channel titles, search queries, captions, and chat keep the original Unicode text. Bundled Roboto and Noto faces cover Latin, Cyrillic, Greek, common Chinese, Japanese, and Korean characters, Arabic, Hebrew, Hindi and several other Indic and Southeast Asian scripts. HarfBuzz shapes joined letters and marks. FriBidi handles mixed left-to-right and right-to-left text. Unsupported symbols display as an outline square. Unicode emoji use the monochrome Noto Emoji face. Twitch channel emotes are separate assets.

## Build

Use the Switch 2 Ounce devkit with `libounce` and the paired `nacptool` and `elf2nro` tools. The archive includes the matching FFmpeg source, Roboto fonts, and C/C++ dependencies. Python 3 embeds the bundled font faces and Pillow generates the wordmark and launcher tile. The build compiles the bundled FreeType, HarfBuzz, and FriBidi sources and needs GNU Make and a native host C compiler.

From the archive root run

```sh
export DEVKITPRO=/path/to/ounce/devkitpro
export FFMPEG_SRC="$PWD/ffmpeg"
bash iz2s/native/build-nro.sh "$PWD/build"
```

Set `IZTWINS2_RETURN_NRO` before building the NRO to embed the matching IZ2S return entry. The optional `iz2s/native/build-nro-bundle.cjs` produces an NRO import archive.

For the native build, set `IZTWINS2_SDK` to the Ounce SDK and `IZTWINS2_PLATFORM` to the RUN32 checkout. Set `IZTWINS2_PACKER` to `tools/sd-launcher/relocatable.py` in RUN23 and `IZTWINS2_HOST_TOOLS` to WitchTube's `starboard` tooling directory. Supply the BTI-enabled hosted SDK archives with `IZTWINS2_HOST_LIBOUNCE`, `IZTWINS2_HOST_LIBC`, and `IZTWINS2_HOST_CXX`. The C++ directory includes the hosted unwind object. The build compiles its media libraries with BTI, validates the hosted image and SPIR-V, and checks the native package with the IZ2S importer.

```sh
export IZTWINS2_BUILD="$PWD/build"
bash iz2s/iza/build-iza.sh "$PWD/dist"
```

`IZTWINS2_JOBS` changes build parallelism. Set `IZTWINS2_CLIENT_ID` to use your own registered public Twitch app Client ID. Twitch's authorization page may display the registered name of the original client used by the default.

## Source and licenses

- The Nintendo Switch 2 client is GPL-3.0-only. Playback requests and sign-in draw on [twiNX at 2d4338b](https://github.com/DomazinUS/twiNX/tree/2d4338b96f1e18978391c63e54cb1df8d22501dc).
- Socket and TLS integration draws on [IZ2S Cheese](https://github.com/byizmysh/iz2s-cheese), MIT.
- The archive includes [FFmpeg at c1b19ee](https://github.com/FFmpeg/FFmpeg/tree/c1b19ee69f2142bb4b098936cc7421d80b3db7e4), configured with LGPL components. Third-party licenses are in `licenses/`, `ffmpeg/`, `textdeps/`, and the corresponding vendored files.
- Text uses [FreeType 2.14.1](https://freetype.org/), [HarfBuzz 10.0.1](https://github.com/harfbuzz/harfbuzz/tree/10.0.1), [FriBidi 1.0.16](https://github.com/fribidi/fribidi/tree/v1.0.16), and [Noto fonts](https://notofonts.github.io/noto-docs/website/use/). The CJK face is subset to the common Han, kana, Hangul, and punctuation blocks.
