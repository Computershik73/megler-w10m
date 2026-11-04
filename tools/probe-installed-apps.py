"""Launch selected installed Android app packages, preserving data, and collect startup logs.
Set MEGLER_PHONE_NAME/MODEL/PORTAL to the verified phone. This checks startup,
not account login, DRM provisioning or video playback.
"""
import argparse
import base64
import json
from pathlib import Path
import re
import time

import phone

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--app', action='append', required=True)
parser.add_argument('--seconds', type=int, default=35)
parser.add_argument('--out', required=True)
args = parser.parse_args()
assert 5 <= args.seconds <= 180
out = Path(args.out)
out.mkdir(parents=True, exist_ok=True)
packages = json.loads(phone.get('/api/app/packagemanager/packages'))['InstalledPackages']
encode = lambda value: base64.b64encode(value.encode()).decode()
results = []
for app in args.app:
    prefix = ('MeglerApp.' + app.replace('_', '-'))[:41]
    matches = [p for p in packages if p['PackageFullName'].split('_')[0].startswith(prefix)]
    if len(matches) != 1:
        raise RuntimeError('expected one installed package for ' + app)
    package = matches[0]
    pfn = package['PackageFullName']
    folder = out/app
    folder.mkdir(exist_ok=True)
    (folder/'package.json').write_text(json.dumps(package, indent=2))
    print('Starting ' + app + ' (' + pfn + ')', flush=True)
    try:
        phone.post('/api/taskmanager/app', method='DELETE', package=encode(pfn))
    except Exception:
        pass
    stale = phone.read_app_file(pfn, 'megler.log')
    phone.post('/api/taskmanager/app', appid=encode(package.get('PackageRelativeId') or 'App'), package=encode(pfn))
    started = time.monotonic()
    end = started + args.seconds
    fresh = None
    while time.monotonic() < end:
        time.sleep(2)
        data = phone.read_app_file(pfn, 'megler.log')
        if not data or data == stale:
            continue
        fresh = data
        text = data.decode('utf-8', 'replace')
        if 'the app has ended (status' in text or 'E fault:' in text:
            break
    if fresh:
        (folder/'megler.log').write_bytes(fresh)
        text = fresh.decode('utf-8', 'replace')
        errors = [line for line in text.splitlines() if re.search(r'the main thread ended|E fault:|the app stopped because|FATAL EXCEPTION', line)]
        result = {'app': app, 'package': pfn, 'fresh_log': True, 'startup_failures': errors,
                  'sqlite_disabled': 'this build of the layer has no SQLite' in text,
                  'first_picture': "window: the app's first picture" in text,
                  'session_ended': [line for line in text.splitlines() if 'session: the app has ended' in line],
                  'observation_limit_seconds': args.seconds,
                  'elapsed_seconds': round(time.monotonic() - started, 1)}
    else:
        result = {'app': app, 'package': pfn, 'fresh_log': False}
    results.append(result)
    print(json.dumps(result), flush=True)
    try:
        phone.post('/api/taskmanager/app', method='DELETE', package=encode(pfn))
    except Exception:
        pass
    (out/'results.json').write_text(json.dumps(results, indent=2))
