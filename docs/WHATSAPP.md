# WhatsApp on the HP Elite x3

## Native abort hook (2026-10-18)

WhatsApp 2.26.39.74 on Meglet 0.1.0.272 passed SoLoader's ABI selection, then
faulted during `libessential.so` initialization. The phone log is archived at
`build/phone/whatsapp-latest-20261018/meglet.log`.

After opening `cache/abort_hook.health`, it requested RX and RWX permissions
on page `00f84000`. Both calls returned success without changing protection:
the page belongs to the host executable, not a guest mapping. It then faulted
at `libessential.so+0x2d1cc`, `strh r0, [r4]`, with `r4=00f84878`. The map for
package 272 identifies that target, `Meglet.exe+0x14878`, as `g_abort`.

Executable protection requests on memory not owned by the guest now return
`-1` with `EACCES`. This lets a hook installer handle the refusal rather than
write into protected host code. Non-executable requests retain the existing
stack guard handling. Host code is never made writable. The mmap regression
checks both RX and RWX requests against the host's bionic `abort` export.

The library's mprotect wrapper at `0x2d3f8` checks for `-1` and records errno;
phone validation is required to establish whether startup can proceed without
the hook. Other initialization failures may still follow.

Validation: `tools/build-tests.sh arm --no-java` passed both runs (4866 and
4964 checks, zero failures), including the new host-code permission checks.
Normal-use package 0.1.0.273 was built and installed on the HP Elite x3 with
app data preserved. The manual launch in `build/phone/whatsapp-273.log` passed
native initialization: the denied executable request at 15:13:26 was handled,
and the app continued drawing through 15:13:54, with no native fault or app
termination in that capture. Network timeouts remain in the log; sign-in and
messaging have not been validated.
