"""tools/report_server.py against a fake GitHub API: the key, the size cap (with and without curl's Expect:
100-continue), the rate limit, hostile member names, the issue's text and labels, GitHub refusing, the agent's
comment, the archive copy, and the game's own sender (tests/bug_report_check.c, when tests/bug_report_check.py has
built it) uploading to the receiver.

Usage: python3 tests/report_server_test.py   (standard library; curl for two tests)
"""
import http.client
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import report_server  # noqa: E402

KEY = 'test-key-0123456789'


class FakeGitHub:
    """Records every call; answers like GitHub's API, or with 422 while refuse is set."""

    def __init__(self):
        self.calls, self.refuse = [], False
        fake = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def handle_one(self, method):
                n = int(self.headers.get('Content-Length', 0))
                body = json.loads(self.rfile.read(n) or b'{}')
                fake.calls.append((method, self.path, body, self.headers.get('Authorization')))
                if fake.refuse:
                    status, out = 422, {'message': 'Validation Failed'}
                elif self.path.endswith('/issues'):
                    status, out = 201, {'number': 41, 'html_url': 'https://github.com/o/r/issues/41'}
                elif '/contents/' in self.path:
                    status, out = 201, {'content': {'html_url': 'https://github.com/o/archive/blob/main/x.zip'}}
                else:
                    status, out = 201, {'id': 1}
                data = json.dumps(out).encode()
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_POST(self):
                self.handle_one('POST')

            def do_PUT(self):
                self.handle_one('PUT')

        self.httpd = ThreadingHTTPServer(('127.0.0.1', 0), H)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.url = f'http://127.0.0.1:{self.httpd.server_address[1]}'


def bundle(kind='crash', note='The bridge stayed down', extra=()):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w') as z:
        z.writestr('manifest.json', '{"format": "nx2navi-save"}')
        z.writestr('save/account.sav', b'a')
        rep = {'kind': kind, 'nx2navi': '0.1.0', 'commit': 'abcdef012345', 'game_version': '1.1.1', 'os': 'Linux 7.2',
               'cpu': 'i9', 'ram': '64 GB', 'gpu': 'RTX 4090', 'driver': 'NVIDIA 580', 'note': note,
               'place': 'in play, area 15 (Hyrule Field), child, 18:20', 'made': '2026-11-10T10:00:00Z',
               'fatal': 'FATAL: rt_dispatch: no function at guest pc 0x71 | x' if kind == 'crash' else '',
               'settings_changed': ['fps_limit = 120'], 'save_files': 1}
        z.writestr('bug-report/report.json', json.dumps(rep))
        z.writestr('bug-report/nx2navi.log', ''.join(f'[host] now {i}\n' for i in range(100)))
        if kind == 'crash':
            z.writestr('bug-report/nx2navi.previous.log', ''.join(f'[host] before {i}\n' for i in range(100)) +
                       'FATAL: rt_dispatch: no function at guest pc 0x71 | x\n  guest frame  0: main+0x10\n```\n')
            z.writestr('bug-report/crash.txt', 'FATAL: rt_dispatch: no function at guest pc 0x71 | x\n'
                                               '  guest frame  0: main+0x10\n')
        for name, data in extra:
            z.writestr(name, data)
    return buf.getvalue()


class ReceiverTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='report-server-'))
        self.gh = FakeGitHub()
        self.start()

    def start(self, rate=50, max_mb=1, agent='', archive=''):
        cfg = report_server.config(['--port', '0', '--store', str(self.tmp / 'store'), '--api', self.gh.url,
                                    '--repo', 'o/r', '--rate', str(rate), '--max-mb', str(max_mb),
                                    '--labels', 'report-test', '--agent-cmd', agent, '--archive-repo', archive])
        cfg.key = KEY
        self.rx = report_server.Receiver(cfg, report_server.GitHub(cfg.api, 'tok', cfg.repo))
        self.httpd = ThreadingHTTPServer(('127.0.0.1', 0), report_server.make_handler(self.rx))
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.port = self.httpd.server_address[1]

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.gh.httpd.shutdown()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def post(self, data, key=KEY, headers=None):
        c = http.client.HTTPConnection('127.0.0.1', self.port, timeout=30)
        h = {'Content-Type': 'application/zip', **({'X-Report-Key': key} if key is not None else {}), **(headers or {})}
        try:
            c.request('POST', '/report', body=data, headers=h)
            r = c.getresponse()
            return r.status, json.loads(r.read() or b'{}')
        except (ConnectionError, BrokenPipeError):
            return 0, {}

    def stored(self):
        return sorted((self.tmp / 'store').glob('*/bundle.zip'))

    def issues(self):
        return [c for c in self.gh.calls if c[1].endswith('/issues')]

    def test_key_refused(self):
        for key in ('wrong-key-000000', '', None):
            status, out = self.post(bundle(), key=key)
            self.assertEqual((status, out.get('error')), (401, 'wrong report_key'))
        self.assertEqual(self.stored(), [])
        self.assertEqual(self.gh.calls, [])

    def test_size_cap(self):
        big = bundle(extra=[('bug-report/big.bin', os.urandom(1200 << 10))])
        status, out = self.post(big)
        self.assertEqual(status, 413)
        self.assertEqual(self.stored(), [])

    @unittest.skipUnless(shutil.which('curl'), 'needs curl')
    def test_size_cap_before_the_body(self):
        # curl sends "Expect: 100-continue" before a large body, as the game's curl does: the refusal comes first
        f = self.tmp / 'big.zip'
        f.write_bytes(bundle(extra=[('bug-report/big.bin', os.urandom(3 << 20))]))
        r = subprocess.run(['curl', '-s', '-S', '-H', f'X-Report-Key: {KEY}', '-H', 'Content-Type: application/zip',
                            '--data-binary', f'@{f}', f'http://127.0.0.1:{self.port}/report'],
                           capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('larger than the 1 MB', json.loads(r.stdout)['error'])

    def test_rate_limit(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.start(rate=2)
        got = [self.post(bundle(kind='report'))[0] for _ in range(3)]
        self.assertEqual(got, [201, 201, 429])
        self.assertEqual(self.post(bundle(), key='wrong-key-000000')[0], 429)   # attempts count, a wrong key's too
        self.assertEqual(len(self.stored()), 2)

    def test_hostile_names(self):
        buf = io.BytesIO(bundle())
        with zipfile.ZipFile(buf, 'a') as z:
            z.writestr('../evil.txt', 'x')
            z.writestr('/abs.txt', 'x')
            z.writestr('a\\..\\..\\b.txt', 'x')
            z.writestr('C:/windows.txt', 'x')
            z.writestr('bug-report/../../up.txt', 'x')
            link = zipfile.ZipInfo('bug-report/link')
            link.external_attr = (0o120777 << 16)
            z.writestr(link, '/etc/passwd')
            z.writestr('bug-report/odd name$`.txt', 'fine')
        status, out = self.post(buf.getvalue())
        self.assertEqual(status, 201, out)
        folder = Path(out['stored']).parent
        self.assertEqual(folder.parent, self.tmp / 'store')
        everything = {p.relative_to(self.tmp).as_posix() for p in self.tmp.rglob('*')}
        self.assertFalse([p for p in everything if 'evil' in p or 'abs.txt' in p or 'up.txt' in p or 'windows' in p
                          or p.endswith('b.txt') or p.endswith('/link')], everything)
        self.assertTrue((folder / 'files/bug-report/odd name__.txt').is_file())
        body = self.issues()[0][2]['body']
        for name in ('../evil.txt', '/abs.txt', 'bug-report/link'):
            self.assertIn(f'`{name}`', body)

    def test_issue_text(self):
        status, out = self.post(bundle(note='@someone it broke'))
        self.assertEqual((status, out['issue_url'], out['issue']), (201, 'https://github.com/o/r/issues/41', 41))
        method, path, issue, auth = self.issues()[0]
        self.assertEqual((method, path, auth), ('POST', '/repos/o/r/issues', 'Bearer tok'))
        self.assertEqual(issue['title'], 'Crash: rt_dispatch: no function at guest pc 0x71 | x')
        self.assertEqual(issue['labels'], ['from-game', 'crash', 'report-test'])
        body = issue['body']
        for want in ('| NX2navi | 0.1.0 (commit abcdef012345) |', '| GPU | RTX 4090 |', '| Driver | NVIDIA 580 |',
                     '| Where | in play, area 15 (Hyrule Field), child, 18:20 |', '| Settings changed | 1: fps_limit = 120 |',
                     '### The fatal line', 'FATAL: rt_dispatch: no function at guest pc 0x71 \\| x' if False else
                     'FATAL: rt_dispatch: no function at guest pc 0x71 | x', '### What the crash report says',
                     '### The last 40 lines of nx2navi.previous.log', '[host] before 99', '### The files in the bundle',
                     '- `bug-report/crash.txt`', '### Where the bundle is', out['stored'], '> @\u200bsomeone it broke'):
            self.assertIn(want, body)
        self.assertNotIn('[host] before 59\n', body)   # the last 40 lines only
        self.assertIn('````text', body)   # the log's own ``` cannot close the block
        self.assertTrue((Path(out['stored']).parent / 'issue.md').is_file())

    def test_plain_report(self):
        status, out = self.post(bundle(kind='report', note='Navi said nothing'))
        issue = self.issues()[0][2]
        self.assertEqual((issue['title'], issue['labels']), ('Report: Navi said nothing', ['from-game', 'report', 'report-test']))
        self.assertIn('### The last 40 lines of nx2navi.log', issue['body'])

    def test_github_refuses(self):
        self.gh.refuse = True
        status, out = self.post(bundle())
        self.assertEqual(status, 502)
        self.assertIn('GitHub refused the issue', out['error'])
        self.assertTrue(Path(out['stored']).is_file())

    def test_not_a_zip(self):
        self.assertEqual(self.post(b'hello')[0], 400)

    def test_agent_and_archive(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        agent = self.tmp / 'agent.sh'
        agent.write_text('#!/bin/sh\necho "context for issue $2 from $(ls "$1")"\n')
        agent.chmod(0o755)
        self.start(agent=str(agent), archive='o/archive')
        status, out = self.post(bundle())
        self.assertEqual(status, 201)
        put = [c for c in self.gh.calls if c[0] == 'PUT']
        self.assertEqual(len(put), 1)
        self.assertRegex(put[0][1], r'^/repos/o/archive/contents/reports/\d{8}-\d{6}-[0-9a-f]{10}\.zip$')
        self.assertIn('https://github.com/o/archive/blob/main/x.zip', self.issues()[0][2]['body'])
        for _ in range(100):
            comments = [c for c in self.gh.calls if c[1].endswith('/issues/41/comments')]
            if comments:
                break
            threading.Event().wait(0.05)
        self.assertEqual(comments[0][2]['body'], 'context for issue 41 from bug-report\nmanifest.json\nsave')

    def test_game_sender(self):
        exe = ROOT / 'build/test/bug_report/bug_report_check'
        zips = sorted((ROOT / 'build/test/bug_report/reports').glob('*crash.zip'))
        if not exe.is_file() or not zips:
            self.skipTest('run tests/bug_report_check.py first')
        ini = self.tmp / 'nx2navi.ini'
        ini.write_text(f'report_url = http://127.0.0.1:{self.port}/report\nreport_key = {KEY}\n')
        r = subprocess.run([str(exe), 'send', str(zips[0])], env=dict(os.environ, BUG_REPORT_CHECK_INI=str(ini)),
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(r.stdout.strip(), 'send 0 answer https://github.com/o/r/issues/41', r.stderr)
        issue = self.issues()[0][2]
        self.assertTrue(issue['title'].startswith('Crash: rt_dispatch'))
        self.assertIn('| Where | in play, area 15 (Hyrule Field), child, 18:20, at 12.5 3.2 -7.8', issue['body'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
