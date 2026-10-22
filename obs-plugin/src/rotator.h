/* Giro de cuadros NV12 por hardware (VTPixelRotationSession). */
#pragma once

#include <CoreVideo/CoreVideo.h>

struct acam_rotator;

struct acam_rotator *acam_rotator_create(void);
void acam_rotator_destroy(struct acam_rotator *r);

/* Gira `src` `degrees` (90, 180, 270) en sentido horario. Devuelve un buffer retenido (CFRelease) o NULL. */
CVPixelBufferRef acam_rotate(struct acam_rotator *r, CVPixelBufferRef src, int degrees);
