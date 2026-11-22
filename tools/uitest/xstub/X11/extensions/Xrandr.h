#pragma once
#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
typedef XID RROutput; typedef XID RRCrtc; typedef XID RRMode; typedef XID RRProvider;
typedef unsigned long XRRModeFlags;
typedef unsigned short Rotation; typedef unsigned short SizeID; typedef unsigned short SubpixelOrder; typedef unsigned short Connection;
#define RR_Rotate_0 1
#define RR_Rotate_90 2
#define RR_Rotate_180 4
#define RR_Rotate_270 8
#define RR_Connected 0
#define RR_Disconnected 1
#define RR_Interlace 0x00000010
#define RR_DoubleScan 0x00000020
#define RRScreenChangeNotify 0
#define RRNotify 1
#define RRScreenChangeNotifyMask (1L << 0)
#define RRCrtcChangeNotifyMask (1L << 1)
#define RROutputChangeNotifyMask (1L << 2)
typedef struct _XRRModeInfo { RRMode id; unsigned int width; unsigned int height; unsigned long dotClock; unsigned int hSyncStart; unsigned int hSyncEnd; unsigned int hTotal; unsigned int hSkew; unsigned int vSyncStart; unsigned int vSyncEnd; unsigned int vTotal; char *name; unsigned int nameLength; XRRModeFlags modeFlags; } XRRModeInfo;
typedef struct _XRRScreenResources { Time timestamp; Time configTimestamp; int ncrtc; RRCrtc *crtcs; int noutput; RROutput *outputs; int nmode; XRRModeInfo *modes; } XRRScreenResources;
typedef struct _XRROutputInfo { Time timestamp; RRCrtc crtc; char *name; int nameLen; unsigned long mm_width; unsigned long mm_height; Connection connection; SubpixelOrder subpixel_order; int ncrtc; RRCrtc *crtcs; int nclone; RROutput *clones; int nmode; int npreferred; RRMode *modes; } XRROutputInfo;
typedef struct _XRRCrtcInfo { Time timestamp; int x, y; unsigned int width, height; RRMode mode; Rotation rotation; int noutput; RROutput *outputs; Rotation rotations; int npossible; RROutput *possible; } XRRCrtcInfo;
typedef struct _XRRCrtcGamma { int size; unsigned short *red; unsigned short *green; unsigned short *blue; } XRRCrtcGamma;
typedef struct { int type; unsigned long serial; Bool send_event; Display *display; Window window; Window root; Time timestamp; Time config_timestamp; SizeID size_index; SubpixelOrder subpixel_order; Rotation rotation; int width; int height; int mwidth; int mheight; } XRRScreenChangeNotifyEvent;
