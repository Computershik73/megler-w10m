"""Measure repeated gestures in an installed Megler app, preserving its data.

The phone must remain unlocked and foreground the tested app. Review the saved
screenshot: a loading/error screen is not a feed or catalogue benchmark.
--tap X,Y may be repeated to reach a screen; it uses surface pixel coordinates.
--no-canvas-copy selects the old composition path for a same-build comparison.
"""
import argparse
import base64
import importlib.util
import json
from pathlib import Path
import sys
import time

import phone

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--app', required=True)
p.add_argument('--build', type=int)
p.add_argument('--package', help='full package name verified by USB list, if the portal cannot enumerate packages')
p.add_argument('--out', type=Path, required=True)
p.add_argument('--startup', type=int, default=30)
p.add_argument('--gestures', type=int, default=8)
p.add_argument('--tap', action='append', default=[])
p.add_argument('--no-canvas-copy', action='store_true')
p.add_argument('--reuse', action='store_true', help='continue the foreground session instead of restarting')
args = p.parse_args()
assert 0 <= args.gestures <= 30 and 0 <= args.startup <= 180
spec = importlib.util.spec_from_file_location('flags', Path(__file__).with_name('phone-flags.py'))
flags = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flags)
spec = importlib.util.spec_from_file_location('report', Path(__file__).with_name('phone-perf-report.py'))
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)
prefix = phone.app_identity_prefix(args.app)
if args.package:
    identity, version, architecture, resource, publisher = args.package.rsplit('_', 4)
    if not identity.startswith(prefix):
        sys.exit('Package identity does not match the requested Android app')
    package = {'PackageFullName': args.package, 'PackageRelativeId': identity + '_' + publisher + '!App'}
else:
    packages = json.loads(phone.get('/api/app/packagemanager/packages'))['InstalledPackages']
    matches = [entry for entry in packages if entry['PackageFullName'].split('_')[0].startswith(prefix)]
    if len(matches) != 1:
        sys.exit('Expected one installed package for ' + args.app)
    package = matches[0]
flags.PFN = package['PackageFullName']
if args.build and '_0.1.0.' + str(args.build) + '_arm_' not in flags.PFN:
    sys.exit('The app is not on the requested build: ' + flags.PFN)
args.out.mkdir(parents=True, exist_ok=True)
(args.out/'package.json').write_text(json.dumps(package, indent=2))
saved_flags = flags.names()
if args.reuse and args.no_canvas_copy != ('no-canvas-copy' in saved_flags):
    sys.exit('Changing the composition path requires a restart; omit --reuse')
encode = lambda text: base64.b64encode(text.encode()).decode()
token = str(time.time_ns())

def capture():
    data = phone.read_app_file(flags.PFN, 'megler.log')
    if data:
        (args.out/'megler.log').write_bytes(data)
        (args.out/'performance.json').write_text(json.dumps(report.summarize(args.out/'megler.log'), indent=2))

try:
    flags.put('trace-fps')
    if args.no_canvas_copy:
        flags.put('no-canvas-copy')
    elif 'no-canvas-copy' in saved_flags:
        flags.remove('no-canvas-copy')
    if not args.reuse:
        try:
            phone.post('/api/taskmanager/app', method='DELETE', package=encode(flags.PFN))
        except Exception:
            pass
    phone.post('/api/taskmanager/app', appid=encode(package.get('PackageRelativeId') or 'App'), package=encode(flags.PFN))
    print('Testing ' + flags.PFN, flush=True)
    time.sleep(args.startup)
    capture()
    if args.build and f'Windows 10 Mobile 0.1.0.{args.build} (' not in (args.out/'megler.log').read_text(encoding='utf-8', errors='replace'):
        sys.exit('The running log does not match the requested build')
    for index, point in enumerate(args.tap):
        x, y = map(int, point.split(','))
        flags.put(f'ui-tap-{x}-{y}-{token}-{index}')
        time.sleep(10)
        capture()
    before = report.summarize(args.out/'megler.log')
    sample_start, interval_start = len(before['scroll_samples']), len(before['scroll_intervals'])
    for index in range(args.gestures):
        flags.put(f'ui-scroll-up-{token}-{index}')
        time.sleep(3.2)
        capture()
        print(f'Gesture {index + 1}/{args.gestures} captured', flush=True)
    flags.put('ui-frame-' + token)
    time.sleep(2)
    screenshot = phone.read_app_file(flags.PFN, 'shot-frame-' + token + '.bmp')
    if screenshot:
        (args.out/'screen.bmp').write_bytes(screenshot)
    capture()
    summary = report.summarize(args.out/'megler.log')
    gesture_samples = summary['scroll_samples'][sample_start:]
    gesture_intervals = summary['scroll_intervals'][interval_start:]
    if len(gesture_samples) < args.gestures:
        sys.exit('Incomplete gesture run; inspect foreground/suspension in the saved log')
    print(json.dumps({'gestures': gesture_samples, 'intervals': gesture_intervals,
                      'native_faults': summary['native_faults'], 'screenshot': bool(screenshot)}, indent=2), flush=True)
finally:
    for name in ('trace-fps', 'no-canvas-copy'):
        if name in saved_flags:
            flags.put(name)
        else:
            flags.remove(name)
