# Standalone Codex subscription adapter

`codex-proxy.c` exposes a local Chat Completions endpoint backed by a ChatGPT
subscription. It performs device-code login, refreshes its own credentials,
and translates HTTP requests directly. It does not execute or require Codex,
Node, Python, or an editor process. Nextvi continues to use HTTP and executes
its own `ex` tools.

This uses the **undocumented Codex subscription backend**, not the public
OpenAI API. Backend or authentication changes may require updating the adapter.
It does not establish an officially supported third-party subscription API.

## Build

Requires a C99 compiler, POSIX sockets and file APIs, libcurl 7.32 or newer with
TLS support, and cJSON. Linux is tested; macOS and BSD portability is intended
but not tested. Native Windows is not implemented.

From this directory:

```sh
./build.sh
```

The script creates `codex-proxy` beside the source files and can be invoked from
any directory. It uses pkg-config for libcurl flags when available, falling back
to `-lcurl`. Override `CC`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS`, or `LDLIBS` as needed:

```sh
CC=clang CFLAGS='-O0 -g -Wall -Wextra' ./build.sh
```

To move the program into a separate project, copy `build.sh`, `codex-proxy.c`,
`cJSON.c`, and `cJSON.h`. There are no editor dependencies or editor build changes.

## Login and run

```sh
./codex-proxy login
./codex-proxy serve --model YOUR_CODEX_MODEL_ID
# Optional default reasoning effort:
./codex-proxy serve --model YOUR_CODEX_MODEL_ID --reasoning-effort high
```

`login` prints a browser URL and a one-time code. Sign in using your ChatGPT
account. Device-code login must be enabled in your ChatGPT security settings
or workspace permissions. There is no browser callback server or Codex binary.
The model must be available to your account; the adapter does not discover
available models or substitute another model.

Credentials are stored in `$HOME/.codex-proxy.json`, created with mode 0600 and
replaced atomically after refresh. They are independent of Codex CLI credentials.
Do not copy a shared refresh token into this file. `--auth-file PATH` selects
another file; its parent directory must already exist. A sibling `.lock` file
prevents concurrent login/serve processes from rotating the same credentials.
Stop the server before logging in again.

The server listens only on `127.0.0.1:8080`. Its default local bearer key is
`local`, matching Nextvi's existing LOCAL configuration. To choose a private
local key, set `CODEX_PROXY_KEY` in the server environment and configure the
same key in Nextvi. This local key is unrelated to the subscription tokens.

Nextvi's current configuration already matches:

```c
static char *api_key = "local";
static char *endpoint = "http://127.0.0.1:8080/v1/chat/completions";
static char *request_extra = "{}";
```

`--model` supplies the model when the HTTP request omits it.
`--reasoning-effort` supplies the effort when the request omits `reasoning_effort`.
Client values take precedence over these defaults; Nextvi can send either field
through `request_extra`. If neither the flag nor the request supplies an effort,
the backend uses its default. Supported effort levels depend on the model and
are validated by the backend. There is no need to change the harness protocol.

## Supported requests

- `POST /v1/chat/completions`: non-streaming JSON responses, text messages,
  function tools, multiple calls, and `role: "tool"` follow-ups.
- Request fields: `model`, `messages`, `tools`, `stream` (false or omitted),
  `reasoning_effort`, and `tool_choice` (`auto`, `none`, or `required`).
- `GET /health`: process health only; it does not verify subscription access.
- `GET /v1/models`: lists only the configured default model, not account
  availability. Empty when no default is configured.

Requests other than `/health` require `Authorization: Bearer <local key>`.
Send `Content-Length`; chunked request bodies and browser Origin headers are
rejected. One request is served at a time. Limits are 16 KiB of HTTP headers
and 32 MiB for each request body and accumulated upstream response stream.
Request reads have a 15-second deadline. `--timeout SECONDS` controls the
upstream request timeout (default 300). Closing the HTTP client cancels the
upstream transfer when libcurl next checks progress.

Although responses to Nextvi are non-streaming, the subscription backend uses
SSE. The adapter collects completed output items and uses them when the terminal
event omits its output or leaves it empty. It waits for a complete terminal
response. Failed, truncated,
and incomplete streams return errors without partially executable tool calls.
It does not automatically retry model requests except once following HTTP 401
and a successful token refresh. HTTP 429 is returned to the caller.

Assistant messages can include `reasoning_content` (a summary) and the
nonstandard `codex_reasoning` array (opaque encrypted reasoning items). Clients
should replay the whole assistant message. Nextvi already preserves it.
The adapter replays encrypted reasoning on subsequent tool rounds without
storing conversation state. Do not print or edit the opaque items.

Images, audio, downstream SSE, tool execution, model discovery, and general
Chat Completions compatibility are outside this small adapter's scope.
Unsupported top-level request fields return HTTP 400.

## Diagnostics and testing

`--upstream URL` and `--auth-base URL` permit endpoint overrides, including a
local mock server. HTTPS is required except for `localhost` or `127.0.0.1` HTTP
URLs. Redirects are not followed. Credentials are sent to the configured
endpoints, so use overrides only for endpoints you control.

Verified locally with GCC, Clang, AddressSanitizer, UndefinedBehaviorSanitizer,
and a mock HTTP service: device polling and token exchange, refresh before
expiry and after 401, credential persistence and locking, multiple tool calls
and follow-ups, reasoning replay, SSE parsing, HTTP errors, fragmented requests,
100-continue, and cancellation. A live subscription request with `gpt-6-luna`
also passed. Regression cases cover empty, null, and omitted terminal output,
reordered and repeated completed items, and truncated streams after tool output.

Protocol references inspected during implementation:

- [OpenAI authentication documentation](https://learn.chatgpt.com/docs/auth)
- [CLIProxyAPI device login](https://github.com/router-for-me/CLIProxyAPI/blob/main/sdk/auth/codex_device.go)
- [CLIProxyAPI request translation](https://github.com/router-for-me/CLIProxyAPI/blob/main/internal/translator/codex/openai/chat-completions/codex_openai_request.go)
- [cezar's C backend](https://github.com/douglascorrea/cezar/blob/master/src/codex_provider.c)

These are protocol references, not dependencies. The implementation is separate;
cJSON retains its own license in its source files.
