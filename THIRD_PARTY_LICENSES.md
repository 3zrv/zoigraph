# Third-party licenses

zoigraph itself is licensed under the GNU GPL v3 (see [LICENSE](LICENSE)). It
statically links the following third-party libraries, fetched at build time via
CMake `FetchContent`. All are permissive (zlib / MIT / public domain) and
GPL-compatible. Their copyright notices are reproduced below as their licenses
require, and this file is bundled into every release binary.

| Library | Version | License | Linked into the shipped binary? |
|---|---|---|---|
| raylib | 5.5 | zlib/libpng | yes |
| GLFW (vendored by raylib) | bundled with raylib 5.5 | zlib/libpng | yes |
| Dear ImGui | v1.92.8 | MIT | yes |
| rlImGui | commit `552a3ad` | zlib/libpng | yes |
| SQLite (amalgamation) | 3.45.2 | Public domain | yes |
| nlohmann/json | v3.11.3 | MIT | yes |
| doctest | v2.4.11 | MIT | no (unit tests only) |

---

## raylib — zlib/libpng License

Copyright (c) 2013-2024 Ramon Santamaria (@raysan5)

This software is provided "as-is", without any express or implied warranty. In no
event will the authors be held liable for any damages arising from the use of this
software.

Permission is granted to anyone to use this software for any purpose, including
commercial applications, and to alter it and redistribute it freely, subject to the
following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you
   wrote the original software. If you use this software in a product, an acknowledgment
   in the product documentation would be appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented
   as being the original software.
3. This notice may not be removed or altered from any source distribution.

## GLFW — zlib/libpng License

Copyright (c) 2002-2006 Marcus Geelnard
Copyright (c) 2006-2019 Camilla Löwy

(Vendored inside raylib. Same zlib/libpng terms as reproduced above for raylib.)

## rlImGui — zlib/libpng License

Copyright (c) 2020-2021 Jeffery Myers

(Same zlib/libpng terms as reproduced above for raylib.)

---

## Dear ImGui — MIT License

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy of this
software and associated documentation files (the "Software"), to deal in the Software
without restriction, including without limitation the rights to use, copy, modify, merge,
publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

## nlohmann/json — MIT License

Copyright (c) 2013-2022 Niels Lohmann

(Same MIT terms as reproduced above for Dear ImGui.)

## doctest — MIT License

Copyright (c) 2016-2023 Viktor Kirilov

(Same MIT terms as reproduced above for Dear ImGui. Used only by the test
binaries; not linked into the shipped `zoigraph` executable.)

---

## SQLite — Public domain

The SQLite amalgamation (3.45.2) is in the public domain. The authors disclaim
copyright; the source carries the blessing:

> May you do good and not evil.
> May you find forgiveness for yourself and forgive others.
> May you share freely, never taking more than you give.
