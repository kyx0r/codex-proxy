#!/bin/sh
# Drive nextvi against the proxy's Anthropic backend on :8081.
# Keystrokes are piped in with idle gaps: the ex tool only runs when the
# input loop is free, and the request round-trips while nextvi waits.
cd "$(dirname "$0")"
PORT=${PORT:-8081}
MODEL=${MODEL:-claude-haiku-4-5}
PROMPT=${PROMPT:-MUST call the ex tool with command "p 1", then say what it printed}
./codex-proxy serve --backend anthropic --port "$PORT" --host 127.0.0.1 \
  --model "$MODEL" --reasoning-effort low >/tmp/codex-proxy-test.log 2>&1 &
proxy=$!
trap 'kill $proxy 2>/dev/null' EXIT INT TERM
sleep 1
{ printf 'a %s\n\033' "$PROMPT"; sleep 25; printf '\003'; sleep 1; printf ':q!\r'; sleep 1; } \
  | ./vi -e
#!/bin/sh


# ESC submits the prompt; the blocking :!sleep holds the editor open
# until the agent's reply lands. Ctrl-C ends the run.
EXINIT=$(printf 'a hello, what model are you?\n\033:!sleep 20\r\003:q') ./vi -e
