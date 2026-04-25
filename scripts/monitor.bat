@echo off
set PORT=COM59
set BAUD=115200
title Box2Robot Monitor (Auto-Reconnect)

:loop
echo.
echo === Monitor %PORT% @ %BAUD% baud ^(auto-reconnect^) ===
pio device monitor -p %PORT% -b %BAUD% --filter direct --filter esp32_exception_decoder
echo.
echo [Disconnected. Reconnecting in 1s... Press Ctrl+C to stop]
timeout /t 1 /nobreak > nul
goto loop
