@echo off
chcp 65001 > nul
setlocal
cd /d "%~dp0"
"C:\Program Files\Google\Chrome\Application\chrome.exe" --headless --disable-gpu --no-sandbox --no-pdf-header-footer --print-to-pdf="%~dp0docs\使用手册.pdf" "file:///%~dp0docs\使用手册.html"
exit /b %ERRORLEVEL%
