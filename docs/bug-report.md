# Bug reports from the game

The settings menu's **Saves** tab has a **Bug report** group with one row, **Send a bug report**. It makes a report
of this moment (the log, the settings, the system, where the player is, the save in use and a picture of the frame),
shows everything that goes before anything goes, lets the player write what happened, and on **Send** uploads it to a
report receiver, which files it as a GitHub issue and answers with the issue's address; the menu shows that address.
After a run that ended in a fatal error or a crash, the next start opens the menu on that run's report by itself.

The game never holds a GitHub token. It uploads to a receiver (`tools/report_server.py`), and only the receiver has
the login that files issues.

The game does not wait for it: the picture costs one frame drawn without the menu and written to disk (56 ms instead
of about 24 in a test at 1080p), and gathering (about a fifth of a second, most of it packing the picture) and
sending run on threads of their own.

## Turning it on

In `nx2navi.ini` (or as `NX2NAVI_REPORT_URL` and `NX2NAVI_REPORT_KEY` in the environment):

```ini
[bug report]
report_url = http://127.0.0.1:8787/report
report_key = <the receiver's key>
```

## Turning it off

Leave `report_url` unset (the default). Then nothing is ever sent: the window's Send button reads **Save the report**
and only writes the zip into the profile's `reports/` folder, and the menu says where. The start after a fatal error
still offers that run's report; **Not now** closes it, and nothing is written.

## What is sent

One zip, the same format as the Saves tab's shared save (`manifest.json` with `"bug_report": true`, `"report"` and
`"commit"`), so the launcher's import checks accept it. The menu lists each member with its size before Send.

| member | what |
|---|---|
| `manifest.json` | the shared save's manifest, plus `report` (`report` or `crash`) and `commit` |
| `save/...` | the save the game would load next, as Export takes it (none when the game has not saved yet) |
| `bug-report/report.json` | what the receiver writes the issue from: kind, versions, commit, OS, CPU, RAM, GPU, driver, where the player is, the fatal line, the note, the settings changed, the files |
| `bug-report/nx2navi.log` | this run's log (its last 8 MiB) |
| `bug-report/nx2navi.previous.log`, `bug-report/crash.txt` | after a fatal error or crash: that run's log and its crash report (the log from its last `FATAL:` line on) |
| `bug-report/nx2navi.ini` | the settings file, secret values replaced by `(left out)` |
| `bug-report/settings.txt` | the settings in effect from the file and the environment, each marked when it differs from its default |
| `bug-report/system.txt` | OS, CPU, memory, GPU and driver |
| `bug-report/state.txt` | where the player is: in play or not, the position, on game 1.1.1 the area index, child or adult and the time of day; the game time since the save loaded; the save file in use and its last save |
| `bug-report/frame.png` | the frame under the menu (not after a crash: a new start shows nothing of it) |
| `bug-report/perf.txt` | the last 30 performance lines, when `NX2NAVI_TRACE` has `perf` |
| `bug-report/note.txt` | what the player wrote |

Every report is also kept on the player's machine, sent or not, as `<profile>/reports/NX2navi-report-<date>_<time>[-crash].zip`.

## What is never sent

- `report_key`, any co-op key setting, and any setting whose name holds secret, token, password or passphrase: the
  zip's `nx2navi.ini` keeps the line with the value `(left out)`, `settings.txt` and `report.json` leave the value
  out, and so does the system information. The key travels only to `report_url`, in a request header; curl reads it
  from a file next to the zip that is deleted as soon as curl ends, so it is never on a command line.
- Key files in the save folder (as Export already leaves out).
- No serial numbers, MAC or IP addresses. The receiver sees the sender's address for its rate limit and logs it once.

What a report can still show: absolute paths in the logs and settings (they hold the user name), the whole save in
use, whatever is on screen under the menu, and the note. The issue shows the log's last 40 lines and the note, so a
public issue shows them too; the save and the picture stay on the receiver's disk.

## Where it goes

The game sends `POST <report_url>` with the zip as the body and the key in `X-Report-Key`, through `curl` (on Linux
and on Windows 10 1803 and later, which has `System32\curl.exe`). The receiver checks the key, keeps the zip under
`<store>/<UTC time>-<hash>/bundle.zip`, unpacks it beside it into `files/`, writes the issue text into `issue.md`,
files the issue on the repository (by default `mooki000/NX2navi`) and answers with its address.

The issue, for a reader:

- **Title**: `Crash: <the fatal line>` after a crash, `Report: <the player's note>` with a note, else
  `Report from the game: <where>`.
- **Labels**: `from-game`, then `crash` or `report`, plus any in `REPORT_LABELS`.
- **Body**: the note as a quote; a table (NX2navi version and commit, game, system, CPU, RAM, GPU, driver, where,
  settings changed from their defaults, when, save files); the fatal line; what the crash report says (its first 60
  lines); the last 40 lines of the log (the crashed run's after a crash); the last performance lines; the bundle's
  files with sizes; and where the bundle is kept on the receiver.
- With `REPORT_AGENT_CMD=tools/report_context.sh`, a comment follows: the `claude` command line tool, allowed to read
  the unpacked bundle and the source and to search the repository's open issues, answers what the log shows, the
  likely place in the code, and similar open issues. Without `claude` (or not logged in) no comment is posted.

The game's menu then shows "Sent. The issue: <address>", or "Not sent: <why>" (no curl, nobody answering, a wrong
key, something answering that is no receiver) with the path of the kept zip.

## Running the receiver on one's own machine

It needs Python 3 (standard library only) and a GitHub login that may file issues on the repository: `gh auth login`
once, or a token in `GITHUB_TOKEN`.

```sh
REPORT_KEY=<a key of 8 characters or more> python3 tools/report_server.py
```

It listens on `127.0.0.1:8787` and keeps bundles in `~/.local/share/nx2navi-reports`. Settings (environment, or the
matching command line options):

| setting | default | what |
|---|---|---|
| `REPORT_KEY` / `REPORT_KEY_FILE` | (required) | the shared key |
| `REPORT_REPO` | `mooki000/NX2navi` | where the issues go |
| `REPORT_BIND`, `REPORT_PORT` | `127.0.0.1`, `8787` | where it listens |
| `REPORT_STORE` | `~/.local/share/nx2navi-reports` | where bundles are kept |
| `REPORT_LABELS` | none | labels added to each issue (comma separated) |
| `REPORT_MAX_MB` | 64 | the largest upload taken; the size is checked before the body is read |
| `REPORT_RATE`, `REPORT_RATE_ALL` | 6, 60 | reports an hour per client address, and from everyone |
| `REPORT_AGENT_CMD`, `REPORT_AGENT_TIMEOUT` | none, 900 | a command whose output becomes a comment (`tools/report_context.sh`) |
| `REPORT_ARCHIVE_REPO` | none | a private repository (it must exist) that gets a copy of each zip |
| `--no-github` | | store the bundle and write `issue.md` only |

## What a hosted receiver needs

- HTTPS in front: a proxy (Caddy or nginx with a certificate) passing to the receiver bound to `127.0.0.1`, and
  `REPORT_TRUST_PROXY=1` so the rate limit counts the address the proxy names in `X-Forwarded-For`. The players'
  `report_url` becomes `https://<host>/report`; curl checks the certificate with the system's store.
- A token in `GITHUB_TOKEN` instead of a `gh` login: a fine-grained token with Issues: write on the repository (and
  Contents: write on the archive repository, if used).
- A long random `REPORT_KEY` given to the testers. One key is shared by all of them: it keeps strangers out, it does
  not tell testers apart.
- Disk for the store; each report is a few MB (the picture and the save are most of it).

## Testing it without a player

- `python3 tests/bug_report_check.py`: the bundle, the secrets left out, the crash note and the next start's offer,
  and the sender's outcomes against a local stand-in receiver.
- `python3 tests/report_server_test.py`: the receiver's guards and issue text against a fake GitHub, and the game's
  sender uploading to it.
- In the game, the menu test hook (`NX2NAVI_MENU_TEST_SET`) drives it: `REPORT_OPEN=1` opens the window as the row
  does, `REPORT_NOTE=<text>` writes the note, `REPORT_SEND=1` presses Send once the list is shown, `REPORT_CANCEL=1`
  presses Not now, and `CRASH=fatal` (or `segv`) ends the run with a fatal error (or a crash) on purpose.
