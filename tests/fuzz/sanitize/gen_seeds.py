# SPDX-License-Identifier: GPL-2.0-or-later
"""Seed corpus and dictionary for the F-SANITIZE-* fuzz targets (T-M3-19). Python standard library only.

Usage: python3 -I -B tests/fuzz/sanitize/gen_seeds.py [--out DIR]

Writes, below DIR (default: the directory of this script):
  corpus/verdict/*.json   F-SANITIZE-01: the example of ARCHITECTURE.md section 3.1, one document per
                          state, valid edge documents, and the hostile documents of U-VERDICT-01..05
  corpus/label/*.bin      F-SANITIZE-02: 8 bytes serial, 1 byte of choices, the raw label
  corpus/display/*.bin    F-SANITIZE-03: 2 bytes capacity, the path
  verdict.dict            libFuzzer dictionary: keys, enum values, escapes
The inputs are synthetic. The output is deterministic, so the committed corpus can be regenerated.
"""
import argparse
import json
import os
import struct

STATES = ["clean", "dirty_flag", "unclean_log", "hibernated", "fast_startup", "unsupported",
          "corrupt", "unknown", "mirror_split", "names_not_nfc"]
REASONS = ["NOT_NTFS", "BITLOCKER", "MOUNT_FAILED", "FLAG_DIRTY", "FLAG_CHKDSK_UNDERWAY",
           "FLAG_PENDING_WORK", "FLAG_UNKNOWN_BITS", "LOG_UNCLEAN", "LOG_V2", "LOG_UNREADABLE",
           "HIBER_SIGNATURE", "HIBER_WAKE", "HIBER_SHORT", "HIBER_UNREADABLE", "VERSION_UNKNOWN",
           "CHECK_FAILED", "MFTMIRR_STALE", "NAMES_NOT_NFC"]
FLAGS = ["DIRTY", "RESIZE_LOG_FILE", "UPGRADE_ON_MOUNT", "MOUNTED_ON_NT4", "DELETE_USN_UNDERWAY",
         "REPAIR_OBJECT_ID", "CHKDSK_UNDERWAY", "MODIFIED_BY_CHKDSK", "UNKNOWN_BITS"]
LOGFILES = ["clean", "unclean", "v2_fast_startup", "unreadable"]
HIBERFILES = ["absent", "inactive", "hibernated", "unreadable"]


def example():
    return {
        "schema": 1, "is_ntfs": True, "ntfs_version": "3.1", "label": "DATA",
        "serial": "0x1A2B3C4D5E6F7788", "total_sectors": 131071, "cluster_size": 4096,
        "sector_size": 512, "volume_flags": ["DIRTY"], "logfile": "clean",
        "logfile_lsn": "0x0000000001A2B3C4", "hiberfile": "absent", "bitlocker": False,
        "state": "dirty_flag", "reason_codes": ["FLAG_DIRTY"], "nonnfc_count": 0, "nonnfc_paths": [],
    }


def doc(**kw):
    d = example()
    d.update(kw)
    return d


def dumps(d, **kw):
    return json.dumps(d, **kw).encode("utf-8")


def verdict_seeds():
    s = {}
    s["example"] = dumps(example(), indent=2) + b"\n"
    s["example-compact"] = dumps(example(), separators=(",", ":"))
    for st in STATES:
        if st == "clean":
            s["state-clean"] = dumps(doc(state="clean", reason_codes=[], volume_flags=[]))
        else:
            s["state-" + st] = dumps(doc(state=st, reason_codes=["CHECK_FAILED"]))
    s["unreadable-log"] = dumps(doc(logfile="unreadable", logfile_lsn=None, state="unknown",
                                    reason_codes=["LOG_UNREADABLE"]))
    s["null-version"] = dumps(doc(ntfs_version=None))
    s["all-codes"] = dumps(doc(state="unsupported", reason_codes=REASONS, volume_flags=FLAGS,
                               nonnfc_count=3, nonnfc_paths=["/a", "/b", "/c"]))
    s["extremes"] = dumps(doc(total_sectors=2 ** 63 - 1, cluster_size=2097152, sector_size=4096,
                              serial="0xFFFFFFFFFFFFFFFF", logfile_lsn="0xFFFFFFFFFFFFFFFF"))
    s["minimums"] = dumps(doc(total_sectors=1, cluster_size=512, serial="0x0000000000000000"))
    s["names-16"] = dumps(doc(state="names_not_nfc", reason_codes=["NAMES_NOT_NFC"], volume_flags=[],
                              nonnfc_count=22, nonnfc_paths=["/d%d/é" % i for i in range(16)]))
    s["names-mixed"] = dumps(doc(reason_codes=["FLAG_DIRTY", "NAMES_NOT_NFC"], nonnfc_count=1,
                                 nonnfc_paths=["/x"]))
    s["label-512-bytes"] = dumps(doc(label="é" * 256))
    s["label-512-ascii"] = dumps(doc(label="x" * 512))
    s["label-controls"] = dumps(doc(label="a\x01\x1b[31m\x7f‮\u0085,remove_hiberfile"))
    s["label-astral"] = dumps(doc(label="\U0001F600\U000F0000\U0010FFFF"), ensure_ascii=False)
    s["label-escapes"] = (b'{"schema":1,"is_ntfs":true,"ntfs_version":"3.1","label":"\\u00e9\\ud83d\\ude00\\n\\t\\/\\"",'
                          b'"serial":"0x1A2B3C4D5E6F7788","total_sectors":131071,"cluster_size":4096,"sector_size":512,'
                          b'"volume_flags":[],"logfile":"clean","logfile_lsn":"0x0000000000000000","hiberfile":"absent",'
                          b'"bitlocker":false,"state":"clean","reason_codes":[],"nonnfc_count":0,"nonnfc_paths":[]}')
    s["path-512"] = dumps(doc(state="names_not_nfc", reason_codes=["NAMES_NOT_NFC"], volume_flags=[],
                              nonnfc_count=1, nonnfc_paths=["/" + "p" * 511]))
    s["path-hostile"] = dumps(doc(state="names_not_nfc", reason_codes=["NAMES_NOT_NFC"], volume_flags=[],
                                  nonnfc_count=2, nonnfc_paths=["/a/\x1b[31mred\x1b[0m", "/b/‮gpj.exe\x07\n"]))
    base = dumps(example())
    s["pad-to-64k"] = base + b" " * (65536 - len(base))
    s["over-64k"] = base + b" " * (65537 - len(base))
    # hostile documents (U-VERDICT-01..05)
    h = {
        "dup-key": '{"schema":1,"schema":1}',
        "dup-key-escaped": '{"schema":1,"sc\\u0068ema":1}',
        "unknown-key": '{"reasons":["text"]}',
        "trailing-garbage": dumps(example()).decode() + "x",
        "second-object": dumps(example()).decode() + "{}",
        "truncated": dumps(example()).decode()[:120],
        "serial-path": dumps(doc(serial="../../../Library/LaunchDaemons/x")).decode(),
        "serial-lower": dumps(doc(serial="0x1a2b3c4d5e6f7788")).decode(),
        "serial-15": dumps(doc(serial="0x1A2B3C4D5E6F778")).decode(),
        "serial-17": dumps(doc(serial="0x1A2B3C4D5E6F77889")).decode(),
        "serial-esc": dumps(doc(serial="0x1A2B3C4D5E6F7788\x1b")).decode(),
        "serial-nul": dumps(doc(serial="0x1A2B3C4D5E6F77\u0000\u0000")).decode(),
        "lsn-hostile": dumps(doc(logfile_lsn="0x1A2B/../..")).decode(),
        "nul-label": dumps(doc(label="a\u0000b")).decode(),
        "lone-surrogate": '{"label":"\\ud800"}',
        "state-clean-with-reason": dumps(doc(state="clean")).decode(),
        "reason-esc": dumps(doc(reason_codes=["\x1b[31mowned"])).decode(),
        "reason-dup": dumps(doc(reason_codes=["FLAG_DIRTY", "FLAG_DIRTY"])).decode(),
        "reason-19": dumps(doc(reason_codes=REASONS + ["FLAG_DIRTY"])).decode(),
        "count-no-code": dumps(doc(nonnfc_count=1, nonnfc_paths=["/x"])).decode(),
        "paths-17": dumps(doc(state="names_not_nfc", reason_codes=["NAMES_NOT_NFC"], volume_flags=[],
                              nonnfc_count=17, nonnfc_paths=["/p%d" % i for i in range(17)])).decode(),
        "path-513": dumps(doc(state="names_not_nfc", reason_codes=["NAMES_NOT_NFC"], volume_flags=[],
                              nonnfc_count=1, nonnfc_paths=["/" + "p" * 512])).decode(),
        "int-sign": dumps(doc()).decode().replace('"total_sectors": 131071', '"total_sectors": -1'),
        "int-fraction": dumps(example(), separators=(",", ":")).decode().replace(":131071", ":131071.0"),
        "int-exponent": dumps(example(), separators=(",", ":")).decode().replace(":131071", ":1.31071e5"),
        "int-leading-zero": dumps(example(), separators=(",", ":")).decode().replace(":131071", ":0131071"),
        "int-huge": dumps(example(), separators=(",", ":")).decode().replace(":131071", ":99999999999999999999999"),
        "deep": '{"volume_flags":' + "[" * 200,
        "nested-array": dumps(doc(reason_codes=[["FLAG_DIRTY"]])).decode(),
        "wrong-type": dumps(doc(is_ntfs="true", label=5)).decode(),
    }
    for k, v in h.items():
        s["hostile-" + k] = v.encode("utf-8") if isinstance(v, str) else v
    s["hostile-invalid-utf8-label"] = dumps(doc(label="@@"), ensure_ascii=False).replace(b"@@", b"\xc0\xaf")
    s["hostile-raw-nul"] = dumps(doc(label="@@")).replace(b"@@", b"a\x00b")
    s["hostile-bom"] = b"\xef\xbb\xbf" + dumps(example())
    s["empty"] = b""
    return s


LABELS = [
    b"DATA", b"X,remove_hiberfile", b"X,allow_other,uid=0", b"../../etc/x", b".hidden", b"a\\,b", b"a\nb",
    "abc‮txt.exe".encode(), "é".encode() * 128, b"", b"   ", b"...", b" . ", b"a b", b"X=1",
    b"\xc0\xaf", b"\x80", b"\xed\xa0\x80", b"\xf8\x88\x80\x80\x80", b"\xe2\x82", b"ab\xff",
    "中".encode() * 128, "\U0001F600".encode() * 64, b"a" * 300, b"NTFS-12345678", b"CON", b"a:b", b"a/b",
    "x／y：z".encode(), "a\u0085b cd￿e".encode(), b"\x00", b"a\x00b", b"+()-_. x",
]


def label_seeds():
    s = {}
    for i, lab in enumerate(LABELS):
        serial = struct.pack(">Q", 0x1A2B3C4D5E6F7788 if i % 2 == 0 else 0xFFFFFFFFABCDEF01)
        s["label-%02d" % i] = serial + bytes([(i * 37) & 0xFF]) + lab
    return s


PATHS = [
    b"/", b"/a/b c/d.txt", "/café/é".encode(), b"/x\x1b[31mred", b"/a,b=c", b"/a\\b", b"/a:b",
    "/‮gpj.exe".encode(), b"/a\x00b", b"/a\xffz", b"/\xc0\xaf", b"/\xed\xa0\x80", b"/.hidden/..",
    "/\U0010FFFD\U000F0000".encode(), b"/" + b"\x01" * 400, b"//", b"",
]


def display_seeds():
    s = {}
    for i, p in enumerate(PATHS):
        cap = [4096, 8, 3, 0, 60, 1000][i % 6]
        s["path-%02d" % i] = struct.pack(">H", cap) + p
    return s


DICT = (['"%s"' % k for k in ("schema", "is_ntfs", "ntfs_version", "label", "serial", "total_sectors",
                              "cluster_size", "sector_size", "volume_flags", "logfile", "logfile_lsn",
                              "hiberfile", "bitlocker", "state", "reason_codes", "nonnfc_count", "nonnfc_paths")]
        + ['"%s"' % v for v in STATES + REASONS + FLAGS + LOGFILES + HIBERFILES]
        + ["true", "false", "null", "[]", "{}", '"0x', '"3.1"', "\\u0000", "\\ud800", "\\udc00",
           "\\ud83d\\ude00", "\\u001b", "\\u202e", "\\n", "\\/", "\\\\", "\\\"", "18446744073709551616",
           "9223372036854775808", "4294967296", "-0", "1e2", "00", "\\x", "\xc0\xaf", "\xed\xa0\x80"])


def dict_value(token):
    """A libFuzzer dictionary value: printable ASCII as is, the rest (and `"` and `\\`) as \\xHH."""
    out = []
    for byte in token.encode("latin-1"):
        if 0x20 <= byte < 0x7F and chr(byte) not in '"\\':
            out.append(chr(byte))
        else:
            out.append("\\x%02x" % byte)
    return "".join(out)


def write_dict(path):
    with open(path, "w", encoding="ascii", newline="\n") as f:
        for i, t in enumerate(DICT):
            f.write('t%d="%s"\n' % (i, dict_value(t)))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=here)
    args = ap.parse_args()
    for sub, seeds, ext in (("verdict", verdict_seeds(), ".json"), ("label", label_seeds(), ".bin"),
                            ("display", display_seeds(), ".bin")):
        d = os.path.join(args.out, "corpus", sub)
        os.makedirs(d, exist_ok=True)
        for name, data in sorted(seeds.items()):
            with open(os.path.join(d, name + ext), "wb") as f:
                f.write(data)
        print("%s: %d seeds" % (sub, len(seeds)))
    write_dict(os.path.join(args.out, "verdict.dict"))


if __name__ == "__main__":
    main()
