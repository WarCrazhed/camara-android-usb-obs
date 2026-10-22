/*
 * Decodificador H.264/HEVC por hardware con VideoToolbox.
 * Entra Annex-B (lo que produce MediaCodec), sale CVPixelBuffer NV12.
 */
#pragma once

#include <CoreVideo/CoreVideo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stream-client.h"

struct acam_frame_info {
	uint64_t timestamp_ns;
	bool full_range;
	bool bt601; /* false = BT.709 */
	int rotation; /* grados horarios que pide el teléfono */
};

typedef void (*acam_frame_cb)(void *param, CVPixelBufferRef image, const struct acam_frame_info *info);

struct acam_decoder;

struct acam_decoder *acam_decoder_create(enum acam_codec codec, acam_frame_cb cb, void *param);
void acam_decoder_destroy(struct acam_decoder *dec);

/* Paquete de configuración (VPS/SPS/PPS o SPS/PPS). */
bool acam_decoder_set_config(struct acam_decoder *dec, const uint8_t *data, size_t size);

/* Un access unit. Los frames anteriores al primer keyframe se descartan. */
bool acam_decoder_decode(struct acam_decoder *dec, const uint8_t *data, size_t size, uint64_t timestamp_ns,
			 bool keyframe, int rotation);
