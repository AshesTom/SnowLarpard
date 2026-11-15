#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Decrypts the protected __TEXT of a Mac OS X 10.6 binary, for reverse engineering only.

    deprotect.py IN OUT

Snow Leopard ships a few binaries (Finder, Dock, loginwindow, WindowServer…) with their __TEXT
segment flagged SG_PROTECTED_VERSION_1: past its first three pages, each 4 KiB page is Blowfish-CBC
under the well-known 64-byte key phrase, with a zero IV per page. The kernel decrypts them at
page-in. (The zero padding after the load commands encrypts to 40 cf 69 2e…, the "magic" seen at
the start of the protected range.) This does it once, on a thin
x86_64 or i386 slice, and clears the flag so disassemblers read the code. The output is for your
own analysis of your own copy: nothing of it goes into this repository or the shell.

Needs libcrypto (OpenSSL).
"""
import ctypes
import ctypes.util
import struct
import sys

KEY = b"ourhardworkbythesewordsguardedpleasedontsteal(c)AppleComputerInc"
PAGE = 0x1000
SKIP = 3 * PAGE          # the first three pages are in the clear
SG_PROTECTED_VERSION_1 = 0x8
LC_SEGMENT, LC_SEGMENT_64 = 0x1, 0x19
MAGIC_BLOWFISH = 0x2e69cf40   # a zero block under the key: the encrypted start of the range

crypto = ctypes.CDLL(ctypes.util.find_library("crypto") or "libcrypto.so")
crypto.OSSL_PROVIDER_load.restype = ctypes.c_void_p
crypto.OSSL_PROVIDER_load.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
crypto.EVP_CIPHER_fetch.restype = ctypes.c_void_p
crypto.EVP_CIPHER_fetch.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
crypto.EVP_CIPHER_CTX_new.restype = ctypes.c_void_p
crypto.EVP_CIPHER_CTX_set_key_length.argtypes = [ctypes.c_void_p, ctypes.c_int]
crypto.EVP_CIPHER_CTX_free.argtypes = [ctypes.c_void_p]
crypto.EVP_DecryptInit_ex.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                      ctypes.c_char_p, ctypes.c_char_p]
crypto.EVP_CIPHER_CTX_set_padding.argtypes = [ctypes.c_void_p, ctypes.c_int]
crypto.EVP_DecryptUpdate.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                     ctypes.POINTER(ctypes.c_int), ctypes.c_char_p, ctypes.c_int]


def blowfish_cipher():
    # Blowfish lives in OpenSSL 3's legacy provider
    crypto.OSSL_PROVIDER_load(None, b"legacy")
    crypto.OSSL_PROVIDER_load(None, b"default")
    cipher = crypto.EVP_CIPHER_fetch(None, b"BF-CBC", None)
    if not cipher:
        sys.exit("libcrypto has no Blowfish (OpenSSL's legacy provider is missing)")
    return cipher


def bf_cbc_decrypt(cipher, data):
    ctx = crypto.EVP_CIPHER_CTX_new()
    try:
        crypto.EVP_DecryptInit_ex(ctx, cipher, None, None, None)
        crypto.EVP_CIPHER_CTX_set_key_length(ctx, len(KEY))
        crypto.EVP_DecryptInit_ex(ctx, None, None, KEY, bytes(8))
        crypto.EVP_CIPHER_CTX_set_padding(ctx, 0)
        out = ctypes.create_string_buffer(len(data) + 16)
        n = ctypes.c_int(0)
        if not crypto.EVP_DecryptUpdate(ctx, out, ctypes.byref(n), data, len(data)):
            raise RuntimeError("Blowfish failed")
        return out.raw[:n.value]
    finally:
        crypto.EVP_CIPHER_CTX_free(ctx)


def segments(buf):
    magic = struct.unpack_from("<I", buf)[0]
    if magic not in (0xfeedface, 0xfeedfacf):
        sys.exit("not a thin little-endian Mach-O (extract the x86_64 slice first)")
    wide = magic == 0xfeedfacf
    ncmds = struct.unpack_from("<I", buf, 16)[0]
    off = 32 if wide else 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", buf, off)
        if cmd in (LC_SEGMENT, LC_SEGMENT_64):
            name = buf[off + 8:off + 24].rstrip(b"\0").decode()
            if wide:
                fileoff, filesize = struct.unpack_from("<QQ", buf, off + 40)
                flags_at = off + 68
            else:
                fileoff, filesize = struct.unpack_from("<II", buf, off + 32)
                flags_at = off + 52
            yield name, fileoff, filesize, flags_at
        off += size


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    buf = bytearray(open(sys.argv[1], "rb").read())
    done = False
    for name, fileoff, filesize, flags_at in segments(buf):
        flags = struct.unpack_from("<I", buf, flags_at)[0]
        if not flags & SG_PROTECTED_VERSION_1:
            continue
        start, end = fileoff + SKIP, fileoff + filesize
        magic = struct.unpack_from("<I", buf, start)[0]
        if magic != MAGIC_BLOWFISH:
            sys.exit(f"{name}: unknown protection {magic:#x}")
        cipher = blowfish_cipher()
        for p in range(start, end - PAGE + 1, PAGE):
            buf[p:p + PAGE] = bf_cbc_decrypt(cipher, bytes(buf[p:p + PAGE]))
        struct.pack_into("<I", buf, flags_at, flags & ~SG_PROTECTED_VERSION_1)
        print(f"{name}: decrypted {(end - start) // PAGE} pages")
        done = True
    if not done:
        sys.exit("no protected segment")
    open(sys.argv[2], "wb").write(buf)


if __name__ == "__main__":
    main()
