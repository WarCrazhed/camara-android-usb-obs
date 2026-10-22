#include "vt-decoder.h"

#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PARAM_SETS 8
#define MAX_PARAM_BYTES 4096

struct acam_decoder {
	enum acam_codec codec;
	acam_frame_cb cb;
	void *param;

	uint8_t params[MAX_PARAM_BYTES];
	size_t params_len;

	CMVideoFormatDescriptionRef format;
	VTDecompressionSessionRef session;
	struct acam_frame_info color;
	bool need_keyframe;
};

/* Siguiente NAL en un buffer Annex-B. */
static bool next_nal(const uint8_t *data, size_t size, size_t *pos, const uint8_t **nal, size_t *nal_size)
{
	size_t i = *pos;
	while (i + 3 <= size && !(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1))
		i++;
	if (i + 3 > size)
		return false;
	size_t start = i + 3;

	size_t j = start;
	while (j + 3 <= size && !(data[j] == 0 && data[j + 1] == 0 && (data[j + 2] == 1 || data[j + 2] == 0)))
		j++;
	size_t end = j + 3 <= size ? j : size;
	while (end > start && data[end - 1] == 0)
		end--; /* ceros finales / del siguiente start code de 4 bytes */

	*nal = data + start;
	*nal_size = end - start;
	*pos = j + 3 <= size ? j : size;
	return *nal_size > 0;
}

static int nal_type(const struct acam_decoder *dec, const uint8_t *nal)
{
	return dec->codec == ACAM_CODEC_HEVC ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f;
}

static bool is_param_set(const struct acam_decoder *dec, int type)
{
	if (dec->codec == ACAM_CODEC_HEVC)
		return type == 32 || type == 33 || type == 34; /* VPS SPS PPS */
	return type == 7 || type == 8;                         /* SPS PPS */
}

static void destroy_session(struct acam_decoder *dec)
{
	if (dec->session) {
		VTDecompressionSessionWaitForAsynchronousFrames(dec->session);
		VTDecompressionSessionInvalidate(dec->session);
		CFRelease(dec->session);
		dec->session = NULL;
	}
	if (dec->format) {
		CFRelease(dec->format);
		dec->format = NULL;
	}
}

static void on_decoded(void *ref, void *frame_ref, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
		       CMTime pts, CMTime duration)
{
	(void)flags;
	(void)duration;
	struct acam_decoder *dec = ref;
	if (status != noErr || !image)
		return;

	struct acam_frame_info info = dec->color;
	info.timestamp_ns = (uint64_t)pts.value;
	info.rotation = (int)(intptr_t)frame_ref;
	dec->cb(dec->param, image, &info);
}

static void read_color(struct acam_decoder *dec)
{
	CFTypeRef full = CMFormatDescriptionGetExtension(dec->format, kCMFormatDescriptionExtension_FullRangeVideo);
	dec->color.full_range = full && CFGetTypeID(full) == CFBooleanGetTypeID() && CFBooleanGetValue(full);

	CFTypeRef matrix = CMFormatDescriptionGetExtension(dec->format, kCMFormatDescriptionExtension_YCbCrMatrix);
	if (matrix)
		dec->color.bt601 = CFEqual(matrix, kCMFormatDescriptionYCbCrMatrix_ITU_R_601_4);
	else
		dec->color.bt601 = CMVideoFormatDescriptionGetDimensions(dec->format).height < 720;
}

static bool create_session(struct acam_decoder *dec)
{
	const uint8_t *sets[MAX_PARAM_SETS];
	size_t sizes[MAX_PARAM_SETS];
	size_t count = 0, pos = 0;
	const uint8_t *nal;
	size_t nal_size;

	/* VideoToolbox quiere VPS, SPS, PPS en ese orden. */
	int order_hevc[] = {32, 33, 34};
	int order_h264[] = {7, 8};
	int *order = dec->codec == ACAM_CODEC_HEVC ? order_hevc : order_h264;
	int norder = dec->codec == ACAM_CODEC_HEVC ? 3 : 2;
	for (int o = 0; o < norder; o++) {
		pos = 0;
		while (count < MAX_PARAM_SETS && next_nal(dec->params, dec->params_len, &pos, &nal, &nal_size)) {
			if (nal_type(dec, nal) == order[o]) {
				sets[count] = nal;
				sizes[count++] = nal_size;
			}
		}
	}

	OSStatus st;
	if (dec->codec == ACAM_CODEC_HEVC)
		st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, count, sets, sizes, 4, NULL,
									&dec->format);
	else
		st = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, count, sets, sizes, 4,
									&dec->format);
	if (st != noErr)
		return false;

	read_color(dec);

	int32_t pixfmt = dec->color.full_range ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
					       : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	CFNumberRef pixnum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixfmt);
	const void *keys[] = {kCVPixelBufferPixelFormatTypeKey};
	const void *vals[] = {pixnum};
	CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
						   &kCFTypeDictionaryValueCallBacks);
	CFRelease(pixnum);

	VTDecompressionOutputCallbackRecord cb = {.decompressionOutputCallback = on_decoded,
						  .decompressionOutputRefCon = dec};
	st = VTDecompressionSessionCreate(kCFAllocatorDefault, dec->format, NULL, attrs, &cb, &dec->session);
	CFRelease(attrs);
	if (st != noErr) {
		destroy_session(dec);
		return false;
	}
	VTSessionSetProperty(dec->session, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
	dec->need_keyframe = true;
	return true;
}

struct acam_decoder *acam_decoder_create(enum acam_codec codec, acam_frame_cb cb, void *param)
{
	struct acam_decoder *dec = calloc(1, sizeof(*dec));
	dec->codec = codec;
	dec->cb = cb;
	dec->param = param;
	dec->need_keyframe = true;
	return dec;
}

void acam_decoder_destroy(struct acam_decoder *dec)
{
	if (!dec)
		return;
	destroy_session(dec);
	free(dec);
}

bool acam_decoder_set_config(struct acam_decoder *dec, const uint8_t *data, size_t size)
{
	if (size > MAX_PARAM_BYTES)
		return false;
	if (dec->session && size == dec->params_len && memcmp(data, dec->params, size) == 0)
		return true;

	destroy_session(dec);
	memcpy(dec->params, data, size);
	dec->params_len = size;
	return create_session(dec);
}

bool acam_decoder_decode(struct acam_decoder *dec, const uint8_t *data, size_t size, uint64_t timestamp_ns,
			 bool keyframe, int rotation)
{
	/* Separa parámetros en línea (algunos encoders los repiten en cada keyframe) y calcula el tamaño AVCC. */
	uint8_t inline_params[MAX_PARAM_BYTES];
	size_t inline_len = 0, avcc_len = 0, pos = 0;
	const uint8_t *nal;
	size_t nal_size;
	while (next_nal(data, size, &pos, &nal, &nal_size)) {
		if (is_param_set(dec, nal_type(dec, nal))) {
			if (inline_len + 4 + nal_size <= sizeof(inline_params)) {
				memcpy(inline_params + inline_len, "\0\0\0\1", 4);
				memcpy(inline_params + inline_len + 4, nal, nal_size);
				inline_len += 4 + nal_size;
			}
		} else {
			avcc_len += 4 + nal_size;
		}
	}
	if (inline_len && !acam_decoder_set_config(dec, inline_params, inline_len))
		return false;
	if (!dec->session || avcc_len == 0)
		return false;
	if (dec->need_keyframe && !keyframe)
		return false;

	CMBlockBufferRef block = NULL;
	if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL, avcc_len, kCFAllocatorDefault, NULL, 0,
					       avcc_len, kCMBlockBufferAssureMemoryNowFlag, &block) != noErr)
		return false;
	char *dst = NULL;
	CMBlockBufferGetDataPointer(block, 0, NULL, NULL, &dst);

	pos = 0;
	while (next_nal(data, size, &pos, &nal, &nal_size)) {
		if (is_param_set(dec, nal_type(dec, nal)))
			continue;
		uint32_t be = CFSwapInt32HostToBig((uint32_t)nal_size);
		memcpy(dst, &be, 4);
		memcpy(dst + 4, nal, nal_size);
		dst += 4 + nal_size;
	}

	CMSampleBufferRef sample = NULL;
	CMSampleTimingInfo timing = {.duration = kCMTimeInvalid,
				     .presentationTimeStamp = CMTimeMake((int64_t)timestamp_ns, 1000000000),
				     .decodeTimeStamp = kCMTimeInvalid};
	size_t sample_size = avcc_len;
	OSStatus st = CMSampleBufferCreateReady(kCFAllocatorDefault, block, dec->format, 1, 1, &timing, 1,
						&sample_size, &sample);
	CFRelease(block);
	if (st != noErr)
		return false;

	st = VTDecompressionSessionDecodeFrame(dec->session, sample, 0, (void *)(intptr_t)rotation, NULL);
	CFRelease(sample);

	if (st == kVTInvalidSessionErr) {
		/* Pasa tras dormir la Mac: se recrea y se espera otro keyframe. */
		uint8_t saved[MAX_PARAM_BYTES];
		size_t saved_len = dec->params_len;
		memcpy(saved, dec->params, saved_len);
		destroy_session(dec);
		dec->params_len = 0;
		acam_decoder_set_config(dec, saved, saved_len);
		return false;
	}
	if (st == noErr)
		dec->need_keyframe = false;
	return st == noErr;
}
