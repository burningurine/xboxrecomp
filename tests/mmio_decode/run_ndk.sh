#!/usr/bin/env bash
# Build and run the mmio_decode test on a Windows x64 host that has only the
# Android NDK's LLVM (no MSVC, no mingw): a freestanding executable with a
# minimal C runtime written out below (printf, memcpy, memset over
# kernel32's WriteFile), linked by lld in its COFF mode. The test itself is
# plain C and builds the same with cl or gcc.
#
#   tests/mmio_decode/run_ndk.sh        (ANDROID_SDK, NDK_VER to override)
set -euo pipefail
export MSYS_NO_PATHCONV=1
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/../../src/platform"
SDK="${ANDROID_SDK:-${LOCALAPPDATA:-/c/Users/$USER/AppData/Local}/Android/Sdk}"
BIN="$SDK/ndk/${NDK_VER:-27.2.12479018}/toolchains/llvm/prebuilt/windows-x86_64/bin"
OUT="${TMPDIR:-/tmp}/mmio_decode_ndk"
w() { cygpath -m "$1" 2>/dev/null || echo "$1"; }
rm -rf "$OUT"; mkdir -p "$OUT/inc"

cat > "$OUT/inc/stdio.h" <<'EOF'
#define NULL ((void *)0)
int printf(const char *fmt, ...);
EOF
cat > "$OUT/inc/string.h" <<'EOF'
#define NULL ((void *)0)
typedef unsigned long long size_t;
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
EOF
cat > "$OUT/crt.c" <<'EOF'
typedef unsigned long long size_t;
typedef __builtin_va_list va_list;
__declspec(dllimport) void *__stdcall GetStdHandle(unsigned long);
__declspec(dllimport) int __stdcall WriteFile(void *, const void *, unsigned long, unsigned long *, void *);
int main(void);

void *memcpy(void *d, const void *s, size_t n)
{ char *o = d; const char *i = s; while (n--) *o++ = *i++; return d; }
void *memset(void *d, int c, size_t n)
{ char *o = d; while (n--) *o++ = (char)c; return d; }

static char buf[4096];
static size_t len;
static void put(char c) { if (len < sizeof buf) buf[len++] = c; }
static void num(unsigned long long v, unsigned base, int width, int upper)
{
    char t[24]; int n = 0;
    do { unsigned d = (unsigned)(v % base); t[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10); v /= base; } while (v);
    while (n < width) t[n++] = '0';
    while (n) put(t[--n]);
}
/* %s %d %u %x %X, with an optional 0-padded width and l/ll. */
int printf(const char *f, ...)
{
    va_list ap; unsigned long n;
    __builtin_va_start(ap, f);
    len = 0;
    for (; *f; f++) {
        int width = 0, ll = 0;
        if (*f != '%') { put(*f); continue; }
        f++;
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        while (*f == 'l') { ll++; f++; }
        switch (*f) {
        case 's': { const char *s = __builtin_va_arg(ap, const char *); while (*s) put(*s++); break; }
        case 'd': { long long v = ll ? __builtin_va_arg(ap, long long) : __builtin_va_arg(ap, int);
                    if (v < 0) { put('-'); v = -v; } num((unsigned long long)v, 10, width, 0); break; }
        case 'u': num(ll ? __builtin_va_arg(ap, unsigned long long) : __builtin_va_arg(ap, unsigned), 10, width, 0); break;
        case 'x': case 'X':
            num(ll ? __builtin_va_arg(ap, unsigned long long) : __builtin_va_arg(ap, unsigned), 16, width, *f == 'X'); break;
        default: put(*f); break;
        }
    }
    __builtin_va_end(ap);
    WriteFile(GetStdHandle((unsigned long)-11), buf, (unsigned long)len, &n, 0);
    return (int)len;
}
int mainCRTStartup(void) { return main(); }
EOF
printf 'LIBRARY kernel32.dll\nEXPORTS\nGetStdHandle\nWriteFile\n' > "$OUT/kernel32.def"

cd "$OUT"
"$BIN/llvm-dlltool.exe" -d kernel32.def -l kernel32.lib -m i386:x86-64
CC=("$BIN/clang.exe" --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wno-unused-function)
"${CC[@]}" -c crt.c -o crt.obj
"${CC[@]}" -I "$(w "$OUT/inc")" -I "$(w "$SRC")" -c "$(w "$HERE/test_main.c")" -o test_main.obj
"$BIN/ld.lld.exe" -flavor link -nologo -subsystem:console -entry:mainCRTStartup -nodefaultlib \
    -out:mmio_decode_test.exe test_main.obj crt.obj kernel32.lib
./mmio_decode_test.exe
