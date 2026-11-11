#!/usr/bin/env python3
"""The bug report receiver: takes the zip the game's Send a bug report uploads (runtime/bug_report.c), keeps it, and
files a GitHub issue about it, formatted for a developer; the issue's address goes back to the game, which shows it.

    REPORT_KEY=<shared key> python3 tools/report_server.py          # here: 127.0.0.1:8787, the issue on mooki000/NX2navi

The game sends POST /report with the zip as the body and the key in X-Report-Key (nx2navi.ini: report_url =
http://127.0.0.1:8787/report, report_key = the same key). The answer is JSON: {"issue_url", "issue", "stored"}, or
{"error"} with 400 (no zip), 401 (wrong key), 411 / 413 (no length, too large), 429 (too many reports) or 502 (GitHub
refused; the bundle is kept all the same).

The upload is never trusted: its size is capped before it is read, a client sends at most REPORT_RATE reports an hour,
the zip is stored under a name the receiver makes, and only its members whose names are plain relative paths are
unpacked (beside it, in files/), each name cleaned and the total capped; any other member is listed as skipped.

Settings (environment; the command line options override them):
  REPORT_KEY / REPORT_KEY_FILE  the shared key the game sends (required)
  REPORT_REPO           the repository the issues go to (mooki000/NX2navi)
  REPORT_BIND, REPORT_PORT   where it listens (127.0.0.1, 8787). On a server: bind it to 127.0.0.1 behind an HTTPS
                        proxy (Caddy, nginx) and set REPORT_TRUST_PROXY=1, so the rate limit counts the address the
                        proxy names in X-Forwarded-For
  REPORT_STORE          where bundles are kept (~/.local/share/nx2navi-reports)
  REPORT_MAX_MB         the largest upload taken (64)
  REPORT_RATE           reports per client address an hour (6); REPORT_RATE_ALL: from everyone an hour (60)
  REPORT_LABELS         labels added to from-game and crash or report (comma separated)
  GITHUB_TOKEN / GH_TOKEN   the token that files the issue; without one, `gh auth token` of this machine's gh login
  REPORT_API            the GitHub API's address (https://api.github.com; the tests point it at a fake)
  REPORT_ARCHIVE_REPO   optional: a private repository (it must exist) that gets a copy of each zip through the
                        contents API, as reports/<id>.zip; GitHub's issues cannot hold a zip
  REPORT_AGENT_CMD      optional: a command run after the issue exists with the unpacked bundle's folder and the issue
                        number as its last two arguments; what it prints becomes a comment on the issue
                        (tools/report_context.sh asks the claude command line tool for context). REPORT_AGENT_TIMEOUT:
                        its time limit in seconds (900)
"""
import argparse
import base64
import collections
import datetime
import hashlib
import hmac
import io
import json
import os
import re
import shlex
import socket
import stat
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

MAX_MEMBERS = 5000
MAX_UNPACKED = 512 << 20      # all members together, unpacked: a zip bomb stops here
MAX_MEMBER = 128 << 20
BODY_MAX = 60000              # GitHub takes 65536 characters in an issue's body
LOG_LINES = 40
SAFE_PART = re.compile(r'[^A-Za-z0-9._ -]')


def log(msg):
    print(time.strftime('%Y-%m-%d %H:%M:%S ') + msg, file=sys.stderr, flush=True)


# ---- the upload
def safe_name(name):
    """The member's name as a relative path of cleaned parts, or None when it is no plain relative path."""
    if not name or len(name) > 240 or '\\' in name or '\0' in name or name.startswith('/') or ':' in name:
        return None
    parts = name.split('/')
    if name.endswith('/'):
        return None   # a folder entry: folders are made for files
    if len(parts) > 6 or any(p in ('', '.', '..') for p in parts):
        return None
    return '/'.join(SAFE_PART.sub('_', p) for p in parts)


def unpack(data, dest):
    """Unpacks the zip's plain members into dest. Returns (files [(name, bytes)], skipped [(name, why)])."""
    files, skipped = [], []
    total = 0
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        infos = z.infolist()
        if len(infos) > MAX_MEMBERS:
            raise ValueError(f'{len(infos)} members, more than {MAX_MEMBERS}')
        for info in infos:
            name = safe_name(info.filename)
            kind = (info.external_attr >> 16) & 0o170000
            if name is None:
                skipped.append((info.filename, 'not a plain relative path'))
                continue
            if kind == stat.S_IFLNK:
                skipped.append((info.filename, 'a link'))
                continue
            if info.file_size > MAX_MEMBER or total + info.file_size > MAX_UNPACKED:
                skipped.append((info.filename, 'too large'))
                continue
            target = dest / name
            if dest.resolve() not in target.resolve().parents:
                skipped.append((info.filename, 'outside the folder'))
                continue
            try:
                with z.open(info) as src:
                    body = src.read(MAX_MEMBER + 1)
            except (zipfile.BadZipFile, NotImplementedError, RuntimeError, OSError) as e:
                skipped.append((info.filename, f'unreadable ({e})'))
                continue
            if len(body) > MAX_MEMBER:
                skipped.append((info.filename, 'too large'))
                continue
            total += len(body)
            target.parent.mkdir(parents=True, exist_ok=True)
            with open(target, 'xb') as f:
                f.write(body)
            files.append((name, len(body)))
    return files, skipped


def read_text(folder, name, limit=4 << 20):
    p = folder / name
    if not p.is_file():
        return ''
    with open(p, 'rb') as f:
        f.seek(max(0, p.stat().st_size - limit))
        return f.read().decode('utf-8', 'replace')


# ---- the issue
def fence(text):
    """text in a fenced code block whose fence no line of it can close."""
    longest = max((len(m) for m in re.findall(r'`+', text)), default=0)
    f = '`' * max(3, longest + 1)
    return f'{f}text\n{text.rstrip()}\n{f}'


def cell(text):
    return (text or '').replace('|', '\\|').replace('\n', ' ').replace('@', '@\u200b').strip() or '?'


def last_lines(text, n):
    lines = [l for l in text.splitlines() if l.strip()]
    return '\n'.join(lines[-n:])


def issue_text(report, folder, files, skipped, stored, archive):
    """(title, body, kind) of the issue."""
    kind = 'crash' if report.get('kind') == 'crash' else 'report'
    note = (report.get('note') or '').strip()
    fatal = (report.get('fatal') or '').strip()
    log_name = 'bug-report/nx2navi.previous.log' if kind == 'crash' and (folder / 'bug-report/nx2navi.previous.log').is_file() \
        else 'bug-report/nx2navi.log'
    log_text = read_text(folder, log_name)
    if kind == 'crash' and not fatal:
        fatal = next((l for l in reversed(log_text.splitlines()) if l.startswith('FATAL: ')), '')
    if kind == 'crash':
        title = 'Crash: ' + (fatal[len('FATAL: '):] if fatal.startswith('FATAL: ') else fatal or 'a fatal error')
    elif note:
        title = 'Report: ' + note
    else:
        title = 'Report from the game' + (f': {report["place"]}' if report.get('place') else '')
    title = re.sub(r'\s+', ' ', title).strip()
    title = title[:117] + '...' if len(title) > 120 else title

    changed = report.get('settings_changed') or []
    rows = [('NX2navi', f'{report.get("nx2navi", "?")} (commit {report.get("commit", "?")})'),
            ('Game', report.get('game_version', '?')), ('System', report.get('os')), ('CPU', report.get('cpu')),
            ('RAM', report.get('ram')), ('GPU', report.get('gpu')), ('Driver', report.get('driver')),
            ('Where', report.get('place')),
            ('Settings changed', f'{len(changed)}: ' + ', '.join(changed[:25]) + (' ...' if len(changed) > 25 else '')
             if changed else 'none'),
            ('Made', report.get('made')), ('Save files', str(report.get('save_files', '?')))]
    out = [f'Filed by the game\'s Send a bug report ({kind}).', '']
    if note:
        out += ['> ' + cell(note), '']
    out += ['| | |', '|---|---|'] + [f'| {k} | {cell(v)} |' for k, v in rows] + ['']
    if fatal:
        out += ['### The fatal line', fence(fatal), '']
    crash = read_text(folder, 'bug-report/crash.txt', 256 << 10)
    if crash:
        lines = crash.splitlines()
        frames = [l for l in lines if 'lifted function' in l or 'host frame' in l or 'guest frame' in l]
        out += ['### What the crash report says',
                f'{len(lines)} lines; {len(frames)} frames named. The first 60:', fence('\n'.join(lines[:60])), '']
    if log_text:
        out += [f'### The last {LOG_LINES} lines of {log_name.split("/")[-1]}', fence(last_lines(log_text, LOG_LINES)), '']
    perf = read_text(folder, 'bug-report/perf.txt', 64 << 10)
    if perf:
        out += ['### The last performance lines', fence(last_lines(perf, 10)), '']
    out += ['### The files in the bundle'] + [f'- `{n}` ({b} bytes)' for n, b in files]
    if skipped:
        out += ['', 'Skipped (not unpacked):'] + [f'- `{cell(n)}`: {w}' for n, w in skipped[:30]]
    out += ['', '### Where the bundle is',
            f'`{stored}` on {socket.gethostname()} (unpacked beside it in `files/`).']
    if archive:
        out.append(f'A copy: {archive}')
    body = '\n'.join(out)
    if len(body) > BODY_MAX:
        body = body[:BODY_MAX] + '\n\n(cut at GitHub\'s length limit; the bundle has the rest)'
    return title, body, kind


# ---- GitHub
class GitHub:
    def __init__(self, api, token, repo):
        self.api, self.token, self.repo = api.rstrip('/'), token, repo

    def call(self, method, path, payload):
        req = urllib.request.Request(self.api + path, data=json.dumps(payload).encode(), method=method, headers={
            'Authorization': f'Bearer {self.token}', 'Accept': 'application/vnd.github+json',
            'X-GitHub-Api-Version': '2022-11-28', 'Content-Type': 'application/json',
            'User-Agent': 'nx2navi-report-server'})
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return json.loads(r.read() or b'{}')
        except urllib.error.HTTPError as e:
            detail = e.read()[:500].decode('utf-8', 'replace')
            raise RuntimeError(f'GitHub answered {e.code} to {method} {path}: {detail}') from None
        except (urllib.error.URLError, OSError) as e:
            raise RuntimeError(f'GitHub could not be reached: {e}') from None

    def create_issue(self, title, body, labels):
        return self.call('POST', f'/repos/{self.repo}/issues', {'title': title, 'body': body, 'labels': labels})

    def comment(self, number, body):
        return self.call('POST', f'/repos/{self.repo}/issues/{number}/comments', {'body': body})

    def store(self, repo, path, data, message):
        content = base64.b64encode(data).decode()
        return self.call('PUT', f'/repos/{repo}/contents/{path}', {'message': message, 'content': content})


def gh_token():
    for name in ('GITHUB_TOKEN', 'GH_TOKEN'):
        if os.environ.get(name):
            return os.environ[name]
    try:
        r = subprocess.run(['gh', 'auth', 'token'], capture_output=True, text=True, timeout=30)
        return r.stdout.strip() if r.returncode == 0 else ''
    except (OSError, subprocess.SubprocessError):
        return ''


# ---- the server
class Limits:
    def __init__(self, per_client, overall):
        self.per_client, self.overall = per_client, overall
        self.seen = collections.defaultdict(collections.deque)
        self.lock = threading.Lock()

    def take(self, client):
        """1 when client may send now (and counts it), 0 when it or everyone sent too many this hour."""
        now = time.monotonic()
        with self.lock:
            for q in (self.seen[client], self.seen['*']):
                while q and now - q[0] > 3600:
                    q.popleft()
            if len(self.seen[client]) >= self.per_client or len(self.seen['*']) >= self.overall:
                return False
            self.seen[client].append(now)
            self.seen['*'].append(now)
            return True


class Receiver:
    def __init__(self, cfg, github):
        self.cfg, self.github = cfg, github
        self.limits = Limits(cfg.rate, cfg.rate_all)
        self.store = Path(cfg.store).expanduser()
        self.store.mkdir(parents=True, exist_ok=True)

    def handle(self, data):
        """(status, answer) for an upload that passed the key, size and rate checks."""
        if not zipfile.is_zipfile(io.BytesIO(data)):
            return 400, {'error': 'the upload is no zip'}
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S')
        base = f'{stamp}-{hashlib.sha256(data).hexdigest()[:10]}'
        for n in range(1, 1000):   # the same zip twice in a second gets a folder of its own
            rid = base if n == 1 else f'{base}-{n}'
            folder = self.store / rid
            try:
                folder.mkdir()
                break
            except FileExistsError:
                continue
        else:
            return 500, {'error': 'the receiver has no free folder for the report'}
        stored = folder / 'bundle.zip'
        stored.write_bytes(data)
        files_dir = folder / 'files'
        files_dir.mkdir()
        try:
            files, skipped = unpack(data, files_dir)
        except (zipfile.BadZipFile, ValueError, OSError) as e:
            return 400, {'error': f'the zip could not be read: {e}', 'stored': str(stored)}
        try:
            report = json.loads(read_text(files_dir, 'bug-report/report.json', 1 << 20) or '{}')
            report = report if isinstance(report, dict) else {}
        except ValueError:
            report = {}
        archive = ''
        if self.cfg.archive_repo and self.github:
            try:
                r = self.github.store(self.cfg.archive_repo, f'reports/{rid}.zip', data, f'Bug report {rid}')
                archive = (r.get('content') or {}).get('html_url') or f'{self.cfg.archive_repo}: reports/{rid}.zip'
            except RuntimeError as e:
                log(f'report {rid}: the copy to {self.cfg.archive_repo} failed: {e}')
        title, body, kind = issue_text(report, files_dir, files, skipped, stored, archive)
        (folder / 'issue.md').write_text(f'# {title}\n\n{body}\n', encoding='utf-8')
        labels = ['from-game', kind] + [l.strip() for l in self.cfg.labels.split(',') if l.strip()]
        if not self.github:
            log(f'report {rid}: stored at {stored}; no GitHub token, so no issue (the text is in issue.md)')
            return 201, {'issue_url': f'file://{folder / "issue.md"}', 'issue': 0, 'stored': str(stored)}
        try:
            issue = self.github.create_issue(title, body, labels)
        except RuntimeError as e:
            log(f'report {rid}: stored at {stored}; the issue failed: {e}')
            return 502, {'error': f'the report is kept on the receiver, but GitHub refused the issue ({e})'[:400],
                         'stored': str(stored)}
        url, number = issue.get('html_url', ''), issue.get('number', 0)
        (folder / 'issue.json').write_text(json.dumps({'url': url, 'number': number}) + '\n', encoding='utf-8')
        log(f'report {rid}: {kind}, {len(data)} bytes, issue {url}')
        if self.cfg.agent_cmd and number:
            threading.Thread(target=self.agent, args=(files_dir, number), daemon=True).start()
        return 201, {'issue_url': url, 'issue': number, 'stored': str(stored)}

    def agent(self, folder, number):
        cmd = shlex.split(self.cfg.agent_cmd) + [str(folder), str(number)]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=self.cfg.agent_timeout)
        except (OSError, subprocess.SubprocessError) as e:
            log(f'issue {number}: REPORT_AGENT_CMD failed: {e}')
            return
        if r.stderr.strip():
            log(f'issue {number}: REPORT_AGENT_CMD said: {r.stderr.strip()[-300:]}')
        text = r.stdout.strip()
        if r.returncode or not text:
            log(f'issue {number}: REPORT_AGENT_CMD gave no comment (exit {r.returncode})')
            return
        try:
            self.github.comment(number, text[:BODY_MAX])
            log(f'issue {number}: the agent\'s context is posted')
        except RuntimeError as e:
            log(f'issue {number}: the comment failed: {e}')


def make_handler(rx):
    cfg = rx.cfg

    class Handler(BaseHTTPRequestHandler):
        server_version = 'nx2navi-report/1'
        # HTTP/1.1 for "Expect: 100-continue", which curl sends before a large body: the checks below answer before
        # any of it is sent, so a refused upload is never read and the game learns why
        protocol_version = 'HTTP/1.1'
        checked = None

        def log_message(self, fmt, *args):   # one line of ours per request instead
            pass

        def answer(self, status, payload):
            data = json.dumps(payload).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(data)
            log(f'{self.client()} {self.command} {self.path}: {status}' + (f' ({payload["error"]})' if 'error' in payload else ''))

        def client(self):
            if cfg.trust_proxy and self.headers.get('X-Forwarded-For'):
                return self.headers['X-Forwarded-For'].split(',')[-1].strip()
            return self.client_address[0]

        def do_GET(self):
            if self.path == '/health':
                return self.answer(200, {'status': 'ok'})
            self.answer(404, {'error': 'POST /report'})

        def refusal(self):
            """(status, answer) when the request is refused on its headers alone, else None. Once per request: the
            rate limit counts every attempt, a wrong key's too."""
            if self.checked is not None:
                return self.checked or None
            self.checked = False
            length = self.headers.get('Content-Length')
            if self.path != '/report':
                self.checked = 404, {'error': 'POST /report'}
            elif not rx.limits.take(self.client()):
                self.checked = 429, {'error': 'too many reports this hour; try again later'}
            elif not hmac.compare_digest((self.headers.get('X-Report-Key') or '').encode(), cfg.key.encode()):
                self.checked = 401, {'error': 'wrong report_key'}
            elif not length or not length.isdigit():
                self.checked = 411, {'error': 'no Content-Length'}
            elif int(length) > cfg.max_mb << 20:
                self.checked = 413, {'error': f'larger than the {cfg.max_mb} MB this receiver takes'}
            return self.checked or None

        def handle_expect_100(self):
            refused = self.refusal() if self.command == 'POST' else None
            if refused:
                self.close_connection = True
                self.answer(*refused)
                return False
            return super().handle_expect_100()

        def do_POST(self):
            self.close_connection = True
            refused = self.refusal()
            if refused:
                return self.answer(*refused)
            length = int(self.headers['Content-Length'])
            data = self.rfile.read(length)
            if len(data) != length:
                return self.answer(400, {'error': 'the upload ended early'})
            try:
                status, payload = rx.handle(data)
            except OSError as e:
                status, payload = 500, {'error': f'the receiver could not store the report: {e}'}
            self.answer(status, payload)

    return Handler


def config(argv=None):
    env = os.environ.get
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bind', default=env('REPORT_BIND', '127.0.0.1'))
    ap.add_argument('--port', type=int, default=int(env('REPORT_PORT', '8787')))
    ap.add_argument('--repo', default=env('REPORT_REPO', 'mooki000/NX2navi'))
    ap.add_argument('--store', default=env('REPORT_STORE', str(Path.home() / '.local/share/nx2navi-reports')))
    ap.add_argument('--max-mb', type=int, default=int(env('REPORT_MAX_MB', '64')))
    ap.add_argument('--rate', type=int, default=int(env('REPORT_RATE', '6')))
    ap.add_argument('--rate-all', type=int, default=int(env('REPORT_RATE_ALL', '60')))
    ap.add_argument('--labels', default=env('REPORT_LABELS', ''))
    ap.add_argument('--api', default=env('REPORT_API', 'https://api.github.com'))
    ap.add_argument('--archive-repo', default=env('REPORT_ARCHIVE_REPO', ''))
    ap.add_argument('--agent-cmd', default=env('REPORT_AGENT_CMD', ''))
    ap.add_argument('--agent-timeout', type=int, default=int(env('REPORT_AGENT_TIMEOUT', '900')))
    ap.add_argument('--trust-proxy', action='store_true', default=env('REPORT_TRUST_PROXY', '0') == '1')
    ap.add_argument('--no-github', action='store_true', help='store and write issue.md only, file no issue')
    cfg = ap.parse_args(argv)
    cfg.key = env('REPORT_KEY', '')
    if not cfg.key and env('REPORT_KEY_FILE'):
        cfg.key = Path(env('REPORT_KEY_FILE')).read_text(encoding='utf-8').strip()
    return cfg


def serve(cfg, ready=None):
    if len(cfg.key) < 8:
        sys.exit('report_server: set REPORT_KEY (or REPORT_KEY_FILE) to a key of 8 characters or more')
    github = None
    if not cfg.no_github:
        token = gh_token()
        if not token:
            sys.exit('report_server: no GitHub token: set GITHUB_TOKEN or log in with `gh auth login` (or --no-github)')
        github = GitHub(cfg.api, token, cfg.repo)
    rx = Receiver(cfg, github)
    httpd = ThreadingHTTPServer((cfg.bind, cfg.port), make_handler(rx))
    log(f'report_server: http://{cfg.bind}:{httpd.server_address[1]}/report -> '
        f'{cfg.repo if github else "no issues"}; bundles in {rx.store}; at most {cfg.max_mb} MB, {cfg.rate} an hour '
        f'a client' + (f'; context by {cfg.agent_cmd}' if cfg.agent_cmd else ''))
    if ready:
        ready(httpd)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()


if __name__ == '__main__':
    serve(config())
