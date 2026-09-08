#!/usr/bin/env python3
"""Find likely Sony FS7 telemetry/status requests in a discovery-proxy log.

The useful requests are often the ones that are repeated by Sony's /rm.html page
and whose RESPONSE changes when the camera state changes. This tool groups the
captured requests and ranks endpoints by the number of distinct textual response
bodies observed.

Suggested capture sequence:
  1. Start fs7_proxy.py and open its /rm.html URL.
  2. Leave the camera idle for a few seconds.
  3. Change one setting at a time: WB, iris, gain/EI, ND, shutter, S&Q.
  4. Start/stop record and perform playback/Rec Review.
  5. Stop the proxy and run this tool on fs7-requests.jsonl.
"""
from __future__ import annotations
import argparse
import collections
import hashlib
import json
import pathlib
import sys

STATIC = ('.html','.htm','.js','.css','.png','.jpg','.jpeg','.gif','.svg','.ico','.woff','.woff2','.ttf')


def short(text: str, limit: int = 900) -> str:
    text = text.replace('\r', '\\r').replace('\n', '\\n')
    return text if len(text) <= limit else text[:limit] + '…'


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument('log', type=pathlib.Path, nargs='?', default=pathlib.Path('fs7-requests.jsonl'))
    p.add_argument('--all', action='store_true', help='include static assets')
    args = p.parse_args()

    groups: dict[tuple, list[dict]] = collections.defaultdict(list)
    for line_no, line in enumerate(args.log.read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip():
            continue
        try:
            e = json.loads(line)
        except json.JSONDecodeError as exc:
            print(f'warning: skipped malformed JSONL line {line_no}: {exc}', file=sys.stderr)
            continue
        path = e.get('path', '')
        bare = path.split('?', 1)[0].lower()
        if not args.all and e.get('method') == 'GET' and bare.endswith(STATIC):
            continue
        if 'responseBody' not in e:
            continue
        key = (e.get('method',''), path, e.get('body',''))
        groups[key].append(e)

    ranked = []
    for key, events in groups.items():
        variants = collections.OrderedDict()
        for e in events:
            body = e.get('responseBody','')
            digest = hashlib.sha1(body.encode('utf-8', errors='replace')).hexdigest()[:10]
            variants.setdefault(digest, body)
        ranked.append((len(variants), len(events), key, variants, events))
    ranked.sort(key=lambda x: (x[0], x[1]), reverse=True)

    if not ranked:
        print('No textual response bodies were found. Use the updated fs7_proxy.py to make a fresh capture.')
        return 1

    print('Likely telemetry/status endpoints are near the top: repeated request + changing response.\n')
    for idx, (variant_count, count, key, variants, events) in enumerate(ranked, 1):
        method, path, request_body = key
        marker = '*** LIKELY FEEDBACK ***' if count > 1 and variant_count > 1 else ''
        print(f'[{idx}] {method} {path}  calls={count} response_variants={variant_count} {marker}')
        if request_body:
            print('    request:', short(request_body, 400))
        print('    content-type:', events[0].get('responseContentType',''))
        for n, (digest, body) in enumerate(list(variants.items())[:4], 1):
            print(f'    response {n} [{digest}]: {short(body)}')
        if variant_count > 4:
            print(f'    ... {variant_count - 4} more response variants')
        print()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
