/* BSI-Quest Android platform layer (implemented in android_glue.c) */
#pragma once
#include <stdint.h>
enum { AIN_KEY=1, AIN_MOUSE, AIN_BUTTON, AIN_WHEEL, AIN_QUIT, AIN_MOUSEABS };   /* AIN_MOUSEABS: a,b = position 0..65535 across the window */
typedef struct { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; } AndroidPad;   /* XInput layout/semantics */
int android_input_next(int *type, int *a, int *b);     /* drain queued keyboard/mouse events */
int android_pad(AndroidPad *p);                         /* 1 = a gamepad is connected */
void *android_window(void);                             /* ANativeWindow* (blocks until the surface exists) */
int android_audio_write(const float *frames, int nframes); /* stereo float32 48 kHz, blocking */
int android_audio_queued(void);
int android_audio_target(void);                          /* frames queued in the output stream, -1 = no output */
