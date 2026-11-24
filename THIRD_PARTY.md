# Third-party notices

IZtwiNS2 is distributed under [GPL-3.0-only](LICENSE). The source tree includes the [third-party license notices](source/licenses/) and the notices distributed with its bundled dependencies.

## Code adapted by IZtwiNS2

- [twiNX at 2d4338b](https://github.com/DomazinUS/twiNX/tree/2d4338b96f1e18978391c63e54cb1df8d22501dc) uses [GNU GPL v3](https://github.com/DomazinUS/twiNX/blob/2d4338b96f1e18978391c63e54cb1df8d22501dc/LICENSE). See the attribution in [Twitch playback](source/iz2s/native/twitch.c) and [device sign-in](source/iz2s/native/account.c).
- [IZ2S Cheese](https://github.com/byizmysh/iz2s-cheese) uses [MIT](source/licenses/IZ2S-Cheese-MIT.txt). See the attribution in [the HTTPS implementation](source/iz2s/native/https.c).

## Player and interface dependencies

- [FFmpeg c1b19ee](https://github.com/FFmpeg/FFmpeg/tree/c1b19ee69f2142bb4b098936cc7421d80b3db7e4) is bundled in [source/ffmpeg](source/ffmpeg/). The release configuration uses LGPL-2.1-or-later components. The [FFmpeg license summary](source/ffmpeg/LICENSE.md) covers additional per-file terms.
- [FreeType 2.14.1](source/textdeps/freetype/LICENSE.TXT) offers the FreeType License and GPL-2.0-or-later. [HarfBuzz 10.0.1](source/textdeps/harfbuzz/COPYING) uses the Old MIT license. [FriBidi 1.0.16](source/textdeps/fribidi/COPYING) uses LGPL-2.1-or-later.
- [Roboto](source/licenses/Roboto-OFL.txt), [Noto fonts](source/licenses/NotoFonts-OFL.txt), [Noto CJK](source/licenses/NotoCJK-OFL.txt), and [Noto Emoji](source/licenses/NotoEmoji-OFL.txt) use SIL Open Font License 1.1. The release uses a subset of Noto CJK.
- [nlohmann/json](source/licenses/nlohmann-MIT.txt) and the [Nayuki QR Code generator](source/iz2s/native/vendor/qrcodegen/qrcodegen.c) use MIT. [stb_image](source/iz2s/native/vendor/stb_image.h) offers MIT and a public-domain alternative.

## Platform notices

- [libounce](source/licenses/libounce-ISC.txt) and [libnx](source/licenses/libnx-ISC.txt) have ISC notices. [mimalloc](source/licenses/mimalloc-MIT.txt) has an MIT notice.
- The NRO embeds the IZ2S Launcher 0.32 return entry. The launcher source and its own component notices are available from [IZ2S Platform](https://github.com/byizmysh/IZ2S-Platform).

See [media sources](docs/media-sources.md) for the gallery.
