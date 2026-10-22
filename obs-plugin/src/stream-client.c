#include "stream-client.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libkern/OSByteOrder.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define MAX_PACKET (32u * 1024u * 1024u)

/* Ejecuta argv[0] con argumentos; stdout a `out` (puede ser NULL). Devuelve el código de salida o -1. */
static int run_cmd(char *const argv[], char *out, size_t outsz)
{
	int pipefd[2];
	if (pipe(pipefd) != 0)
		return -1;

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addclose(&fa, pipefd[0]);
	posix_spawn_file_actions_addclose(&fa, pipefd[1]);

	pid_t pid;
	int rc = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(pipefd[1]);
	if (rc != 0) {
		close(pipefd[0]);
		return -1;
	}

	size_t len = 0;
	char discard[512];
	for (;;) {
		char *dst = discard;
		size_t room = sizeof(discard);
		if (out && len + 1 < outsz) {
			dst = out + len;
			room = outsz - len - 1;
		}
		ssize_t n = read(pipefd[0], dst, room);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		if (dst != discard)
			len += (size_t)n;
	}
	if (out && outsz)
		out[len] = '\0';
	close(pipefd[0]);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void trim(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
		s[--n] = '\0';
}

bool acam_find_adb(const char *override, char *out, size_t outsz)
{
	if (override && *override) {
		if (access(override, X_OK) == 0) {
			snprintf(out, outsz, "%s", override);
			return true;
		}
		return false;
	}

	const char *home = getenv("HOME");
	const char *sdk = getenv("ANDROID_HOME");
	char candidates[6][1024];
	int n = 0;
	if (sdk)
		snprintf(candidates[n++], sizeof(candidates[0]), "%s/platform-tools/adb", sdk);
	if (home)
		snprintf(candidates[n++], sizeof(candidates[0]), "%s/Library/Android/sdk/platform-tools/adb", home);
	snprintf(candidates[n++], sizeof(candidates[0]), "/opt/homebrew/bin/adb");
	snprintf(candidates[n++], sizeof(candidates[0]), "/usr/local/bin/adb");

	/* Último recurso: el adb que viaja dentro del plugin (…/Contents/MacOS/x → …/Contents/Resources/adb). */
	Dl_info info;
	if (dladdr((const void *)acam_find_adb, &info) && info.dli_fname) {
		char *slash = strrchr(info.dli_fname, '/');
		if (slash)
			snprintf(candidates[n++], sizeof(candidates[0]), "%.*s/../Resources/adb",
				 (int)(slash - info.dli_fname), info.dli_fname);
	}

	for (int i = 0; i < n; i++) {
		if (access(candidates[i], X_OK) == 0) {
			snprintf(out, outsz, "%s", candidates[i]);
			return true;
		}
	}
	return false;
}

static void device_name(const char *adb, const char *serial, const char *model, char *out, size_t outsz)
{
	char buf[256] = {0};
	char *argv[] = {(char *)adb, "-s", (char *)serial, "shell", "getprop", "ro.product.marketname", NULL};
	if (run_cmd(argv, buf, sizeof(buf)) == 0) {
		trim(buf);
		if (*buf) {
			snprintf(out, outsz, "%s", buf);
			return;
		}
	}
	/* `model:POCO_X7_Pro` → "POCO X7 Pro" */
	snprintf(out, outsz, "%s", model && *model ? model : serial);
	for (char *p = out; *p; p++)
		if (*p == '_')
			*p = ' ';
}

int acam_list_devices(const char *adb, struct acam_device *out, int max)
{
	static char buf[16384];
	char *argv[] = {(char *)adb, "devices", "-l", NULL};
	if (run_cmd(argv, buf, sizeof(buf)) != 0)
		return 0;

	int count = 0;
	char *save = NULL;
	for (char *line = strtok_r(buf, "\n", &save); line && count < max; line = strtok_r(NULL, "\n", &save)) {
		char serial[128], state[64];
		if (sscanf(line, "%127s %63s", serial, state) != 2 || strcmp(serial, "List") == 0 || serial[0] == '*')
			continue;

		char model[128] = {0};
		const char *m = strstr(line, "model:");
		if (m)
			sscanf(m + 6, "%127s", model);

		struct acam_device *d = &out[count++];
		snprintf(d->serial, sizeof(d->serial), "%s", serial);
		d->ready = strcmp(state, "device") == 0;
		if (d->ready)
			device_name(adb, serial, model, d->name, sizeof(d->name));
		else
			snprintf(d->name, sizeof(d->name), "%s (%s)", model[0] ? model : serial, state);
	}
	return count;
}

bool acam_launch_app(const char *adb, const char *serial)
{
	char *argv[] = {(char *)adb, "-s", (char *)serial, "shell", "am", "start", "-n", ACAM_ACTIVITY, NULL};
	char buf[1024];
	if (run_cmd(argv, buf, sizeof(buf)) != 0)
		return false;
	return strstr(buf, "Error") == NULL;
}

int acam_forward(const char *adb, const char *serial)
{
	char remote[32];
	snprintf(remote, sizeof(remote), "tcp:%d", ACAM_DEVICE_PORT);
	char *argv[] = {(char *)adb, "-s", (char *)serial, "forward", "tcp:0", remote, NULL};
	char buf[64] = {0};
	if (run_cmd(argv, buf, sizeof(buf)) != 0)
		return -1;
	int port = atoi(buf);
	return port > 0 ? port : -1;
}

void acam_forward_remove(const char *adb, const char *serial, int port)
{
	char local[32];
	snprintf(local, sizeof(local), "tcp:%d", port);
	char *argv[] = {(char *)adb, "-s", (char *)serial, "forward", "--remove", local, NULL};
	run_cmd(argv, NULL, 0);
}

void acam_set_recv_timeout(int fd, int timeout_ms)
{
	struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int acam_connect(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	int one = 1;
	int rcvbuf = 8 * 1024 * 1024;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool read_exact(int fd, void *dst, size_t n)
{
	uint8_t *p = dst;
	while (n) {
		ssize_t r = recv(fd, p, n, 0);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		p += r;
		n -= (size_t)r;
	}
	return true;
}

static bool write_all(int fd, const void *src, size_t n)
{
	const uint8_t *p = src;
	while (n) {
		ssize_t w = send(fd, p, n, 0);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return false;
		p += w;
		n -= (size_t)w;
	}
	return true;
}

int acam_handshake(int fd, const struct acam_request *req, struct acam_header *hdr, char *err, size_t errsz,
		   int timeout_ms)
{
	char json[512];
	int len = snprintf(json, sizeof(json),
			   "{\"width\":%d,\"height\":%d,\"fps\":%d,\"bitrate\":%d,\"codec\":\"%s\","
			   "\"facing\":\"%s\",\"camera_id\":\"%s\",\"stabilization\":%s,\"zoom\":%.2f}\n",
			   req->width, req->height, req->fps, req->bitrate, req->codec == ACAM_CODEC_H264 ? "h264" : "hevc",
			   req->front ? "front" : "back", req->camera_id, req->stabilization ? "true" : "false",
			   req->zoom < 1 ? 1.0 : req->zoom);
	if (!write_all(fd, json, (size_t)len))
		return -1;

	acam_set_recv_timeout(fd, timeout_ms);
	uint8_t h[16];
	if (!read_exact(fd, h, sizeof(h)) || memcmp(h, "OCAM", 4) != 0)
		return -1;

	if (h[5] == 0xFF) {
		uint16_t mlen_be;
		char msg[512] = "error desconocido";
		if (read_exact(fd, &mlen_be, 2)) {
			uint16_t mlen = OSSwapBigToHostInt16(mlen_be);
			if (mlen >= sizeof(msg))
				mlen = sizeof(msg) - 1;
			if (read_exact(fd, msg, mlen))
				msg[mlen] = '\0';
		}
		snprintf(err, errsz, "%s", msg);
		return 0;
	}

	hdr->codec = h[5] == 1 ? ACAM_CODEC_HEVC : ACAM_CODEC_H264;
	hdr->fps = (h[6] << 8) | h[7];
	hdr->width = (h[8] << 8) | h[9];
	hdr->height = (h[10] << 8) | h[11];
	uint32_t zoom100 = ((uint32_t)h[12] << 24) | ((uint32_t)h[13] << 16) | ((uint32_t)h[14] << 8) | h[15];
	hdr->max_zoom = zoom100 >= 100 ? zoom100 / 100.0f : 1.0f;
	return 1;
}

bool acam_read_packet(int fd, uint8_t **buf, size_t *cap, int64_t *pts_us, uint32_t *flags, uint32_t *size)
{
	uint8_t h[16];
	if (!read_exact(fd, h, sizeof(h)))
		return false;

	uint64_t pts;
	uint32_t f, s;
	memcpy(&pts, h, 8);
	memcpy(&f, h + 8, 4);
	memcpy(&s, h + 12, 4);
	*pts_us = (int64_t)OSSwapBigToHostInt64(pts);
	*flags = OSSwapBigToHostInt32(f);
	*size = OSSwapBigToHostInt32(s);
	if (*size == 0 || *size > MAX_PACKET)
		return false;

	if (*cap < *size) {
		size_t ncap = *size + (*size >> 1);
		uint8_t *nbuf = realloc(*buf, ncap);
		if (!nbuf)
			return false;
		*buf = nbuf;
		*cap = ncap;
	}
	return read_exact(fd, *buf, *size);
}

bool acam_send_zoom(int fd, float zoom)
{
	char line[64];
	int len = snprintf(line, sizeof(line), "{\"zoom\":%.2f}\n", zoom);
	return write_all(fd, line, (size_t)len);
}
