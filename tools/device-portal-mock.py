#!/usr/bin/env python3
"""A Windows Device Portal of the tests' own, which answers as an Xbox's does, for the Xbox build's portal client
(app/Megler/shell/DevicePortal.cpp; mg_tests --portal-windows, which tools/build-tests.sh win runs against this).

    python tools/device-portal-mock.py --ready FILE [--cert PEM --key PEM] [--user NAME --password WORD]
                                       [--received DIR] [--state-polls N] [--busy-first N] [--log FILE]

What it answers, as Microsoft's WindowsDevicePortalWrapper and DesktopMode_uwp's App Installer talk to the portal (an
Xbox's at https://localhost:11443; no console was asked to make this):

- Basic authentication on every request when --user is given: 401 with "WWW-Authenticate: Basic" otherwise;
- a CSRF token handed out as the cookie CSRF-Token (with a second cookie, WMID, as the portal sets more than one) on any
  GET, which a POST or a DELETE must send back both in its Cookie header and in X-CSRF-Token, or be refused 403;
- GET /api/os/info: who it is;
- POST /api/app/packagemanager/package?package=NAME, a multipart form whose files are named by their file names: the one
  named NAME is the package, a ZIP with AppxManifest.xml, whose identity makes the installed package's full name
  (<name>_<version>_<processor>__<publisher id>, the publisher id from the Publisher as Windows makes it); every file is
  kept in --received. A package whose name has "refuse" in it is refused, as the deployment engine refuses one;
- GET /api/app/packagemanager/state: 404 before any install, 204 while one goes (--state-polls times after the form,
  --busy-first times before the first, as another client's install would be), then the result as JSON (Code, CodeText,
  Reason, Success);
- GET /api/app/packagemanager/packages: {"InstalledPackages": [...]}, two of the console's own among them;
- DELETE /api/app/packagemanager/package?package=FULLNAME: the package uninstalled, 404 with a reason when it is not.

It listens on 127.0.0.1 alone, on a port of its own choosing, and writes "<scheme>://127.0.0.1:<port>" to --ready once
it listens; with --cert and --key over TLS, with that certificate (one nothing vouches for, made for another name, as the
console's own is). --log has a line for each request: method, path, status, and what the client sent of authentication,
cookies and token (whether, not what).
"""

import argparse
import base64
import hashlib
import http.server
import io
import json
import os
import re
import secrets
import ssl
import sys
import threading
import urllib.parse
import xml.etree.ElementTree as ET
import zipfile


def publisher_id(publisher):
    """Windows's publisher id: the first 8 bytes of the SHA-256 of the UTF-16 publisher, in Crockford's base 32."""
    digest = hashlib.sha256(publisher.encode("utf-16-le")).digest()[:8]
    bits = "".join(format(b, "08b") for b in digest) + "0"
    alphabet = "0123456789abcdefghjkmnpqrstvwxyz"
    return "".join(alphabet[int(bits[i:i + 5], 2)] for i in range(0, 65, 5))


class Portal:
    def __init__(self, args):
        self.args = args
        self.lock = threading.Lock()
        self.token = secrets.token_urlsafe(24)
        self.wmid = secrets.token_hex(8)
        self.installed = {}
        self.state = None          # None: no install yet; else {"polls": n, "result": {...}}
        self.busy = args.busy_first
        self.log = open(args.log, "a", encoding="utf-8") if args.log else None
        for system in ("Microsoft.Xbox.DevHome_100.2410.0.0_x64__8wekyb3d8bbwe",
                       "Microsoft.XboxDevices_1.0.0.0_x64__8wekyb3d8bbwe"):
            name, version = system.split("_")[0], system.split("_")[1]
            self.installed[system] = {"name": name, "version": version, "publisher": "CN=Microsoft Corporation, O=Microsoft Corporation, "
                                      "L=Redmond, S=Washington, C=US", "origin": 2}

    def note(self, line):
        if self.log:
            with self.lock:
                self.log.write(line + "\n")
                self.log.flush()


def read_body(handler):
    if handler.headers.get("Transfer-Encoding", "").lower() == "chunked":
        out = io.BytesIO()
        while True:
            size = int(handler.rfile.readline().strip().split(b";")[0], 16)
            if size == 0:
                handler.rfile.readline()
                break
            out.write(handler.rfile.read(size))
            handler.rfile.readline()
        return out.getvalue()
    length = int(handler.headers.get("Content-Length", "0"))
    return handler.rfile.read(length) if length else b""


def multipart_files(content_type, body):
    """The form's parts as (name, file name, bytes)."""
    match = re.search(r'boundary="?([^";]+)"?', content_type or "")
    if not match:
        return None
    boundary = b"--" + match.group(1).encode()
    parts = []
    for piece in body.split(boundary)[1:]:
        if piece.startswith(b"--"):
            break
        piece = piece[2:] if piece.startswith(b"\r\n") else piece
        head, _, data = piece.partition(b"\r\n\r\n")
        if data.endswith(b"\r\n"):
            data = data[:-2]
        disposition = ""
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-disposition:"):
                disposition = line.decode("utf-8", "replace")
        name = re.search(r'\bname="?([^";]*)"?', disposition)
        filename = re.search(r'\bfilename="?([^";]*)"?', disposition)
        parts.append((name.group(1) if name else "", filename.group(1) if filename else "", data))
    return parts


def identity_of(package_bytes):
    """The package's identity from its AppxManifest.xml: name, version, processor, publisher; or why there is none."""
    try:
        with zipfile.ZipFile(io.BytesIO(package_bytes)) as z:
            manifest = z.read("AppxManifest.xml")
    except (zipfile.BadZipFile, KeyError) as e:
        return None, "not a package: %s" % e
    root = ET.fromstring(manifest)
    for element in root.iter():
        if element.tag.endswith("}Identity") or element.tag == "Identity":
            return (element.get("Name"), element.get("Version"), element.get("ProcessorArchitecture", "neutral"),
                    element.get("Publisher")), None
    return None, "its manifest has no Identity"


def make_handler(portal):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):  # quiet: the --log file has what matters
            pass

        def send(self, status, body=b"", content_type="application/json", cookies=False, extra=()):
            if isinstance(body, (dict, list)):
                body = json.dumps(body).encode()
            elif isinstance(body, str):
                body = body.encode()
            self.send_response(status)
            if body:
                self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            if cookies:
                self.send_header("Set-Cookie", "CSRF-Token=%s; Path=/; Secure" % portal.token)
                self.send_header("Set-Cookie", "WMID=%s; Path=/; Secure; HttpOnly" % portal.wmid)
            for k, v in extra:
                self.send_header(k, v)
            self.end_headers()
            if body:
                self.wfile.write(body)
            self.sent_status = status

        def authorized(self):
            if not portal.args.user:
                return True
            got = self.headers.get("Authorization", "")
            want = "Basic " + base64.b64encode(("%s:%s" % (portal.args.user, portal.args.password)).encode()).decode()
            return got == want

        def csrf_ok(self):
            cookie = self.headers.get("Cookie", "")
            return self.headers.get("X-CSRF-Token") == portal.token and ("CSRF-Token=" + portal.token) in cookie

        def handle_any(self, method):
            url = urllib.parse.urlsplit(self.path)
            query = urllib.parse.parse_qs(url.query)
            self.sent_status = 0
            body = read_body(self) if method in ("POST", "PUT") else b""
            try:
                if not self.authorized():
                    self.send(401, {"Reason": "Authentication required"}, extra=[("WWW-Authenticate", 'Basic realm="Windows Device Portal"')])
                    return
                if method != "GET" and not self.csrf_ok():
                    self.send(403, {"Reason": "CSRF token missing or wrong"})
                    return
                self.route(method, url.path, query, body)
            finally:
                portal.note("%s %s %d auth=%s cookie=%s csrf=%s fetch=%s" % (
                    method, self.path, self.sent_status,
                    "basic" if self.headers.get("Authorization", "").startswith("Basic ") else "none",
                    "csrf" if "CSRF-Token=" in self.headers.get("Cookie", "") else "none",
                    "yes" if self.headers.get("X-CSRF-Token") else "no",
                    "yes" if self.headers.get("CSRF-Token") == "Fetch" else "no"))

        def route(self, method, path, query, body):
            if method == "GET" and path == "/api/os/info":
                self.send(200, {"ComputerName": "XBOX-MOCK", "Language": "en-US", "OsEdition": "Xbox", "OsEditionId": 0,
                                "OsVersion": "10.0.26100.6588", "Platform": "Xbox One"}, cookies=True)
            elif method == "GET" and path == "/api/app/packagemanager/state":
                with portal.lock:
                    if portal.busy > 0:
                        portal.busy -= 1
                        self.send(204, cookies=True)
                        return
                    state = portal.state
                    if state is None:
                        self.send(404, {"Reason": "No installation action was found"}, cookies=True)
                    elif state["polls"] > 0:
                        state["polls"] -= 1
                        self.send(204, cookies=True)
                    else:
                        self.send(200, state["result"], cookies=True)
            elif method == "POST" and path == "/api/app/packagemanager/package":
                self.install(query.get("package", [""])[0], body)
            elif method == "DELETE" and path == "/api/app/packagemanager/package":
                full = query.get("package", [""])[0]
                with portal.lock:
                    if full in portal.installed:
                        del portal.installed[full]
                        self.send(200)
                    else:
                        self.send(404, {"Code": -2147009296, "CodeText": "Package not found", "Reason": "No package is installed by that name",
                                        "Success": False})
            elif method == "GET" and path == "/api/app/packagemanager/packages":
                with portal.lock:
                    listed = []
                    for full, p in sorted(portal.installed.items()):
                        major, minor, build, revision = (int(x) for x in p["version"].split("."))
                        pid = full.rsplit("_", 1)[-1]
                        listed.append({"CanUninstall": True, "Name": p["name"], "PackageFamilyName": p["name"] + "_" + pid,
                                       "PackageFullName": full, "PackageOrigin": p["origin"],
                                       "PackageRelativeId": p["name"] + "_" + pid + "!App", "Publisher": p["publisher"],
                                       "Version": {"Build": build, "Major": major, "Minor": minor, "Revision": revision}})
                self.send(200, {"InstalledPackages": listed}, cookies=True)
            else:
                self.send(404, {"Reason": "No such API"}, cookies=method == "GET")

        def install(self, package, body):
            parts = multipart_files(self.headers.get("Content-Type"), body)
            if not package or parts is None:
                self.send(400, {"Reason": "A package and a multipart form are wanted"})
                return
            os.makedirs(portal.args.received, exist_ok=True)
            data = None
            for _, filename, content in parts:
                with open(os.path.join(portal.args.received, os.path.basename(filename)), "wb") as f:
                    f.write(content)
                if filename == package:
                    data = content
            if data is None:
                self.send(400, {"Reason": "The form has no file named " + package})
                return
            identity, why = identity_of(data)
            if "refuse" in package:
                result = {"Code": -2147009287, "CodeText": "Install failed. Please contact your software vendor.",
                          "Reason": "Deployment Add operation with target volume C: on Package %s failed (the mock refuses it)" % package,
                          "Success": False}
            elif identity is None:
                result = {"Code": -2147009296, "CodeText": "The package could not be opened.", "Reason": why, "Success": False}
            else:
                name, version, arch, publisher = identity
                full = "%s_%s_%s__%s" % (name, version, arch, publisher_id(publisher))
                with portal.lock:
                    portal.installed[full] = {"name": name, "version": version, "publisher": publisher, "origin": 5}
                result = {"Code": 0, "CodeText": "The operation completed successfully.", "Reason": "", "Success": True}
            with portal.lock:
                portal.state = {"polls": portal.args.state_polls, "result": result}
            self.send(200)

        def do_GET(self):
            self.handle_any("GET")

        def do_POST(self):
            self.handle_any("POST")

        def do_DELETE(self):
            self.handle_any("DELETE")

    return Handler


class Server(http.server.ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        # A client that drops the connection -- the tests' own that refuses the certificate does, in its handshake -- is no
        # error of the portal's.
        if isinstance(sys.exc_info()[1], (ConnectionError, ssl.SSLError)):
            return
        super().handle_error(request, client_address)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--ready", required=True)
    parser.add_argument("--cert")
    parser.add_argument("--key")
    parser.add_argument("--user", default="")
    parser.add_argument("--password", default="")
    parser.add_argument("--received", default="received")
    parser.add_argument("--state-polls", type=int, default=2)
    parser.add_argument("--busy-first", type=int, default=0)
    parser.add_argument("--log")
    args = parser.parse_args()
    portal = Portal(args)
    server = Server(("127.0.0.1", 0), make_handler(portal))
    scheme = "http"
    if args.cert and args.key:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.cert, args.key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        scheme = "https"
    with open(args.ready + ".part", "w") as f:
        f.write("%s://127.0.0.1:%d\n" % (scheme, server.server_address[1]))
    os.replace(args.ready + ".part", args.ready)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
