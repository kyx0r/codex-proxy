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
#include <sys/time.h>
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


/* Anthropic Claude subscription login: OAuth 2.0 authorization code + PKCE.
 * Client and endpoint values track the Claude Code CLI public client. */
static const char *backend = "codex";
static const char *anthropic_client_id = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
static const char *anthropic_authorize = "https://claude.ai/oauth/authorize";
static const char *anthropic_manual_redirect = "https://platform.claude.com/oauth/code/callback";
static const char *anthropic_token_url = "https://platform.claude.com/v1/oauth/token";
static const char *anthropic_scopes = "org:create_api_key user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload";
static const char *anthropic_beta = "oauth-2025-04-20,token-efficient-tools-2025-02-19";
static char anthropic_redirect[64];
static int code_port = 54222;
static const char *anthropic_upstream = "https://api.anthropic.com/v1/messages";
static long thinking_budget;
static int manual_code;

struct sha256 {
	unsigned int h[8];
	unsigned long long bits;
	unsigned char buf[64];
	size_t len;
};
static const unsigned int sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static unsigned int rotr32(unsigned int x, unsigned int n)
{
	return (x >> n) | (x << (32 - n));
}

static void sha256_block(struct sha256 *s)
{
	unsigned int w[64], a, b, c, d, e, f, g, h, t1, t2;
	int i;
	for (i = 0; i < 16; i++)
		w[i] = ((unsigned int)s->buf[i * 4] << 24)
		       | ((unsigned int)s->buf[i * 4 + 1] << 16)
		       | ((unsigned int)s->buf[i * 4 + 2] << 8)
		       | (unsigned int)s->buf[i * 4 + 3];
	for (i = 16; i < 64; i++) {
		t1 = w[i - 15];
		t2 = w[i - 2];
		w[i] = (rotr32(t1, 7) ^ rotr32(t1, 18) ^ (t1 >> 3)) + w[i - 16]
		       + (rotr32(t2, 17) ^ rotr32(t2, 19) ^ (t2 >> 10)) + w[i - 7];
	}
	a = s->h[0];
	b = s->h[1];
	c = s->h[2];
	d = s->h[3];
	e = s->h[4];
	f = s->h[5];
	g = s->h[6];
	h = s->h[7];
	for (i = 0; i < 64; i++) {
		t1 = h + (rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25))
		     + ((e & f) ^ (~e & g)) + sha256_k[i] + w[i];
		t2 = (rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22))
		     + ((a & b) ^ (a & c) ^ (b & c));
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	s->h[0] += a;
	s->h[1] += b;
	s->h[2] += c;
	s->h[3] += d;
	s->h[4] += e;
	s->h[5] += f;
	s->h[6] += g;
	s->h[7] += h;
}

static void sha256_init(struct sha256 *s)
{
	static const unsigned int h0[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};
	int i;
	s->bits = 0;
	s->len = 0;
	for (i = 0; i < 8; i++)
		s->h[i] = h0[i];
}

static void sha256_update(struct sha256 *s, const void *data, size_t n)
{
	const unsigned char *p = data;
	size_t k;
	s->bits += (unsigned long long)n * 8;
	while (n) {
		k = 64 - s->len;
		if (k > n)
			k = n;
		memcpy(s->buf + s->len, p, k);
		s->len += k;
		p += k;
		n -= k;
		if (s->len < 64)
			return;
		sha256_block(s);
		s->len = 0;
	}
}

static void sha256_final(struct sha256 *s, unsigned char out[32])
{
	unsigned char pad = 0x80, tail[8];
	unsigned long long bits = s->bits;
	int i;
	sha256_update(s, &pad, 1);
	while (s->len != 56) {
		pad = 0;
		sha256_update(s, &pad, 1);
	}
	for (i = 0; i < 8; i++)
		tail[i] = (unsigned char)(bits >> (56 - 8 * i));
	sha256_update(s, tail, 8);
	for (i = 0; i < 32; i++)
		out[i] = (unsigned char)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

static void sha256_hex(const char *text, char out[65])
{
	static const char digits[] = "0123456789abcdef";
	unsigned char digest[32];
	struct sha256 s;
	int i;
	sha256_init(&s);
	sha256_update(&s, text, strlen(text));
	sha256_final(&s, digest);
	for (i = 0; i < 32; i++) {
		out[i * 2] = digits[digest[i] >> 4];
		out[i * 2 + 1] = digits[digest[i] & 15];
	}
	out[64] = 0;
}

static const char b64_alphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static void base64url(const unsigned char *in, size_t n, char *out)
{
	size_t i = 0, o = 0;
	unsigned int v;
	while (i + 3 <= n) {
		v = ((unsigned int)in[i] << 16) | ((unsigned int)in[i + 1] << 8)
		    | in[i + 2];
		out[o++] = b64_alphabet[(v >> 18) & 63];
		out[o++] = b64_alphabet[(v >> 12) & 63];
		out[o++] = b64_alphabet[(v >> 6) & 63];
		out[o++] = b64_alphabet[v & 63];
		i += 3;
	}
	if (i + 1 == n) {
		v = (unsigned int)in[i] << 16;
		out[o++] = b64_alphabet[(v >> 18) & 63];
		out[o++] = b64_alphabet[(v >> 12) & 63];
	} else if (i + 2 == n) {
		v = ((unsigned int)in[i] << 16) | ((unsigned int)in[i + 1] << 8);
		out[o++] = b64_alphabet[(v >> 18) & 63];
		out[o++] = b64_alphabet[(v >> 12) & 63];
		out[o++] = b64_alphabet[(v >> 6) & 63];
	}
	out[o] = 0;
}

static int random_bytes(unsigned char *out, size_t n)
{
	unsigned char *p = out;
	ssize_t k;
	size_t got = 0;
	int fd = open("/dev/urandom", O_RDONLY), retries = 0;
	if (fd < 0)
		return -1;
	while (got < n && retries++ < 64) {
		k = read(fd, p + got, n - got);
		if (k > 0)
			got += (size_t)k;
		else if (k < 0 && errno == EINTR)
			continue;
		else
			break;
	}
	close(fd);
	return got == n ? 0 : -1;
}

static void pkce_value(const char *text, char *out)
{
	unsigned char digest[32];
	struct sha256 s;
	sha256_init(&s);
	sha256_update(&s, text, strlen(text));
	sha256_final(&s, digest);
	base64url(digest, 32, out);
}


static void pkce_token(char *out)
{
	unsigned char raw[32];
	unsigned long long seed;
	struct sha256 s;
	if (random_bytes(raw, sizeof(raw))) {
		seed = (unsigned long long)time(NULL)
		       ^ ((unsigned long long)getpid() << 11)
		       ^ ((unsigned long long)clock() << 3);
		sha256_init(&s);
		sha256_update(&s, &seed, sizeof(seed));
		sha256_final(&s, raw);
	}
	base64url(raw, sizeof(raw), out);
}
struct buf {
	char *s;
	size_t n, cap;
};

static void *allocate(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) {
		fputs("out of memory\n", stderr);
		exit(1);
	}
	return p;
}

static void append(struct buf *b, const char *s, size_t n)
{
	char *p;
	size_t cap;
	if (b->n + n + 1 > b->cap) {
		cap = b->cap ? b->cap : 256;
		while (cap < b->n + n + 1)
			cap *= 2;
		p = allocate(cap);
		if (b->n)
			memcpy(p, b->s, b->n);
		free(b->s);
		b->s = p;
		b->cap = cap;
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
	if (value)
		cJSON_AddItemToObject(to, key, cJSON_Duplicate(value, 1));
}

static cJSON *failure(const char *message)
{
	cJSON *j = cJSON_CreateObject(), *e = cJSON_AddObjectToObject(j, "error");

	cJSON_AddStringToObject(e, "message", message ? message : "request failed");
	if (strcmp(backend, "anthropic")) {
		cJSON_AddStringToObject(e, "type", "proxy_error");
		return j;
	}
	cJSON_AddStringToObject(j, "type", "error");
	cJSON_AddStringToObject(e, "type", "api_error");
	return j;
}
static void reply(int fd, int status, cJSON *j);
static void reply_invalid(int fd, const char *message)
{
	cJSON *j, *e;

	if (strcmp(backend, "anthropic")) {
		reply(fd, 400, failure(message));
		return;
	}
	j = cJSON_CreateObject();
	e = cJSON_AddObjectToObject(j, "error");
	cJSON_AddStringToObject(j, "type", "error");
	if (e) {
		cJSON_AddStringToObject(e, "type", "invalid_request_error");
		cJSON_AddStringToObject(e, "message", message);
	}
	reply(fd, 400, j);
}


static void onsignal(int sig)
{
	stopped = sig;
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
		if (n > 0)
			return 1;
		if (!n || errno != EINTR)
			break;
	}
	return 0;
}

static int sendall(int fd, const char *s, size_t n)
{
	long long deadline = milliseconds() + 15000;
	ssize_t k;
	while (n && waitfd(fd, POLLOUT, deadline)) {
		k = send(fd, s, n, 0);
		if (k < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (k <= 0)
			break;
		s += k;
		n -= (size_t)k;
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
	if (!sendall(fd, header, (size_t)n))
		sendall(fd, body, strlen(body));
	free(body);
	cJSON_Delete(j);
}

static size_t receive_body(char *s, size_t size, size_t count, void *opaque)
{
	struct buf *b = opaque;
	size_t n;
	if (size && count > BODY_MAX / size)
		return 0;
	n = size * count;
	if (n > BODY_MAX - b->n)
		return 0;
	append(b, s, n);
	return n;
}

static int progress(void *opaque, curl_off_t a, curl_off_t b,
		    curl_off_t c, curl_off_t d)
{
	int fd = *(int *)opaque;
	char ch;
	struct pollfd p = {fd, POLLIN, 0};
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	if (stopped)
		return 1;
	if (fd >= 0 && poll(&p, 1, 0) > 0 &&
	    (p.revents & (POLLHUP | POLLERR | POLLNVAL)))
		return 1;
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
	if (!curl)
		return -1;
	append(&h, "Content-Type: ", 14);
	append(&h, type, strlen(type));
	headers = curl_slist_append(headers, h.s);
	free(h.s);
	h = (struct buf) {
		0
	};
	headers = curl_slist_append(headers, "Expect:");
	if (auth) {
		append(&h, "Authorization: Bearer ", 22);
		append(&h, field(auth, "access_token"), strlen(field(auth, "access_token")));
		headers = curl_slist_append(headers, h.s);
		free(h.s);
		h = (struct buf) {
			0
		};
		if (!strcmp(backend, "anthropic")) {
			append(&h, "anthropic-version: 2023-06-01", 29);
			headers = curl_slist_append(headers, h.s);
			free(h.s);
			h = (struct buf) {
				0
			};
			append(&h, "anthropic-beta: ", 16);
			append(&h, anthropic_beta, strlen(anthropic_beta));
			headers = curl_slist_append(headers, h.s);
			free(h.s);
			h = (struct buf) {
				0
			};
			append(&h, "User-Agent: claude-cli/2.0.0 (external, cli)", 44);
			headers = curl_slist_append(headers, h.s);
			free(h.s);
			h = (struct buf) {
				0
			};
			headers = curl_slist_append(headers,
				"Accept: text/event-stream,application/json");
		} else {
			append(&h, "ChatGPT-Account-ID: ", 19);
			append(&h, field(auth, "account_id"), strlen(field(auth, "account_id")));
			headers = curl_slist_append(headers, h.s);
			free(h.s);
			headers = curl_slist_append(headers, "Accept: text/event-stream");
		}
	} else
		headers = curl_slist_append(headers, "Accept: application/json");
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
	if (code != CURLE_OK)
		snprintf(error_text, sizeof(error_text),
			 "HTTPS request failed: %s", curl_easy_strerror(code));
	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);
	return code == CURLE_OK ? 0 : -1;
}

static int refresh(cJSON **auth);
static const char *reason(long status)
{
	switch (status) {
	case 200:
		return "OK";
	case 400:
		return "Bad Request";
	case 401:
		return "Unauthorized";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 413:
		return "Content Too Large";
	case 429:
		return "Too Many Requests";
	case 500:
		return "Internal Server Error";
	case 502:
		return "Bad Gateway";
	case 529:
		return "Overloaded";
	default:
		return "Error";
	}
}

static void raw_reply(int fd, long status, const char *type,
		      const char *body, size_t n)
{
	char header[256];
	int len = snprintf(header, sizeof(header),
			   "HTTP/1.1 %ld %s\r\nContent-Type: %s\r\n"
			   "Content-Length: %zu\r\nConnection: close\r\n"
			   "Cache-Control: no-store\r\n\r\n", status, reason(status),
			   type, n);

	if (len <= 0 || (size_t)len >= sizeof(header))
		return;
	if (!sendall(fd, header, (size_t)len))
		sendall(fd, body, n);
}

static long effort_budget(const char *effort)
{
	if (!effort)
		return 0;
	if (!strcasecmp(effort, "minimal") || !strcasecmp(effort, "low"))
		return 1024;
	if (!strcasecmp(effort, "medium"))
		return 8000;
	if (!strcasecmp(effort, "high"))
		return 16000;
	return 0;
}

/* Some Messages clients send Chat Completions tool descriptors. */
static void openai_tool(cJSON *tool, const char *key, const char *name)
{
	cJSON *fn = get(tool, "function");
	cJSON *item = fn ? cJSON_DetachItemFromObject(fn, key) : NULL;

	if (!item)
		return;
	cJSON_DeleteItemFromObject(tool, name);
	cJSON_AddItemToObject(tool, name, item);
}

static void openai_tools(cJSON *chat)
{
	cJSON *tools = get(chat, "tools"), *tool, *item;

	if (!cJSON_IsArray(tools))
		return;
	cJSON_ArrayForEach(tool, tools) {
		if (!cJSON_IsObject(tool))
			continue;
		if (!strcasecmp(field(tool, "type"), "function")
		    && cJSON_IsObject(get(tool, "function"))) {
			openai_tool(tool, "name", "name");
			openai_tool(tool, "description", "description");
			openai_tool(tool, "input_schema", "input_schema");
			openai_tool(tool, "parameters", "input_schema");
			cJSON_DeleteItemFromObject(tool, "function");
			cJSON_DeleteItemFromObject(tool, "type");
		}
		if (!get(tool, "type") && !get(tool, "input_schema")
		    && cJSON_IsString(get(tool, "name"))) {
			item = cJSON_AddObjectToObject(tool, "input_schema");
			if (item)
				cJSON_AddStringToObject(item, "type", "object");
		}
	}
}

/* The Messages gateway rejects fields that Chat Completions clients send. */
static void openai_choice(cJSON *chat)
{
	cJSON *item = get(chat, "tool_choice"), *fn, *name, *pick;
	const char *mode;

	if (!item)
		return;
	if (cJSON_IsObject(item)) {
		fn = get(item, "function");
		name = fn ? get(fn, "name") : NULL;
		if (strcasecmp(field(item, "type"), "function")
		    || !cJSON_IsString(name))
			return;
		pick = cJSON_CreateObject();
		if (pick) {
			cJSON_AddStringToObject(pick, "type", "tool");
			cJSON_AddStringToObject(pick, "name", name->valuestring);
			cJSON_ReplaceItemInObject(chat, "tool_choice", pick);
		}
		return;
	}
	if (!cJSON_IsString(item)) {
		cJSON_DeleteItemFromObject(chat, "tool_choice");
		return;
	}
	mode = item->valuestring;
	if (!strcasecmp(mode, "none") || !strcasecmp(mode, "required")) {
		pick = cJSON_CreateObject();
		if (pick) {
			cJSON_AddStringToObject(pick, "type",
			    strcasecmp(mode, "none") ? "any" : "none");
			cJSON_ReplaceItemInObject(chat, "tool_choice", pick);
		}
		return;
	}
	cJSON_DeleteItemFromObject(chat, "tool_choice");
}
/*
 * Chat keeps tool calls on assistant messages and tool answers in
 * role "tool"; Messages wants tool_use blocks and tool_result blocks.
 */
static void openai_message(cJSON *message)
{
	cJSON *calls = cJSON_DetachItemFromObject(message, "tool_calls");
	cJSON *call, *content, *blocks, *block, *fn, *item, *parsed;

	if (!cJSON_IsArray(calls)) {
		cJSON_Delete(calls);
		return;
	}
	content = get(message, "content");
	blocks = cJSON_IsArray(content) ? content : NULL;
	if (!blocks) {
		blocks = cJSON_CreateArray();
		if (!blocks) {
			cJSON_Delete(calls);
			return;
		}
		if (cJSON_IsString(content)) {
			item = cJSON_CreateObject();
			if (item) {
				cJSON_AddStringToObject(item, "type", "text");
				cJSON_AddStringToObject(item, "text",
				    content->valuestring);
				cJSON_AddItemToArray(blocks, item);
			}
}
		cJSON_ReplaceItemInObject(message, "content", blocks);
	}
	cJSON_ArrayForEach(call, calls) {
		fn = get(call, "function");
		if (!cJSON_IsObject(fn))
			continue;
		block = cJSON_CreateObject();
		if (!block)
			break;
		cJSON_AddStringToObject(block, "type", "tool_use");
		item = get(call, "id");
		if (cJSON_IsString(item))
			cJSON_AddStringToObject(block, "id", item->valuestring);
		item = get(fn, "name");
		cJSON_AddStringToObject(block, "name",
		    cJSON_IsString(item) ? item->valuestring : "");
		item = get(fn, "arguments");
		parsed = cJSON_IsString(item) ? cJSON_Parse(item->valuestring)
		    : cJSON_IsObject(item) ? cJSON_Duplicate(item, 1) : NULL;
		cJSON_AddItemToObject(block, "input",
		    parsed ? parsed : cJSON_CreateObject());
		cJSON_AddItemToArray(blocks, block);
	}
	cJSON_Delete(calls);
}


static void openai_stop(cJSON *chat)
{
	cJSON *item = get(chat, "stop"), *list;

	if (!item)
		return;
	list = cJSON_IsArray(item) ? cJSON_DetachItemFromObject(chat, "stop") : NULL;
	if (!list && cJSON_IsString(item)) {
		list = cJSON_CreateArray();
		if (list)
			cJSON_AddItemToArray(list,
			    cJSON_CreateString(item->valuestring));
	}
	cJSON_DeleteItemFromObject(chat, "stop");
	if (!list || !list->child) {
		cJSON_Delete(list);
		return;
	}
	cJSON_DeleteItemFromObject(chat, "stop_sequences");
	cJSON_AddItemToObject(chat, "stop_sequences", list);
}

/* Messages API has no system role; hoist system text to top level. */
static void openai_system(cJSON *chat)
{
	struct buf sys = {0};

	size_t i = 0;
	cJSON *messages = get(chat, "messages"), *item, *part;

	if (!cJSON_IsArray(messages))
		return;

	for (i = 0; i < (size_t)cJSON_GetArraySize(messages); i++) {
		item = cJSON_GetArrayItem(messages, (int)i);
		if (strcmp(field(item, "role"), "system"))
			continue;
		part = get(item, "content");
		if (cJSON_IsString(part) && part->valuestring[0]) {
			if (sys.n)
				append(&sys, "\n", 1);
			append(&sys, part->valuestring,
			       strlen(part->valuestring));
		}
		free(cJSON_DetachItemFromArray(messages, (int)i));
		i--;
	}
	if (!sys.s)
		return;
	cJSON_AddStringToObject(chat, "system", sys.s);
	free(sys.s);
}


/* Absent keys must be added; ReplaceItem would drop the value. */
static void set_item(cJSON *object, const char *key, cJSON *item)
{
	if (cJSON_GetObjectItemCaseSensitive(object, key))
		cJSON_ReplaceItemInObject(object, key, item);
	else
		cJSON_AddItemToObject(object, key, item);
}
	/* Chat roles and tool traffic become Messages API blocks. */
static void openai_messages(cJSON *chat)
{
	cJSON *messages = get(chat, "messages"), *item, *part, *calls, *call;
	cJSON *blocks, *block, *fn;

	if (!cJSON_IsArray(messages))
		return;
	cJSON_ArrayForEach(item, messages) {
		part = get(item, "content");
		calls = get(item, "tool_calls");

		cJSON_DeleteItemFromObject(item, "reasoning_content");
		cJSON_DeleteItemFromObject(item, "provider");
		cJSON_DeleteItemFromObject(item, "system_fingerprint");
		if (!strcmp(field(item, "role"), "tool")) {
			cJSON_ReplaceItemInObject(item, "role",
			    cJSON_CreateString("user"));
			block = cJSON_CreateObject();
			if (!block)
				continue;
			cJSON_AddStringToObject(block, "type", "tool_result");
			cJSON_AddStringToObject(block, "tool_use_id",
			    field(item, "tool_call_id"));
			cJSON_AddItemToObject(block, "content",
			    part ? cJSON_Duplicate(part, 1)
				 : cJSON_CreateString(""));
			blocks = cJSON_CreateArray();
			if (!blocks) {
				cJSON_Delete(block);
				continue;
			}
			cJSON_AddItemToArray(blocks, block);
			set_item(item, "content", blocks);
			cJSON_DeleteItemFromObject(item, "tool_call_id");
			continue;
		}
		if (!cJSON_IsArray(calls) || !calls->child)
			continue;
		blocks = cJSON_CreateArray();
		if (!blocks)
			continue;
		if (part && cJSON_IsString(part) && part->valuestring[0]) {
			block = cJSON_CreateObject();
			cJSON_AddStringToObject(block, "type", "text");
			cJSON_AddStringToObject(block, "text", part->valuestring);
			cJSON_AddItemToArray(blocks, block);
		}
		cJSON_ArrayForEach(call, calls) {
			fn = get(call, "function");
			block = cJSON_CreateObject();
			if (!block)
				continue;
			cJSON_AddStringToObject(block, "type", "tool_use");
			copy(block, "id", get(call, "id"));
			copy(block, "name", get(fn, "name"));
			part = get(fn, "arguments");

			fn = part && cJSON_IsString(part)
			    ? cJSON_Parse(part->valuestring) : NULL;
			cJSON_AddItemToObject(block, "input",
			    fn ? fn : cJSON_CreateObject());
			cJSON_AddItemToArray(blocks, block);
		}
		set_item(item, "content", blocks);
		cJSON_DeleteItemFromObject(item, "tool_calls");
	}
}

static void openai_request(cJSON *chat)
{
	static const char *const junk[] = {"audio", "function_call", "functions",
	    "logprobs", "max_retries", "modalities", "n", "object", "prediction",
	    "presence_penalty", "prompt_cache_key", "frequency_penalty",
	    "reasoning_effort", "response_format", "seed", "stop", "store",
	    "stream_options", "top_logprobs", "verbosity", "web_search_options",
	    "parallel_tool_calls", "reasoning_budget_tokens", "user"};
	cJSON *item;
	size_t i;

	openai_tools(chat);
	openai_choice(chat);
	openai_stop(chat);

	openai_system(chat);

	openai_messages(chat);
	item = get(chat, "service_tier");
	if (item && (!cJSON_IsString(item)
	    || (strcmp(item->valuestring, "auto")
	        && strcmp(item->valuestring, "standard_only"))))
		cJSON_DeleteItemFromObject(chat, "service_tier");
	for (i = 0; i < sizeof(junk) / sizeof(*junk); i++)
		cJSON_DeleteItemFromObject(chat, junk[i]);
}


/* Anthropic Messages reply translated into a Chat Completions reply. */
static cJSON *anthropic_chat(cJSON *msg)
{
	cJSON *chat, *choices, *choice, *message, *calls, *blocks, *item, *call,
	       *fn, *usage, *u;
	struct buf text = {0}, reasoning = {0};
	double i = 0, o = 0;
	const char *stop = field(msg, "stop_reason");

	if (!cJSON_IsObject(msg) || !cJSON_IsArray(get(msg, "content")))
		return NULL;
	chat = cJSON_CreateObject();
	choices = cJSON_AddArrayToObject(chat, "choices");
	choice = cJSON_CreateObject();
	message = cJSON_AddObjectToObject(choice, "message");
	calls = cJSON_AddArrayToObject(message, "tool_calls");
	if (!chat || !choices || !choice || !message || !calls) {
		cJSON_Delete(chat);
		return NULL;
	}
	cJSON_AddStringToObject(message, "role", "assistant");
	cJSON_AddStringToObject(chat, "id", field(msg, "id"));
	cJSON_AddStringToObject(chat, "object", "chat.completion");
	copy(chat, "model", get(msg, "model"));
	cJSON_AddNumberToObject(choice, "index", 0);
	cJSON_AddItemToArray(choices, choice);
	blocks = get(msg, "content");
	cJSON_ArrayForEach(item, blocks) {
		if (!strcmp(field(item, "type"), "text")) {
			append(&text, field(item, "text"), strlen(field(item, "text")));
		} else if (!strcmp(field(item, "type"), "tool_use")) {
			char *input = cJSON_PrintUnformatted(get(item, "input"));
			call = cJSON_CreateObject();
			cJSON_AddStringToObject(call, "id", field(item, "id"));
			cJSON_AddStringToObject(call, "type", "function");
			fn = cJSON_AddObjectToObject(call, "function");
			cJSON_AddStringToObject(fn, "name", field(item, "name"));
			cJSON_AddStringToObject(fn, "arguments", input ? input : "{}");
			free(input);
			cJSON_AddItemToArray(calls, call);

		} else if (!strcmp(field(item, "type"), "thinking")) {
			append(&reasoning, field(item, "thinking"),
				 strlen(field(item, "thinking")));
		}
	}
	cJSON_AddStringToObject(message, "content", text.s ? text.s : "");

		if (reasoning.s) {
			cJSON_AddStringToObject(message, "reasoning_content",
					    reasoning.s);
			free(reasoning.s);
			reasoning.s = NULL;
		}
	free(text.s);
	cJSON_AddStringToObject(choice, "finish_reason", !strcmp(stop, "tool_use")
	    ? "tool_calls" : !strcmp(stop, "max_tokens") ? "length" : "stop");
	usage = get(msg, "usage");
	if (usage) {
		item = get(usage, "input_tokens");
		if (item)
			i = cJSON_GetNumberValue(item);
		item = get(usage, "output_tokens");
		if (item)
			o = cJSON_GetNumberValue(item);
		u = cJSON_CreateObject();
		if (u) {
			cJSON_AddNumberToObject(u, "prompt_tokens", i);
			cJSON_AddNumberToObject(u, "completion_tokens", o);
			cJSON_AddNumberToObject(u, "total_tokens", i + o);
			cJSON_AddItemToObject(chat, "usage", u);
		}
	}
	return chat;
}
static void complete_anthropic(int fd, const char *path, cJSON *chat, cJSON **auth)
{
	struct buf body = {0};
	char url[512], *encoded;
	double number;
	long budget = thinking_budget, status = 0;
	int rc, attempt, stream, chat_api = !strcmp(path, "/v1/chat/completions");
	cJSON *item;

	if (!cJSON_IsObject(chat)) {
		reply_invalid(fd, "request body must be a JSON object");
		return;
	}
	item = get(chat, "reasoning_effort");
	if (!budget && cJSON_IsString(item))
		budget = effort_budget(item->valuestring);

	if (!budget) {
		item = get(chat, "reasoning_budget_tokens");
		if (cJSON_IsNumber(item)) {
			number = cJSON_GetNumberValue(item);
			budget = number < 1024 ? 0 : (long) number;
			if (budget > 512000)
				budget = 512000;
		}
	}
	openai_request(chat);

	if (chat_api) {
		/* Chat Completions clients get one translated JSON reply. */
		cJSON_DeleteItemFromObject(chat, "stream");
	}
	if (!get(chat, "model")) {
		if (!model) {
			reply_invalid(fd, "model required; pass --model or send one");
			return;
		}
		cJSON_AddStringToObject(chat, "model", model);
	}
	item = get(chat, "max_tokens");
	if (!item && !strcmp(path, "/v1/messages")) {
		reply_invalid(fd, "max_tokens required");
		return;
	}
	if (item && !cJSON_IsNumber(item)) {
		reply_invalid(fd, "max_tokens must be a number");
		return;
	}
	if (item && !strcmp(path, "/v1/messages/count_tokens"))
		cJSON_DeleteItemFromObject(chat, "max_tokens");
	number = item ? cJSON_GetNumberValue(item) : 0;
	stream = cJSON_IsTrue(get(chat, "stream"));
	if (!budget)
		budget = effort_budget(reasoning_effort);
	if (!item && chat_api) {
		number = budget >= 1024 ? (double)budget + 2048 : 8000;
		cJSON_AddNumberToObject(chat, "max_tokens", number);
	}
	if (budget && number && number <= (double)budget + 1024)
		budget = number > 2048 ? (long)(number - 1024) : 0;
	if (budget >= 1024 && (!strcmp(path, "/v1/messages") || chat_api)
	    && !get(chat, "thinking")) {
		item = cJSON_AddObjectToObject(chat, "thinking");
		if (item) {
			cJSON_AddStringToObject(item, "type", "enabled");
			cJSON_AddNumberToObject(item, "budget_tokens", (double)budget);
		}
	}
	encoded = cJSON_PrintUnformatted(chat);
	if (!encoded) {
		reply(fd, 500, failure("cannot encode request"));
		return;
	}
	number = 0;
	item = get(*auth, "expires_at");
	if (item)
		number = cJSON_GetNumberValue(item);
	if (number > 0 && number < (double)time(NULL) + 60 && refresh(auth)) {
		free(encoded);
		reply(fd, 401, failure("subscription refresh failed; stop the server and"
		      " run login"));
		return;
	}
	snprintf(url, sizeof(url), "%s%s", anthropic_upstream,
		 !strcmp(path, "/v1/messages/count_tokens") ? "/count_tokens" : "");
	for (attempt = 0; attempt < 2; attempt++) {
		rc = post(url, encoded, "application/json", *auth, &body, &status, fd);
		if (rc || status != 401 || attempt || refresh(auth))
			break;
		free(body.s);
		body = (struct buf) {
			0
		};
	}
	free(encoded);
	if (rc) {
		free(body.s);
		reply(fd, 502, failure(error_text));
		return;
	}
	if (!body.s || !body.n) {
		free(body.s);
		snprintf(error_text, sizeof(error_text),
			 "inference backend returned HTTP %ld with no body", status);
		reply(fd, status >= 400 && status <= 599 ? (int)status : 502,
		      failure(error_text));
		return;

	}

	if (chat_api && status == 200) {
		cJSON *msg = cJSON_ParseWithOpts(body.s, NULL, 1),
		       *chat = msg ? anthropic_chat(msg) : NULL;
		char *text = chat ? cJSON_PrintUnformatted(chat) : NULL;
		if (text) {
			free(body.s);
			body.s = text;
			body.n = strlen(text);
		}
		if (chat)
			cJSON_Delete(chat);
		if (msg)
			cJSON_Delete(msg);
	}
	raw_reply(fd, status,
		  status == 200 && stream ? "text/event-stream" : "application/json",
		  body.s, body.n);
	free(body.s);
}

static cJSON *auth_post(const char *path, cJSON *j, int form, long *status)
{
	struct buf url = {0}, body = {0}, out = {0};
	cJSON *p, *result = NULL;
	CURL *curl = curl_easy_init();
	char *encoded;
	if (!curl)
		return NULL;
	append(&url, auth_base, strlen(auth_base));
	append(&url, path, strlen(path));
	if (form) {
		cJSON_ArrayForEach(p, j) {
			encoded = curl_easy_escape(curl, str(p), 0);
			if (!encoded) {
				free(url.s);
				free(body.s);
				curl_easy_cleanup(curl);
				return NULL;
			}
			if (body.n)
				append(&body, "&", 1);
			append(&body, p->string, strlen(p->string));
			append(&body, "=", 1);
			append(&body, encoded, strlen(encoded));
			curl_free(encoded);
		}
	} else
		body.s = cJSON_PrintUnformatted(j);
	if (!post(url.s, body.s, form ? "application/x-www-form-urlencoded" :
		  "application/json", NULL, &out, status, -1) && out.s)
		result = cJSON_ParseWithOpts(out.s, NULL, 1);
	curl_easy_cleanup(curl);
	free(url.s);
	free(body.s);
	free(out.s);
	return result;
}


/* Token endpoints answer JSON in both directions; auth_post only speaks form or Codex paths. */
static cJSON *token_request(const char *url, cJSON *req, long *status)
{
	struct buf out = {0};
	cJSON *result = NULL;
	char *s = cJSON_PrintUnformatted(req);
	if (!s)
		return NULL;
	if (!post(url, s, "application/json", NULL, &out, status, -1) && out.s)
		result = cJSON_ParseWithOpts(out.s, NULL, 1);
	if (!result && out.s)
		snprintf(error_text, sizeof(error_text), "unexpected token reply: %.180s",
			 out.s);
	free(s);
	free(out.s);
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
	if (!s || !(end = strchr(++s, '.')))
		return NULL;
	for (; s < end; s++) {
		if (*s >= 'A' && *s <= 'Z')
			v = *s - 'A';
		else if (*s >= 'a' && *s <= 'z')
			v = *s - 'a' + 26;
		else if (*s >= '0' && *s <= '9')
			v = *s - '0' + 52;
		else if (*s == '-')
			v = 62;
		else if (*s == '_')
			v = 63;
		else {
			free(b.s);
			return NULL;
		}
		bits = (bits << 6) | (unsigned int)v;
		nbits += 6;
		if (nbits >= 8) {
			nbits -= 8;
			ch = (char)(bits >> nbits);
			append(&b, &ch, 1);
		}
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
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return -1;
		s += k;
		n -= (size_t)k;
	}
	return 0;
}

static int save_auth(cJSON *j)
{
	struct buf path = {0};
	char *s = cJSON_PrintUnformatted(j);
	int fd, ok = 0;
	append(&path, auth_file, strlen(auth_file));
	append(&path, ".XXXXXX", 7);
	fd = mkstemp(path.s);
	if (fd >= 0) {
		ok = !writeall(fd, s, strlen(s)) && !fsync(fd);
		if (close(fd))
			ok = 0;
		if (ok && rename(path.s, auth_file))
			ok = 0;
		if (!ok)
			unlink(path.s);
	}
	free(s);
	free(path.s);
	return ok ? 0 : -1;
}

static int valid_auth(cJSON *j)
{
	const char *access = field(j, "access_token");
	const char *account = field(j, "account_id");
	if (!*access || strpbrk(access, "\r\n"))
		return 0;
	/* Only the Codex backend routes requests by account id. */
	if (strcmp(backend, "anthropic") && (!*account || strpbrk(account, "\r\n")))
		return 0;
	return 1;
}

static cJSON *load_auth(void)
{
	int fd = open(auth_file, O_RDONLY);
	struct buf b = {0};
	char chunk[4096];
	ssize_t n;
	cJSON *j = NULL;
	if (fd < 0)
		return NULL;
	while ((n = read(fd, chunk, sizeof(chunk))) > 0 && b.n + (size_t)n <= 65536)
		append(&b, chunk, (size_t)n);
	if (!n && b.s)
		j = cJSON_ParseWithOpts(b.s, NULL, 1);
	close(fd);
	free(b.s);
	if (!valid_auth(j)) {
		cJSON_Delete(j);
		return NULL;
	}
	return j;
}

static cJSON *token_auth(cJSON *tokens, cJSON *old)
{
	cJSON *j = cJSON_CreateObject(), *jwt = claims(field(tokens, "id_token"));
	cJSON *info = get(jwt, "https://api.openai.com/auth");
	const char *account = field(info, "chatgpt_account_id");
	double expires = cJSON_GetNumberValue(get(tokens, "expires_in"));
	if (!*account)
		account = field(old, "account_id");
	cJSON_AddStringToObject(j, "account_id", account);
	cJSON_AddStringToObject(j, "access_token", field(tokens, "access_token"));
	cJSON_AddStringToObject(j, "refresh_token", *field(tokens, "refresh_token") ?
				field(tokens, "refresh_token") : field(old, "refresh_token"));
	cJSON_Delete(jwt);
	jwt = claims(field(tokens, "access_token"));
	if (expires > 0 && expires < 365 * 86400)
		cJSON_AddNumberToObject(j, "expires_at", (double)time(NULL) + expires);
	else
		copy(j, "expires_at", get(jwt, "exp"));
	cJSON_Delete(jwt);
	if (!valid_auth(j)) {
		cJSON_Delete(j);
		return NULL;
	}
	return j;
}

/* Anthropic answers with a flat token payload; no JWT claims to decode. */
static cJSON *anthropic_token_auth(cJSON *tokens, cJSON *old,
		const char *code_verifier)
{
	cJSON *j = cJSON_CreateObject();
	double expires = cJSON_GetNumberValue(get(tokens, "expires_in"));
	const char *access = field(tokens, "access_token");
	const char *fresh = *field(tokens, "refresh_token") ?
		field(tokens, "refresh_token") : field(old, "refresh_token");
	if (!*access) {
		cJSON_Delete(j);
		return NULL;
	}
	cJSON_AddStringToObject(j, "backend", "anthropic");
	cJSON_AddStringToObject(j, "access_token", access);
	if (*fresh)
		cJSON_AddStringToObject(j, "refresh_token", fresh);
	if (expires > 0 && expires < 365 * 86400)
		cJSON_AddNumberToObject(j, "expires_at", (double)time(NULL) + expires);
	if (code_verifier && *code_verifier)
		cJSON_AddStringToObject(j, "code_verifier", code_verifier);
	if (!valid_auth(j)) {
		cJSON_Delete(j);
		return NULL;
	}
	return j;
}

static int refresh(cJSON **auth)
{
	cJSON *req = cJSON_CreateObject(), *tokens, *fresh;
	long status = 0;
	if (!*field(*auth, "refresh_token")) {
		cJSON_Delete(req);
		return -1;
	}
	if (!strcmp(backend, "anthropic")) {
		cJSON_AddStringToObject(req, "client_id", anthropic_client_id);
		cJSON_AddStringToObject(req, "grant_type", "refresh_token");
		cJSON_AddStringToObject(req, "refresh_token", field(*auth, "refresh_token"));
		cJSON_AddStringToObject(req, "scope", anthropic_scopes);
		tokens = token_request(anthropic_token_url, req, &status);
		fresh = status == 200 ? anthropic_token_auth(tokens, *auth, NULL) : NULL;
	} else {
		cJSON_AddStringToObject(req, "client_id", client_id);
		cJSON_AddStringToObject(req, "grant_type", "refresh_token");
		cJSON_AddStringToObject(req, "refresh_token", field(*auth, "refresh_token"));
		tokens = auth_post("/oauth/token", req, 1, &status);
		fresh = status == 200 ? token_auth(tokens, *auth) : NULL;
	}
	cJSON_Delete(req);
	cJSON_Delete(tokens);
	if (!fresh)
		return -1;
	/* Keep rotated credentials in memory even if persistence fails. */
	cJSON_Delete(*auth);
	*auth = fresh;
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
	if (!*code)
		code = field(device, "usercode");
	if (status != 200 || !*code || !*field(device, "device_auth_id")) {
		fprintf(stderr,
			"device login unavailable (HTTP %ld); enable device-code login in ChatGPT security settings\n",
			status);
		cJSON_Delete(device);
		return 1;
	}
	if (cJSON_IsNumber(get(device, "interval")))
		interval = get(device, "interval")->valueint;
	else if (*field(device, "interval"))
		interval = atoi(field(device, "interval"));
	if (interval < 1)
		interval = 5;
	if (interval > 60)
		interval = 60;
	fprintf(stderr, "Open %s/codex/device and enter: %s\n", auth_base, code);
	pollreq = cJSON_CreateObject();
	copy(pollreq, "device_auth_id", get(device, "device_auth_id"));
	cJSON_AddStringToObject(pollreq, "user_code", code);
	cJSON_Delete(device);
	deadline = milliseconds() + 900000;
	while (!stopped && milliseconds() < deadline) {
		tokens = auth_post("/api/accounts/deviceauth/token", pollreq, 0, &status);
		if (status == 200)
			break;
		cJSON_Delete(tokens);
		tokens = NULL;
		if (status != 403 && status != 404)
			break;
		poll(NULL, 0, interval * 1000);
	}
	cJSON_Delete(pollreq);
	if (!tokens || !*field(tokens, "authorization_code")
	    || !*field(tokens, "code_verifier")) {
		fprintf(stderr, "device login did not complete (HTTP %ld)\n", status);
		cJSON_Delete(tokens);
		return 1;
	}
	req = cJSON_CreateObject();
	cJSON_AddStringToObject(req, "client_id", client_id);
	cJSON_AddStringToObject(req, "grant_type", "authorization_code");
	cJSON_AddStringToObject(req, "code", field(tokens, "authorization_code"));
	copy(req, "code_verifier", get(tokens, "code_verifier"));
	cJSON_AddStringToObject(req, "redirect_uri",
				"https://auth.openai.com/deviceauth/callback");
	cJSON_Delete(tokens);
	tokens = auth_post("/oauth/token", req, 1, &status);
	cJSON_Delete(req);
	auth = status == 200 ? token_auth(tokens, NULL) : NULL;
	cJSON_Delete(tokens);
	if (!auth || save_auth(auth)) {
		fputs("could not obtain or save subscription credentials\n", stderr);
		cJSON_Delete(auth);
		return 1;
	}
	cJSON_Delete(auth);
	fprintf(stderr, "Login saved to %s\n", auth_file);
	return 0;
}

/* Loopback listener that catches the OAuth redirect for a browser callback. */
static int code_listener(void)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
	struct sockaddr_in addr;
	struct timeval idle = {
		300, 0
	};
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)code_port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof(idle))
	    || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes))
	    || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 1)) {
		if (fd >= 0)
			close(fd);
		snprintf(error_text, sizeof(error_text),
			 "cannot listen on 127.0.0.1:%d: %s", code_port, strerror(errno));
		return -1;
	}
	return fd;
}

/* Read one HTTP GET request line and return its target path. */
static int read_request_target(int fd, char *out, size_t size)
{
	char request[HEADER_MAX], *start = NULL, *stop = NULL;
	size_t n = 0;
	ssize_t k;
	*out = 0;
	while (n + 1 < sizeof(request)) {
		k = recv(fd, request + n, 1, 0);
		if (k == 1) {
			if (request[n] == '\n')
				break;
			n++;
			continue;
		}
		if (k < 0 && errno == EINTR)
			continue;
		return -1;
	}
	request[n] = 0;
	if (strncmp(request, "GET ", 4))
		return -1;
	start = request + 4;
	stop = start;
	while (*stop && *stop != ' ' && *stop != '\r' && *stop != '\n')
		stop++;
	if (stop == start || (size_t)(stop - start) >= size)
		return -1;
	memcpy(out, start, (size_t)(stop - start));
	out[stop - start] = 0;
	return 0;
}

static void finish_page(int fd)
{
	static const char page[] = "HTTP/1.1 200 OK\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"Connection: close\r\n\r\n"
		"<!doctype html><title>codex-proxy</title>"
		"<p>Anthropic login captured. Return to the terminal.</p>\r\n";
	sendall(fd, page, sizeof(page) - 1);
}


static int capture_query(char *query, char *out, size_t size)
{
	char *value = strstr(query, "code=");
	size_t n;
	if (!value || strchr(value, '%'))
		return 0;
	value += 5;
	n = strcspn(value, "& \r\n");
	if (!n || n >= size)
		return 0;
	memcpy(out, value, n);
	out[n] = 0;
	return 1;
}

static int wait_for_code(int listener, const char *state, char *code, size_t size)
{
	long long deadline = milliseconds() + 600000;
	char target[1024], *query;
	int fd;
	while (!stopped && milliseconds() < deadline) {
		fd = accept(listener, NULL, 0);
		if (fd < 0) {
			if (errno == EINTR)
				continue;
			snprintf(error_text, sizeof(error_text),
				 "login listener failed: %s", strerror(errno));
			return -1;
		}
		if (!read_request_target(fd, target, sizeof(target))) {
			query = strchr(target, '?');
			if (query && strstr(target, state)
			    && capture_query(query, code, size)) {
				finish_page(fd);
				close(fd);
				return 0;
			}
		}
		close(fd);
	}
	snprintf(error_text, sizeof(error_text), "no authorization code received");
	return -1;
}

static int anthropic_login(void)
{
	char verifier[65], challenge[65], state[65], target[HEADER_MAX], code[512];
	char *encoded;
	struct buf url = {0};
	cJSON *req, *tokens, *auth = NULL;
	CURL *curl = curl_easy_init();
	long status = 0;
	int listener = -1, result = 1;
	if (!curl) {
		snprintf(error_text, sizeof(error_text), "cannot initialize HTTPS");
		fputs(error_text, stderr);
		return 1;
	}
	encoded = curl_easy_escape(curl, anthropic_scopes, 0);
	if (!encoded) {
		snprintf(error_text, sizeof(error_text), "cannot encode OAuth scopes");
		fputs(error_text, stderr);
		goto done;
	}
	if (manual_code) {
		snprintf(anthropic_redirect, sizeof(anthropic_redirect), "%s",
			 anthropic_manual_redirect);
	} else {
		listener = code_listener();
		if (listener < 0) {
			fputs(error_text, stderr);
			fputs("; retry with --manual\n", stderr);
			goto done;
		}
		snprintf(anthropic_redirect, sizeof(anthropic_redirect),
			 "http://localhost:%d/callback", code_port);
	}
	pkce_token(verifier);
	pkce_value(verifier, challenge);
	pkce_token(state);
	append(&url, anthropic_authorize, strlen(anthropic_authorize));
	append(&url, "?code=true&client_id=", 21);
	append(&url, anthropic_client_id, strlen(anthropic_client_id));
	append(&url, "&response_type=code&redirect_uri=", 33);
	encoded = curl_easy_escape(curl, anthropic_redirect, 0);
	if (!encoded) {
		snprintf(error_text, sizeof(error_text), "cannot encode OAuth value");
		fputs(error_text, stderr);
		goto done;
	}
	append(&url, encoded, strlen(encoded));
	append(&url, "&scope=", 7);
	encoded = curl_easy_escape(curl, anthropic_scopes, 0);
	if (!encoded) {
		snprintf(error_text, sizeof(error_text), "cannot encode OAuth value");
		fputs(error_text, stderr);
		goto done;
	}
	append(&url, encoded, strlen(encoded));
	append(&url, "&code_challenge=", 16);
	append(&url, challenge, strlen(challenge));
	append(&url, "&code_challenge_method=S256&state=", 34);
	append(&url, state, strlen(state));
	fprintf(stderr, "Anthropic login: open this URL in a browser\n\n%s\n\n", url.s);
	if (!manual_code)
		fprintf(stderr, "or rerun with --manual and paste the redirect URL\n");
	if (manual_code) {
		fputs("Redirect URL: ", stderr);
		fflush(stderr);
		if (!fgets(target, sizeof(target), stdin) || ferror(stdin)) {
			fputs("no redirect URL entered\n", stderr);
			goto done;
		}
		target[strcspn(target, "\r\n")] = 0;
		encoded = strchr(target, '?');
		if (!encoded || !capture_query(encoded, code, sizeof(code))) {
			fputs("the pasted URL carries no code= parameter\n", stderr);
			goto done;
		}
		if (!*state || !strstr(target, state)) {
			fputs("state mismatch; start the login again\n", stderr);
			goto done;
		}
	} else if (wait_for_code(listener, state, code, sizeof(code))) {
		fputs(error_text, stderr);
		fputc('\n', stderr);
		goto done;
	}
	req = cJSON_CreateObject();
	cJSON_AddStringToObject(req, "grant_type", "authorization_code");
	cJSON_AddStringToObject(req, "code", code);
	cJSON_AddStringToObject(req, "redirect_uri", anthropic_redirect);
	cJSON_AddStringToObject(req, "client_id", anthropic_client_id);
	cJSON_AddStringToObject(req, "code_verifier", verifier);
	cJSON_AddStringToObject(req, "state", state);
	tokens = token_request(anthropic_token_url, req, &status);
	cJSON_Delete(req);
	if (!tokens) {
		fputs(error_text, stderr);
		fputc('\n', stderr);
		goto done;
	}
	if (status < 200 || status > 299) {
		fprintf(stderr, "token exchange failed (HTTP %ld): %s\n", status,
			field(tokens, "error"));
		cJSON_Delete(tokens);
		goto done;
	}
	auth = anthropic_token_auth(tokens, NULL, verifier);
	cJSON_Delete(tokens);
	if (!auth) {
		fputs("the token reply has no usable access token\n", stderr);
		goto done;
	}
	if (save_auth(auth)) {
		fputs("could not save credentials\n", stderr);
		cJSON_Delete(auth);
		goto done;
	}
	cJSON_Delete(auth);
	fprintf(stderr, "Login saved to %s\n", auth_file);
	result = 0;
done:
	if (listener >= 0)
		close(listener);
	free(url.s);
	curl_easy_cleanup(curl);
	return result;
}
static cJSON *message_item(const char *role, const char *text)
{
	cJSON *j = cJSON_CreateObject(), *parts, *part;
	cJSON_AddStringToObject(j, "type", "message");
	cJSON_AddStringToObject(j, "role", role);
	parts = cJSON_AddArrayToObject(j, "content");
	part = cJSON_CreateObject();
	cJSON_AddStringToObject(part, "type", !strcmp(role,
				"assistant") ? "output_text" : "input_text");
	cJSON_AddStringToObject(part, "text", text);
	cJSON_AddItemToArray(parts, part);
	return j;
}

/* Text-only Chat Completions; the caller owns execution and conversation history. */
static cJSON *translate_request(cJSON *chat)
{
	cJSON *req = cJSON_CreateObject(), *input = cJSON_AddArrayToObject(req,
		     "input");
	cJSON *tools = cJSON_AddArrayToObject(req, "tools"), *m, *t, *fn, *item,
	       *content;
	cJSON *messages = get(chat, "messages"), *extra;
	const char *name = *field(chat, "model") ? field(chat, "model") : model, *role;
	struct buf instructions = {0};
	const char *error = "invalid messages or tools";
	if (!cJSON_IsObject(chat) || !name || !*name || !cJSON_IsArray(messages)
	    || !messages->child) {
		error = "model and a nonempty messages array are required (or set --model)";
		goto fail;
	}
	if (get(chat, "stream") && !cJSON_IsFalse(get(chat, "stream"))) {
		error = "this adapter supports stream:false only";
		goto fail;
	}
	/* Reject controls we cannot honor rather than silently changing their meaning. */
	cJSON_ArrayForEach(extra, chat) {
		if (!strcmp(extra->string, "model") || !strcmp(extra->string, "messages") ||
		    !strcmp(extra->string, "tools") || !strcmp(extra->string, "stream") ||
		    !strcmp(extra->string, "reasoning_effort")
		    || !strcmp(extra->string, "tool_choice"))
			continue;
		error = "unsupported request field; supported: model, messages, tools, stream, reasoning_effort, tool_choice";
		goto fail;
	}
	cJSON_AddStringToObject(req, "model", name);
	cJSON_AddBoolToObject(req, "store", 0);
	cJSON_AddBoolToObject(req, "stream", 1);
	cJSON_AddBoolToObject(req, "parallel_tool_calls", 1);
	cJSON_AddItemToArray(cJSON_AddArrayToObject(req, "include"),
			     cJSON_CreateString("reasoning.encrypted_content"));
	cJSON_ArrayForEach(m, messages) {
		role = field(m, "role");
		content = get(m, "content");
		if (content && !cJSON_IsString(content) && !cJSON_IsNull(content)) {
			error = "message content must be text or null";
			goto fail;
		}
		if (!strcmp(role, "system")) {
			if (instructions.n)
				append(&instructions, "\n\n", 2);
			append(&instructions, str(content), strlen(str(content)));
		} else if (!strcmp(role, "tool")) {
			if (!*field(m, "tool_call_id") || !cJSON_IsString(content))
				goto fail;
			item = cJSON_CreateObject();
			cJSON_AddStringToObject(item, "type", "function_call_output");
			copy(item, "call_id", get(m, "tool_call_id"));
			copy(item, "output", content);
			cJSON_AddItemToArray(input, item);
		} else if (!strcmp(role, "user") || !strcmp(role, "developer")
			   || !strcmp(role, "assistant")) {
			extra = get(m, "codex_reasoning");
			if (extra && (!cJSON_IsArray(extra) || strcmp(role, "assistant")))
				goto fail;
			cJSON_ArrayForEach(t, extra) {
				if (strcmp(field(t, "type"), "reasoning") || !*field(t, "encrypted_content"))
					goto fail;
				cJSON_AddItemToArray(input, cJSON_Duplicate(t, 1));
			}
			if (cJSON_IsString(content) && (*str(content) || strcmp(role, "assistant")))
				cJSON_AddItemToArray(input, message_item(role, str(content)));
			extra = get(m, "tool_calls");
			if (extra && (!cJSON_IsArray(extra) || strcmp(role, "assistant")))
				goto fail;
			cJSON_ArrayForEach(t, extra) {
				fn = get(t, "function");
				if (strcmp(field(t, "type"), "function") || !*field(t, "id") ||
				    !*field(fn, "name") || !cJSON_IsString(get(fn, "arguments")))
					goto fail;
				item = cJSON_CreateObject();
				cJSON_AddStringToObject(item, "type", "function_call");
				copy(item, "call_id", get(t, "id"));
				copy(item, "name", get(fn, "name"));
				copy(item, "arguments", get(fn, "arguments"));
				cJSON_AddItemToArray(input, item);
			}
		} else
			goto fail;
	}
	cJSON_AddStringToObject(req, "instructions",
				instructions.s ? instructions.s : "");
	extra = get(chat, "tools");
	if (extra && !cJSON_IsArray(extra))
		goto fail;
	cJSON_ArrayForEach(t, extra) {
		fn = get(t, "function");
		if (strcmp(field(t, "type"), "function") || !*field(fn, "name")
		    || !cJSON_IsObject(get(fn, "parameters")))
			goto fail;
		item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "type", "function");
		copy(item, "name", get(fn, "name"));
		copy(item, "description", get(fn, "description"));
		copy(item, "parameters", get(fn, "parameters"));
		copy(item, "strict", get(fn, "strict"));
		cJSON_AddItemToArray(tools, item);
	}
	extra = get(chat, "tool_choice");
	if (extra) {
		if (strcmp(str(extra), "auto") && strcmp(str(extra), "none")
		    && strcmp(str(extra), "required")) {
			error = "tool_choice must be auto, none, or required";
			goto fail;
		}
		copy(req, "tool_choice", extra);
	}
	item = cJSON_AddObjectToObject(req, "reasoning");
	cJSON_AddStringToObject(item, "summary", "auto");
	extra = get(chat, "reasoning_effort");
	if (extra && !cJSON_IsString(extra)) {
		error = "reasoning_effort must be a string";
		goto fail;
	}
	if (extra)
		copy(item, "effort", extra);
	else if (reasoning_effort)
		cJSON_AddStringToObject(item, "effort", reasoning_effort);
	free(instructions.s);
	return req;
fail:
	snprintf(error_text, sizeof(error_text), "%s", error);
	free(instructions.s);
	cJSON_Delete(req);
	return NULL;
}

/* Diagnostics are opt-in: upstream messages can contain private request data.
 * Enable only with access-controlled stderr: CODEX_PROXY_DEBUG_ERRORS=1.
 * Bound and escape each value to prevent multiline/terminal log injection.
 */
static void diagnostic_value(char *out, size_t size, const char *value)
{
	static const char hex[] = "0123456789abcdef";
	size_t n = 0;
	const unsigned char *p = (const unsigned char *)value;
	while (*p && n + 7 < size) {
		unsigned char c = *p++;
		if (c == '"' || c == '\\') {
			out[n++] = '\\';
			out[n++] = (char)c;
		} else if (c < 32 || c >= 127) {
			out[n++] = '\\';
			out[n++] = 'x';
			out[n++] = hex[c >> 4];
			out[n++] = hex[c & 15];
		} else
			out[n++] = (char)c;
	}
	if (*p) {
		out[n++] = '.';
		out[n++] = '.';
		out[n++] = '.';
	}
	out[n] = 0;
}

static void log_upstream_event(cJSON *event, const char *summary)
{
	const char *debug = getenv("CODEX_PROXY_DEBUG_ERRORS");
	cJSON *response = get(event, "response"), *detail = get(response, "error");
	char code[96], message[512], param[96], reason[96];
	if (!debug || strcmp(debug, "1")) {
		fprintf(stderr, "%s (set CODEX_PROXY_DEBUG_ERRORS=1 for details; "
				"diagnostics may contain sensitive data)\n", summary);
		return;
	}
	if (!cJSON_IsObject(detail))
		detail = get(event, "error");
	if (!cJSON_IsObject(detail))
		detail = event;
	diagnostic_value(code, sizeof(code), field(detail, "code"));
	diagnostic_value(message, sizeof(message), field(detail, "message"));
	diagnostic_value(param, sizeof(param), field(detail, "param"));
	diagnostic_value(reason, sizeof(reason), field(get(response,
			 "incomplete_details"), "reason"));
	fprintf(stderr, "%s: code=\"%s\" message=\"%s\" param=\"%s\" reason=\"%s\"\n",
		summary, code, message, param, reason);
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
		end = strchr(line, '\n');
		n = end ? (size_t)(end - line) : strlen(line);
		if (n && line[n - 1] == '\r')
			n--;
		if (n >= 5 && !memcmp(line, "data:", 5)) {
			size_t start = n > 5 && line[5] == ' ' ? 6 : 5;
			if (data.n)
				append(&data, "\n", 1);
			append(&data, line + start, n - start);
		}
		if (!n || !end) {
			event = data.s ? cJSON_ParseWithOpts(data.s, NULL, 1) : NULL;
			if (!strcmp(field(event, "type"), "response.output_item.done")) {
				item = get(event, "item");
				index = get(event, "output_index");
				if (!cJSON_IsObject(item) || !cJSON_IsNumber(index) ||
				    index->valuedouble < 0 || index->valuedouble > INT_MAX ||
				    index->valuedouble != index->valueint) {
					cJSON_Delete(event);
					error = "invalid completed output item";
					goto fail;
				}
				position = 0;
				cJSON_ArrayForEach(saved, items) {
					if (get(saved, "output_index")->valueint >= index->valueint)
						break;
					position++;
				}
				if (saved && get(saved, "output_index")->valueint == index->valueint)
					cJSON_ReplaceItemInArray(items, position, cJSON_Duplicate(event, 1));
				else
					cJSON_InsertItemInArray(items, position, cJSON_Duplicate(event, 1));
			} else if (!strcmp(field(event, "type"), "response.completed")) {
				cJSON_Delete(response);
				response = cJSON_Duplicate(get(event, "response"), 1);
			} else if (!strcmp(field(event, "type"), "response.failed")) {
				error = "upstream response failed";
				log_upstream_event(event, error);
				cJSON_Delete(event);
				goto fail;
			} else if (!strcmp(field(event, "type"), "response.incomplete")) {
				error = "upstream response incomplete";
				log_upstream_event(event, error);
				cJSON_Delete(event);
				goto fail;
			} else if (!strcmp(field(event, "type"), "error")) {
				error = "upstream response error";
				log_upstream_event(event, error);
				cJSON_Delete(event);
				goto fail;
			}
			cJSON_Delete(event);
			free(data.s);
			data = (struct buf) {
				0
			};
		}
		line = end ? end + 1 : NULL;
	}
	if (!response)
		goto fail;
	output = get(response, "output");
	/* Codex may send the actual output only in preceding output_item.done events. */
	if ((!output || cJSON_IsNull(output) || (cJSON_IsArray(output)
			&& !output->child)) && items->child) {
		cJSON_DeleteItemFromObject(response, "output");
		output = cJSON_AddArrayToObject(response, "output");
		expected = 0;
		cJSON_ArrayForEach(saved, items) {
			if (get(saved, "output_index")->valueint != expected++) {
				error = "upstream stream is missing a completed output item";
				goto fail;
			}
			cJSON_AddItemToArray(output, cJSON_Duplicate(get(saved, "item"), 1));
		}
	}
	free(data.s);
	cJSON_Delete(items);
	return response;
fail:
	snprintf(error_text, sizeof(error_text), "%s", error);
	free(data.s);
	cJSON_Delete(items);
	cJSON_Delete(response);
	return NULL;
}

static cJSON *translate_response(cJSON *response)
{
	cJSON *chat = cJSON_CreateObject(), *choices = cJSON_AddArrayToObject(chat,
		      "choices");
	cJSON *choice = cJSON_CreateObject(), *message = cJSON_AddObjectToObject(choice,
			"message");
	cJSON *calls = cJSON_AddArrayToObject(message, "tool_calls"),
	       *reasoning = cJSON_CreateArray();
	cJSON *item, *part, *call, *fn, *usage, *u;
	struct buf text = {0}, summary = {0};
	if (strcmp(field(response, "status"), "completed")
	    || !cJSON_IsArray(get(response, "output")))
		goto fail;
	cJSON_AddItemToArray(choices, choice);
	cJSON_AddStringToObject(message, "role", "assistant");
	cJSON_AddStringToObject(chat, "id", field(response, "id"));
	cJSON_AddStringToObject(chat, "object", "chat.completion");
	copy(chat, "model", get(response, "model"));
	copy(chat, "created", get(response, "created_at"));
	cJSON_AddNumberToObject(choice, "index", 0);
	cJSON_ArrayForEach(item, get(response, "output")) {
		if (!strcmp(field(item, "type"), "message")) {
			cJSON_ArrayForEach(part, get(item, "content")) {
				if (!strcmp(field(part, "type"), "output_text"))
					append(&text, field(part, "text"), strlen(field(part, "text")));
				else if (!strcmp(field(part, "type"), "refusal"))
					append(&text, field(part, "refusal"), strlen(field(part, "refusal")));
			}
		} else if (!strcmp(field(item, "type"), "function_call")) {
			if (!*field(item, "call_id") || !*field(item, "name")
			    || !cJSON_IsString(get(item, "arguments")))
				goto fail;
			call = cJSON_CreateObject();
			copy(call, "id", get(item, "call_id"));
			cJSON_AddStringToObject(call, "type", "function");
			fn = cJSON_AddObjectToObject(call, "function");
			copy(fn, "name", get(item, "name"));
			copy(fn, "arguments", get(item, "arguments"));
			cJSON_AddItemToArray(calls, call);
		} else if (!strcmp(field(item, "type"), "reasoning")) {
			if (*field(item, "encrypted_content"))
				cJSON_AddItemToArray(reasoning, cJSON_Duplicate(item, 1));
			cJSON_ArrayForEach(part, get(item, "summary")) append(&summary, field(part,
					"text"), strlen(field(part, "text")));
		} else
			goto fail;
	}
	if (!text.n && !calls->child)
		goto fail;
	cJSON_AddStringToObject(message, "content", text.s ? text.s : "");
	cJSON_AddStringToObject(choice, "finish_reason",
				calls->child ? "tool_calls" : "stop");
	if (!calls->child)
		cJSON_DeleteItemFromObject(message, "tool_calls");
	if (summary.n)
		cJSON_AddStringToObject(message, "reasoning_content", summary.s);
	if (reasoning->child)
		cJSON_AddItemToObject(message, "codex_reasoning", reasoning);
	else
		cJSON_Delete(reasoning);
	u = get(response, "usage");
	if (cJSON_IsObject(u)) {
		usage = cJSON_AddObjectToObject(chat, "usage");
		copy(usage, "prompt_tokens", get(u, "input_tokens"));
		copy(usage, "completion_tokens", get(u, "output_tokens"));
		copy(usage, "total_tokens", get(u, "total_tokens"));
		copy(usage, "prompt_tokens_details", get(u, "input_tokens_details"));
		copy(usage, "completion_tokens_details", get(u, "output_tokens_details"));
	}
	free(text.s);
	free(summary.s);
	return chat;
fail:
	if (!choices->child)
		cJSON_Delete(choice);
	cJSON_Delete(chat);
	cJSON_Delete(reasoning);
	free(text.s);
	free(summary.s);
	snprintf(error_text, sizeof(error_text),
		 "upstream returned an unsupported or incomplete response");
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
	if (!req) {
		reply(fd, 400, failure(error_text));
		return;
	}
	encoded = cJSON_PrintUnformatted(req);
	cJSON_Delete(req);
	expiry = cJSON_GetNumberValue(get(*auth, "expires_at"));
	if (expiry > 0 && expiry < (double)time(NULL) + 60 && refresh(auth)) {
		free(encoded);
		reply(fd, 401,
		      failure("subscription refresh failed; stop the server and run login"));
		return;
	}
	for (attempt = 0; attempt < 2; attempt++) {
		rc = post(upstream, encoded, "application/json", *auth, &body, &status, fd);
		if (rc || status != 401 || attempt || refresh(auth))
			break;
		free(body.s);
		body = (struct buf) {
			0
		};
	}
	free(encoded);
	if (rc) {
		free(body.s);
		reply(fd, 502, failure(error_text));
		return;
	}
	if (status != 200) {
		/* Do not echo untrusted upstream bodies, which may contain credentials. */
		snprintf(error_text, sizeof(error_text),
			 "subscription backend returned HTTP %ld%s", status,
			 status == 401 ? "; stop the server and run login" : "");
		free(body.s);
		reply(fd, status >= 400
		      && status <= 599 ? (int)status : 502, failure(error_text));
		return;
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
	if (!strncmp(s, "127.0.0.1", 9))
		p = s + 9;
	else if (!strncmp(s, "localhost", 9))
		p = s + 9;
	else
		return 0;
	if (!*p)
		return 1;
	if (*p++ != ':' || !*p)
		return 0;
	for (; *p; p++)
		if (*p < '0' || *p > '9')
			return 0;
	return 1;
}

static void serve_client(int fd, cJSON **auth)
{
	struct buf b = {0};
	char chunk[4096], method[16], path[128], version[16], junk, *end, *line, *save,
	     *colon, *value;
	char *authorization = NULL, *host = NULL;
	size_t header_size, length = 0;
	ssize_t n;
	int have_length = 0, expect_continue = 0, status = 400;
	int anthropic_api = !strcmp(backend, "anthropic");
	long long deadline = milliseconds() + 15000;
	const char *error = "invalid HTTP request";
	cJSON *j;
	while (waitfd(fd, POLLIN, deadline)) {
		n = recv(fd, chunk, sizeof(chunk), 0);
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (n <= 0)
			goto done;
		if (memchr(chunk, 0, (size_t)n))
			goto bad;
		append(&b, chunk, (size_t)n);
		end = strstr(b.s, "\r\n\r\n");
		if (end)
			break;
		if (b.n > HEADER_MAX)
			goto bad;
	}
	if (!b.s || !(end = strstr(b.s, "\r\n\r\n")))
		goto bad;
	header_size = (size_t)(end - b.s) + 4;
	if (header_size > HEADER_MAX)
		goto bad;
	*end = 0;
	line = strtok_r(b.s, "\r\n", &save);
	if (!line || sscanf(line, "%15s %127s %15s %c", method, path, version,
			    &junk) != 3 ||
	    (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0")))
		goto bad;
	if ((end = strchr(path, '?')))
		*end = 0;
	while ((line = strtok_r(NULL, "\r\n", &save))) {
		colon = strchr(line, ':');
		if (!colon)
			goto bad;
		*colon = 0;
		value = colon + 1;
		while (*value == ' ' || *value == '	')
			value++;
		end = value + strlen(value);
		while (end > value && (end[-1] == ' ' || end[-1] == '	'))
			*--end = 0;
		if (!strcasecmp(line, "Content-Length")) {
			if (have_length++ || !*value)
				goto bad;
			for (; *value; value++) {
				if (*value < '0' || *value > '9' || length > BODY_MAX / 10)
					goto bad;
				length = length * 10 + (size_t)(*value - '0');
			}
			if (length > BODY_MAX) {
				status = 413;
				goto bad;
			}
		} else if (!strcasecmp(line, "Transfer-Encoding"))
			goto bad;
		else if (!strcasecmp(line, "Origin")) {
			status = 403;
			error = "browser requests are not accepted";
			goto bad;
		} else if (!strcasecmp(line, "Host")) {
			if (host)
				goto bad;
			host = value;
		} else if (!strcasecmp(line, "Authorization")) {
			if (authorization)
				goto bad;
			authorization = value;
		} else if (!strcasecmp(line, "Expect")) {
			if (strcasecmp(value, "100-continue")) {
				status = 417;
				goto bad;
			}
			expect_continue = 1;
		}
	}
	if (!host || !*host) {
		goto bad;
	}
	if (!strcmp(method, "GET") && !strcmp(path, "/health")) {
		j = cJSON_CreateObject();
		cJSON_AddStringToObject(j, "status", "ok");
		reply(fd, 200, j);
		goto done;
	}
	if (!authorization || strncmp(authorization, "Bearer ", 7)
	    || strcmp(authorization + 7, local_key)) {
		status = 401;
		error = "invalid local proxy key";
		goto bad;
	}
	if (!strcmp(method, "GET") && !strcmp(path, "/v1/models")) {
		j = cJSON_CreateObject();
		cJSON_AddStringToObject(j, "object", "list");
		cJSON *data = cJSON_AddArrayToObject(j, "data"), *entry;
		if (model) {
			entry = cJSON_CreateObject();
			cJSON_AddStringToObject(entry, "id", model);
			cJSON_AddStringToObject(entry, "object", "model");
			cJSON_AddItemToArray(data, entry);
		}
		reply(fd, 200, j);
		goto done;
	}
	if (strcmp(path, "/v1/chat/completions")
	    && !(anthropic_api && (!strcmp(path, "/v1/messages")
		|| !strcmp(path, "/v1/messages/count_tokens")))) {
		status = 404;
		error = "unknown endpoint";
		goto bad;
	}
	if (strcmp(method, "POST")) {
		status = 405;
		error = "use POST";
		goto bad;
	}
	if (!have_length) {
		status = 411;
		error = "Content-Length required";
		goto bad;
	}
	if (b.n - header_size > length) {
		status = 400;
		error = "body longer than Content-Length";
		goto bad;
	}
	if (expect_continue && b.n - header_size < length)
		sendall(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
	while (b.n - header_size < length && waitfd(fd, POLLIN, deadline)) {
		size_t remaining = length - (b.n - header_size);
		n = recv(fd, chunk, remaining < sizeof(chunk) ? remaining : sizeof(chunk), 0);
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (n <= 0)
			goto done;
		if (memchr(chunk, 0, (size_t)n))
			goto bad;
		append(&b, chunk, (size_t)n);
	}
	if (b.n - header_size != length) {
		status = 408;
		error = "request body timed out";
		goto bad;
	}
	j = cJSON_ParseWithOpts(b.s + header_size, NULL, 1);
	if (!j)
		goto bad;
	if (anthropic_api)
		complete_anthropic(fd, path, j, auth);
	else
		complete(fd, j, auth);
	cJSON_Delete(j);
	goto done;
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
	append(&path, auth_file, strlen(auth_file));
	append(&path, ".lock", 5);
	fd = open(path.s, O_RDWR | O_CREAT, 0600);
	free(path.s);
	memset(&lock, 0, sizeof(lock));
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	if (fd < 0 || fcntl(fd, F_SETLK, &lock) < 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	return fd;
}

static int valid_url(const char *url)
{
	const char *end;
	char host[128];
	size_t n;
	if (!strncmp(url, "https://", 8))
		return url[8] && !strpbrk(url, "\r\n");
	if (strncmp(url, "http://", 7))
		return 0;
	end = strchr(url + 7, '/');
	n = end ? (size_t)(end - url - 7) : strlen(url + 7);
	if (n >= sizeof(host))
		return 0;
	memcpy(host, url + 7, n);
	host[n] = 0;
	return local_host(host);
}

static void usage(void)
{
	fputs("Usage: codex-proxy login|serve [options]\n"
	      "  --auth-file PATH    Credentials (default: $HOME/.codex-proxy.json)\n"
	      "  --model MODEL       Default model for requests without model\n"
	      "  --reasoning-effort LEVEL  Default effort; requests can override it\n"
	      "  --port PORT         Listener port (default: 8080)\n"
	      "  --host IPv4         Listener address (default: 127.0.0.1)\n"
	      "  --timeout SECONDS   Upstream timeout (default: 300)\n"
	      "  --upstream URL     Responses endpoint override\n"
	      "  --backend NAME      codex (default) or anthropic\n"
	      "  --token-url URL     Anthropic token endpoint override\n"
	      "  --authorize-url URL Anthropic authorize endpoint override\n"
	      "  --code-port PORT    Login redirect port for Anthropic login (54222)\n"
	      "  --manual            Anthropic login without a loopback listener\n"
	      "  --auth-base URL    Authentication service override\n"
	      "  --thinking-budget N   Anthropic thinking budget (1024-512000)\n"
	      "  --upstream-anthropic URL Anthropic Messages endpoint override\n"
	      "CODEX_PROXY_KEY sets the local HTTP key (default: local).\n", stderr);
}

int main(int argc, char **argv)
{
	int i, port = 8080, lockfd, server, fd, yes = 1, result = 0;
	long value;
	char *end;
	const char *home = getenv("HOME"), *key = getenv("CODEX_PROXY_KEY");
	const char *bind_ip = "127.0.0.1";
	struct buf default_path = {0};
	struct sockaddr_in addr;
	struct sigaction sa;
	struct in_addr bind_addr;
	cJSON *auth;
	cJSON_Hooks hooks = {allocate, free};
	cJSON_InitHooks(&hooks);
	if (argc < 2 || (strcmp(argv[1], "login") && strcmp(argv[1], "serve"))) {
		usage();
		return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 1;
	}
	if (key && *key)
		local_key = key;
	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--manual"))
			continue;
		manual_code = 1;
		for (; i < argc - 1; i++)
			argv[i] = argv[i + 1];
		argc--;
		break;
	}
	for (i = 2; i < argc; i += 2) {
		if (i + 1 == argc) {
			usage();
			return 1;
		}
		if (!strcmp(argv[i], "--auth-file"))
			auth_file = argv[i + 1];
		else if (!strcmp(argv[i], "--model"))
			model = argv[i + 1];
		else if (!strcmp(argv[i], "--reasoning-effort")) {
			if (!*argv[i + 1]) {
				usage();
				return 1;
			}
			reasoning_effort = argv[i + 1];
		} else if (!strcmp(argv[i], "--upstream"))
			upstream = argv[i + 1];
		else if (!strcmp(argv[i], "--auth-base"))
			auth_base = argv[i + 1];
		else if (!strcmp(argv[i], "--backend")) {
			if (strcmp(argv[i + 1], "codex")
			    && strcmp(argv[i + 1], "anthropic")) {
				usage();
				return 1;
			}
			backend = argv[i + 1];
		} else if (!strcmp(argv[i], "--upstream-anthropic"))
			anthropic_upstream = argv[i + 1];
		else if (!strcmp(argv[i], "--token-url"))
			anthropic_token_url = argv[i + 1];
		else if (!strcmp(argv[i], "--authorize-url"))
			anthropic_authorize = argv[i + 1];
		else if (!strcmp(argv[i], "--host"))
			bind_ip = argv[i + 1];
		else if (!strcmp(argv[i], "--thinking-budget")) {
			errno = 0;
			value = strtol(argv[i + 1], &end, 10);
			if (errno || *end || value < 1024 || value > 512000) {
				usage();
				return 1;
			}
			thinking_budget = value;
		}
		else if (!strcmp(argv[i], "--port") || !strcmp(argv[i], "--timeout")
			   || !strcmp(argv[i], "--code-port")) {
			errno = 0;
			value = strtol(argv[i + 1], &end, 10);
			if (errno || *end || value < 1 || value > 65535) {
				usage();
				return 1;
			}
			if (!strcmp(argv[i], "--port"))
				port = (int)value;
			else if (!strcmp(argv[i], "--timeout"))
				timeout = value;
			else
				code_port = (int)value;
		} else {
			usage();
			return 1;
		}
	}

	if (!auth_file && home && *home) {
		const char *name = !strcmp(backend, "anthropic") ?
			"/.anthropic-proxy.json" : "/.codex-proxy.json";
		append(&default_path, home, strlen(home));
		append(&default_path, name, strlen(name));
		auth_file = default_path.s;
	}
	if (inet_pton(AF_INET, bind_ip, &bind_addr) != 1) {
		fprintf(stderr, "invalid --host IPv4 address: %s\n", bind_ip);
		return 1;
	}
	if (!auth_file || !*auth_file || !valid_url(upstream) || !valid_url(auth_base)
	    || !valid_url(anthropic_token_url) || !valid_url(anthropic_authorize)
	    || !valid_url(anthropic_upstream)
	    || strpbrk(local_key, "\r\n")) {
		fputs("invalid path, key, or URL (HTTPS or loopback HTTP required)\n", stderr);
		return 1;
	}
	umask(077);
	lockfd = lock_auth();
	if (lockfd < 0) {
		fputs("cannot lock credential file; check its directory and stop other instances first\n",
		      stderr);
		return 1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = onsignal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	if (curl_global_init(CURL_GLOBAL_DEFAULT))
		return 1;
	if (!strcmp(argv[1], "login")) {
		result = strcmp(backend, "anthropic") ? login() : anthropic_login();
		goto done;
	}
	auth = load_auth();
	if (!auth) {
		fputs("no usable subscription credentials; run codex-proxy login\n", stderr);
		result = 1;
		goto done;
	}
	server = socket(AF_INET, SOCK_STREAM, 0);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr = bind_addr;
	if (server < 0
	    || setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) ||
	    bind(server, (struct sockaddr *)&addr, sizeof(addr)) || listen(server, 8)) {
		perror("listen");
		result = 1;
	} else {
		fprintf(stderr, "Listening on http://%s:%d\n", bind_ip, port);
		while (!stopped) {
			fd = accept(server, NULL, NULL);
			if (fd < 0) {
				if (errno == EINTR)
					continue;
				perror("accept");
				result = 1;
				break;
			}
			fcntl(fd, F_SETFL, O_NONBLOCK);
			serve_client(fd, &auth);
			close(fd);
		}
	}
	if (server >= 0)
		close(server);
	cJSON_Delete(auth);
done:
	close(lockfd);
	curl_global_cleanup();
	free(default_path.s);
	if (stopped == SIGINT && isatty(STDERR_FILENO))
		fputc('\n', stderr);
	return result;
}
