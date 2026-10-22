/*
 * Fuente de OBS «Cámara Android (USB)»: usa un teléfono con la app OBS Cam como cámara por USB (adb).
 */
#include <obs-module.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <util/platform.h>
#include <util/threading.h>

#include "stream-client.h"
#include "rotator.h"
#include "vt-decoder.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-android-cam", "en-US")

#define T(s) obs_module_text(s)
#define MAX_DEVICES 16
#define REBASE_THRESHOLD_NS (150 * 1000000LL)
#define RELAUNCH_INTERVAL_NS (8 * 1000000000ULL)
#define ZOOM_MAX_DEFAULT 10.0f
#define ZOOM_STEP 1.15f

struct acam_source {
	obs_source_t *source;

	/* config, solo se toca con el hilo detenido */
	char *serial;
	char *adb_override;
	struct acam_request req;
	int rotation; /* -1 = automática (la que manda el teléfono) */
	struct acam_rotator *rotator;
	float max_zoom;
	obs_hotkey_pair_id zoom_hotkeys;
	obs_hotkey_id zoom_reset_hotkey;

	pthread_t thread;
	bool thread_running;
	volatile bool stop;
	pthread_mutex_t fd_mutex;
	int fd;

	pthread_mutex_t status_mutex;
	char status[256];

	int64_t base_local_ns;
	int64_t base_device_ns;
	bool have_base;
};

static void set_status(struct acam_source *s, const char *fmt, ...)
{
	char buf[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	pthread_mutex_lock(&s->status_mutex);
	bool changed = strcmp(buf, s->status) != 0;
	if (changed)
		snprintf(s->status, sizeof(s->status), "%s", buf);
	pthread_mutex_unlock(&s->status_mutex);
	if (changed)
		blog(LOG_INFO, "[android-cam] %s: %s", obs_source_get_name(s->source), buf);
}

static void wait_ms(struct acam_source *s, int ms)
{
	for (int i = 0; i < ms / 50 && !os_atomic_load_bool(&s->stop); i++)
		os_sleep_ms(50);
}

/* Reloj del teléfono → reloj de OBS, manteniendo la cadencia original; se reajusta si deriva. */
static uint64_t map_timestamp(struct acam_source *s, int64_t pts_us)
{
	int64_t device_ns = pts_us * 1000;
	int64_t now = (int64_t)os_gettime_ns();
	int64_t mapped = s->base_local_ns + (device_ns - s->base_device_ns);
	if (!s->have_base || llabs(mapped - now) > REBASE_THRESHOLD_NS) {
		s->base_local_ns = now;
		s->base_device_ns = device_ns;
		s->have_base = true;
		mapped = now;
	}
	return (uint64_t)mapped;
}

static void output_frame(struct acam_source *s, CVPixelBufferRef image, const struct acam_frame_info *info);

static void on_frame(void *param, CVPixelBufferRef image, const struct acam_frame_info *info)
{
	struct acam_source *s = param;
	int degrees = s->rotation >= 0 ? s->rotation : info->rotation;
	if (degrees == 0 || !s->rotator) {
		output_frame(s, image, info);
		return;
	}
	CVPixelBufferRef rotated = acam_rotate(s->rotator, image, degrees);
	if (rotated) {
		output_frame(s, rotated, info);
		CFRelease(rotated);
	}
}

static void output_frame(struct acam_source *s, CVPixelBufferRef image, const struct acam_frame_info *info)
{
	if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
		return;

	struct obs_source_frame frame = {0};
	frame.width = (uint32_t)CVPixelBufferGetWidth(image);
	frame.height = (uint32_t)CVPixelBufferGetHeight(image);
	frame.format = VIDEO_FORMAT_NV12;
	frame.timestamp = info->timestamp_ns;
	frame.full_range = info->full_range;
	for (size_t p = 0; p < 2; p++) {
		frame.data[p] = CVPixelBufferGetBaseAddressOfPlane(image, p);
		frame.linesize[p] = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(image, p);
	}
	video_format_get_parameters_for_format(info->bt601 ? VIDEO_CS_601 : VIDEO_CS_709,
					       info->full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL,
					       VIDEO_FORMAT_NV12, frame.color_matrix, frame.color_range_min,
					       frame.color_range_max);
	obs_source_output_video(s->source, &frame);
	CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
}

static bool resolve_serial(struct acam_source *s, const char *adb, char *out, size_t outsz)
{
	struct acam_device devs[MAX_DEVICES];
	int n = acam_list_devices(adb, devs, MAX_DEVICES);
	for (int i = 0; i < n; i++) {
		bool wanted = !s->serial || !*s->serial || strcmp(devs[i].serial, s->serial) == 0;
		if (!wanted)
			continue;
		if (!devs[i].ready) {
			set_status(s, T("Status.Unauthorized"), devs[i].name);
			return false;
		}
		snprintf(out, outsz, "%s", devs[i].serial);
		return true;
	}
	set_status(s, "%s", T("Status.NoDevice"));
	return false;
}

/* Recibe y decodifica hasta que se corte la conexión o se pida parar. */
static void stream_loop(struct acam_source *s, int fd, const struct acam_header *hdr)
{
	struct acam_decoder *dec = acam_decoder_create(hdr->codec, on_frame, s);
	uint8_t *buf = NULL;
	size_t cap = 0;
	int64_t pts;
	uint32_t flags, size;

	s->have_base = false;
	acam_set_recv_timeout(fd, 3000);
	while (!os_atomic_load_bool(&s->stop) && acam_read_packet(fd, &buf, &cap, &pts, &flags, &size)) {
		if (flags & ACAM_FLAG_CONFIG)
			acam_decoder_set_config(dec, buf, size);
		else
			acam_decoder_decode(dec, buf, size, map_timestamp(s, pts), flags & ACAM_FLAG_KEY,
					     ACAM_FLAG_ROTATION(flags));
	}

	acam_decoder_destroy(dec);
	free(buf);
}

static void *capture_thread(void *data)
{
	struct acam_source *s = data;
	os_set_thread_name("android-cam");

	char adb[1024], serial[128];
	uint64_t last_launch = 0;

	while (!os_atomic_load_bool(&s->stop)) {
		if (!acam_find_adb(s->adb_override, adb, sizeof(adb))) {
			set_status(s, "%s", T("Status.NoAdb"));
			wait_ms(s, 3000);
			continue;
		}
		if (!resolve_serial(s, adb, serial, sizeof(serial))) {
			wait_ms(s, 2000);
			continue;
		}

		/* Abre la app si no responde; espaciado para no traerla al frente en bucle. */
		if (!last_launch || os_gettime_ns() - last_launch > RELAUNCH_INTERVAL_NS) {
			acam_launch_app(adb, serial);
			last_launch = os_gettime_ns();
			wait_ms(s, 1500);
		}

		int port = acam_forward(adb, serial);
		if (port < 0) {
			set_status(s, "%s", T("Status.ForwardFailed"));
			wait_ms(s, 2000);
			continue;
		}

		int fd = acam_connect(port);
		pthread_mutex_lock(&s->fd_mutex);
		s->fd = fd;
		pthread_mutex_unlock(&s->fd_mutex);

		if (fd >= 0 && !os_atomic_load_bool(&s->stop)) {
			set_status(s, "%s", T("Status.Connecting"));
			struct acam_header hdr;
			char err[512];
			int rc = acam_handshake(fd, &s->req, &hdr, err, sizeof(err), 10000);
			if (rc == 1) {
				s->max_zoom = hdr.max_zoom;
				set_status(s, T("Status.Streaming"), hdr.width, hdr.height, hdr.fps,
					   hdr.codec == ACAM_CODEC_HEVC ? "HEVC" : "H.264");
				last_launch = os_gettime_ns();
				stream_loop(s, fd, &hdr);
				obs_source_output_video(s->source, NULL);
				if (!os_atomic_load_bool(&s->stop))
					set_status(s, "%s", T("Status.Disconnected"));
			} else if (rc == 0) {
				set_status(s, T("Status.PhoneError"), err);
				wait_ms(s, 3000);
			} else {
				set_status(s, "%s", T("Status.WaitingApp"));
			}
		}

		pthread_mutex_lock(&s->fd_mutex);
		s->fd = -1;
		pthread_mutex_unlock(&s->fd_mutex);
		if (fd >= 0)
			close(fd);
		acam_forward_remove(adb, serial, port);
		wait_ms(s, 1000);
	}
	return NULL;
}

static void stop_thread(struct acam_source *s)
{
	if (!s->thread_running)
		return;
	os_atomic_set_bool(&s->stop, true);
	pthread_mutex_lock(&s->fd_mutex);
	if (s->fd >= 0)
		shutdown(s->fd, SHUT_RDWR);
	pthread_mutex_unlock(&s->fd_mutex);
	pthread_join(s->thread, NULL);
	s->thread_running = false;
	obs_source_output_video(s->source, NULL);
}

static void start_thread(struct acam_source *s)
{
	os_atomic_set_bool(&s->stop, false);
	s->thread_running = pthread_create(&s->thread, NULL, capture_thread, s) == 0;
}

// --- obs_source_info ---

static const char *acam_get_name(void *unused)
{
	return T("AndroidCamera");
}

static void apply_zoom(struct acam_source *s, float zoom)
{
	float max = s->max_zoom > 1 ? s->max_zoom : ZOOM_MAX_DEFAULT;
	s->req.zoom = zoom < 1 ? 1 : zoom > max ? max : zoom;
	pthread_mutex_lock(&s->fd_mutex);
	if (s->fd >= 0)
		acam_send_zoom(s->fd, s->req.zoom);
	pthread_mutex_unlock(&s->fd_mutex);
}

/* ¿Cambia algo que obliga a reabrir la cámara? (zoom, rotación y búfer se aplican en vivo) */
static bool needs_restart(const struct acam_source *s, const struct acam_request *r, const char *serial,
			  const char *adb)
{
	const struct acam_request *o = &s->req;
	return !s->thread_running || strcmp(serial, s->serial ? s->serial : "") != 0 ||
	       strcmp(adb, s->adb_override ? s->adb_override : "") != 0 || r->width != o->width ||
	       r->height != o->height || r->fps != o->fps || r->bitrate != o->bitrate || r->codec != o->codec ||
	       r->front != o->front || strcmp(r->camera_id, o->camera_id) != 0 || r->stabilization != o->stabilization;
}

static void acam_update(void *data, obs_data_t *settings)
{
	struct acam_source *s = data;
	const char *serial = obs_data_get_string(settings, "device");
	const char *adb = obs_data_get_string(settings, "adb_path");

	struct acam_request r = {0};
	int w = 3840, h = 2160;
	sscanf(obs_data_get_string(settings, "resolution"), "%dx%d", &w, &h);
	r.width = w;
	r.height = h;
	r.fps = (int)obs_data_get_int(settings, "fps");
	r.bitrate = (int)obs_data_get_int(settings, "bitrate") * 1000000;
	r.codec = strcmp(obs_data_get_string(settings, "codec"), "h264") == 0 ? ACAM_CODEC_H264 : ACAM_CODEC_HEVC;
	r.front = strcmp(obs_data_get_string(settings, "facing"), "front") == 0;
	snprintf(r.camera_id, sizeof(r.camera_id), "%s", obs_data_get_string(settings, "camera_id"));
	r.stabilization = obs_data_get_bool(settings, "stabilization");
	r.zoom = (float)obs_data_get_double(settings, "zoom");

	s->rotation = (int)obs_data_get_int(settings, "rotation");
	obs_source_set_async_unbuffered(s->source, obs_data_get_bool(settings, "low_latency"));

	if (!needs_restart(s, &r, serial, adb)) {
		if (r.zoom != s->req.zoom)
			apply_zoom(s, r.zoom);
		return;
	}

	stop_thread(s);
	bfree(s->serial);
	bfree(s->adb_override);
	s->serial = bstrdup(serial);
	s->adb_override = bstrdup(adb);
	s->req = r;
	start_thread(s);
}

/* Atajos: cada pulsación acerca/aleja un 15 %; el valor se guarda en la fuente. */
static void zoom_by(struct acam_source *s, float factor)
{
	apply_zoom(s, factor ? s->req.zoom * factor : 1.0f);
	obs_data_t *settings = obs_source_get_settings(s->source);
	obs_data_set_double(settings, "zoom", s->req.zoom);
	obs_data_release(settings);
}

static bool zoom_in_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	if (pressed)
		zoom_by(data, ZOOM_STEP);
	return pressed;
}

static bool zoom_out_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	if (pressed)
		zoom_by(data, 1.0f / ZOOM_STEP);
	return pressed;
}

static void zoom_reset_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	if (pressed)
		zoom_by(data, 0);
}

static void *acam_create(obs_data_t *settings, obs_source_t *source)
{
	struct acam_source *s = bzalloc(sizeof(*s));
	s->source = source;
	s->fd = -1;
	s->rotator = acam_rotator_create();
	pthread_mutex_init(&s->fd_mutex, NULL);
	pthread_mutex_init(&s->status_mutex, NULL);
	snprintf(s->status, sizeof(s->status), "%s", T("Status.Starting"));
	s->zoom_hotkeys = obs_hotkey_pair_register_source(source, "android_cam.zoom_in", T("ZoomIn"),
							   "android_cam.zoom_out", T("ZoomOut"), zoom_in_hotkey,
							   zoom_out_hotkey, s, s);
	s->zoom_reset_hotkey =
		obs_hotkey_register_source(source, "android_cam.zoom_reset", T("ZoomReset"), zoom_reset_hotkey, s);
	acam_update(s, settings);
	return s;
}

static void acam_destroy(void *data)
{
	struct acam_source *s = data;
	obs_hotkey_pair_unregister(s->zoom_hotkeys);
	obs_hotkey_unregister(s->zoom_reset_hotkey);
	stop_thread(s);
	pthread_mutex_destroy(&s->fd_mutex);
	pthread_mutex_destroy(&s->status_mutex);
	bfree(s->serial);
	bfree(s->adb_override);
	acam_rotator_destroy(s->rotator);
	bfree(s);
}

static void acam_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "device", "");
	obs_data_set_default_string(settings, "resolution", "3840x2160");
	obs_data_set_default_int(settings, "fps", 30);
	obs_data_set_default_int(settings, "bitrate", 50);
	obs_data_set_default_string(settings, "codec", "hevc");
	obs_data_set_default_string(settings, "facing", "back");
	obs_data_set_default_string(settings, "camera_id", "");
	obs_data_set_default_bool(settings, "stabilization", true);
	obs_data_set_default_bool(settings, "low_latency", true);
	obs_data_set_default_int(settings, "rotation", -1);
	obs_data_set_default_double(settings, "zoom", 1.0);
	obs_data_set_default_string(settings, "adb_path", "");
}

static void fill_devices(obs_property_t *list, const char *adb_override, const char *selected)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, T("Device.Auto"), "");

	char adb[1024];
	if (!acam_find_adb(adb_override, adb, sizeof(adb))) {
		obs_property_list_add_string(list, T("Device.NoAdb"), "__none__");
		return;
	}

	struct acam_device devs[MAX_DEVICES];
	int n = acam_list_devices(adb, devs, MAX_DEVICES);
	bool found = false;
	for (int i = 0; i < n; i++) {
		char label[300];
		snprintf(label, sizeof(label), "%s  [%s]", devs[i].name, devs[i].serial);
		obs_property_list_add_string(list, label, devs[i].serial);
		found |= selected && strcmp(selected, devs[i].serial) == 0;
	}
	if (selected && *selected && !found) {
		char label[300];
		snprintf(label, sizeof(label), "%s  [%s]", T("Device.Disconnected"), selected);
		obs_property_list_add_string(list, label, selected);
	}
}

static bool refresh_clicked(obs_properties_t *props, obs_property_t *button, void *data)
{
	struct acam_source *s = data;
	obs_data_t *settings = obs_source_get_settings(s->source);
	fill_devices(obs_properties_get(props, "device"), obs_data_get_string(settings, "adb_path"),
		     obs_data_get_string(settings, "device"));
	obs_data_release(settings);
	return true;
}

static obs_properties_t *acam_properties(void *data)
{
	struct acam_source *s = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *dev = obs_properties_add_list(props, "device", T("Device"), OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_STRING);
	if (s) {
		obs_data_t *settings = obs_source_get_settings(s->source);
		fill_devices(dev, obs_data_get_string(settings, "adb_path"), obs_data_get_string(settings, "device"));
		obs_data_release(settings);
	}
	obs_properties_add_button2(props, "refresh", T("Refresh"), refresh_clicked, s);

	if (s) {
		pthread_mutex_lock(&s->status_mutex);
		char status[300];
		snprintf(status, sizeof(status), "%s: %s", T("Status"), s->status);
		pthread_mutex_unlock(&s->status_mutex);
		obs_properties_add_text(props, "status_info", status, OBS_TEXT_INFO);
	}

	obs_property_t *facing = obs_properties_add_list(props, "facing", T("Camera"), OBS_COMBO_TYPE_LIST,
							 OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(facing, T("Camera.Back"), "back");
	obs_property_list_add_string(facing, T("Camera.Front"), "front");

	obs_property_t *res = obs_properties_add_list(props, "resolution", T("Resolution"), OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(res, "3840×2160 (4K)", "3840x2160");
	obs_property_list_add_string(res, "2560×1440 (1440p)", "2560x1440");
	obs_property_list_add_string(res, "1920×1080 (1080p)", "1920x1080");
	obs_property_list_add_string(res, T("Resolution.Full2160"), "2880x2160");
	obs_property_list_add_string(res, T("Resolution.Full1440"), "1920x1440");
	obs_property_list_add_string(res, "1280×720 (720p)", "1280x720");

	obs_property_t *fps = obs_properties_add_list(props, "fps", T("FPS"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps, "60", 60);
	obs_property_list_add_int(fps, "30", 30);
	obs_property_list_add_int(fps, "24", 24);

	obs_property_t *codec = obs_properties_add_list(props, "codec", T("Codec"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(codec, "HEVC (H.265)", "hevc");
	obs_property_list_add_string(codec, "H.264", "h264");

	obs_property_t *br = obs_properties_add_int_slider(props, "bitrate", T("Bitrate"), 5, 150, 5);
	obs_property_int_set_suffix(br, " Mbps");

	obs_property_t *zoom = obs_properties_add_float_slider(props, "zoom", T("Zoom"), 1.0,
							       s && s->max_zoom > 1 ? s->max_zoom : ZOOM_MAX_DEFAULT, 0.1);
	obs_property_float_set_suffix(zoom, "×");

	obs_property_t *rot = obs_properties_add_list(props, "rotation", T("Rotation"), OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(rot, T("Rotation.Auto"), -1);
	obs_property_list_add_int(rot, "0°", 0);
	obs_property_list_add_int(rot, "90°", 90);
	obs_property_list_add_int(rot, "180°", 180);
	obs_property_list_add_int(rot, "270°", 270);

	obs_properties_add_bool(props, "stabilization", T("Stabilization"));
	obs_properties_add_bool(props, "low_latency", T("LowLatency"));

	obs_properties_t *adv = obs_properties_create();
	obs_properties_add_text(adv, "camera_id", T("CameraId"), OBS_TEXT_DEFAULT);
	obs_properties_add_path(adv, "adb_path", T("AdbPath"), OBS_PATH_FILE, NULL, NULL);
	obs_properties_add_group(props, "advanced", T("Advanced"), OBS_GROUP_NORMAL, adv);

	return props;
}

static struct obs_source_info android_cam_info = {
	.id = "android_usb_camera",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.icon_type = OBS_ICON_TYPE_CAMERA,
	.get_name = acam_get_name,
	.create = acam_create,
	.destroy = acam_destroy,
	.update = acam_update,
	.get_defaults = acam_defaults,
	.get_properties = acam_properties,
};

bool obs_module_load(void)
{
	obs_register_source(&android_cam_info);
	return true;
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Usa un teléfono Android con la app OBS Cam como cámara por USB (hasta 4K, HEVC por hardware).";
}
