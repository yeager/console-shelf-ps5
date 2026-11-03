# SPDX-License-Identifier: GPL-3.0-only
"""Fetch and verify pinned target dependencies into .cache/ps5 only."""
import json
from ps5_common import CACHE, ROOT, fetch, prepare_archive


def prepare_update_ca():
    pins = json.loads((ROOT / "native/ps5/dependencies.json").read_text())
    record = pins["updateCa"]
    return fetch(record["url"], CACHE / "downloads" / "isshelf-update-ca.pem",
                 record["sha256"], record["bytes"])


def prepare_mbedtls():
    pins = json.loads((ROOT / "native/ps5/dependencies.json").read_text())
    record = pins["mbedtls"]
    root = prepare_archive(record, "mbedtls-3.6.7.tar.bz2", CACHE / "mbedtls-3.6.7",
                           "mbedtls-3.6.7/")
    for required in (root / "include/mbedtls/ssl.h", root / "library/ssl_tls.c", root / "LICENSE"):
        if not required.is_file():
            raise RuntimeError(f"Mbed TLS dependency has an unexpected layout: {required}")
    return root


def prepare():
    pins = json.loads((ROOT / "native/ps5/dependencies.json").read_text())
    sdk = prepare_archive(pins["sdk"], "ps5-payload-sdk-v0.42.zip", CACHE / "sdk-v0.42", "ps5-payload-sdk/")
    ports = prepare_archive(pins["ports"], "pacbrew-v0.40.2.tar.gz", CACHE / "ports-v0.40.2",
                            "opt/ps5-payload-sdk/target/user/homebrew/")
    zlib = prepare_archive(pins["zlib"], "zlib-1.3.2.tar.gz", CACHE / "zlib-1.3.2", "zlib-1.3.2/")
    for required in [sdk / "target/include/stdlib.h", sdk / "bin/prospero-lld",
                     ports / "lib/libSDL2.a", zlib / "zlib.h"]:
        if not required.is_file():
            raise RuntimeError(f"Dependency has an unexpected layout: {required}")
    return sdk, ports, zlib


if __name__ == "__main__":
    print("Verified isolated dependencies:", *(str(p) for p in prepare()),
          str(prepare_mbedtls()), str(prepare_update_ca()), sep="\n")
