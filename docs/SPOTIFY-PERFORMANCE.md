# Spotify monitor investigation

The baseline phone logs are in `build/phone/spotify-performance` (build 223).
GPU drawing is active. One 63.4-second frame-meter window contains only one
drawn frame and reports 38.1 seconds waiting to enter Java monitors. The
summary identifies contention, but cannot identify the monitor or owner.

## Capturing ownership

Builds 225/226 add an opt-in `trace-monitors` flag. Enable with
`python tools/phone-flags.py set trace-monitors`, then restart Meglet and open
Spotify. The observer watches the Java main thread (not the XAML UI thread).
Build 225's 200 ms threshold did not capture a single long entry wait despite
seconds of accumulated contention in the reproduced stall. Build 226 samples
waits lasting at least 1 ms and adds per-object completed-wait totals (count,
sum, maximum, most recently observed owner and entry method; first 64 objects,
top four reported every five seconds). Addresses are numeric and may be reused
after collection; these totals are diagnostic, not retained object references.
For a sampled wait it records:

- Waiting thread ID/name, object address/class, observed owner ID, duration.
- Up to 32 Java frames with signatures, DEX PCs and source line numbers.
- The owner's current stack, and up to two further owners if it too waits
  to enter a monitor.

The observer samples at most once every two seconds. It holds the VM metadata
lock to exclude collection and thread-stack destruction, briefly suspends
the sampled threads, copies the frames, resumes all threads, and only then
formats/logs the result. Ownership is checked again after suspending an owner
so a thread that already released the lock is not reported as its holder.
The normal blocking lock acquisition remains unchanged. The watched object
is also a GC root for the duration of its entry wait.

This diagnoses `monitor-enter`/synchronized-method contention, not all native
locks, `Object.wait`, network waits or the runtime metadata lock. Missing
owner stacks can mean ownership changed during the snapshot or that the
thread could not be suspended. A cycle of owner IDs is recorded without
suspending the same thread twice.

Remove just this flag after diagnosis (preserving other flags):
`python tools/phone.py rm '\LocalState\flags\trace-monitors'`.
It is read at Java-app startup; restart Meglet to disable the observer in an
already-running process. Do not use `phone-flags.py clear` for this cleanup.

The ARM regression suites after instrumentation pass 4531 and 4625 checks
with zero failures (`build/codex-monitor-tests.log`). These runs validate
normal monitor/GC behavior; the opt-in observer is checked on the phone.

## Result: the cause, fixed (commit 27dff4f)

Build 226's trace named the monitor. Spotify's main thread waited on its own
`android/os/MessageQueue` 3477 times in 7 s (2.6 s in all). The owner each
time was the thread `broadcast-delivery`, in `MessageQueue.enqueue` from
`meglet.Broadcasts.send` from
`com.spotify.widgets.npvwidget.WidgetUpdateCoordinator.a`.

`WidgetUpdateCoordinator` is a manifest receiver for Spotify's own
`ACTION_USER_LOGIN`, `ACTION_USER_LOGOUT` and `ACTION_END_SESSION`. It passes
each to its widget providers by an intent with the same action that names the
provider (`setComponent`). `Broadcasts.send` gave such an intent to every
receiver whose filter lists the action as well, so the coordinator heard its
own relay and relayed it again, without end. Android gives an intent that
names a component to that component alone. The overlay's `Broadcasts.send`
now does the same, chooses manifest receivers by `meglet.Manifest.match`, and
skips disabled receivers. The data app's step "broadcast-explicit" covers it:
before the fix it logged `relay=50 target=49 off=50 registered=50` (the
step's bound), after it `relay=1 target=1 off=0 registered=1`.

On the phone, build 227 (the same trace flag) ran Spotify for 100 s:
- no wait on the MessageQueue at all, and 41 ms of monitor waits in all;
- frames throughout. Build 223 had a 63.4 s window of one frame, 38.1 s of
  it waiting for monitors.
Logs: `build/phone/spotify-227`. The trace flag was removed afterwards.

Still open: after 100 s the home screen showed its loading spinner with
Spotify's "Slow connection" banner. No stub of a missing import was called,
and no Java network exception was logged. The live heap grew from 70 MB to
135 MB over the run (about 200 threads), with collections of 250-390 ms
every 5-8 s.
