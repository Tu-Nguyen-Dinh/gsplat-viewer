@echo off
rem Phase 4: 3 M splat demo scene drawn with level of detail (at most 1 M splats). Toggle LOD in the panel to compare FPS.
cd /d "%~dp0.."
if not exist build\demo_3m.ply build\gsplat_viewer.exe --gen build\demo_3m.ply 3000000
start "" build\gsplat_viewer.exe build\demo_3m.ply --lod 1000000
