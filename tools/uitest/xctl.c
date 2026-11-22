// Tiny X11 driver: xctl cmd... ; cmds: move X Y | down B | up B | click B | key NAME | kdown NAME | kup NAME | sleep MS | shot F.ppm
#include <X11/Xlib.h>
#include <X11/Xutil.h>
extern int XTestFakeMotionEvent(Display*, int, int, int, unsigned long);
extern int XTestFakeButtonEvent(Display*, unsigned int, Bool, unsigned long);
extern int XTestFakeKeyEvent(Display*, unsigned int, Bool, unsigned long);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char** argv) {
  Display* d = XOpenDisplay(NULL);
  if (!d) { fprintf(stderr, "no display\n"); return 1; }
  for (int i = 1; i < argc; ++i) {
    const char* c = argv[i];
    if (!strcmp(c, "move")) { XTestFakeMotionEvent(d, -1, atoi(argv[i+1]), atoi(argv[i+2]), 0); i += 2; }
    else if (!strcmp(c, "down")) { XTestFakeButtonEvent(d, atoi(argv[++i]), True, 0); }
    else if (!strcmp(c, "up")) { XTestFakeButtonEvent(d, atoi(argv[++i]), False, 0); }
    else if (!strcmp(c, "click")) { int b = atoi(argv[++i]); XTestFakeButtonEvent(d, b, True, 0); XFlush(d); usleep(60000); XTestFakeButtonEvent(d, b, False, 0); }
    else if (!strcmp(c, "key") || !strcmp(c, "kdown") || !strcmp(c, "kup")) {
      KeyCode k = XKeysymToKeycode(d, XStringToKeysym(argv[i+1]));
      if (strcmp(c, "kup")) XTestFakeKeyEvent(d, k, True, 0);
      if (!strcmp(c, "key")) { XFlush(d); usleep(80000); }
      if (strcmp(c, "kdown")) XTestFakeKeyEvent(d, k, False, 0);
      ++i;
    }
    else if (!strcmp(c, "sleep")) { XFlush(d); usleep(atoi(argv[++i]) * 1000); }
    else if (!strcmp(c, "shot")) {
      XFlush(d); XSync(d, False);
      Window r = DefaultRootWindow(d); XWindowAttributes a; XGetWindowAttributes(d, r, &a);
      XImage* im = XGetImage(d, r, 0, 0, a.width, a.height, AllPlanes, ZPixmap);
      FILE* f = fopen(argv[++i], "wb"); fprintf(f, "P6\n%d %d\n255\n", a.width, a.height);
      for (int y = 0; y < a.height; ++y) for (int x = 0; x < a.width; ++x) { unsigned long p = XGetPixel(im, x, y); unsigned char rgb[3] = {(p >> 16) & 255, (p >> 8) & 255, p & 255}; fwrite(rgb, 1, 3, f); }
      fclose(f); XDestroyImage(im);
    }
    XFlush(d);
    usleep(20000);
  }
  XCloseDisplay(d);
  return 0;
}
