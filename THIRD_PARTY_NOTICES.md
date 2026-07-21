# Third-Party Notices

DirettaRendererUPnP incorporates third-party software. This file reproduces the
copyright notices and licence terms those components require.

The project's own source code is licensed under the MIT License — see
[LICENSE](LICENSE) for its copyright holders and terms.

> **Note:** the Diretta Host SDK is **not** included in this repository. It is
> proprietary software by Yu Harada, licensed for personal use only, and must be
> obtained separately. See the [README](README.md) for details.

---

## FastMemcpy

High-performance SIMD `memcpy` implementation.

| | |
|---|---|
| **Upstream** | https://github.com/skywind3000/FastMemcpy |
| **Author** | Linwei (`skywind3000@163.com`), 2015 |
| **Licence** | MIT |

**Incorporated in:** `src/FastMemcpy_Avx.h` — the AVX/AVX2 accelerated
`memcpy_fast` implementation, which carries the upstream attribution header.

Other SIMD copy headers under `src/` (`FastMemcpy_Audio.h`,
`FastMemcpy_Audio_AVX512.h`, `memcpyfast.h`, `memcpyfast2.h`,
`memcpyfast_audio.h`) build on or declare this API; the audio-specific variants
are this project's own work and are covered by [LICENSE](LICENSE).

### MIT License

```
Copyright (c) 2015 Linwei

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
