/*
 * Prueba de punta a punta sin OBS: abre la app, conecta, decodifica N frames y reporta.
 *
 *   ./build/probe [serial] [ancho] [alto] [fps] [hevc|h264] [segundos] [back|front]
 *
 * Con ACAM_ZOOM_TEST=<carpeta>: guarda un JPEG a 1×, pide zoom 3× en vivo y guarda otro.
 */
#include <ImageIO/ImageIO.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/rotator.h"
#include "../src/stream-client.h"
#include "../src/vt-decoder.h"

static int decoded;
static size_t last_w, last_h;
static bool last_full, last_601;
static int last_rotation = -1;
static int rotated;
static size_t rot_w, rot_h;
static struct acam_rotator *rotator;
static const char *snapshot_path; /* se pone desde main; el callback guarda el siguiente frame */

static void save_jpeg(CVPixelBufferRef img, const char *path)
{
	CGImageRef cg = NULL;
	if (VTCreateCGImageFromCVPixelBuffer(img, NULL, &cg) != noErr)
		return;
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
	CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.jpeg"), 1, NULL);
	if (dst) {
		CGImageDestinationAddImage(dst, cg, NULL);
		CGImageDestinationFinalize(dst);
		CFRelease(dst);
		printf("captura: %s\n", path);
	}
	CFRelease(url);
	CGImageRelease(cg);
}

static uint64_t now_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void on_frame(void *param, CVPixelBufferRef img, const struct acam_frame_info *info)
{
	(void)param;
	decoded++;
	last_w = CVPixelBufferGetWidth(img);
	last_h = CVPixelBufferGetHeight(img);
	last_full = info->full_range;
	last_601 = info->bt601;
	if (info->rotation != last_rotation) {
		last_rotation = info->rotation;
		printf("rotación pedida por el teléfono: %d°\n", last_rotation);
	}
	if (snapshot_path) {
		save_jpeg(img, snapshot_path);
		snapshot_path = NULL;
	}
	/* Mismo camino que el plugin: girar en el hilo de callback de VideoToolbox. */
	if (info->rotation) {
		CVPixelBufferRef r = acam_rotate(rotator, img, info->rotation);
		if (r) {
			rotated++;
			rot_w = CVPixelBufferGetWidth(r);
			rot_h = CVPixelBufferGetHeight(r);
			CFRelease(r);
		}
	}
}

int main(int argc, char **argv)
{
	char adb[1024];
	if (!acam_find_adb(NULL, adb, sizeof(adb))) {
		fprintf(stderr, "adb no encontrado\n");
		return 1;
	}

	struct acam_device devs[8];
	int n = acam_list_devices(adb, devs, 8);
	for (int i = 0; i < n; i++)
		printf("dispositivo: %s  %s  %s\n", devs[i].serial, devs[i].name, devs[i].ready ? "listo" : "NO listo");

	const char *serial = argc > 1 && strcmp(argv[1], "-") != 0 ? argv[1] : (n ? devs[0].serial : NULL);
	if (!serial) {
		fprintf(stderr, "sin dispositivos\n");
		return 1;
	}

	struct acam_request req = {
		.width = argc > 2 ? atoi(argv[2]) : 3840,
		.height = argc > 3 ? atoi(argv[3]) : 2160,
		.fps = argc > 4 ? atoi(argv[4]) : 30,
		.bitrate = 50000000,
		.codec = argc > 5 && strcmp(argv[5], "h264") == 0 ? ACAM_CODEC_H264 : ACAM_CODEC_HEVC,
		.front = argc > 7 && strcmp(argv[7], "front") == 0,
		.stabilization = true,
	};
	int seconds = argc > 6 ? atoi(argv[6]) : 10;
	if (getenv("ACAM_CAMERA_ID"))
		snprintf(req.camera_id, sizeof(req.camera_id), "%s", getenv("ACAM_CAMERA_ID"));

	acam_launch_app(adb, serial);
	usleep(1500 * 1000);
	int port = acam_forward(adb, serial);
	printf("forward tcp:%d → tcp:%d\n", port, ACAM_DEVICE_PORT);
	int fd = acam_connect(port);
	if (fd < 0) {
		fprintf(stderr, "no conecta\n");
		return 1;
	}

	struct acam_header hdr;
	char err[512];
	int rc = acam_handshake(fd, &req, &hdr, err, sizeof(err), 10000);
	if (rc != 1) {
		fprintf(stderr, rc == 0 ? "teléfono respondió error: %s\n" : "sin respuesta del teléfono%s\n",
			rc == 0 ? err : "");
		return 1;
	}
	printf("cabecera: %s %dx%d @ %d fps, zoom máx %.1f×\n", hdr.codec == ACAM_CODEC_HEVC ? "HEVC" : "H.264",
	       hdr.width, hdr.height, hdr.fps, hdr.max_zoom);
	acam_set_recv_timeout(fd, 3000);

	rotator = acam_rotator_create();
	struct acam_decoder *dec = acam_decoder_create(hdr.codec, on_frame, NULL);
	uint8_t *buf = NULL;
	size_t cap = 0;
	int64_t pts;
	uint32_t flags, size;
	int packets = 0, keys = 0;
	uint64_t bytes = 0, t0 = 0, first_pts = 0, last_pts = 0;
	uint64_t start = now_ns();
	const char *zoom_dir = getenv("ACAM_ZOOM_TEST");
	int zoom_step = 0;
	static char snap1[1024], snap2[1024];

	while (acam_read_packet(fd, &buf, &cap, &pts, &flags, &size)) {
		if (flags & ACAM_FLAG_CONFIG) {
			printf("config %u bytes → %s\n", size, acam_decoder_set_config(dec, buf, size) ? "ok" : "FALLÓ");
			continue;
		}
		if (!t0) {
			t0 = now_ns();
			first_pts = (uint64_t)pts;
			printf("primer frame a los %.0f ms\n", (t0 - start) / 1e6);
		}
		last_pts = (uint64_t)pts;
		packets++;
		bytes += size;
		if (flags & ACAM_FLAG_KEY)
			keys++;
		acam_decoder_decode(dec, buf, size, (uint64_t)pts * 1000, flags & ACAM_FLAG_KEY,
				     ACAM_FLAG_ROTATION(flags));
		if (zoom_dir) {
			uint64_t el = now_ns() - t0;
			if (zoom_step == 0 && el > 2500000000ull) {
				snprintf(snap1, sizeof(snap1), "%s/zoom-1x.jpg", zoom_dir);
				snapshot_path = snap1;
				zoom_step = 1;
			} else if (zoom_step == 1 && el > 2700000000ull) {
				printf("enviando zoom 3× → %s\n", acam_send_zoom(fd, 3.0f) ? "ok" : "FALLÓ");
				zoom_step = 2;
			} else if (zoom_step == 2 && el > 4000000000ull) {
				snprintf(snap2, sizeof(snap2), "%s/zoom-3x.jpg", zoom_dir);
				snapshot_path = snap2;
				zoom_step = 3;
			}
		}
		if (now_ns() - t0 > (uint64_t)seconds * 1000000000ull)
			break;
	}

	double secs = (now_ns() - t0) / 1e9;
	double pts_secs = (last_pts - first_pts) / 1e6;
	printf("\n%d paquetes (%d keyframes) en %.1f s → %.1f fps recibidos, %.1f fps según pts\n", packets, keys, secs,
	       packets / secs, pts_secs > 0 ? (packets - 1) / pts_secs : 0);
	printf("bitrate real: %.1f Mbps\n", bytes * 8 / secs / 1e6);
	printf("decodificados: %d frames, %zux%zu, rango %s, matriz %s\n", decoded, last_w, last_h,
	       last_full ? "completo" : "limitado", last_601 ? "BT.601" : "BT.709");

	acam_decoder_destroy(dec);
	if (rotated)
		printf("girados: %d frames → %zux%zu\n", rotated, rot_w, rot_h);
	acam_rotator_destroy(rotator);
	close(fd);
	acam_forward_remove(adb, serial, port);
	free(buf);
	return decoded > 0 ? 0 : 1;
}
