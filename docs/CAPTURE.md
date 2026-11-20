# Capturing a scene with a phone

Gaussian Splatting needs many photos of the same static scene from different, known viewpoints.
The camera poses are not known in advance: COLMAP (Structure-from-Motion) recovers them by matching
features between overlapping frames. Most failures come from the video, not the software, so a
two-minute video shot carefully beats a ten-minute careless one.

## Camera settings

| Setting | Value | Why |
|---|---|---|
| Resolution / frame rate | 1080p or 4K, 30 fps | 4K gives sharper detail; 30 fps keeps per-frame motion blur low |
| Lens | Main (1×) lens, **no zoom** | Zoom and lens switching change the focal length mid-video; COLMAP assumes one camera |
| Exposure / focus | **Lock** (long-press on the subject in most Android camera apps → AE/AF lock) | Brightness changes between frames look like different surfaces to the trainer |
| Stabilisation | Off if the app allows it | Electronic stabilisation crops and warps frames slightly differently each time |
| HDR / beauty / night modes | Off | They alter colours frame to frame |
| Orientation | Landscape | More overlap between neighbouring frames |

## How to move

- **Walk around the subject, do not rotate on the spot.** Parallax (near things moving more than
  far things) is what lets SfM recover depth. Spinning in place gives none.
- **Slowly**: about one full circle per 30–60 s. Each frame should overlap its neighbours by ≥ 70 %.
- **Two or three loops at different heights** (chest, above, below) for an object; for a room, walk
  along the walls looking inwards, then through the middle.
- Keep the subject filling roughly half the frame; avoid pointing at blank walls or the sky.
- Finish where you started (closing the loop helps COLMAP correct drift).

## Good and bad subjects

| Works well | Struggles |
|---|---|
| Textured, matte objects (shoes, plants, statues, food, furniture) | Mirrors, glass, chrome, water |
| Static scenes with even, diffuse light (overcast day, bright room) | Moving people, pets, cars, leaves in wind |
| Rooms with posters, books, clutter | White walls, repetitive tiles, dark rooms |

## First test

1. **Small object on a table** (e.g. a shoe or a plant), 3 loops, about 1–2 minutes.
2. **A room or an outdoor corner**, about 2 minutes.

Copy the `.mp4` files from the phone to `F:\gsplat-viewer\scenes\<name>\<name>.mp4`
(the `scenes/` folder is git-ignored).

## Pipeline

```
video.mp4 ─ gsplat_prep ─► images/ (sharp frames) ─ COLMAP ─► camera poses + sparse points
          ─ Brush ─► <name>.ply ─► gsplat_viewer (and gsplat_eval for PSNR / SSIM)
```

`tools/capture.ps1 scenes/<name>/<name>.mp4` runs every step and opens the result.
