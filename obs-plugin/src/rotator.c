#include "rotator.h"

#include <VideoToolbox/VideoToolbox.h>
#include <stdlib.h>

struct acam_rotator {
	VTPixelRotationSessionRef session;
	int degrees;
	CVPixelBufferPoolRef pool;
	size_t pool_w, pool_h;
	OSType pool_format;
};

struct acam_rotator *acam_rotator_create(void)
{
	return calloc(1, sizeof(struct acam_rotator));
}

void acam_rotator_destroy(struct acam_rotator *r)
{
	if (!r)
		return;
	if (r->session) {
		VTPixelRotationSessionInvalidate(r->session);
		CFRelease(r->session);
	}
	if (r->pool)
		CVPixelBufferPoolRelease(r->pool);
	free(r);
}

static bool ensure_pool(struct acam_rotator *r, size_t w, size_t h, OSType format)
{
	if (r->pool && r->pool_w == w && r->pool_h == h && r->pool_format == format)
		return true;
	if (r->pool) {
		CVPixelBufferPoolRelease(r->pool);
		r->pool = NULL;
	}

	int32_t iw = (int32_t)w, ih = (int32_t)h, fmt = (int32_t)format;
	CFNumberRef nw = CFNumberCreate(NULL, kCFNumberSInt32Type, &iw);
	CFNumberRef nh = CFNumberCreate(NULL, kCFNumberSInt32Type, &ih);
	CFNumberRef nf = CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt);
	CFDictionaryRef iosurface = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
						       &kCFTypeDictionaryValueCallBacks);
	const void *keys[] = {kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferPixelFormatTypeKey,
			      kCVPixelBufferIOSurfacePropertiesKey};
	const void *vals[] = {nw, nh, nf, iosurface};
	CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 4, &kCFTypeDictionaryKeyCallBacks,
						   &kCFTypeDictionaryValueCallBacks);
	CVReturn rc = CVPixelBufferPoolCreate(NULL, NULL, attrs, &r->pool);
	CFRelease(attrs);
	CFRelease(iosurface);
	CFRelease(nw);
	CFRelease(nh);
	CFRelease(nf);
	if (rc != kCVReturnSuccess)
		return false;

	r->pool_w = w;
	r->pool_h = h;
	r->pool_format = format;
	return true;
}

CVPixelBufferRef acam_rotate(struct acam_rotator *r, CVPixelBufferRef src, int degrees)
{
	if (!r->session && VTPixelRotationSessionCreate(NULL, &r->session) != noErr)
		return NULL;

	if (degrees != r->degrees) {
		CFStringRef angle = degrees == 90    ? kVTRotation_CW90
				    : degrees == 180 ? kVTRotation_180
						     : kVTRotation_CCW90;
		if (VTSessionSetProperty(r->session, kVTPixelRotationPropertyKey_Rotation, angle) != noErr)
			return NULL;
		r->degrees = degrees;
	}

	size_t w = CVPixelBufferGetWidth(src), h = CVPixelBufferGetHeight(src);
	bool swap = degrees != 180;
	if (!ensure_pool(r, swap ? h : w, swap ? w : h, CVPixelBufferGetPixelFormatType(src)))
		return NULL;

	CVPixelBufferRef dst = NULL;
	if (CVPixelBufferPoolCreatePixelBuffer(NULL, r->pool, &dst) != kCVReturnSuccess)
		return NULL;
	if (VTPixelRotationSessionRotateImage(r->session, src, dst) != noErr) {
		CFRelease(dst);
		return NULL;
	}
	return dst;
}
