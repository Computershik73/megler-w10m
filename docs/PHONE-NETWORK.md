# HP Elite x3 network investigation — 2026-10-16

Branch: `codex/hp-elite-x3-network`. Device Portal target: HP Elite x3,
`mc-0011fa4f4eb9`. Logs are local build artifacts, not committed user data.

## Why VK mp3 mod and Spotify reported no network

The build 221 sweep reproduced the same failure in both applications:
`java.lang.NoClassDefFoundError: android/net/ssl/SSLSockets`.

- VK mp3 mod 190 (`build.apk`, package `com.vkmp3mod.android`):
  `Android10Platform.enableSessionTickets` → `configureTlsExtensions` →
  `RealConnection.connectTls`. The login request also reached this failure.
  Log: `build/phone/codex-sweep-221/26-build/megler.log`.
- Spotify 9.1.90.2270: the networking dispatcher died with the same missing
  class, twice. Its obfuscated stack starts at `p.b04.a` / `p.a04.d`.
  Log: the `19-Spotify_*` directory in that sweep.
- F-Droid, MX Player and fgcos.scanwords had the same missing class.

Megler reports SDK 34, so OkHttp selects Android's newer TLS adapter. The
framework lacked the Android 10 socket helpers; Android 9's core library
also lacked public ALPN methods on SSLParameters and SSLSocket. These calls
failed before the application could complete its HTTPS request. The generic
network error did not describe a DNS failure or an absent Internet connection:
native network checks and external HttpsURLConnection requests already passed.

## Changes

- Implement `android.net.ssl.SSLSockets` using the actual Conscrypt provider.
- Add public application-protocol methods through a small boot DEX overlay,
  built from the existing Android 9 sources plus `.java.members` files. It
  loads before core-oj; the downloaded system image remains unchanged.
- Extend Conscrypt's existing Platform SSLParameters bridge for sockets and
  engines. The original method still transfers SNI, endpoint verification and
  the other parameters; the bridge also transfers ALPN. Conscrypt performs
  negotiation and certificate validation as before.
- Supply `InputEvent.isFromSource`, which separately crashed Spotify on touch.
- Return the framework's PackageInstaller from PackageManager, so F-Droid can
  register its session callbacks during startup.
- Package `data.apk` and `network.apk` for repeatable device checks and add
  `tools/phone-sweep.py` to save fresh logs, APK reports and screenshots for
  each sequential launch. It restores previous automation flags afterwards.

## Verification

Build 222 was tested on the phone. `build/phone/codex-alpn-222/01-network`
records a successful real TLS handshake to Google with `ALPN=h2` and
`TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256`. This test checks defensive
copies and invalid ALPN arguments, socket and engine parameter round trips,
retention of HTTPS endpoint verification, and the actual negotiated protocol.
It also receives HTTP 200 with bodies from example.com over HTTP and HTTPS,
and from Google over HTTPS. Trust managers and hostname checks are not disabled.

The ARM runtime regression run (`build/codex-tests-alpn.log`) passed both
4531-check and 4625-check configurations with zero failures.
On build 222 the phone's `data.apk` also passes all 48 checks, including
loopback TLS, HTTPS, refusal of an untrusted self-signed certificate, and the
PackageInstaller regression (`build/phone/codex-targets-222/04-data`).

F-Droid's build 222 log now shows successful HTTPS downloads of `entry.jar`
and 23.7 MB of the repository index during the observation window, instead
of the missing-class error. Its background
worker cancellation messages are separate from the TLS failure.

In `build/phone/codex-targets-222`, Spotify and VK mp3 mod both remain running
for the 60-second observation window without the former TLS exceptions.
Spotify also survives the user's touch/keyboard interaction, and the user
confirmed that its "no network" message no longer appears.

VK now receives an anonymous authorization response and advances to the
server's CAPTCHA challenge. The challenge page is blank because `megler.Web`
is still registered as `Absent` in `core/src/drivers/mg_java.c`; the Windows
shell has no WebView host. This is a separate, unresolved blocker to completing
that login flow. Do not bypass the challenge or describe VK login as fixed.
The logs contain session URLs/tokens and are intentionally not copied here.

The same missing-class failure is gone in the follow-up MX Player and
fgcos.scanwords runs (`build/phone/codex-tls-apps-222`); both stay running for
35 seconds. MX Player still reports peer-closed TLS connections from its
metrics uploader, and an advertising request attempts to resolve `http` as
a hostname. These are not evidence that every endpoint or app feature works.

Build 223 is now installed and packages the same runtime/framework changes
without `--auto`, for normal use after the diagnostic runs.

## Scope of the application sweep

`build/phone/codex-sweep-221/results.json` records all 39 APK selections
(37 then present, plus the packaged data/network diagnostics). `state=done`
means the automation finished, **not** that an app is fully functional.
Some apps still have unrelated launch failures: Apple Music calls the Linux
kuser memory barrier, Aurora requests missing modern system colors, Brave,
Firefox, MAX, Super S22 and ULTRAGL have native faults, and Instagram variants,
AnTuTu, Facebook Lite and Yandex have other compatibility failures. Absence
of a fatal log line alone is not a functional test of the other apps.

Login, account operations and music playback are not implied by a successful
startup or TLS handshake. No account credentials are entered by this sweep.

## Meta Lite TLS factory — 2026-10-21

On HP Elite x3 build 374, both separately installed Lite apps hide an exception
while preparing their main connection behind a network error. Instagram Lite
calls `setTrustManagers` on the null returned by
`SSLCertificateSocketFactory.getDefault`. Facebook Lite 532.0.0.6.105 calls
`getClass` on the same null in `X/1zT.A00`, DEX offset 33, repeating roughly
every two seconds. The latter was confirmed against its unpacked DEX.
Diagnostic logs are in `build/phone/lite-network`; they are not committed.
Other HTTPS connections do exchange data. A failed numeric-host probe in the
resolver trace followed by a successful name lookup is not itself a DNS outage.

The framework overlay implements the legacy factory on the boot Conscrypt
provider: real sockets, key/trust managers, SNI, handshake/write timeouts,
ALPN/NPN configuration, session tickets and persistent session caching.
String-host socket overloads complete the handshake and verify the hostname;
address-only and unconnected overloads follow Android's deferred verification
semantics. Default factories use the system trust roots. Only the explicit
Android `getInsecure` API uses an insecure trust manager. No global TLS bypass
or fallback from failed initialization to different trust settings is added.

The loopback TLS regression in `data.apk` uses this factory with a trusted
local certificate, rejects a mismatching hostname, then resets trust managers
and requires the system trust manager to reject that local certificate. It
also exercises unconnected socket options and ALPN list encoding.
