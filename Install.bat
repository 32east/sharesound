@echo off
chcp 65001 >nul
title sharesound - install
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
echo.
pause
