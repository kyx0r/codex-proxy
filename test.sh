#!/bin/sh
# Test multi-turn conversation and tool calling against the proxy's
# Anthropic backend. Run from a terminal to also drive a live nextvi run.
cd "$(dirname "$0")"
PORT=${PORT:-8081}
MODEL=${MODEL:-claude-haiku-4-5}
KEY=${CODEX_PROXY_KEY:-local}
URL=http://127.0.0.1:$PORT/v1/chat/completions
LOGS=${HOME}/.nextvi/logs
FAILED=0

pass() { printf 'ok    %s\n' "$1"; }
fail() { printf 'FAIL  %s\n%s\n' "$1" "${2:+$(printf '%s' "$2" | head -3)}"; FAILED=$((FAILED + 1)); }

probe() {
    curl -s -m 180 -X POST "$URL" -H 'Content-Type: application/json' \
        -H "Authorization: Bearer $KEY" -d "$1"
}

./codex-proxy serve --backend anthropic --port "$PORT" --host 127.0.0.1 \
    --model "$MODEL" >/tmp/codex-proxy-test.log 2>&1 &
proxy=$!
trap 'kill $proxy 2>/dev/null' EXIT INT TERM
waited=0
while [ "$waited" -lt 50 ]; do
    grep -q Listening /tmp/codex-proxy-test.log && break
    sleep 0.2
    waited=$((waited + 1))
done
if ! grep -q Listening /tmp/codex-proxy-test.log; then
    printf 'FAIL  proxy did not start\n'
    tail -3 /tmp/codex-proxy-test.log
    exit 1
fi

TOOLS='[{"type":"function","function":{"name":"ex","description":"Run an ex command in nextvi","parameters":{"type":"object","properties":{"command":{"type":"string","description":"ex command"}},"required":["command"]}}}]'

# 1. A later turn replays the previous assistant reply, which nextvi stores
# with an empty tool_calls array. Relaying that field breaks the request.
R=$(probe '{"model":"'"$MODEL"'","messages":[{"role":"user","content":"Say OK."},{"role":"assistant","content":"OK","tool_calls":[]},{"role":"user","content":"Say OK again."}]}')
case "$R" in
    *'"finish_reason":"stop"'*) pass 'turn 2 replays an assistant tool_calls message' ;;
    *) fail 'turn 2 replays an assistant tool_calls message' "$R" ;;
esac

# 2. Tool arguments and a role tool reply must become tool_use/tool_result.
R=$(probe '{"model":"'"$MODEL"'","messages":[{"role":"system","content":"Call the ex tool when asked to print."},{"role":"user","content":"Print line 1, then read the result."},{"role":"assistant","content":"","tool_calls":[{"id":"call_1","type":"function","function":{"name":"ex","arguments":"{\"command\":\"1p\"}"}}]},{"role":"tool","tool_call_id":"call_1","content":"TOOLMARK alpha"}],"tools":'"$TOOLS"'}')
case "$R" in
    *TOOLMARK*) pass 'tool result is read back on the next turn' ;;
    *) fail 'tool result is read back on the next turn' "$R" ;;
esac

# 3. A forced call has to come back as Chat tool_calls for nextvi to run.
R=$(probe '{"model":"'"$MODEL"'","messages":[{"role":"user","content":"Print line 1 with the ex tool."}],"tools":'"$TOOLS"',"tool_choice":"required"}')
case "$R" in
    *'"tool_calls":[{"id"'*) pass 'forced tool call is relayed as tool_calls' ;;
    *) fail 'forced tool call is relayed as tool_calls' "$R" ;;
esac

# 4. Two tool rounds plus a final answer: the whole multi turn loop.
R=$(probe '{"model":"'"$MODEL"'","messages":[{"role":"user","content":"Print line 1, then line 2, then report both."},{"role":"assistant","content":"","tool_calls":[{"id":"call_1","type":"function","function":{"name":"ex","arguments":"{\"command\":\"1p\"}"}}]},{"role":"tool","tool_call_id":"call_1","content":"LINEONE"},{"role":"assistant","content":"","tool_calls":[{"id":"call_2","type":"function","function":{"name":"ex","arguments":"{\"command\":\"2p\"}"}}]},{"role":"tool","tool_call_id":"call_2","content":"LINETWO"},{"role":"user","content":"Report both lines now."}],"tools":'"$TOOLS"'}')
case "$R" in
    *LINEONE*LINETWO*|*LINETWO*LINEONE*) pass 'two tool rounds feed the final answer' ;;
    *) fail 'two tool rounds feed the final answer' "$R" ;;
esac

# 5. Live nextvi run: prompt, ex tool call, tool result, second turn.
# The editor only drives the agent when it has a terminal.
if [ -t 1 ]; then
    PROMPT='Call the ex tool with command "1p", then quote what it printed.'
    ls -1 "$LOGS" > /tmp/nextvi-before.txt 2>/dev/null
    EXINIT=$(printf 'a %s\n\033!sleep 900 >/dev/null 2>&1\n' "$PROMPT") \
        ./_vi -e >/tmp/nextvi-test.log 2>&1 &
    vi=$!
    printf 'starting nextvi, expect one ex tool call and a second turn\n'

    waited=0
    new=
    while [ "$waited" -lt 300 ]; do
        new=$(ls -1t "$LOGS" 2>/dev/null | grep -Fxvf /tmp/nextvi-before.txt | head -1)
        [ -n "$new" ] && break
        sleep 1
        waited=$((waited + 1))
    done
    if [ -z "$new" ]; then
        fail 'the agent run created no log directory'
    else
        convo=$LOGS/$new/conversation
        printf 'agent log: %s\n' "$convo"
        waited=0
        while [ "$waited" -lt 300 ]; do
            grep -q '^EX ' "$convo" && break
            sleep 1
            waited=$((waited + 1))
        done
        grep -q '^EX ' "$convo" && pass 'agent issued an ex tool call' ||
            fail 'agent issued an ex tool call' "$(tail -6 "$convo")"
        waited=0
        while [ "$waited" -lt 120 ]; do
            grep -q '^RESULT ' "$convo" && break
            sleep 1
            waited=$((waited + 1))
        done
        grep -q '^RESULT ' "$convo" && pass 'tool result was relayed' ||
            fail 'tool result was relayed' "$(tail -6 "$convo")"
        # Turn two replays the assistant tool_calls message, so a request
        # error here means that field was relayed to the gateway.
        waited=0
        while [ "$waited" -lt 300 ]; do
            grep -q '^NOTICE' "$convo" && break
            [ "$(grep -c '^TURN' "$convo")" -ge 2 ] &&
                grep -q '^ASSISTANT' "$convo" && break
            sleep 1
            waited=$((waited + 1))
        done
        grep -q '^NOTICE' "$convo" &&
            fail 'turn 2 failed' "$(grep -A 1 -m 1 '^NOTICE' "$convo")"
        grep -q '^NOTICE' "$convo" ||
            pass 'turn 2 ran on the tool result without an error'
        kill $vi 2>/dev/null
    fi
else
    printf 'skip  live nextvi run (no terminal; run ./test.sh interactively)\n'
fi

[ "$FAILED" -eq 0 ] && printf 'PASS\n'
exit $FAILED
