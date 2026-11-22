#pragma once
#include <X11/Xlib.h>
typedef int XcursorBool; typedef unsigned int XcursorUInt; typedef XcursorUInt XcursorDim; typedef XcursorUInt XcursorPixel;
typedef struct _XcursorImage { XcursorUInt version; XcursorDim size; XcursorDim width; XcursorDim height; XcursorDim xhot; XcursorDim yhot; XcursorUInt delay; XcursorPixel *pixels; } XcursorImage;
