#pragma once
#include <X11/Xlib.h>
#include <X11/extensions/XI2.h>
typedef struct { int deviceid; int mask_len; unsigned char *mask; } XIEventMask;
typedef struct { int mask_len; unsigned char *mask; double *values; } XIValuatorState;
typedef struct { int type; unsigned long serial; Bool send_event; Display *display; int extension; int evtype; Time time; int deviceid; int sourceid; int detail; int flags; XIValuatorState valuators; double *raw_values; } XIRawEvent;
