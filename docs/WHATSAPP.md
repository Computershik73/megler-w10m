# WhatsApp on the HP Elite x3

## Native abort hook (2026-10-18)

WhatsApp 2.26.39.74 on Megler 0.1.0.272 passed SoLoader's ABI selection, then
faulted during `libessential.so` initialization. The phone log is archived at
`build/phone/whatsapp-latest-20261018/megler.log`.

After opening `cache/abort_hook.health`, it requested RX and RWX permissions
on page `00f84000`. Both calls returned success without changing protection:
the page belongs to the host executable, not a guest mapping. It then faulted
at `libessential.so+0x2d1cc`, `strh r0, [r4]`, with `r4=00f84878`. The map for
package 272 identifies that target, `Megler.exe+0x14878`, as `g_abort`.

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

## Out of addresses: Go's arena and a heap five times what lived (2026-11-08)

On Megler 0.1.0.671 WhatsApp ended twice on the HP Elite x3, each time at the
end of its 32-bit process's 2 GB of addresses (`elite-logs-1108`: run 1
registering, run 2 restoring from Google Drive). Two things held them:

- **Go's runtime in `libgojni.so`** (Psiphon's tunnel core, Go 1.26.5) staked
  out its heap's arena up front as it started, as Go does on any 32-bit
  processor: 516 MB in run 2 ("1 of 528384 KB" in the addresses' census) and,
  where 516 were not to be had, 260 MB in run 1 ("1 of 266240 KB"), for a heap
  of a few megabytes. Its arenas' metadata is 0x242000 bytes in that version,
  not the "~258 MB" of the source's comment. Now refused on 32-bit hosts
  ([ARCHITECTURE.md](ARCHITECTURE.md), "Go's heap's addresses").
- **The Java heap** grew to 565 MB of blocks (run 1) and 377 (run 2) for some
  80 to 95 MB that lived, as the collections that stopped the app at the end
  found. Each collection alongside the app took what the heap held after it
  for what lived, the 100 to 320 MB the app's threads (Google Play services'
  among them) made while it marked, at the lowest priority, for 4 to 16 s; its
  budget was half of that, and threads waited only at three budgets. Run 1:
  made between collections 35, 44, 105, 193, 265 MB; marking 2.2, 4.3, 9.6,
  11.2, 16.4 s; "live" 52, 103, 174, 256, 319 MB. The arenas were never given
  back, so the addresses stayed at the most the heap had had. Then the mark
  stack, allocated at each collection as long as the heap had objects (6 MB),
  found no stretch of addresses that long: every collection was given up
  ("not collected", 4706 times in 20 s of run 2) until an
  `OutOfMemoryError`; in run 1 the XAML of Megler's permission question could
  not get memory either. Now the budget comes from what the collection traced,
  what was made while it marked counts toward the next, threads wait at two
  budgets and a collection that falls behind marks at the usual priority, the
  budget keeps within the free addresses, empty arenas go back whole, and the
  mark stack is the collector's own, kept ([JAVA.md](JAVA.md), "When it runs"
  and "The heap").

What to look for in the next log of WhatsApp: the line "go: libgojni.so's
runtime asked to reserve 516 MB up front: refused on a 32-bit Windows, Go
reserves as it grows" once; no allocation of 528384 or 266240 KB in "addresses
by kind" (Go's heap grows 4 MB at a time now, in allocations of 4096 or 8192
KB, named "Go's runtime in libgojni.so" where they are among the four sizes
that hold the most); in each "gc: alongside the app" line, "live N MB as
traced" near what the collections that stop the app find, "the heap's blocks"
within about three times that, the arenas "given back" when the app's garbage
falls, the budget, and ", behind and hurried" where the app outran the
marking; no "not collected" lines (or the thread and the reason they name).

## The first screen on a Lumia 650 (2026-11-10)

WhatsApp's first screen (`com.whatsapp.registration.app.EULA`) showed its
picture, its title, the privacy text and the language, but never its green
"Accept and continue" button (0.1.0.685, 720 x 1280 at 320 dpi): nobody could
register. The button is in an androidx `CardView` (`eula_accept_view`) at the
bottom of the `NestedScrollView`'s content, just below the scroll view's own
bottom edge (a content of 1296 px in a view of 1136, the card's 112 px from
1136 down). Where it should be stayed the screen's background colour when
WhatsApp scrolled there itself, when a finger dragged the content up, and
with the flag `no-node-damage`.

In 2.26.39.74 the `NestedScrollView` has a global layout listener (`X.CbG`)
that, once the screen is laid out, finds the content taller than the view
and gives the card `setCardElevation(8)` and `setBackgroundColor(...)`: a new
`ColorDrawable` in place of CardView's rounded one, whose bounds stay empty
until the card is drawn. CardView clips itself to its outline, and Megler's
`View.drawFromParent` put that clip into the parent's display list before
the card's own list was recorded -- a rectangle of nothing, kept for as long
as the parent's list, which nothing recorded again. Neither the card nor its
button was drawn; the frame walk, the screen scrolled, met 17 nodes, the
screen's views but the button. A view's outline is its own node's now,
worked out as its list is recorded ([JAVA.md](JAVA.md), the overlay's
`View.java`), and the test app `scroll` makes the screen and scrolls to the
cards, with the frame walk and without. WhatsApp 2.26.39.74 itself in the
test program (`mg_tests --java app` with its .apks, 720 x 1280 at 320 dpi,
its content dragged up) showed the same before: the button's place white,
the frame walk at 17 nodes; now "Agree and continue" is drawn in its card
and the walk meets 18.
