@echo off
rem Release step 1 (read-only): git state + Android build check, see release_check.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0release_check.ps1"
