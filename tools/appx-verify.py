"""Checks a package (.appx) the way Windows will, as far as a PC without the signer's trust can: for packages made by
core/src/appx/mg_appx.c (tools/appx-tool), and for makeappx's and signtool's, to check this script itself.

    python tools/appx-verify.py <package.appx>

- every file's blocks against AppxBlockMap.xml (SHA-256 of each 64 KB block; the file's size; its local header's size);
- the package digests (AXPC, AXCD, AXCT, AXBM, and AXCI when there is a code integrity catalog) against those in the
  signature's SpcIndirectDataContent;
- the signer's messageDigest against the SpcIndirectDataContent's content, and the RSA signature over the signer's
  attributes with the certificate's key (openssl, which Git for Windows has).
Says each check and ends 0 when all pass.
"""
import base64
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib


def der(b, i):
    """(tag, content start, content end, next) of the TLV at i."""
    tag = b[i]
    n = b[i + 1]
    j = i + 2
    if n & 0x80:
        k = n & 0x7f
        n = int.from_bytes(b[j:j + k], "big")
        j += k
    return tag, j, j + n, j + n


def children(b, start, end):
    i = start
    while i < end:
        t, s, e, i = der(b, i)
        yield t, s, e


def main():
    path = sys.argv[1]
    d = open(path, "rb").read()
    ok = True

    def check(what, good, detail=""):
        nonlocal ok
        ok = ok and good
        print(("ok   " if good else "FAIL ") + what + (f" ({detail})" if detail else ""))

    cd = d.find(b"PK\x01\x02")
    entries = []
    p = cd
    while d[p:p + 4] == b"PK\x01\x02":
        method, = struct.unpack("<H", d[p + 10:p + 12])
        csz, usz = struct.unpack("<II", d[p + 20:p + 28])
        nlen, xlen, clen = struct.unpack("<HHH", d[p + 28:p + 34])
        lho, = struct.unpack("<I", d[p + 42:p + 46])
        name = d[p + 46:p + 46 + nlen].decode()
        entries.append(dict(name=name, method=method, csz=csz, usz=usz, lho=lho, cd=d[p:p + 46 + nlen + xlen + clen]))
        p += 46 + nlen + xlen + clen
    check("central directory", len(entries) > 0, f"{len(entries)} entries")

    def content(e):
        lho = e["lho"]
        ln, lx = struct.unpack("<HH", d[lho + 26:lho + 30])
        raw = d[lho + 30 + ln + lx:lho + 30 + ln + lx + e["csz"]]
        return zlib.decompress(raw, -15) if e["method"] == 8 else raw

    by = {e["name"]: e for e in entries}
    sig = by.get("AppxSignature.p7x")
    check("AppxSignature.p7x is the last entry", sig is not None and entries[-1] is sig)

    # The block map.
    bm = content(by["AppxBlockMap.xml"]).decode("utf-8")
    files = re.findall(r'<File Name="([^"]*)" Size="(\d+)" LfhSize="(\d+)">(.*?)</File>|<File Name="([^"]*)" Size="(\d+)" LfhSize="(\d+)"/>', bm, re.S)
    listed = set()
    for f in files:
        name, size, lfh, inner = (f[0], f[1], f[2], f[3]) if f[0] else (f[4], f[5], f[6], "")
        zipname = name.replace("\\", "/").replace("&amp;", "&")
        listed.add(zipname)
        e = by.get(zipname)
        if not e:
            check(f"block map's {name} in the package", False)
            continue
        data = content(e)
        hashes = re.findall(r'<Block Hash="([^"]*)"', inner)
        want = [base64.b64encode(hashlib.sha256(data[i:i + 65536]).digest()).decode() for i in range(0, len(data), 65536)]
        lho = e["lho"]
        ln, lx = struct.unpack("<HH", d[lho + 26:lho + 30])
        good = int(size) == len(data) and hashes == want and int(lfh) == 30 + ln + lx
        check(f"block map: {name}", good, f"{len(data)} bytes, {len(hashes)} blocks")
    for e in entries:
        if e["name"] not in listed and e["name"] not in ("AppxBlockMap.xml", "[Content_Types].xml", "AppxSignature.p7x") \
                and not e["name"].startswith("AppxMetadata/"):
            check(f"{e['name']} in the block map", False)

    # The signature.
    p7 = content(sig)
    check("signature begins with PKCX", p7[:4] == b"PKCX")
    b = p7[4:]
    _, s, e, _ = der(b, 0)                       # ContentInfo
    kids = list(children(b, s, e))
    _, s2, e2 = kids[1]                          # [0] EXPLICIT
    _, s3, e3, _ = der(b, s2)                    # SignedData
    sd = list(children(b, s3, e3))
    ci = sd[2]                                   # contentInfo
    ci_kids = list(children(b, ci[1], ci[2]))
    _, xs, xe = ci_kids[1]                       # [0] EXPLICIT SpcIndirectDataContent
    t, spc_s, spc_e, _ = der(b, xs)
    spc_content = b[spc_s:spc_e]
    spc_kids = list(children(b, spc_s, spc_e))
    dig_kids = list(children(b, spc_kids[1][1], spc_kids[1][2]))
    blob = b[dig_kids[1][1]:dig_kids[1][2]]
    check("package digest begins with APPX", blob[:4] == b"APPX", f"{len(blob)} bytes")
    tags = {}
    i = 4
    while i + 36 <= len(blob):
        tags[blob[i:i + 4].decode()] = blob[i + 4:i + 36]
        i += 36

    sig_lho = sig["lho"]
    axpc = hashlib.sha256(d[:sig_lho]).digest()
    cdbytes = b"".join(e["cd"] for e in entries if e["name"] != "AppxSignature.p7x")
    n = len(entries) - 1
    z64 = struct.pack("<4sQHHIIQQQQ", b"PK\x06\x06", 44, 0x2d, 0x2d, 0, 0, n, n, len(cdbytes), sig_lho)
    loc = struct.pack("<4sIQI", b"PK\x06\x07", 0, sig_lho + len(cdbytes), 1)
    eocd = struct.pack("<4sHHHHIIH", b"PK\x05\x06", 0, 0, 0xffff, 0xffff, 0xffffffff, 0xffffffff, 0)
    axcd = hashlib.sha256(cdbytes + z64 + loc + eocd).digest()
    want = {"AXPC": axpc, "AXCD": axcd,
            "AXCT": hashlib.sha256(content(by["[Content_Types].xml"])).digest(),
            "AXBM": hashlib.sha256(content(by["AppxBlockMap.xml"])).digest()}
    if "AppxMetadata/CodeIntegrity.cat" in by:
        want["AXCI"] = hashlib.sha256(content(by["AppxMetadata/CodeIntegrity.cat"])).digest()
    for tag in want:
        check(f"digest {tag}", tags.get(tag) == want[tag])
    check("no other digests", set(tags) == set(want), ",".join(tags))

    # The signer: messageDigest, and the RSA signature with the certificate's key.
    cert_tlv = None
    signer = None
    for t, s_, e_ in sd:
        if t == 0xA0:
            _, cs, ce, _ = der(b, s_)
            cert_tlv = b[s_:ce]
        if t == 0x31:
            signer = list(children(b, s_, e_))[0]
    si = list(children(b, signer[1], signer[2]))
    attrs = [k for k in si if k[0] == 0xA0][0]
    md = None
    for t, s_, e_ in children(b, attrs[1], attrs[2]):
        a = list(children(b, s_, e_))
        oid = b[a[0][1]:a[0][2]]
        if oid == bytes.fromhex("2a864886f70d010904"):
            v = list(children(b, a[1][1], a[1][2]))[0]
            md = b[v[1]:v[2]]
    check("messageDigest", md == hashlib.sha256(spc_content).digest())
    enc = [k for k in si if k[0] == 0x04][-1]
    signature = b[enc[1]:enc[2]]
    # What was signed: the attributes' [0] IMPLICIT element with its tag made a SET's.
    signed = b""
    i = signer[1]
    while i < signer[2]:
        tag, cs, ce, nxt = der(b, i)
        if tag == 0xA0:
            signed = bytes([0x31]) + b[i + 1:nxt]
            break
        i = nxt
    with tempfile.TemporaryDirectory() as tmp:
        open(os.path.join(tmp, "cert.der"), "wb").write(cert_tlv)
        open(os.path.join(tmp, "attrs.der"), "wb").write(signed)
        open(os.path.join(tmp, "sig.bin"), "wb").write(signature)
        r1 = subprocess.run(["openssl", "x509", "-inform", "DER", "-in", "cert.der", "-pubkey", "-noout", "-out", "pub.pem"],
                            cwd=tmp, capture_output=True, text=True)
        r2 = subprocess.run(["openssl", "dgst", "-sha256", "-verify", "pub.pem", "-signature", "sig.bin", "attrs.der"],
                            cwd=tmp, capture_output=True, text=True)
        subj = subprocess.run(["openssl", "x509", "-inform", "DER", "-in", "cert.der", "-noout", "-subject", "-enddate"],
                              cwd=tmp, capture_output=True, text=True).stdout.strip().replace("\n", "; ")
        check("RSA signature over the signer's attributes", r1.returncode == 0 and "Verified OK" in r2.stdout, subj)
    print("ALL OK" if ok else "SOME CHECKS FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
