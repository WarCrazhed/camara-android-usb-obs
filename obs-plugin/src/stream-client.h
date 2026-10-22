/*
 * Cliente del protocolo de OBS Cam (app Android) sobre adb.
 * Sin dependencias de libobs: lo usan el plugin y la herramienta `probe`.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ACAM_DEVICE_PORT 27183
#define ACAM_ACTIVITY "com.camara.obscam/.MainActivity"

#define ACAM_FLAG_CONFIG 1u
#define ACAM_FLAG_KEY 2u
/* Grados (0/90/180/270) en sentido horario para ver el cuadro derecho, según el giro del teléfono. */
#define ACAM_FLAG_ROTATION(flags) ((int)(((flags) >> 2) & 3u) * 90)

enum acam_codec {
	ACAM_CODEC_H264 = 0,
	ACAM_CODEC_HEVC = 1,
};

struct acam_device {
	char serial[128];
	char name[128];
	bool ready; /* false: no autorizado / offline */
};

struct acam_request {
	int width;
	int height;
	int fps;
	int bitrate;
	enum acam_codec codec;
	bool front;
	char camera_id[16];
	bool stabilization;
	float zoom;
};

struct acam_header {
	enum acam_codec codec;
	int width;
	int height;
	int fps;
	float max_zoom; /* 1 = el teléfono no permite zoom */
};

/* Ruta de adb: `override` si existe, si no busca en las rutas habituales. NULL si no hay. */
bool acam_find_adb(const char *override, char *out, size_t outsz);

/* Lista dispositivos de `adb devices`. Devuelve cuántos llenó. */
int acam_list_devices(const char *adb, struct acam_device *out, int max);

/* Abre la app en el teléfono (también la trae al frente si estaba en segundo plano). */
bool acam_launch_app(const char *adb, const char *serial);

/* `adb forward tcp:0 tcp:27183`. Devuelve el puerto local o -1. */
int acam_forward(const char *adb, const char *serial);
void acam_forward_remove(const char *adb, const char *serial, int port);

int acam_connect(int port);

/*
 * Envía la petición y lee la cabecera. Devuelve:
 *   1  ok, `hdr` lleno
 *   0  el teléfono respondió con error (mensaje en `err`)
 *  -1  sin respuesta (la app no está escuchando o se cortó)
 */
int acam_handshake(int fd, const struct acam_request *req, struct acam_header *hdr, char *err, size_t errsz,
		   int timeout_ms);

/* Lee un paquete; agranda `*buf` si hace falta. false si se cortó la conexión. */
bool acam_read_packet(int fd, uint8_t **buf, size_t *cap, int64_t *pts_us, uint32_t *flags, uint32_t *size);

void acam_set_recv_timeout(int fd, int timeout_ms);

/* Cambia el zoom en vivo durante la transmisión. */
bool acam_send_zoom(int fd, float zoom);
