# Building on Windows

The IPA builds as a native Windows CLI with **MSVC**, used exactly as on Linux:
`ipa.exe -r 0 -f contrib/sample_eim_cfg.ber` and so on. There is no Cygwin, no
MinGW runtime and no POSIX emulation layer in the resulting binary — it links
the MS CRT, `winscard.dll`, libcurl and OpenSSL and nothing else.

MSYS2 appears once, as a *build-time* toolbox for the ASN.1 code generator. It
contributes no code to the binary; see [asn1c](#3-asn1c-via-msys2) below.

---

## Prerequisites

| Component | Version | Why |
|---|---|---|
| Visual Studio | 2019 16.8+ or 2022, "Desktop development with C++" | `/std:c11` is required — see [Notes](#c-standard) |
| CMake | 3.14+ | 3.13 builds, but golden-file tests need `--ignore-eol` from 3.14 |
| vcpkg | current | OpenSSL, libcurl, jansson |
| MSYS2 | current | builds and runs `asn1c` |

---

## 1. Dependencies via vcpkg

```bat
git clone https://github.com/microsoft/vcpkg C:\src\vcpkg
C:\src\vcpkg\bootstrap-vcpkg.bat
C:\src\vcpkg\vcpkg install openssl:x64-windows curl[openssl]:x64-windows jansson:x64-windows
```

> **`curl[openssl]` is not optional.** vcpkg's default libcurl on Windows uses
> **Schannel**, and `http.c` reaches into the OpenSSL `SSL_CTX` through
> `CURLOPT_SSL_CTX_FUNCTION` to install the eUICC-provisioned TLS credentials
> (`trustedCertificateTls`, `trustedEimPkTls`, and the eUICC-backed client
> key). With a Schannel build, libcurl returns `CURLE_NOT_BUILT_IN` for that
> option and the IPA logs an explicit error at the first eIM request rather
> than failing the handshake mysteriously. See
> [Why libcurl and not WinHTTP](#why-libcurl-and-not-winhttp).

`jansson` is optional; without it the ESipa JSON binding compiles to a stub and
only the ASN.1 binding works, exactly as on Linux.

## 2. MSYS2

```bat
:: https://www.msys2.org/ — then, in the MSYS2 UCRT64 shell:
pacman -S --needed base-devel mingw-w64-x86_64-toolchain autoconf automake libtool bison flex perl patch
```

`perl` and `patch` are needed by `asn1/gen_libasn.sh`, not by asn1c itself.

## 3. asn1c via MSYS2

`asn1c` has no native Windows build. It is plain C with no library
dependencies (`libtasn1` is an unrelated project and is **not** required), so
it compiles in MSYS2 without trouble. Build it in the **MINGW64/UCRT64**
subsystem so the result is a native Win32 executable rather than one linked
against `msys-2.0.dll`:

```bash
git clone https://github.com/vlm/asn1c && cd asn1c
test -f configure || autoreconf -iv
./configure --prefix=/mingw64
make && make install
```

Put `C:\msys64\mingw64\bin` and `C:\msys64\usr\bin` on `PATH` so CMake finds
both `asn1c.exe` and `bash.exe`.

**This is a build-time tool dependency, in the same category as Perl for
building OpenSSL.** asn1c only emits C source, which MSVC then compiles; no
MSYS2 component is linked into `ipa.exe`.

### Without MSYS2

If you would rather not install it, generate the codec elsewhere and point the
build at the result:

```bash
# on any Linux box with asn1c, once (and again whenever a .asn changes):
bash asn1/gen_libasn.sh "$(command -v asn1c)" ./asn1 ./libasn-gen
```

```bat
cmake -S . -B build -DIPA_LIBASN_GEN_DIR=C:/src/ipa/libasn-gen ...
```

The generated tree is toolchain-independent — the MSVC fixups live in
`asn1/0002-asn1c-msvc-compatibility.patch` and are applied on every
generation, on every platform — so a tree produced on Linux is directly usable
here. CMake warns if a `.asn` is newer than the tree you pointed it at.

## 4. Configure and build

```bat
cmake -S . -B build -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE=C:/src/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expect **9/9 tests to pass**. The binary is `build\src\ipa\Debug\ipa.exe`.

## 5. Running

A PC/SC reader needs no extra software: the Smart Card service (`SCardSvr`) and
`winscard.dll` are part of Windows. Check the reader is visible with
`certutil -scinfo`, then use the IPA exactly as documented in
[GETTING_STARTED.md](GETTING_STARTED.md).

---

## Design notes

### Why libcurl and not WinHTTP

Swapping libcurl for WinHTTP was considered and rejected. `http.c` does not use
curl as a generic HTTP client — it uses `CURLOPT_SSL_CTX_FUNCTION` to reach the
OpenSSL `SSL_CTX` before each handshake and install three things the eUICC
supplies: a DER trust anchor with `X509_V_FLAG_PARTIAL_CHAIN`, a
trust-anchor-by-public-key verify callback for `trustedEimPkTls`, and an
`EC_KEY_METHOD` that routes ECDSA signing into the card for client
authentication.

Schannel exposes no equivalent hook. A WinHTTP backend would mean disabling
Schannel's own validation and reimplementing SGP.32 chain verification against
CryptoAPI, plus a custom CNG key storage provider for the eUICC-backed key —
several hundred lines of Windows-only security code, and a second, divergent
trust implementation to keep correct at merge time.

Keeping libcurl + OpenSSL means **`http.c` is byte-identical across all
platforms**. The cost is one build flag: `curl[openssl]`.

### PC/SC

`scard.c` was already written against the WinSCard API, which is Microsoft's
original and which pcsc-lite clones. On Windows it therefore needs no porting,
only different headers (`<windows.h>` instead of pcsc-lite's `<wintypes.h>`)
and a local `pcsc_stringify_error()`, which pcsc-lite provides and the Windows
SDK does not.

### C standard

MSVC has no C99 mode; without `/std:c11` `cl.exe` falls back to a C89-era
dialect that rejects `inline` (used in `utils.h`) and the designated array
initialisers in `libipa/log.c`. `cmake/IpaCompilerFlags.cmake` therefore
requests C11 on MSVC and leaves the other toolchains on C99 as before. C11
support requires **VS 2019 16.8 or newer**.

---

## Behavioural differences from the POSIX build

Everything below is deliberate and documented at the point of implementation.

| Area | Difference |
|---|---|
| `getopt` | `src/ipa/compat/getopt_win32.c` implements POSIX `getopt`, verified case-by-case against glibc. It does **not** implement the GNU argument-permutation extension, so options after a non-option argument are not parsed. The IPA takes no positional arguments, so this is unreachable in normal use. |
| `SIGUSR1` | The MS CRT has no `SIGUSR1`; the handler in `main.c` is guarded by `#ifdef SIGUSR1` and is simply absent on Windows. |
| `-DM32` | Not supported. Select the architecture at configure time: `cmake -A Win32`. |
| `-DENABLE_SANITIZE` | Maps to MSVC's `/fsanitize=address` (VS 2019 16.9+), with `/RTC` and incremental linking disabled as ASan requires. |
| `-DMEM_EMIT_DEBUG` | Works; `malloc_usable_size()` maps to `_msize()`, wrapped to tolerate `NULL` as the POSIX versions do. |
| `nvstate.bin` | Unchanged. `struct ipa_nvstate` stays packed via `#pragma pack` — verified identical at 44 bytes, alignment 1 — so the file format matches the Linux build's byte for byte on the same architecture. |

## Files added for the port

| File | Purpose |
|---|---|
| `cmake/IpaCompilerFlags.cmake` | Resolves `-Wall` / `-w` / `-g` / `-m32` / `-fsanitize` into per-toolchain spellings |
| `cmake/RunAndCapture.cmake` | Runs a test and captures stdout/stderr, replacing `sh -c "prog > out 2> err"` |
| `include/onomondo/ipa/compat.h` | Struct-packing macros, plus the POSIX interfaces the MS CRT lacks |
| `src/ipa/compat/compat_win32.c` | `sleep()`; the only TU that includes `<windows.h>` |
| `src/ipa/compat/getopt_win32.c` | POSIX `getopt` |
| `asn1/0002-asn1c-msvc-compatibility.patch` | Fixes two MSVC assumptions in asn1c 0.9.28's runtime |
| `.gitattributes` | Keeps binary fixtures and patches from being CRLF-mangled on checkout |
