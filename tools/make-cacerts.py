#!/usr/bin/env python3
"""make-cacerts.py BUNDLE OUT [--source TEXT]

The certificate authorities apps trust, as Android lays them out (etc/security/cacerts): from a bundle of PEM certificates
-- Mozilla's root store as curl extracts it (https://curl.se/ca/cacert.pem: the roots Mozilla trusts for websites) -- one
file a root in OUT, named for its subject as Android names them (OpenSSL's X509_NAME_hash_old, a dot and a number for
subjects of the same hash), holding the certificate in PEM, then its text and its SHA-1 fingerprint, as AOSP's files do
(system/ca-certificates/files). OUT is emptied of such files first; a README.md there says where they came from.

Roots on DISTRUSTED are left out whatever the bundle says: a root whose keys or whose intermediates' keys were
compromised, which a bundle of before its removal would still have. Needs openssl on the PATH (Git for Windows has it).
"""
import argparse
import base64
import hashlib
import os
import re
import subprocess
import sys

# SHA-256 of each root's DER, and why it is never trusted.
DISTRUSTED = {
    # eMudhra's emSign ECC Root CA - G3 (India): eight intermediates under it had private keys anyone could work out from
    # their certificates (reported 2026-10-17, revoked 2026-10-18/19); Mozilla removed the root, Chrome dropped it on
    # 2026-10-27, and eMudhra left the TLS business. AOSP's list (system/ca-certificates, 2025) still has it.
    '86a1ecba089c4a8d3bbe2734c612ba341d813e043cf9e8a862cd5c57a36bbe6b': 'emSign ECC Root CA - G3 (eMudhra): intermediates\' keys compromised, 2026-10',
}

PEM = re.compile(r'-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----', re.S)


def openssl(args, pem):
    p = subprocess.run(['openssl', 'x509'] + args, input=pem.encode('ascii'), capture_output=True)
    if p.returncode:
        raise SystemExit('openssl x509 %s failed: %s' % (' '.join(args), p.stderr.decode('utf-8', 'replace')))
    return p.stdout.decode('utf-8', 'replace').replace('\r\n', '\n')


def pem_of(der):
    b64 = base64.b64encode(der).decode('ascii')
    return '-----BEGIN CERTIFICATE-----\n' + '\n'.join(b64[i:i + 64] for i in range(0, len(b64), 64)) + '\n-----END CERTIFICATE-----\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bundle')
    ap.add_argument('out')
    ap.add_argument('--source', default='')
    a = ap.parse_args()
    text = open(a.bundle, encoding='utf-8', errors='replace').read()
    dated = re.search(r'Certificate data from Mozilla as of: (.*)', text)
    ders = [base64.b64decode(''.join(m.group(1).split())) for m in PEM.finditer(text)]
    if not ders:
        raise SystemExit('no certificates in ' + a.bundle)
    os.makedirs(a.out, exist_ok=True)
    for n in os.listdir(a.out):
        if re.fullmatch(r'[0-9a-f]{8}\.[0-9]+', n):
            os.remove(os.path.join(a.out, n))
    seen, left_out, written = set(), [], []
    for der in ders:
        digest = hashlib.sha256(der).hexdigest()
        if digest in seen:
            continue
        seen.add(digest)
        pem = pem_of(der)
        subject = openssl(['-noout', '-subject'], pem).strip().replace('subject=', '', 1)
        if digest in DISTRUSTED:
            left_out.append((subject, DISTRUSTED[digest]))
            continue
        h = openssl(['-noout', '-subject_hash_old'], pem).strip()
        k = 0
        while os.path.exists(os.path.join(a.out, '%s.%d' % (h, k))):
            k += 1
        name = '%s.%d' % (h, k)
        body = pem + openssl(['-noout', '-text'], pem)
        sha1 = openssl(['-noout', '-fingerprint', '-sha1'], pem).strip().split('=', 1)[1]
        body += 'SHA1 Fingerprint=' + sha1 + '\n'
        with open(os.path.join(a.out, name), 'w', encoding='utf-8', newline='\n') as f:
            f.write(body)
        written.append((name, subject))
    bundle_sha = hashlib.sha256(open(a.bundle, 'rb').read()).hexdigest()
    with open(os.path.join(a.out, 'README.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# The certificate authorities apps trust\n\n')
        f.write('Made by `tools/make-cacerts.py` from %s%s (SHA-256 %s): %d roots, one file each, laid out as Android\'s '
                '`etc/security/cacerts` (`tools/dvm-android-root.sh` copies them into the Android root).\n\n'
                % (a.source or os.path.basename(a.bundle), (', Mozilla\'s data as of ' + dated.group(1).strip()) if dated else '',
                   bundle_sha, len(written)))
        f.write('Left out whatever the bundle says (`DISTRUSTED` in the tool):\n\n')
        in_bundle = {why for _, why in left_out}
        for why in DISTRUSTED.values():
            f.write('- %s%s\n' % (why, ' -- it was in the bundle' if why in in_bundle else ' -- not in this bundle'))
        f.write('\nThe roots:\n\n')
        for name, subject in sorted(written):
            f.write('- `%s` %s\n' % (name, subject))
    print('%d roots written to %s; left out: %s' % (len(written), a.out, ', '.join(s for s, _ in left_out) or 'none of the bundle'))


if __name__ == '__main__':
    main()
