# Third-party notices

QODE itself is released under the [MIT License](LICENSE), copyright © 2026 Zahid Khalilov. It also ships or embeds the third-party material listed below. Each part stays under its own license, and the logos are not covered by QODE's MIT license.

## Fonts (embedded in the binary)

| Font | Author | License | Files |
| --- | --- | --- | --- |
| JetBrains Mono (editor, terminal) | JetBrains s.r.o. | SIL Open Font License 1.1 | `resources/fonts/JetBrainsMono-*.ttf`, license in [`resources/fonts/OFL-JetBrainsMono.txt`](resources/fonts/OFL-JetBrainsMono.txt) |
| Plus Jakarta Sans (UI) | Tokotype | SIL Open Font License 1.1 | `resources/fonts/PlusJakartaSans-*.ttf`, license in [`resources/fonts/OFL-PlusJakartaSans.txt`](resources/fonts/OFL-PlusJakartaSans.txt) |

## Icons (embedded in the binary)

- **Lucide** (<https://lucide.dev>): most line icons in `resources/new-icons/`. ISC License, copyright © Lucide Contributors. Parts of Lucide come from **Feather** (<https://feathericons.com>), MIT License, copyright © 2013-2023 Cole Bemis.

  > Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies.
  >
  > THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

- **Brand logos** in `resources/new-icons/`, taken from **Simple Icons** (<https://simpleicons.org>; the SVG files are released under CC0 1.0). The logos are trademarks of their owners. QODE uses them only to mark the matching files, projects and providers, and is not endorsed by these companies.

  | File | Logo | Trademark of |
  | --- | --- | --- |
  | `kotlin.svg` | Kotlin | JetBrains s.r.o. (notice below) |
  | `ktor.svg` | Ktor | JetBrains s.r.o. (notice below) |
  | `gradle.svg` | Gradle | Gradle, Inc. |
  | `apachemaven.svg` | Apache Maven | The Apache Software Foundation |
  | `spring.svg`, `springboot.svg` | Spring, Spring Boot | Broadcom Inc. |
  | `claude.svg`, `claudecode.svg` | Claude, Claude Code | Anthropic PBC |
  | `gemini.svg` | Google Gemini | Google LLC |

  As JetBrains' brand guidelines require:

  > Copyright © 2026 JetBrains s.r.o. Kotlin and the Kotlin logo are trademarks of JetBrains s.r.o.
  >
  > Copyright © 2026 JetBrains s.r.o. Ktor and the Ktor logo are trademarks of JetBrains s.r.o.

## Qt

QODE is built with the **Qt framework** (<https://www.qt.io>), used under the **GNU Lesser General Public License v3** (LGPL-3.0). QODE links to Qt dynamically and does not modify it.

- The `qode` Debian package uses the distribution's Qt packages.
- The `qode-bundled` / `qode-bundled-noweb` packages, the Windows installer and the portable zip ship unmodified Qt libraries next to QODE (under `/opt/qode` or the install folder). You may replace them with your own build of the same Qt version. The Qt source code is available from <https://download.qt.io/official_releases/qt/>.
- The builds with **Qt WebEngine** also ship Chromium, which comes with its own third-party licenses (BSD and others). Qt lists them at <https://doc.qt.io/qt-6/qtwebengine-licensing.html>.
- The bundled Qt also brings ICU (Unicode License), FFmpeg (LGPL, Qt Multimedia) and zlib (zlib License) as part of Qt's own libraries.

## Other runtime parts

- **zlib** (zlib License, Jean-loup Gailly and Mark Adler): the system library on Linux, Qt's bundled copy on Windows.
- **Microsoft Visual C++ runtime** (Windows builds): redistributed under the Visual Studio redistribution terms.

## Not distributed with QODE

QODE can download and run tools such as language servers (for example kotlin-lsp, typescript-language-server, clangd), JDKs and Node packages, and it starts tools that are already installed (`git`, `claude`, compilers, build tools). They are fetched from their official sources at the user's request, are not part of QODE's packages, and are covered by their own licenses.
