@echo off
rem Release step 2: commit + tag v2.0.0 + push (see release_push.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0release_push.ps1"
