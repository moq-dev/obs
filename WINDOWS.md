## Windows Build Instructions

### Build Setup

```powershell
Set-ExecutionPolicy Bypass -Scope Process -Force; [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.ServicePointManager]::SecurityProtocol -bor 3072; iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))
 choco install -y git
 choco install -y cmake --installargs 'ADD_CMAKE_TO_PATH=System'
 choco install visualstudio2026buildtools --package-parameters "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
 choco install rustup
```

During the rustup installation choose the `nightly` variant.

Locate "Visual Studio Installer". Click "Modify". Choose "Desktop Development with C++". Check C++ ATL For x64

### Build the OBS fork

```powershell
cd obs-studio
cmake -G "Visual Studio 18 2026" -A x64 --preset windows-x64
cmake --build --preset windows-x64
```

### Build the obs-moq plugin and install

```powershell
cd obs
cmake -G "Visual Studio 18 2026" -A x64 --preset windows-x64 -DMOQ_LOCAL="../moq"
cmake --build --preset windows-x64 --target install
```

## Debugging Moq Plugin

```powershell
 $env:RUST_LOG="debug"; $env:RUST_BACKTRACE=1; $env:OBS_LOG_LEVEL="debug"; Set-Location "build_x64\rundir\RelWithDebInfo\bin\64bit"; & .\obs64.exe --verbose
```
