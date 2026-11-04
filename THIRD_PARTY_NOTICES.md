# Third-party notices

RecompTrophy includes the following, each under its own license. Their sources are in this repository.

| Component | Used for | License | Where |
|---|---|---|---|
| [libmspack](https://www.cabextract.org.uk/libmspack/) (Stuart Caie) — its LZX decoder | Decompressing the game's executable | GNU LGPL 2.1 ([COPYING.LIB](third_party/libmspack/COPYING.LIB)) | `third_party/libmspack/` |
| [tiny-AES-c](https://github.com/kokke/tiny-AES-c) (kokke) | Decrypting the game's executable | The Unlicense (public domain) | `third_party/tiny-aes-c/` |
| [stb_image](https://github.com/nothings/stb) (Sean Barrett) | Reading the achievements' pictures | Public domain or MIT (in the file) | `third_party/stb/stb_image.h` |
| [stb_truetype](https://github.com/nothings/stb) (Sean Barrett), as Dear ImGui ships it | Drawing the text | Public domain or MIT (in the file) | `third_party/stb/imstb_truetype.h` |
| [miniz](https://github.com/richgel999/miniz) (Rich Geldreich), as SDL3 ships it | Writing the trophies' pictures (PNG) | The Unlicense (in the file) | `third_party/stb/miniz.h` |
| [QR Code generator](https://www.nayuki.io/page/qr-code-generator-library) (Project Nayuki) | The Discord QR code | MIT (in the files) | `src/third_party/qrcodegen/` |
| [Noto Sans](https://fonts.google.com/noto) (Google) | The app's lettering | SIL Open Font License 1.1 ([OFL.txt](fonts/OFL.txt)) | `fonts/` |

## libmspack (LGPL 2.1)
RecompTrophy links libmspack's LZX decoder (`lzxd.c`) statically. Its unmodified source is in `third_party/libmspack/`, and RecompTrophy's own source is in this repository, so you can rebuild the app with another version of libmspack (`tools/build.sh`).
