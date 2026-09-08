#!/usr/bin/env python3
from __future__ import annotations
import json, pathlib, re, subprocess, sys, tempfile, py_compile

ROOT = pathlib.Path(__file__).resolve().parents[1]
errors: list[str] = []

# JSON artifacts
for p in ROOT.rglob('*.json'):
    try:
        json.loads(p.read_text(encoding='utf-8'))
    except Exception as e:
        errors.append(f'{p.relative_to(ROOT)}: invalid JSON: {e}')

# Python tools compile
for p in (ROOT / 'tools').glob('*.py'):
    if p.name == 'validate_project.py':
        continue
    try:
        py_compile.compile(str(p), doraise=True)
    except Exception as e:
        errors.append(f'{p.relative_to(ROOT)}: Python compile failed: {e}')

h = (ROOT / 'include/AppTypes.h').read_text(encoding='utf-8')
cpp = (ROOT / 'src/AppTypes.cpp').read_text(encoding='utf-8')

def count_block(name: str) -> int:
    m = re.search(rf'{name}\s*=\s*\{{\{{(.*?)\}}\}};', cpp, re.S)
    if not m:
        return -1
    return len(re.findall(r'^\s*\{"[^"]+",', m.group(1), re.M))

m = re.search(r'COMMAND_COUNT\s*=\s*(\d+)', h)
command_count = int(m.group(1)) if m else -1
m = re.search(r'CAMERA_STATE_COUNT\s*=\s*(\d+)', h)
state_count = int(m.group(1)) if m else -1
if command_count != count_block('COMMANDS'):
    errors.append(f'COMMAND_COUNT={command_count}, descriptor entries={count_block("COMMANDS")}')
if state_count != count_block('CAMERA_STATES'):
    errors.append(f'CAMERA_STATE_COUNT={state_count}, descriptor entries={count_block("CAMERA_STATES")}')
for semantic in ('ready', 'writing', 'buffer_ready'):
    if f'{{"{semantic}"' not in cpp:
        errors.append(f'missing semantic camera state {semantic}')

# Normal LAN addressing defaults to DHCP; 10.77.7.2 remains an explicit static recovery profile.
if 'bool ethernetDhcp = true;' not in h or '10.77.7.2' not in h:
    errors.append('Ethernet must default to DHCP while retaining the 10.77.7.2 static recovery profile')

# Stop + Replay must prefer camera state, with a bounded delay fallback.
engine = (ROOT / 'src/CommandEngine.cpp').read_text(encoding='utf-8')
for token in ('cameraReadyForReview()', 'freshStateSince("ready", macroStartedMs_)', 'freshStateSince("writing", macroStartedMs_)', 'FallbackDelay', 'replayTimeoutMs'):
    if token not in engine:
        errors.append(f'state-driven Stop+Replay missing {token}')

# Sequence engine must be present and bounded.
seq_h = (ROOT / 'src/SequenceEngine.h').read_text(encoding='utf-8')
seq_cpp = (ROOT / 'src/SequenceEngine.cpp').read_text(encoding='utf-8')
for token in ('SLOT_COUNT = 8', 'MAX_STEPS = 16', 'WaitState', 'abort(', 'stateCodeFor'):
    if token not in seq_h and token not in seq_cpp:
        errors.append(f'sequence engine missing {token}')
config = (ROOT / 'src/ConfigStore.cpp').read_text(encoding='utf-8')
if '10s Record + State Replay (template)' not in config or 'seq["enabled"] = false' not in config:
    errors.append('sequence template must exist and remain disabled by default')
if 'value->updatedMs - r.stepStartedMs' not in seq_cpp:
    errors.append('sequence wait-state must require a post-step telemetry sample')

# Sony socket must be forced onto the camera Wi-Fi interface.
sony = (ROOT / 'src/SonyRemote.cpp').read_text(encoding='utf-8')
if '::send(fd,' not in sony:
    errors.append('SonyRemote POSIX socket send must be qualified as ::send')
if 'bind(fd' not in sony or 'WiFi.localIP()' not in sony:
    errors.append('Sony camera socket must bind to Wi-Fi source interface')
for token in ('validMethod(', 'forbiddenHeader(', 'maxBodyBytes = std::min'):
    if token not in sony:
        errors.append(f'Sony request hardening missing {token}')
for token in ('Content-Length', 'Truncated HTTP response body', 'Malformed HTTP status line', 'ChunkState'):
    if token not in sony:
        errors.append(f'Sony HTTP framing hardening missing {token}')

# Feedback/discovery requirements.
web = (ROOT / 'src/WebApp.cpp').read_text(encoding='utf-8')
for endpoint in ('/api/v1/camera-state', '/api/v1/telemetry-map', '/api/v1/sequences/status', '/api/v1/wifi-scan'):
    if endpoint not in web:
        errors.append(f'missing web endpoint {endpoint}')
telemetry = (ROOT / 'src/TelemetryManager.cpp').read_text(encoding='utf-8')
for token in ('trueValues', 'falseValues', 'normalize(', 'updatedAnyState'):
    if token not in telemetry:
        errors.append(f'telemetry normalisation missing {token}')
proxy = (ROOT / 'tools/fs7_proxy.py').read_text(encoding='utf-8')
for token in ('responseBody', 'captureMs', 'requestCaptureMs', 'responseCaptureMs', 'durationMs',
              'eventId', 'requestCookieNames', 'responseSetCookieNames', 'authorizationPresent'):
    if token not in proxy:
        errors.append(f'discovery proxy must log {token}')
if not (ROOT / 'tools/fs7_feedback_analyze.py').exists():
    errors.append('missing feedback analyzer')
if 'cameraState_.json(store_.config().telemetryFreshMs)' not in web or 'fullExportJson(true)' not in web:
    errors.append('web API must expose state freshness and always redact configuration exports')
if 'board_build.partitions = min_spiffs.csv' not in (ROOT / 'platformio.ini').read_text(encoding='utf-8'):
    errors.append('reviewed build must retain safe application headroom with min_spiffs.csv')
if 'bblanchon/ArduinoJson@7.4.3' not in (ROOT / 'platformio.ini').read_text(encoding='utf-8'):
    errors.append('ArduinoJson must be pinned to 7.4.3')
if 'Access-Control-Allow-Origin' in web:
    errors.append('management API must not enable wildcard CORS')
arbiter = (ROOT / 'include/OperationArbiter.h').read_text(encoding='utf-8')
for token in ('OperationOwner', 'StopReplay', 'Sequence8', 'automationActive'):
    if token not in arbiter:
        errors.append(f'central operation arbiter missing {token}')
if 'stop_replay is asynchronous and cannot be used as a sequence step' not in config:
    errors.append('sequence config must reject asynchronous stop_replay steps')

# VSM production transport is deliberately undecided. Legacy REST files must not sit at vsm root.
if (ROOT / 'vsm/FS7-Bridge-GenericIO-SimpleREST.json').exists():
    errors.append('Generic REST VSM definition must remain under vsm/legacy, not production root')
if not (ROOT / 'vsm/legacy/FS7-Bridge-GenericIO-SimpleREST-v0.1.1.json').exists():
    errors.append('legacy Generic REST reference is missing')

# Embedded JavaScript syntax.
pages = (ROOT / 'src/WebPages.h').read_text(encoding='utf-8')
for unsafe in ('${x.value}', '${x.name}', '${x.phase}', '${x.lastError}'):
    if unsafe in pages:
        errors.append(f'unescaped dynamic HTML token remains: {unsafe}')
if 'n.ssid' in pages and 'list.replaceChildren()' not in pages:
    errors.append('Wi-Fi scan SSIDs must be rendered with DOM textContent')
scripts = re.findall(r'<script>(.*?)</script>', pages, re.S)
if len(scripts) < 7:
    errors.append(f'expected at least 7 embedded pages/scripts, found {len(scripts)}')
for i, script in enumerate(scripts, 1):
    with tempfile.NamedTemporaryFile('w', suffix='.js', delete=False, encoding='utf-8') as f:
        f.write(script)
        name = f.name
    try:
        r = subprocess.run(['node', '--check', name], capture_output=True, text=True)
        if r.returncode:
            errors.append(f'embedded JavaScript #{i} syntax: {r.stderr.strip()}')
    finally:
        pathlib.Path(name).unlink(missing_ok=True)

# Core file inventory.
for rel in [
    'platformio.ini', 'README.md', 'docs/INTEGRATION.md', 'src/main.cpp', 'src/SonyRemote.cpp',
    'src/CameraState.cpp', 'src/TelemetryManager.cpp', 'src/SequenceEngine.cpp',
    'src/VsmGeneric.cpp', 'tools/fs7_proxy.py', 'docs/FIRST_FLASH.md'
]:
    if not (ROOT / rel).exists():
        errors.append(f'missing {rel}')

if errors:
    print('VALIDATION FAILED')
    for e in errors:
        print(' -', e)
    sys.exit(1)

print(
    f'VALIDATION OK: {command_count} commands, {state_count} camera-state fields, '
    'state-driven replay, 8-slot sequence engine, generic control model, JSON/Python/embedded JS parsed.'
)
