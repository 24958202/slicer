# Image Slicer — saucer + C++23

✨ Key Features
Interactive Drag & Drop Slicing: Easily carve out custom rectangular slices directly on the canvas.

Rich Mouse-over Effects: Assign advanced hover animations per slice:

Zoom in, Brighten, Darken, Grayscale, Fade out

Color Overlay & Highlight Borders

Image Swap on Hover

HTML Table Export: Generates clean, zero-gap HTML tables (index.html) using row/column span math alongside an organized image/ folder containing your sliced assets.

Responsive & Fixed Layouts: Toggle between fixed pixel dimensions and responsive width scaling for modern web deployment.

## Build

Requires CMake 3.25+, Git, a C++23 compiler and standard library compatible with
saucer 7 (GCC 14+, Clang 18+, or MSVC 19.43+; platform-specific checks also apply).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

Run `build/slicer` with a single-config generator, or
`build/Release/slicer.exe` with Visual Studio on Windows.

Platform dependencies:
- Windows: recent Windows SDK and the WebView2 runtime.
- macOS: a suitable Apple toolchain; native WebKit is used.
- Linux default: GTK 4.12+, WebKitGTK 6.0, libadwaita and json-glib development
  packages, plus pkg-config. An appropriately installed Qt backend can instead
  be selected with `-Dsaucer_backend=Qt6` (Qt 6.7+).

FetchContent downloads the pinned sources and saucer fetches its own dependencies.
The revised CMake configuration explicitly declares Glaze 5.7.2 before saucer,
forces source fetching, disables Glaze's default SYSTEM include setting, and
prioritizes its ordinary include directory for both saucer and slicer. A normal
-I/usr/local/include can outrank -isystem paths, so dependency fetching alone
was insufficient. Do not manually add another Glaze include directory or remove
`raw_string` from saucer's serializer source.
Do not disable saucer's toolchain checks merely to force an unsupported build.

### Rebuild after the Glaze header mismatch (macOS)

Use a fresh build directory; the old build can retain the wrong dependency.
From the extracted project folder:

```sh
env -u CPATH -u CPLUS_INCLUDE_PATH -u C_INCLUDE_PATH -u OBJC_INCLUDE_PATH \
  cmake -S . -B build-glaze-v2 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
env -u CPATH -u CPLUS_INCLUDE_PATH -u C_INCLUDE_PATH -u OBJC_INCLUDE_PATH \
  cmake --build build-glaze-v2 --parallel
./build-glaze-v2/slicer
```

If compilation still selects `/usr/local/include/glaze`, capture a verbose build:

```sh
env -u CPATH -u CPLUS_INCLUDE_PATH -u C_INCLUDE_PATH -u OBJC_INCLUDE_PATH \
  cmake --build build-glaze-v2 --verbose --parallel 1 2>&1 | tee build-glaze-v2/build.log
```

Inspect the failing compiler invocation for manually supplied include directories.
If CPATH or CPLUS_INCLUDE_PATH injects incompatible headers, retry with those
variables unset for both configure and build, using another fresh build directory.
The macOS rebuild has not been performed by the assistant; success must be
confirmed from the actual build output.


## Features

- Open image and choose hover image through native desktop file dialogs.
- Load typed UTF-8 paths, ~/ expansion, and image file drag/drop.
- Browse export folders through a native directory dialog.
- Draw rectangles to carve a non-overlapping, complete image partition.
  Surrounding fragments retain their parent link/effect settings; the new
  selected rectangle starts with default settings.
- Select slices; set URL, new-tab behavior, all nine hover-effect choices,
  amount, color, transition time, and hover image.
- Undo (100 entries), undoable Reset with confirmation, Fit, pan and zoom.
- Preview in a sandboxed iframe; preview links do not navigate.
- Export PNGs plus a span-based HTML table, with optional proportional
  responsive sizing, CSS hover/focus effects and safe new-tab link attributes.
- Export checks for existing generated files and requests overwrite confirmation.
- Pointer capture/cancellation, busy-operation guards, strict Base64 decoding,
  generated-output-name restrictions, symlink-output rejection, and write checks.

## Controls

S = Slice; C = Select; F = Fit; Ctrl/Cmd+Z = Undo outside input fields.
Escape closes preview or cancels a drag.
Wheel zooms at the cursor. Middle-drag, Alt-left-drag, or left-drag outside the
image pans. Rectangles must be at least 10 x 10 image pixels.

## Limitations / export behavior

Images are decoded by the platform webview. PNG/JPEG and other formats supported
by that webview can be opened; SVG/WebP/AVIF support varies by platform. An animated
source is frozen to one decoded frame on load. External-resource SVGs may fail
canvas-origin checks. Transparent pixels are preserved in PNG exports.

Limits: 128 MiB source/output files, 64 megapixels, 32767 pixels per edge,
1000 slices and one million elementary table cells. Practical webview canvas
limits can be lower; canvas export errors are reported.

Output: `<chosen folder>/image/slice_NN.png`, optional `_hover.png` counterparts,
and `<chosen folder>/index.html` (written last). A partial export is possible if
an error occurs. Existing matching generated files are overwritten only after
confirmation. Obsolete/unrelated files are deliberately not deleted. Export is
not a multi-file transaction or a security boundary against hostile concurrent
filesystem changes.

## Validation performed

- Embedded JavaScript syntax check with Node.
- Mock-DOM JavaScript tests: 150 overlapping partition operations, coverage and
  area checks, undo, all effect CSS brace checks, URL escaping/scheme validation,
  native-dialog bridge callbacks, preview/export, and all editor button bindings.
- C++ native helper tests compiled with GCC 12 in C++23 mode: 1000 Base64
  roundtrips, malformed padding rejection, allowed-output paths, file read/write,
  symlink rejection, UTF-8 paths, tilde expansion, file URL parsing, encoded
  filename reads and file writes to URL-selected output folders.
- Updated mock-DOM tests verify typed image URLs and export-folder URLs are
  normalized before native image reads and export writes.
- API usage checked against the pinned saucer and desktop source headers/examples.
- Revised CMake tested using real CPM and Glaze 5.7.2 with mock saucer/desktop
  targets: opts.raw_string compiled successfully despite a deliberately
  conflicting include directory; generated compile commands put the pinned
  non-system Glaze include first for both consumers. This is not a full GUI build.

The complete saucer application was NOT built or run here: the available compiler
is GCC 12, below saucer 7's requirement, and the GUI backend development packages
are not installed. Real native dialogs, GUI rendering and browser table layout
remain to be tested on the target platform. No executable is supplied.

Tests are kept in `tests/` in the project workspace. They are logic checks, not
real-browser automation or GUI integration tests.
