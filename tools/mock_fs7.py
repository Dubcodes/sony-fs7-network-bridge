#!/usr/bin/env python3
"""Tiny local stand-in for the FS7 web server, used to validate discovery tools.

This is NOT an FS7 protocol implementation. Its endpoints must never be copied
into a real bridge Sony map. It provides deterministic HTML/JS, Basic Auth,
commands and changing status responses so the discovery tooling can be tested.
"""
from __future__ import annotations
import argparse, base64, json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

HTML = b'''<!doctype html><html><head><script src="/remote.js"></script></head>
<body><button onclick="cmd('record')">REC</button><button onclick="cmd('review')">REVIEW</button><button onclick="cmd('wb')">WB</button><pre id="s"></pre></body></html>'''
JS = b'''async function cmd(name){return fetch('/mock/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'command='+encodeURIComponent(name)});}async function poll(){let r=await fetch('/mock/status');document.getElementById('s').textContent=await r.text();}setInterval(poll,500);poll();'''

class Handler(BaseHTTPRequestHandler):
    server_version = "MockFS7/0.2"
    def log_message(self, fmt, *args): pass
    def _auth(self):
        want = "Basic " + base64.b64encode(f"{self.server.user}:{self.server.password}".encode()).decode()  # type: ignore[attr-defined]
        if self.headers.get('Authorization') == want: return True
        self.send_response(401); self.send_header('WWW-Authenticate','Basic realm="MockFS7"'); self.send_header('Content-Length','0'); self.end_headers(); return False
    def _send(self, status, data, ctype):
        self.send_response(status); self.send_header('Content-Type',ctype); self.send_header('Set-Cookie','MOCKSESSION=do-not-log-this-secret; Path=/; HttpOnly'); self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)
    def do_GET(self):
        if not self._auth(): return
        if self.path in ('/rm.html','/rms.html','/rmt.html'): self._send(200,HTML,'text/html; charset=utf-8'); return
        if self.path=='/remote.js': self._send(200,JS,'application/javascript'); return
        if self.path=='/mock/status':
            self._send(200,json.dumps(self.server.state).encode(),'application/json')  # type: ignore[attr-defined]
            return
        self._send(404,b'not found','text/plain')
    def do_POST(self):
        if not self._auth(): return
        n=int(self.headers.get('Content-Length','0') or 0); body=self.rfile.read(n).decode(errors='replace')
        if self.path!='/mock/control': self._send(404,b'not found','text/plain'); return
        command=parse_qs(body).get('command',[''])[0]
        state=self.server.state  # type: ignore[attr-defined]
        if command=='record': state['recording']=not state['recording']
        elif command=='review': state['playback']=not state['playback']
        elif command=='wb': state['white']['kelvin']=5600 if state['white']['kelvin']==3200 else 3200
        self._send(200,json.dumps({'ok':True,'received':body}).encode(),'application/json')

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--listen',default='127.0.0.1'); ap.add_argument('--port',type=int,default=18080); ap.add_argument('--user',default='admin'); ap.add_argument('--password',default='pxw-fs7'); a=ap.parse_args()
    s=ThreadingHTTPServer((a.listen,a.port),Handler); s.user=a.user; s.password=a.password; s.state={'recording':False,'playback':False,'white':{'kelvin':3200},'iris':2.8,'gainDb':0.0,'sqFps':150}  # type: ignore[attr-defined]
    print(f'Mock FS7 at http://{a.listen}:{a.port}/rm.html (test only)')
    try:s.serve_forever()
    except KeyboardInterrupt:pass
    finally:s.server_close()
if __name__=='__main__': main()
