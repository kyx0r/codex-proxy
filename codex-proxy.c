/* Standalone Chat Completions adapter for the Codex subscription backend.
 * Build: cc -std=c99 -O2 codex-proxy.c cJSON.c -lcurl -o codex-proxy
 */
#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <curl/curl.h>
#include "cJSON.h"

#define BODY_MAX (32U * 1024U * 1024U)
#define HEADER_MAX 16384U
static const char *client_id = "app_EMoamEEZ73f0CkXaXp7hrann";
static const char *auth_base = "https://auth.openai.com";
static const char *upstream = "https://chatgpt.com/backend-api/codex/responses";
static const char *auth_file, *model, *reasoning_effort, *local_key = "local";
static long timeout = 300;
static volatile sig_atomic_t stopped;
static char error_text[256];

struct buf { char *s; size_t n, cap; };

static void *allocate(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) { fputs("out of memory\n", stderr); exit(1); }
	return p;
}

static void append(struct buf *b, const char *s, size_t n)
{
	char *p;
	size_t cap;
	if (b->n + n + 1 > b->cap) {
		cap = b->cap ? b->cap : 256;
		while (cap < b->n + n + 1) cap *= 2;
		p = allocate(cap);
		if (b->n) memcpy(p, b->s, b->n);
		free(b->s); b->s = p; b->cap = cap;
	}
	memcpy(b->s + b->n, s, n);
	b->n += n;
	b->s[b->n] = 0;
}

static const char *str(cJSON *j)
{
	return j && cJSON_IsString(j) ? j->valuestring : "";
}

static cJSON *get(cJSON *j, const char *key)
{
	return cJSON_GetObjectItemCaseSensitive(j, key);
}

static const char *field(cJSON *j, const char *key)
{
	return str(get(j, key));
}

static void copy(cJSON *to, const char *key, cJSON *value)
{
	if (value) cJSON_AddItemToObject(to, key, cJSON_Duplicate(value, 1));
}

static cJSON *failure(const char *message)
{
	cJSON *j = cJSON_CreateObject(), *e = cJSON_AddObjectToObject(j, "error");
	cJSON_AddStringToObject(e, "message", message);
	cJSON_AddStringToObject(e, "type", "proxy_error");
	return j;
}

static void onsignal(int sig)
{
	(void)sig;
	stopped = 1;
}

static long long milliseconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int waitfd(int fd, short events, long long deadline)
{
	struct pollfd p = {fd, events, 0};
	long long left;
	int n;
	while (!stopped && (left = deadline - milliseconds()) > 0) {
		n = poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left);
		if (n > 0) return 1;
		if (!n || errno != EINTR) break;
	}
	return 0;
}

static int sendall(int fd, const char *s, size_t n)
{
	long long deadline = milliseconds() + 15000;
	ssize_t k;
	while (n && waitfd(fd, POLLOUT, deadline)) {
		k = send(fd, s, n, 0);
		if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
		if (k <= 0) break;
		s += k; n -= (size_t)k;
	}
	return n ? -1 : 0;
}

static void reply(int fd, int status, cJSON *j)
{
	char header[256], *body = cJSON_PrintUnformatted(j);
	int n = snprintf(header, sizeof(header),
		"HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
		"Content-Length: %zu\r\nConnection: close\r\n"
		"Cache-Control: no-store\r\n\r\n", status,
		status == 200 ? "OK" : "Error", strlen(body));
	if (!sendall(fd, header, (size_t)n)) sendall(fd, body, strlen(body));
	free(body);
	cJSON_Delete(j);
}

static size_t receive_body(char *s, size_t size, size_t count, void *opaque)
{
	struct buf *b = opaque;
	size_t n;
	if (size && count > BODY_MAX / size) return 0;
	n = size * count;
	if (n > BODY_MAX - b->n) return 0;
	append(b, s, n);
	return n;
}

static int progress(void *opaque, curl_off_t a, curl_off_t b,
		curl_off_t c, curl_off_t d)
{
	int fd = *(int *)opaque;
	char ch;
	struct pollfd p = {fd, POLLIN, 0};
	(void)a; (void)b; (void)c; (void)d;
	if (stopped) return 1;
	if (fd >= 0 && poll(&p, 1, 0) > 0 &&
			(p.revents & (POLLHUP | POLLERR | POLLNVAL))) return 1;
	if (fd >= 0 && (p.revents & POLLIN) && recv(fd, &ch, 1, MSG_PEEK) == 0)
		return 1;
	return 0;
}

/* No redirects: subscription credentials belong only to the selected upstream. */
static int post(const char *url, const char *body, const char *type,
		cJSON *auth, struct buf *out, long *status, int fd)
{
	CURL *curl = curl_easy_init();
	CURLcode code;
	struct curl_slist *headers = NULL;
	struct buf h = {0};
	if (!curl) return -1;
	append(&h, "Content-Type: ", 14); append(&h, type, strlen(type));
	headers = curl_slist_append(headers, h.s);
	free(h.s); h = (struct buf){0};
	headers = curl_slist_append(headers, "Expect:");
	if (auth) {
		append(&h, "Authorization: Bearer ", 22);
		append(&h, field(auth, "access_token"), strlen(field(auth, "access_token")));
		headers = curl_slist_append(headers, h.s);
		free(h.s); h = (struct buf){0};
		append(&h, "ChatGPT-Account-ID: ", 19);
		append(&h, field(auth, "account_id"), strlen(field(auth, "account_id")));
		headers = curl_slist_append(headers, h.s);
		free(h.s);
		headers = curl_slist_append(headers, "Accept: text/event-stream");
	} else headers = curl_slist_append(headers, "Accept: application/json");
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(body));
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "nextvi-codex-proxy/1");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_body);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, auth ? timeout : 30L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &fd);
	code = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
	if (code != CURLE_OK) snprintf(error_text, sizeof(error_text),
		"HTTPS request failed: %s", curl_easy_strerror(code));
	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);
	return code == CURLE_OK ? 0 : -1;
}

static cJSON *auth_post(const char *path, cJSON *j, int form, long *status)
{
	struct buf url = {0}, body = {0}, out = {0};
	cJSON *p, *result = NULL;
	CURL *curl = curl_easy_init();
	char *encoded;
	if (!curl) return NULL;
	append(&url, auth_base, strlen(auth_base)); append(&url, path, strlen(path));
	if (form) {
		cJSON_ArrayForEach(p, j) {
			encoded = curl_easy_escape(curl, str(p), 0);
			if (!encoded) { free(url.s); free(body.s); curl_easy_cleanup(curl); return NULL; }
			if (body.n) append(&body, "&", 1);
			append(&body, p->string, strlen(p->string)); append(&body, "=", 1);
			append(&body, encoded, strlen(encoded)); curl_free(encoded);
		}
	} else body.s = cJSON_PrintUnformatted(j);
	if (!post(url.s, body.s, form ? "application/x-www-form-urlencoded" :
			"application/json", NULL, &out, status, -1) && out.s)
		result = cJSON_ParseWithOpts(out.s, NULL, 1);
	curl_easy_cleanup(curl);
	free(url.s); free(body.s); free(out.s);
	return result;
}

/* JWT payload is used only for metadata from the TLS-authenticated token reply. */
static cJSON *claims(const char *token)
{
	const char *s = strchr(token, '.'), *end;
	struct buf b = {0};
	unsigned int bits = 0;
	int nbits = 0, v;
	char ch;
	cJSON *j;
	if (!s || !(end = strchr(++s, '.'))) return NULL;
	for (; s < end; s++) {
		if (*s >= 'A' && *s <= 'Z') v = *s - 'A';
		else if (*s >= 'a' && *s <= 'z') v = *s - 'a' + 26;
		else if (*s >= '0' && *s <= '9') v = *s - '0' + 52;
		else if (*s == '-') v = 62;
		else if (*s == '_') v = 63;
		else { free(b.s); return NULL; }
		bits = (bits << 6) | (unsigned int)v; nbits += 6;
		if (nbits >= 8) { nbits -= 8; ch = (char)(bits >> nbits); append(&b, &ch, 1); }
	}
	j = b.s ? cJSON_ParseWithOpts(b.s, NULL, 1) : NULL;
	free(b.s);
	return j;
}

static int writeall(int fd, const char *s, size_t n)
{
	ssize_t k;
	while (n) {
		k = write(fd, s, n);
		if (k < 0 && errno == EINTR) continue;
		if (k <= 0) return -1;
		s += k; n -= (size_t)k;
	}
	return 0;
}

static int save_auth(cJSON *j)
{
	struct buf path = {0};
	char *s = cJSON_PrintUnformatted(j);
	int fd, ok = 0;
	append(&path, auth_file, strlen(auth_file)); append(&path, ".XXXXXX", 7);
	fd = mkstemp(path.s);
	if (fd >= 0) {
		ok = !writeall(fd, s, strlen(s)) && !fsync(fd);
		if (close(fd)) ok = 0;
		if (ok && rename(path.s, auth_file)) ok = 0;
		if (!ok) unlink(path.s);
	}
	free(s); free(path.s);
	return ok ? 0 : -1;
}

static int valid_auth(cJSON *j)
{
	const char *access = field(j, "access_token"), *account = field(j, "account_id");
	return *access && *account && !strpbrk(access, "\r\n") && !strpbrk(account, "\r\n");
}

static cJSON *load_auth(void)
{
	int fd = open(auth_file, O_RDONLY);
	struct buf b = {0};
	char chunk[4096];
	ssize_t n;
	cJSON *j = NULL;
	if (fd < 0) return NULL;
	while ((n = read(fd, chunk, sizeof(chunk))) > 0 && b.n + (size_t)n <= 65536)
		append(&b, chunk, (size_t)n);
	if (!n && b.s) j = cJSON_ParseWithOpts(b.s, NULL, 1);
	close(fd); free(b.s);
	if (!valid_auth(j)) { cJSON_Delete(j); return NULL; }
	return j;
}

static cJSON *token_auth(cJSON *tokens, cJSON *old)
{
	cJSON *j = cJSON_CreateObject(), *jwt = claims(field(tokens, "id_token"));
	cJSON *info = get(jwt, "https://api.openai.com/auth");
	const char *account = field(info, "chatgpt_account_id");
	double expires = cJSON_GetNumberValue(get(tokens, "expires_in"));
	if (!*account) account = field(old, "account_id");
	cJSON_AddStringToObject(j, "account_id", account);
	cJSON_AddStringToObject(j, "access_token", field(tokens, "access_token"));
	cJSON_AddStringToObject(j, "refresh_token", *field(tokens, "refresh_token") ?
		field(tokens, "refresh_token") : field(old, "refresh_token"));
	cJSON_Delete(jwt);
	jwt = claims(field(tokens, "access_token"));
	if (expires > 0 && expires < 365 * 86400)
		cJSON_AddNumberToObject(j, "expires_at", (double)time(NULL) + expires);
	else copy(j, "expires_at", get(jwt, "exp"));
	cJSON_Delete(jwt);
	if (!valid_auth(j)) { cJSON_Delete(j); return NULL; }
	return j;
}

static int refresh(cJSON **auth)
{
	cJSON *req = cJSON_CreateObject(), *tokens, *fresh;
	long status = 0;
	if (!*field(*auth, "refresh_token")) { cJSON_Delete(req); return -1; }
	cJSON_AddStringToObject(req, "client_id", client_id);
	cJSON_AddStringToObject(req, "grant_type", "refresh_token");
	cJSON_AddStringToObject(req, "refresh_token", field(*auth, "refresh_token"));
	tokens = auth_post("/oauth/token", req, 1, &status);
	cJSON_Delete(req);
	fresh = status == 200 ? token_auth(tokens, *auth) : NULL;
	cJSON_Delete(tokens);
	if (!fresh) return -1;
	/* Keep rotated credentials in memory even if persistence fails. */
	cJSON_Delete(*auth); *auth = fresh;
	if (save_auth(fresh)) {
		fputs("could not save refreshed credentials; stop and log in again\n", stderr);
		return -1;
	}
	return 0;
}

static int login(void)
{
	cJSON *req = cJSON_CreateObject(), *device, *pollreq, *tokens = NULL, *auth;
	long status = 0;
	int interval = 5;
	long long deadline;
	const char *code;
	cJSON_AddStringToObject(req, "client_id", client_id);
	device = auth_post("/api/accounts/deviceauth/usercode", req, 0, &status);
	cJSON_Delete(req);
	code = field(device, "user_code");
	if (!*code) code = field(device, "usercode");
	if (status != 200 || !*code || !*field(device, "device_auth_id")) {
		fprintf(stderr, "device login unavailable (HTTP %ld); enable device-code login in ChatGPT security settings\n", status);
		cJSON_Delete(device); return 1;
	}
	if (cJSON_IsNumber(get(device, "interval"))) interval = get(device, "interval")->valueint;
	else if (*field(device, "interval")) interval = atoi(field(device, "interval"));
	if (interval < 1) interval = 5;
	if (interval > 60) interval = 60;
	fprintf(stderr, "Open %s/codex/device and enter: %s\n", auth_base, code);
	pollreq = cJSON_CreateObject();
	copy(pollreq, "device_auth_id", get(device, "device_auth_id"));
	cJSON_AddStringToObject(pollreq, "user_code", code);
	cJSON_Delete(device);
	deadline = milliseconds() + 900000;
	while (!stopped && milliseconds() < deadline) {
		tokens = auth_post("/api/accounts/deviceauth/token", pollreq, 0, &status);
		if (status == 200) break;
		cJSON_Delete(tokens); tokens = NULL;
		if (status != 403 && status != 404) break;
		poll(NULL, 0, interval * 1000);
	}
	cJSON_Delete(pollreq);
	if (!tokens || !*field(tokens, "authorization_code") || !*field(tokens, "code_verifier")) {
		fprintf(stderr, "device login did not complete (HTTP %ld)\n", status);
		cJSON_Delete(tokens); return 1;
	}
	req = cJSON_CreateObject();
	cJSON_AddStringToObject(req, "client_id", client_id);
	cJSON_AddStringToObject(req, "grant_type", "authorization_code");
	cJSON_AddStringToObject(req, "code", field(tokens, "authorization_code"));
	copy(req, "code_verifier", get(tokens, "code_verifier"));
	cJSON_AddStringToObject(req, "redirect_uri", "https://auth.openai.com/deviceauth/callback");
	cJSON_Delete(tokens);
	tokens = auth_post("/oauth/token", req, 1, &status);
	cJSON_Delete(req);
	auth = status == 200 ? token_auth(tokens, NULL) : NULL;
	cJSON_Delete(tokens);
	if (!auth || save_auth(auth)) {
		fputs("could not obtain or save subscription credentials\n", stderr);
		cJSON_Delete(auth); return 1;
	}
	cJSON_Delete(auth);
	fprintf(stderr, "Login saved to %s\n", auth_file);
	return 0;
}

static cJSON *message_item(const char *role, const char *text)
{
	cJSON *j = cJSON_CreateObject(), *parts, *part;
	cJSON_AddStringToObject(j, "type", "message");
	cJSON_AddStringToObject(j, "role", role);
	parts = cJSON_AddArrayToObject(j, "content"); part = cJSON_CreateObject();
	cJSON_AddStringToObject(part, "type", !strcmp(role, "assistant") ? "output_text" : "input_text");
	cJSON_AddStringToObject(part, "text", text); cJSON_AddItemToArray(parts, part);
	return j;
}

/* Text-only Chat Completions; the caller owns execution and conversation history. */
static cJSON *translate_request(cJSON *chat)
{
	cJSON *req = cJSON_CreateObject(), *input = cJSON_AddArrayToObject(req, "input");
	cJSON *tools = cJSON_AddArrayToObject(req, "tools"), *m, *t, *fn, *item, *content;
	cJSON *messages = get(chat, "messages"), *extra;
	const char *name = *field(chat, "model") ? field(chat, "model") : model, *role;
	struct buf instructions = {0};
	const char *error = "invalid messages or tools";
	if (!cJSON_IsObject(chat) || !name || !*name || !cJSON_IsArray(messages) || !messages->child) {
		error = "model and a nonempty messages array are required (or set --model)"; goto fail;
	}
	if (get(chat, "stream") && !cJSON_IsFalse(get(chat, "stream"))) {
		error = "this adapter supports stream:false only"; goto fail;
	}
	/* Reject controls we cannot honor rather than silently changing their meaning. */
	cJSON_ArrayForEach(extra, chat) {
		if (!strcmp(extra->string, "model") || !strcmp(extra->string, "messages") ||
			!strcmp(extra->string, "tools") || !strcmp(extra->string, "stream") ||
			!strcmp(extra->string, "reasoning_effort") || !strcmp(extra->string, "tool_choice")) continue;
		error = "unsupported request field; supported: model, messages, tools, stream, reasoning_effort, tool_choice";
		goto fail;
	}
	cJSON_AddStringToObject(req, "model", name);
	cJSON_AddBoolToObject(req, "store", 0); cJSON_AddBoolToObject(req, "stream", 1);
	cJSON_AddBoolToObject(req, "parallel_tool_calls", 1);
	cJSON_AddItemToArray(cJSON_AddArrayToObject(req, "include"), cJSON_CreateString("reasoning.encrypted_content"));
	cJSON_ArrayForEach(m, messages) {
		role = field(m, "role"); content = get(m, "content");
		if (content && !cJSON_IsString(content) && !cJSON_IsNull(content)) {
			error = "message content must be text or null"; goto fail;
		}
		if (!strcmp(role, "system")) {
			if (instructions.n) append(&instructions, "\n\n", 2);
			append(&instructions, str(content), strlen(str(content)));
		} else if (!strcmp(role, "tool")) {
			if (!*field(m, "tool_call_id") || !cJSON_IsString(content)) goto fail;
			item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "type", "function_call_output");
			copy(item, "call_id", get(m, "tool_call_id")); copy(item, "output", content);
			cJSON_AddItemToArray(input, item);
		} else if (!strcmp(role, "user") || !strcmp(role, "developer") || !strcmp(role, "assistant")) {
			extra = get(m, "codex_reasoning");
			if (extra && (!cJSON_IsArray(extra) || strcmp(role, "assistant"))) goto fail;
			cJSON_ArrayForEach(t, extra) {
				if (strcmp(field(t, "type"), "reasoning") || !*field(t, "encrypted_content")) goto fail;
				cJSON_AddItemToArray(input, cJSON_Duplicate(t, 1));
			}
			if (cJSON_IsString(content) && (*str(content) || strcmp(role, "assistant")))
				cJSON_AddItemToArray(input, message_item(role, str(content)));
			extra = get(m, "tool_calls");
			if (extra && (!cJSON_IsArray(extra) || strcmp(role, "assistant"))) goto fail;
			cJSON_ArrayForEach(t, extra) {
				fn = get(t, "function");
				if (strcmp(field(t, "type"), "function") || !*field(t, "id") ||
					!*field(fn, "name") || !cJSON_IsString(get(fn, "arguments"))) goto fail;
				item = cJSON_CreateObject(); cJSON_AddStringToObject(item, "type", "function_call");
				copy(item, "call_id", get(t, "id")); copy(item, "name", get(fn, "name"));
				copy(item, "arguments", get(fn, "arguments")); cJSON_AddItemToArray(input, item);
			}
		} else goto fail;
	}
	cJSON_AddStringToObject(req, "instructions", instructions.s ? instructions.s : "");
	extra = get(chat, "tools");
	if (extra && !cJSON_IsArray(extra)) goto fail;
	cJSON_ArrayForEach(t, extra) {
		fn = get(t, "function");
		if (strcmp(field(t, "type"), "function") || !*field(fn, "name") || !cJSON_IsObject(get(fn, "parameters"))) goto fail;
		item = cJSON_CreateObject(); cJSON_AddStringToObject(item, "type", "function");
		copy(item, "name", get(fn, "name")); copy(item, "description", get(fn, "description"));
		copy(item, "parameters", get(fn, "parameters")); copy(item, "strict", get(fn, "strict"));
		cJSON_AddItemToArray(tools, item);
	}
	extra = get(chat, "tool_choice");
	if (extra) {
		if (strcmp(str(extra), "auto") && strcmp(str(extra), "none") && strcmp(str(extra), "required")) {
			error = "tool_choice must be auto, none, or required"; goto fail;
		}
		copy(req, "tool_choice", extra);
	}
	item = cJSON_AddObjectToObject(req, "reasoning");
	cJSON_AddStringToObject(item, "summary", "auto");
	extra = get(chat, "reasoning_effort");
	if (extra && !cJSON_IsString(extra)) {
		error = "reasoning_effort must be a string"; goto fail;
	}
	if (extra) copy(item, "effort", extra);
	else if (reasoning_effort) cJSON_AddStringToObject(item, "effort", reasoning_effort);
	free(instructions.s);
	return req;
fail:
	snprintf(error_text, sizeof(error_text), "%s", error);
	free(instructions.s); cJSON_Delete(req);
	return NULL;
}

static cJSON *sse_response(char *body)
{
	char *line = body, *end;
	cJSON *event, *response = NULL, *items = cJSON_CreateArray();
	cJSON *item, *saved, *index, *output;
	struct buf data = {0};
	size_t n;
	int position, expected;
	const char *error = "upstream stream ended without a completed response";
	/* SSE events may contain multiple data lines and either LF or CRLF. */
	while (line && *line) {
		end = strchr(line, '\n'); n = end ? (size_t)(end - line) : strlen(line);
		if (n && line[n - 1] == '\r') n--;
		if (n >= 5 && !memcmp(line, "data:", 5)) {
			size_t start = n > 5 && line[5] == ' ' ? 6 : 5;
			if (data.n) append(&data, "\n", 1);
			append(&data, line + start, n - start);
		}
		if (!n || !end) {
			event = data.s ? cJSON_ParseWithOpts(data.s, NULL, 1) : NULL;
			if (!strcmp(field(event, "type"), "response.output_item.done")) {
				item = get(event, "item"); index = get(event, "output_index");
				if (!cJSON_IsObject(item) || !cJSON_IsNumber(index) ||
						index->valuedouble < 0 || index->valuedouble > INT_MAX ||
						index->valuedouble != index->valueint) {
					cJSON_Delete(event); error = "invalid completed output item"; goto fail;
				}
				position = 0;
				cJSON_ArrayForEach(saved, items) {
					if (get(saved, "output_index")->valueint >= index->valueint) break;
					position++;
				}
				if (saved && get(saved, "output_index")->valueint == index->valueint)
					cJSON_ReplaceItemInArray(items, position, cJSON_Duplicate(event, 1));
				else cJSON_InsertItemInArray(items, position, cJSON_Duplicate(event, 1));
			} else if (!strcmp(field(event, "type"), "response.completed")) {
				cJSON_Delete(response); response = cJSON_Duplicate(get(event, "response"), 1);
			} else if (!strcmp(field(event, "type"), "response.failed") ||
					!strcmp(field(event, "type"), "response.incomplete") ||
					!strcmp(field(event, "type"), "error")) {
				cJSON_Delete(event); error = "upstream response failed or was incomplete"; goto fail;
			}
			cJSON_Delete(event); free(data.s); data = (struct buf){0};
		}
		line = end ? end + 1 : NULL;
	}
	if (!response) goto fail;
	output = get(response, "output");
	/* Codex may send the actual output only in preceding output_item.done events. */
	if ((!output || cJSON_IsNull(output) || (cJSON_IsArray(output) && !output->child)) && items->child) {
		cJSON_DeleteItemFromObject(response, "output");
		output = cJSON_AddArrayToObject(response, "output");
		expected = 0;
		cJSON_ArrayForEach(saved, items) {
			if (get(saved, "output_index")->valueint != expected++) {
				error = "upstream stream is missing a completed output item"; goto fail;
			}
			cJSON_AddItemToArray(output, cJSON_Duplicate(get(saved, "item"), 1));
		}
	}
	free(data.s); cJSON_Delete(items);
	return response;
fail:
	snprintf(error_text, sizeof(error_text), "%s", error);
	free(data.s); cJSON_Delete(items); cJSON_Delete(response);
	return NULL;
}

static cJSON *translate_response(cJSON *response)
{
	cJSON *chat = cJSON_CreateObject(), *choices = cJSON_AddArrayToObject(chat, "choices");
	cJSON *choice = cJSON_CreateObject(), *message = cJSON_AddObjectToObject(choice, "message");
	cJSON *calls = cJSON_AddArrayToObject(message, "tool_calls"), *reasoning = cJSON_CreateArray();
	cJSON *item, *part, *call, *fn, *usage, *u;
	struct buf text = {0}, summary = {0};
	if (strcmp(field(response, "status"), "completed") || !cJSON_IsArray(get(response, "output"))) goto fail;
	cJSON_AddItemToArray(choices, choice);
	cJSON_AddStringToObject(message, "role", "assistant");
	cJSON_AddStringToObject(chat, "id", field(response, "id"));
	cJSON_AddStringToObject(chat, "object", "chat.completion");
	copy(chat, "model", get(response, "model")); copy(chat, "created", get(response, "created_at"));
	cJSON_AddNumberToObject(choice, "index", 0);
	cJSON_ArrayForEach(item, get(response, "output")) {
		if (!strcmp(field(item, "type"), "message")) {
			cJSON_ArrayForEach(part, get(item, "content")) {
				if (!strcmp(field(part, "type"), "output_text")) append(&text, field(part, "text"), strlen(field(part, "text")));
				else if (!strcmp(field(part, "type"), "refusal")) append(&text, field(part, "refusal"), strlen(field(part, "refusal")));
			}
		} else if (!strcmp(field(item, "type"), "function_call")) {
			if (!*field(item, "call_id") || !*field(item, "name") || !cJSON_IsString(get(item, "arguments"))) goto fail;
			call = cJSON_CreateObject(); copy(call, "id", get(item, "call_id"));
			cJSON_AddStringToObject(call, "type", "function"); fn = cJSON_AddObjectToObject(call, "function");
			copy(fn, "name", get(item, "name")); copy(fn, "arguments", get(item, "arguments")); cJSON_AddItemToArray(calls, call);
		} else if (!strcmp(field(item, "type"), "reasoning")) {
			if (*field(item, "encrypted_content")) cJSON_AddItemToArray(reasoning, cJSON_Duplicate(item, 1));
			cJSON_ArrayForEach(part, get(item, "summary")) append(&summary, field(part, "text"), strlen(field(part, "text")));
		} else goto fail;
	}
	if (!text.n && !calls->child) goto fail;
	cJSON_AddStringToObject(message, "content", text.s ? text.s : "");
	cJSON_AddStringToObject(choice, "finish_reason", calls->child ? "tool_calls" : "stop");
	if (!calls->child) cJSON_DeleteItemFromObject(message, "tool_calls");
	if (summary.n) cJSON_AddStringToObject(message, "reasoning_content", summary.s);
	if (reasoning->child) cJSON_AddItemToObject(message, "codex_reasoning", reasoning);
	else cJSON_Delete(reasoning);
	u = get(response, "usage");
	if (cJSON_IsObject(u)) {
		usage = cJSON_AddObjectToObject(chat, "usage");
		copy(usage, "prompt_tokens", get(u, "input_tokens")); copy(usage, "completion_tokens", get(u, "output_tokens"));
		copy(usage, "total_tokens", get(u, "total_tokens"));
		copy(usage, "prompt_tokens_details", get(u, "input_tokens_details"));
		copy(usage, "completion_tokens_details", get(u, "output_tokens_details"));
	}
	free(text.s); free(summary.s);
	return chat;
fail:
	if (!choices->child) cJSON_Delete(choice);
	cJSON_Delete(chat); cJSON_Delete(reasoning); free(text.s); free(summary.s);
	snprintf(error_text, sizeof(error_text), "upstream returned an unsupported or incomplete response");
	return NULL;
}

static void complete(int fd, cJSON *chat, cJSON **auth)
{
	cJSON *req = translate_request(chat), *response, *result;
	struct buf body = {0};
	char *encoded;
	long status = 0;
	double expiry;
	int rc, attempt;
	if (!req) { reply(fd, 400, failure(error_text)); return; }
	encoded = cJSON_PrintUnformatted(req); cJSON_Delete(req);
	expiry = cJSON_GetNumberValue(get(*auth, "expires_at"));
	if (expiry > 0 && expiry < (double)time(NULL) + 60 && refresh(auth)) {
		free(encoded); reply(fd, 401, failure("subscription refresh failed; stop the server and run login")); return;
	}
	for (attempt = 0; attempt < 2; attempt++) {
		rc = post(upstream, encoded, "application/json", *auth, &body, &status, fd);
		if (rc || status != 401 || attempt || refresh(auth)) break;
		free(body.s); body = (struct buf){0};
	}
	free(encoded);
	if (rc) { free(body.s); reply(fd, 502, failure(error_text)); return; }
	if (status != 200) {
		/* Do not echo untrusted upstream bodies, which may contain credentials. */
		snprintf(error_text, sizeof(error_text), "subscription backend returned HTTP %ld%s", status,
			status == 401 ? "; stop the server and run login" : "");
		free(body.s); reply(fd, status >= 400 && status <= 599 ? (int)status : 502, failure(error_text)); return;
	}
	response = body.s ? sse_response(body.s) : NULL;
	free(body.s);
	result = response ? translate_response(response) : NULL;
	cJSON_Delete(response);
	reply(fd, result ? 200 : 502, result ? result : failure(error_text));
}

static int local_host(const char *s)
{
	const char *p;
	if (!strncmp(s, "127.0.0.1", 9)) p = s + 9;
	else if (!strncmp(s, "localhost", 9)) p = s + 9;
	else return 0;
	if (!*p) return 1;
	if (*p++ != ':' || !*p) return 0;
	for (; *p; p++) if (*p < '0' || *p > '9') return 0;
	return 1;
}

static void serve_client(int fd, cJSON **auth)
{
	struct buf b = {0};
	char chunk[4096], method[16], path[128], version[16], junk, *end, *line, *save, *colon, *value;
	char *authorization = NULL, *host = NULL;
	size_t header_size, length = 0;
	ssize_t n;
	int have_length = 0, expect_continue = 0, status = 400;
	long long deadline = milliseconds() + 15000;
	const char *error = "invalid HTTP request";
	cJSON *j;
	while (waitfd(fd, POLLIN, deadline)) {
		n = recv(fd, chunk, sizeof(chunk), 0);
		if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
		if (n <= 0) goto done;
		if (memchr(chunk, 0, (size_t)n)) goto bad;
		append(&b, chunk, (size_t)n);
		end = strstr(b.s, "\r\n\r\n");
		if (end) break;
		if (b.n > HEADER_MAX) goto bad;
	}
	if (!b.s || !(end = strstr(b.s, "\r\n\r\n"))) goto bad;
	header_size = (size_t)(end - b.s) + 4;
	if (header_size > HEADER_MAX) goto bad;
	*end = 0;
	line = strtok_r(b.s, "\r\n", &save);
	if (!line || sscanf(line, "%15s %127s %15s %c", method, path, version, &junk) != 3 ||
		(strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))) goto bad;
	while ((line = strtok_r(NULL, "\r\n", &save))) {
		colon = strchr(line, ':'); if (!colon) goto bad;
		*colon = 0; value = colon + 1;
		while (*value == ' ' || *value == '\t') value++;
		end = value + strlen(value);
		while (end > value && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
		if (!strcasecmp(line, "Content-Length")) {
			if (have_length++ || !*value) goto bad;
			for (; *value; value++) {
				if (*value < '0' || *value > '9' || length > BODY_MAX / 10) goto bad;
				length = length * 10 + (size_t)(*value - '0');
			}
			if (length > BODY_MAX) { status = 413; goto bad; }
		} else if (!strcasecmp(line, "Transfer-Encoding")) goto bad;
		else if (!strcasecmp(line, "Origin")) { status = 403; error = "browser requests are not accepted"; goto bad; }
		else if (!strcasecmp(line, "Host")) { if (host) goto bad; host = value; }
		else if (!strcasecmp(line, "Authorization")) { if (authorization) goto bad; authorization = value; }
		else if (!strcasecmp(line, "Expect")) {
			if (strcasecmp(value, "100-continue")) { status = 417; goto bad; }
			expect_continue = 1;
		}
	}
	if (!host || !local_host(host)) { status = 403; error = "Host must be localhost or 127.0.0.1"; goto bad; }
	if (!strcmp(method, "GET") && !strcmp(path, "/health")) {
		j = cJSON_CreateObject(); cJSON_AddStringToObject(j, "status", "ok"); reply(fd, 200, j); goto done;
	}
	if (!authorization || strncmp(authorization, "Bearer ", 7) || strcmp(authorization + 7, local_key)) {
		status = 401; error = "invalid local proxy key"; goto bad;
	}
	if (!strcmp(method, "GET") && !strcmp(path, "/v1/models")) {
		j = cJSON_CreateObject(); cJSON_AddStringToObject(j, "object", "list");
		cJSON *data = cJSON_AddArrayToObject(j, "data"), *entry;
		if (model) {
			entry = cJSON_CreateObject(); cJSON_AddStringToObject(entry, "id", model);
			cJSON_AddStringToObject(entry, "object", "model"); cJSON_AddItemToArray(data, entry);
		}
		reply(fd, 200, j); goto done;
	}
	if (strcmp(path, "/v1/chat/completions")) { status = 404; error = "unknown endpoint"; goto bad; }
	if (strcmp(method, "POST")) { status = 405; error = "use POST"; goto bad; }
	if (!have_length) { status = 411; error = "Content-Length required"; goto bad; }
	if (b.n - header_size > length) goto bad;
	if (expect_continue && b.n - header_size < length) sendall(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
	while (b.n - header_size < length && waitfd(fd, POLLIN, deadline)) {
		size_t remaining = length - (b.n - header_size);
		n = recv(fd, chunk, remaining < sizeof(chunk) ? remaining : sizeof(chunk), 0);
		if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
		if (n <= 0) goto done;
		if (memchr(chunk, 0, (size_t)n)) goto bad;
		append(&b, chunk, (size_t)n);
	}
	if (b.n - header_size != length) { status = 408; error = "request body timed out"; goto bad; }
	j = cJSON_ParseWithOpts(b.s + header_size, NULL, 1);
	if (!j) goto bad;
	complete(fd, j, auth); cJSON_Delete(j); goto done;
bad:
	reply(fd, status, failure(error));
done:
	free(b.s);
}

static int lock_auth(void)
{
	struct buf path = {0};
	struct flock lock;
	int fd;
	append(&path, auth_file, strlen(auth_file)); append(&path, ".lock", 5);
	fd = open(path.s, O_RDWR | O_CREAT, 0600); free(path.s);
	memset(&lock, 0, sizeof(lock)); lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET;
	if (fd < 0 || fcntl(fd, F_SETLK, &lock) < 0) {
		if (fd >= 0) close(fd);
		return -1;
	}
	return fd;
}

static int valid_url(const char *url)
{
	const char *end;
	char host[128];
	size_t n;
	if (!strncmp(url, "https://", 8)) return url[8] && !strpbrk(url, "\r\n");
	if (strncmp(url, "http://", 7)) return 0;
	end = strchr(url + 7, '/'); n = end ? (size_t)(end - url - 7) : strlen(url + 7);
	if (n >= sizeof(host)) return 0;
	memcpy(host, url + 7, n); host[n] = 0;
	return local_host(host);
}

static void usage(void)
{
	fputs("Usage: codex-proxy login|serve [options]\n"
		"  --auth-file PATH    Credentials (default: $HOME/.codex-proxy.json)\n"
		"  --model MODEL       Default model for requests without model\n"
		"  --reasoning-effort LEVEL  Default effort; requests can override it\n"
		"  --port PORT         Loopback listener (default: 8080)\n"
		"  --timeout SECONDS   Upstream timeout (default: 300)\n"
		"  --upstream URL     Responses endpoint override\n"
		"  --auth-base URL    Authentication service override\n"
		"CODEX_PROXY_KEY sets the local HTTP key (default: local).\n", stderr);
}

int main(int argc, char **argv)
{
	int i, port = 8080, lockfd, server, fd, yes = 1, result = 0;
	long value;
	char *end;
	const char *home = getenv("HOME"), *key = getenv("CODEX_PROXY_KEY");
	struct buf default_path = {0};
	struct sockaddr_in addr;
	struct sigaction sa;
	cJSON *auth;
	cJSON_Hooks hooks = {allocate, free};
	cJSON_InitHooks(&hooks);
	if (argc < 2 || (strcmp(argv[1], "login") && strcmp(argv[1], "serve"))) {
		usage(); return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 1;
	}
	if (key && *key) local_key = key;
	if (home && *home) {
		append(&default_path, home, strlen(home)); append(&default_path, "/.codex-proxy.json", 18);
		auth_file = default_path.s;
	}
	for (i = 2; i < argc; i += 2) {
		if (i + 1 == argc) { usage(); return 1; }
		if (!strcmp(argv[i], "--auth-file")) auth_file = argv[i + 1];
		else if (!strcmp(argv[i], "--model")) model = argv[i + 1];
		else if (!strcmp(argv[i], "--reasoning-effort")) {
			if (!*argv[i + 1]) { usage(); return 1; }
			reasoning_effort = argv[i + 1];
		}
		else if (!strcmp(argv[i], "--upstream")) upstream = argv[i + 1];
		else if (!strcmp(argv[i], "--auth-base")) auth_base = argv[i + 1];
		else if (!strcmp(argv[i], "--port") || !strcmp(argv[i], "--timeout")) {
			errno = 0; value = strtol(argv[i + 1], &end, 10);
			if (errno || *end || value < 1 || value > 65535) { usage(); return 1; }
			if (!strcmp(argv[i], "--port")) port = (int)value; else timeout = value;
		} else { usage(); return 1; }
	}
	if (!auth_file || !*auth_file || !valid_url(upstream) || !valid_url(auth_base) || strpbrk(local_key, "\r\n")) {
		fputs("invalid path, key, or URL (HTTPS or loopback HTTP required)\n", stderr); return 1;
	}
	umask(077);
	lockfd = lock_auth();
	if (lockfd < 0) { fputs("cannot lock credential file; check its directory and stop other instances first\n", stderr); return 1; }
	memset(&sa, 0, sizeof(sa)); sa.sa_handler = onsignal; sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	if (curl_global_init(CURL_GLOBAL_DEFAULT)) return 1;
	if (!strcmp(argv[1], "login")) { result = login(); goto done; }
	auth = load_auth();
	if (!auth) { fputs("no usable subscription credentials; run codex-proxy login\n", stderr); result = 1; goto done; }
	server = socket(AF_INET, SOCK_STREAM, 0);
	memset(&addr, 0, sizeof(addr)); addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port); addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (server < 0 || setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) ||
		bind(server, (struct sockaddr *)&addr, sizeof(addr)) || listen(server, 8)) {
		perror("listen"); result = 1;
	} else {
		fprintf(stderr, "Listening on http://127.0.0.1:%d\n", port);
		while (!stopped) {
			fd = accept(server, NULL, NULL);
			if (fd < 0) { if (errno == EINTR) continue; perror("accept"); result = 1; break; }
			fcntl(fd, F_SETFL, O_NONBLOCK);
			serve_client(fd, &auth); close(fd);
		}
	}
	if (server >= 0) close(server);
	cJSON_Delete(auth);
done:
	close(lockfd); curl_global_cleanup(); free(default_path.s);
	return result;
}
