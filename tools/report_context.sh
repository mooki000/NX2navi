#!/bin/bash
# Context for a bug report's issue, for tools/report_server.py's REPORT_AGENT_CMD: asks the claude command line tool,
# with read-only tools, what the unpacked bundle's log shows, where in this repository the problem likely sits, and
# which open issues of the repository look like it; prints its answer (Markdown) for the receiver to post as a
# comment. Without the claude tool it prints nothing and logs one line, so no comment is posted.
#   REPORT_AGENT_CMD=tools/report_context.sh python3 tools/report_server.py
#   tools/report_context.sh <unpacked bundle folder> <issue number>
# REPORT_SOURCE_DIR: the source the answer points into (default: this repository); REPORT_REPO: the issues' repository.
set -u
bundle=${1:?usage: report_context.sh <unpacked bundle folder> <issue number>}
issue=${2:?usage: report_context.sh <unpacked bundle folder> <issue number>}
source_dir=${REPORT_SOURCE_DIR:-$(cd "$(dirname "$0")/.." && pwd)}
repo=${REPORT_REPO:-mooki000/NX2navi}
if ! command -v claude >/dev/null 2>&1; then
    echo "report_context: the claude command line tool is not installed; no context for issue $issue" >&2
    exit 0
fi
if [ ! -d "$bundle/bug-report" ]; then
    echo "report_context: $bundle holds no bug-report folder; no context for issue $issue" >&2
    exit 0
fi
prompt="You are helping a developer with issue #$issue of the GitHub repository $repo, filed from a bug report of the
game runtime NX2navi. The current folder is the report, unpacked: bug-report/report.json (versions, system, where the
player was, the fatal line), bug-report/nx2navi.log (the run's log), and after a crash bug-report/crash.txt and
bug-report/nx2navi.previous.log; the runtime's source is in $source_dir. Only read files and run the gh commands you are
allowed; change nothing. Answer in Markdown, plainly and briefly, with three sections:
1. What the log shows: the lines that matter (quote them), and what happened just before.
2. The likely place in the code: files and functions in $source_dir (runtime/...), with why. Say when it is a guess.
3. Similar open issues: search the open issues of $repo with gh (gh issue list -R $repo --state open --search ...) and
   list the ones that look like this one by number and title, or say none.
Do not repeat the report's table; do not name people."
cd "$bundle" || exit 0
exec claude -p "$prompt" --output-format text --permission-mode dontAsk --add-dir "$source_dir" \
    --tools "Read,Grep,Glob,Bash" \
    --allowedTools "Read Grep Glob Bash(gh issue list:*) Bash(gh search issues:*)"
