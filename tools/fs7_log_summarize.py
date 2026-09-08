#!/usr/bin/env python3
"""Summarize non-static requests captured by fs7_proxy.py."""
from __future__ import annotations
import argparse, collections, json, pathlib, sys

STATIC = ('.html','.htm','.js','.css','.png','.jpg','.jpeg','.gif','.svg','.ico','.woff','.woff2','.ttf')

def main():
    p=argparse.ArgumentParser()
    p.add_argument('log', type=pathlib.Path, nargs='?', default=pathlib.Path('fs7-requests.jsonl'))
    a=p.parse_args()
    groups=collections.OrderedDict()
    for line_no, line in enumerate(a.log.read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip(): continue
        try:
            e=json.loads(line)
        except json.JSONDecodeError as exc:
            print(f'warning: skipped malformed JSONL line {line_no}: {exc}', file=sys.stderr)
            continue
        path=e.get('path','')
        bare=path.split('?',1)[0].lower()
        # Keep non-GETs even when the path has a static-looking suffix.
        if e.get('method')=='GET' and bare.endswith(STATIC):
            continue
        key=(e.get('method'),path,e.get('contentType',''),e.get('body',''))
        groups.setdefault(key,[]).append(e)
    if not groups:
        print('No non-static requests found. Check the raw JSONL log; the Sony page may use GET/query requests that need manual filtering.')
        return 0
    for i,(key,events) in enumerate(groups.items(),1):
        method,path,ctype,body=key
        print(f'[{i}] count={len(events)}  {method} {path}')
        if ctype: print(f'    Content-Type: {ctype}')
        if body: print(f'    Body: {body[:2000]}')
        first=events[0]
        wall=first.get('requestTime',first.get('time',''))
        timing=''
        if 'requestCaptureMs' in first:
            timing=f"  Capture: {first.get('requestCaptureMs')} ms"
        if 'durationMs' in first:
            timing+=f"  Duration: {first.get('durationMs')} ms"
        print(f"    First: {wall}  Response: {first.get('responseStatus','?')}{timing}")
        print()
    return 0

if __name__=='__main__':
    raise SystemExit(main())
