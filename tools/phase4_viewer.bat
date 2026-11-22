@echo off
rem Phase 4: phong scene + LAS layer. Press M and click two points to measure; panel "Layers, measure, LOD".
cd /d "%~dp0.."
start "" build\gsplat_viewer.exe scenes\phong\phong.ply --las scenes\phong\phong_points.las --las-transform 1,1,0,0,0,0,0,0
