"""Build and run tests/bug_report_check.c: the game's side of a bug report without the game. runtime/bug_report.c with
save_share.c, save_zip.c, fs_host.c, save_summary.c and cfg.c, the rest of the runtime stubbed:

- a crash: a run's fatal error leaves a note in the log folder; the next start finds it, offers the report, and the
  report carries that run's log and its crash report from the last FATAL line on; a start after that finds none
- the bundle: the shared save's zip (launcher/saves.py check_manifest takes its manifest) with report.json, both logs,
  crash.txt, nx2navi.ini, settings.txt, system.txt, state.txt (the game thread's answer: area, age, time of day),
  frame.png, perf.txt (the last 30 perf lines) and note.txt; a preview writes nothing
- left out: report_key from nx2navi.ini and from the environment, a co-op key, a token and a password, in every member
- sending through curl to a receiver played by this script: no report_url, no curl, nobody listening, a refusal, an
  answer that is no receiver's, and a sent report (the key in its header, the zip as its body); the zip stays in every
  case and the options file with the key never does

Usage: bug_report_check.py   (Python 3.12 or later, clang, curl; no game files)
"""
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
ACCOUNT = '5555666677778888' + '1111222233334444'   # tests/bug_report_check.c account_uid, as fs_host.c names it
SECRETS = [b'secret-ini-key-456', b'secret-env-key-123', b'coop-secret-789', b'tok-abc-0001', b'hunter2-pw']

from launcher import saves  # noqa: E402


def main():
    work = ROOT / 'build' / 'test' / 'bug_report'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    exe = work / 'bug_report_check'
    rt = ROOT / 'runtime'
    cc = os.environ.get('CC') or shutil.which('clang') or 'clang'
    subprocess.check_call([cc, '-std=gnu11', '-O1', '-g', '-w', '-I', str(rt), str(ROOT / 'tests' / 'bug_report_check.c'),
                           *(str(rt / f) for f in ('bug_report.c', 'save_share.c', 'save_zip.c', 'fs_host.c',
                                                   'save_summary.c', 'cfg.c', 'platform/linux.c', 'platform/common.c')),
                           '-o', str(exe), '-lpthread', '-ldl', '-lm', '-lSDL3'])
    bad = 0

    def check(ok, what):
        nonlocal bad
        print(('ok   ' if ok else 'FAIL ') + what)
        bad += not ok

    logs = work / 'logs'
    logs.mkdir()
    ini = work / 'nx2navi.ini'   # next to the executable, where save_share_add_settings reads it
    ini.write_text('[frame rate]\nfps_limit = 120\n[bug report]\nreport_url = http://127.0.0.1:9/report\n'
                   'report_key = secret-ini-key-456\ncoop_room_key = coop-secret-789\nupload_token = tok-abc-0001\n'
                   '# report_key = a comment stays\n', encoding='utf-8')
    env = dict(os.environ, BUG_REPORT_CHECK_INI=str(ini), BUG_REPORT_CHECK_LOGDIR=str(logs),
               NX2NAVI_REPORT_KEY='secret-env-key-123', NX2NAVI_SERVER_PASSWORD='hunter2-pw')

    def run(*args, log=None, extra=None):
        e = dict(env, **(extra or {}))
        if log:
            e['BUG_REPORT_CHECK_LOG'] = str(log)
        r = subprocess.run([str(exe), *map(str, args)], env=e, capture_output=True, text=True, timeout=120)
        check(r.returncode == 0, f'{args[0]} exited with {r.returncode} {r.stderr[-300:] if r.returncode else ""}')
        return r.stdout

    # a run that ends in a fatal error: its log, then its note
    crashed = logs / 'run1.log'
    crashed.write_text('[host] boot\n' + ''.join(f'[host] line {i}\n' for i in range(200)) +
                       'FATAL: rt_dispatch: no function at guest pc 0x7100001234\n  x0  0000000000000001\n'
                       '  guest frame  0: 0x71000abcd (main+0xabcd)\n', encoding='utf-8')
    check(run('crash', log=crashed).strip() == 'noted', 'the fatal run notes itself')
    check((logs / 'crash-pending.txt').read_text() == f'log {crashed}\n', 'the note names its log')

    profile = work / 'profile'
    acct = profile / 'save' / ACCOUNT
    for name, data in {'account.sav': b'a', 'option.sav': b'o', 'slot_00/progress.sav': b'p' * 900}.items():
        (acct / name).parent.mkdir(parents=True, exist_ok=True)
        (acct / name).write_bytes(data)
    now = logs / 'run2.log'
    now.write_text('[host] log: run 2\n' + ''.join(f'[host] perf: frame {i * 60}: interval avg 16.6 ms\n' for i in range(35)) +
                   '[host] menu: open\n', encoding='utf-8')
    png = work / 'frame.png'
    png.write_bytes(b'\x89PNG\r\n\x1a\n' + b'picture' * 10)
    out = work / 'reports'
    text = run('make', profile, out, png, 'The bridge did not rise at 18:20', log=now)
    check(f'crash 1 fatal FATAL: rt_dispatch: no function at guest pc 0x7100001234' in text, 'the next start offers the crash')
    check(not (logs / 'crash-pending.txt').exists(), 'the note is taken')
    m = re.search(r'preview 1 bytes (\d+) path \[\]', text)
    check(m is not None and int(m.group(1)) > 0, 'a preview is made in memory')
    m = re.search(r'result 1 path (.+)', text)
    check(m is not None, 'the report is written')
    zips = sorted(out.glob('*.zip'))
    check(len(zips) == 1 and m and Path(m.group(1)) == zips[0], f'only the report is in the folder: {zips}')
    check(re.fullmatch(r'NX2navi-report-\d{4}-\d\d-\d\d_\d\d-\d\d-\d\d-crash\.zip', zips[0].name) is not None,
          f'its name {zips[0].name}')
    summary = text.split('summary:\n', 1)[-1]
    for want in ('NX2navi 9.9.9-check, commit 0123456789ab-modified, game 1.1.1', 'GPU: Check GPU',
                 'Where: in play, area 15 (Hyrule Field), child, 18:20, at 12.5 3.2 -7.8',
                 'The last run\'s fatal error: FATAL: rt_dispatch', 'bug-report/crash.txt', 'save/  3 files'):
        check(want in summary, f'the summary shows "{want}"')
    with zipfile.ZipFile(zips[0]) as z:
        check(z.testzip() is None, 'the zip\'s CRCs')
        names = set(z.namelist())
        want = {'manifest.json', 'save/account.sav', 'save/option.sav', 'save/slot_00/progress.sav'} | {
            'bug-report/' + n for n in ('report.json', 'nx2navi.log', 'nx2navi.previous.log', 'crash.txt', 'nx2navi.ini',
                                        'settings.txt', 'system.txt', 'state.txt', 'frame.png', 'perf.txt', 'note.txt')}
        check(names == want, f'the members: {sorted(names ^ want)} differ')
        member = {n: z.read(n) for n in names}
    for s in SECRETS:
        check(not any(s in data for data in member.values()), f'{s.decode()} is in no member')
    check(b'report_key = (left out)' in member['bug-report/nx2navi.ini'] and
          b'coop_room_key = (left out)' in member['bug-report/nx2navi.ini'] and
          b'# report_key = a comment stays' in member['bug-report/nx2navi.ini'], 'nx2navi.ini keeps its keys, not values')
    man = json.loads(member['manifest.json'])
    saves.check_manifest(man)
    check((man['format'], man['bug_report'], man['report'], man['commit'], man['files']) ==
          ('nx2navi-save', True, 'crash', '0123456789ab-modified', 3), f'the manifest {man}')
    rep = json.loads(member['bug-report/report.json'])
    check(rep['kind'] == 'crash' and rep['fatal'] == 'FATAL: rt_dispatch: no function at guest pc 0x7100001234',
          f'report.json kind and fatal: {rep["kind"]} {rep["fatal"]}')
    check(rep['note'] == 'The bridge did not rise at 18:20' and rep['os'] == 'CheckOS 1.0' and
          rep['driver'] == 'CheckDriver 580.1' and rep['game_version'] == '1.1.1', 'report.json system and note')
    check('fps_limit = 120' in ' '.join(rep['settings_changed']) and
          not any('KEY' in s.upper() or 'PASSWORD' in s.upper() or 'TOKEN' in s.upper() for s in rep['settings_changed']),
          f'report.json settings changed: {rep["settings_changed"]}')
    check({f['name'] for f in rep['files']} >= {'bug-report/crash.txt', 'bug-report/frame.png'}, 'report.json files')
    crash_txt = member['bug-report/crash.txt'].decode()
    check(crash_txt.startswith('FATAL: rt_dispatch') and 'guest frame  0' in crash_txt, 'crash.txt from the FATAL line on')
    check(member['bug-report/nx2navi.previous.log'] == crashed.read_bytes(), 'the crashed run\'s log')
    check(member['bug-report/nx2navi.log'] == now.read_bytes(), 'this run\'s log')
    perf = member['bug-report/perf.txt'].decode().splitlines()
    check(len(perf) == 30 and perf[0].endswith('frame 300: interval avg 16.6 ms') and 'frame 2040' in perf[-1],
          f'perf.txt: the last 30 perf lines ({len(perf)})')
    state = member['bug-report/state.txt'].decode()
    for want in ('In play: yes, the player at 12.5 3.2 -7.8', 'Area: index 15 (Hyrule Field)', 'Age: child',
                 'Time of day: 18:20', 'Game time since the save loaded: 754 s', 'Save file in use: 1'):
        check(want in state, f'state.txt: "{want}"')
    settings = member['bug-report/settings.txt'].decode()
    check('fps_limit = 120   (default ' in settings and 'NX2NAVI_REPORT_KEY=(left out) (environment)' in settings and
          'coop_room_key = (left out)' in settings and 'upload_token = (left out)' in settings, 'settings.txt')
    check(member['bug-report/frame.png'] == png.read_bytes(), 'frame.png')
    check(member['bug-report/note.txt'] == b'The bridge did not rise at 18:20', 'note.txt')

    # the start after that: no note, a plain report
    text = run('make', profile, out, '', log=now)
    check('crash 0 ' in text and '-crash' not in text.split('result 1 path ')[-1].split('\n')[0], 'a start after: no crash')

    # sending
    zip_path = zips[0]
    seen = {}

    class Fake(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_POST(self):
            n = int(self.headers.get('Content-Length', 0))
            seen.update(path=self.path, key=self.headers.get('X-Report-Key'), body=self.rfile.read(n),
                        type=self.headers.get('Content-Type'))
            mode = self.path.strip('/')
            status, body, kind = {
                'refuse': (401, b'{"error": "wrong report_key"}', 'application/json'),
                'html': (200, b'<html>a login page</html>', 'text/html'),
            }.get(mode, (201, b'{"issue_url": "https://github.com/mooki000/NX2navi/issues/77", "issue": 77}',
                         'application/json'))
            self.send_response(status)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    httpd = ThreadingHTTPServer(('127.0.0.1', 0), Fake)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    port = httpd.server_address[1]

    def send(url, extra=None):
        e = dict(extra or {})
        if url is not None:
            e['NX2NAVI_REPORT_URL'] = url
        out = run('send', zip_path, extra=e).strip()
        left = [p.name for p in zip_path.parent.iterdir() if p.suffix in ('.curl', '.answer', '.error')]
        check(zip_path.is_file() and not left, f'after "{out[:60]}": the zip stays, nothing else ({left})')
        return out

    ini.write_text(ini.read_text().replace('report_url = http://127.0.0.1:9/report\n', ''))
    check(send(None).startswith('send 1 answer report_url is not set'), 'no report_url: not sent')
    check(send('ftp://x/').startswith('send 1 '), 'no http address: not sent')
    empty = work / 'empty-bin'
    empty.mkdir()
    check(send(f'http://127.0.0.1:{port}/', {'PATH': str(empty)}).startswith('send 2 answer curl could not be started'),
          'no curl: says so')
    closed = ThreadingHTTPServer(('127.0.0.1', 0), Fake)
    closed_port = closed.server_address[1]
    closed.server_close()
    check(send(f'http://127.0.0.1:{closed_port}/report').startswith(f'send 3 answer http://127.0.0.1:{closed_port}/report '
                                                                   'did not answer (curl exit code 7'), 'nobody listening')
    check(send(f'http://127.0.0.1:{port}/refuse') == 'send 4 answer the receiver refused it: wrong report_key',
          'a refusal')
    check(send(f'http://127.0.0.1:{port}/html').startswith('send 5 answer'), 'an answer that is no receiver\'s')
    out_line = send(f'http://127.0.0.1:{port}/report')
    check(out_line == 'send 0 answer https://github.com/mooki000/NX2navi/issues/77', f'sent: {out_line}')
    check(seen.get('key') == 'secret-env-key-123' and seen.get('body') == zip_path.read_bytes() and
          seen.get('type') == 'application/zip', 'the receiver got the key (the environment wins) and the zip')
    httpd.shutdown()
    print(f'bug_report_check: {"all passed" if not bad else f"{bad} failed"}')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
