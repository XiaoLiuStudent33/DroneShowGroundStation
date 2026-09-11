@echo off
setlocal
pushd "%~dp0"
if not exist "x64\Release\DroneGCS.exe" (
  echo Build ConsoleGCS Release first.
  pause
  popd
  exit /b 1
)
"x64\Release\DroneGCS.exe" "gcs-config-simulation.ini"
popd
endlocal
