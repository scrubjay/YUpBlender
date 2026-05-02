# =============================================================================
# reconfigure_gpu.ps1
# Reconfigures the Blender CMake build with CUDA + OptiX GPU support.
# Targets: NVIDIA GeForce RTX 3090 (Ampere sm_86)
#
# PREREQUISITES:
#   1. NVIDIA OptiX SDK 8.x installed
#      Download: https://developer.nvidia.com/designworks/optix/downloads/legacy
#   2. CUDA Toolkit 11.x or 12.x installed
#      Download: https://developer.nvidia.com/cuda-downloads
#   3. Original Blender build already configured (CMakeCache.txt exists)
# =============================================================================

$ErrorActionPreference = "Stop"

$BuildDir   = "$PSScriptRoot\build_windows_x64_vc17_Release"
$SourceDir  = "$PSScriptRoot\blender"
$CacheFile  = "$BuildDir\CMakeCache.txt"

# --- Validate build directory exists ---
if (-not (Test-Path $CacheFile)) {
    Write-Error "CMakeCache.txt not found at '$CacheFile'. Run the initial Blender build first."
    exit 1
}

# =============================================================================
# Auto-detect OptiX SDK installation path
# =============================================================================
$OptiXSearchRoots = @(
    "C:\ProgramData\NVIDIA Corporation",
    "C:\Program Files\NVIDIA Corporation",
    "C:\Program Files (x86)\NVIDIA Corporation"
)

$OptiXRoot = $null
foreach ($root in $OptiXSearchRoots) {
    if (Test-Path $root) {
        $found = Get-ChildItem -Path $root -Directory -Filter "OptiX SDK *" |
                 Sort-Object Name -Descending |
                 Select-Object -First 1
        if ($found) {
            $OptiXRoot = $found.FullName
            break
        }
    }
}

if (-not $OptiXRoot) {
    Write-Host ""
    Write-Host "  [ERROR] OptiX SDK not found." -ForegroundColor Red
    Write-Host ""
    Write-Host "  Please install the NVIDIA OptiX 8.x SDK from:" -ForegroundColor Yellow
    Write-Host "  https://developer.nvidia.com/designworks/optix/downloads/legacy" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "  After installing, re-run this script." -ForegroundColor Yellow
    exit 1
}

# =============================================================================
# Auto-detect CUDA Toolkit installation path
# =============================================================================
$CudaRoot = $env:CUDA_PATH
if (-not $CudaRoot) {
    # Try to find via registry
    $CudaRegKey = "HKLM:\SOFTWARE\NVIDIA Corporation\GPU Computing Toolkit\CUDA"
    if (Test-Path $CudaRegKey) {
        $CudaVersion = (Get-ChildItem $CudaRegKey | Sort-Object PSChildName -Descending | Select-Object -First 1).PSChildName
        $CudaRoot = (Get-ItemProperty "$CudaRegKey\$CudaVersion").InstallDir
    }
}
if (-not $CudaRoot) {
    # Fallback: search common locations
    $CudaRoot = Get-ChildItem "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA" -Directory -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending | Select-Object -ExpandProperty FullName -First 1
}

# =============================================================================
# Print detected configuration
# =============================================================================
Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  Blender GPU Reconfiguration Script" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "  Build dir  : $BuildDir" -ForegroundColor White
Write-Host "  Source dir : $SourceDir" -ForegroundColor White
Write-Host "  OptiX SDK  : $OptiXRoot" -ForegroundColor Green
if ($CudaRoot) {
    Write-Host "  CUDA root  : $CudaRoot" -ForegroundColor Green
} else {
    Write-Host "  CUDA root  : (not found - will rely on PATH)" -ForegroundColor Yellow
}
Write-Host "  GPU arch   : sm_86  (RTX 3090 / Ampere)" -ForegroundColor Green
Write-Host ""

# =============================================================================
# Confirm before proceeding
# =============================================================================
$confirm = Read-Host "Proceed with reconfiguration? [Y/n]"
if ($confirm -match '^[Nn]') {
    Write-Host "Aborted." -ForegroundColor Yellow
    exit 0
}

# =============================================================================
# Build cmake arguments
# =============================================================================
$CmakeArgs = @(
    "-B", $BuildDir,
    "-S", $SourceDir,

    # Enable CUDA kernel compilation (required for GPU rendering)
    "-DWITH_CYCLES_CUDA_BINARIES=ON",

    # Enable OptiX ray-tracing backend
    "-DWITH_CYCLES_DEVICE_OPTIX=ON",

    # Enable CUDA compute backend
    "-DWITH_CYCLES_DEVICE_CUDA=ON",

    # Point CMake to the OptiX SDK
    "-DOPTIX_ROOT_DIR=$OptiXRoot",

    # Only compile for RTX 3090 (sm_86) to save time.
    # Add sm_89 for RTX 4090, sm_120 for RTX 5090 if needed.
    "-DCYCLES_CUDA_BINARIES_ARCH=sm_86"

)

if ($CudaRoot) {
    # Convert backslashes to forward slashes - CMake 4.x FindCUDA.cmake
    # fails to parse paths with spaces when they contain backslashes.
    $CudaRootFwd  = $CudaRoot -replace '\\', '/'
    $NvccExe      = "$CudaRootFwd/bin/nvcc.exe"
    $OptiXRootFwd = $OptiXRoot -replace '\\', '/'

    # Passing CUDA_NVCC_EXECUTABLE directly bypasses the broken
    # find_program() call in FindCUDA.cmake that breaks on paths with spaces.
    $CmakeArgs += "-DCUDA_TOOLKIT_ROOT_DIR=$CudaRootFwd"
    $CmakeArgs += "-DCUDA_NVCC_EXECUTABLE=$NvccExe"
    $CmakeArgs += "-DOPTIX_ROOT_DIR=$OptiXRootFwd"   # update to fwd-slash too

    Write-Host "  nvcc path  : $NvccExe" -ForegroundColor Green
    if (-not (Test-Path ($NvccExe -replace '/', '\'))) {
        Write-Host "  [WARNING] nvcc.exe not found at expected path!" -ForegroundColor Yellow
    }
}

# =============================================================================
# Run CMake reconfigure
# =============================================================================
Write-Host ""
Write-Host "Running CMake reconfigure..." -ForegroundColor Cyan
Write-Host ""

cmake @CmakeArgs

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "  [ERROR] CMake configuration failed (exit code $LASTEXITCODE)." -ForegroundColor Red
    Write-Host "  Check the output above for details." -ForegroundColor Yellow
    exit $LASTEXITCODE
}

# =============================================================================
# Verify OptiX was found after reconfigure
# =============================================================================
$OptiXFound = Select-String -Path $CacheFile -Pattern "OPTIX_INCLUDE_DIR:PATH=.*include" -Quiet
if (-not $OptiXFound) {
    Write-Host ""
    Write-Host "  [WARNING] CMake succeeded but OptiX include dir was not resolved." -ForegroundColor Yellow
    Write-Host "  Check that your OptiX SDK contains an 'include' subfolder." -ForegroundColor Yellow
    Write-Host "  Expected path: $OptiXRoot\include" -ForegroundColor Yellow
} else {
    Write-Host ""
    Write-Host "  [OK] OptiX SDK detected successfully." -ForegroundColor Green
}

# =============================================================================
# Prompt to build
# =============================================================================
Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  CMake reconfiguration complete!" -ForegroundColor Green
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "  NOTE: Building CUDA kernels takes a long time (20-60 min)." -ForegroundColor Yellow
Write-Host "        Go get a coffee." -ForegroundColor Yellow
Write-Host ""

$buildNow = Read-Host "Start the build now? [Y/n]"
if ($buildNow -notmatch '^[Nn]') {
    Write-Host ""
    Write-Host "Building Blender with GPU support..." -ForegroundColor Cyan
    Write-Host "(Using all available CPU cores)" -ForegroundColor White
    Write-Host ""

    $Cores = (Get-CimInstance Win32_Processor).NumberOfLogicalProcessors

    cmake --build $BuildDir --config Release --target install --parallel $Cores

    if ($LASTEXITCODE -eq 0) {
        Write-Host ""
        Write-Host "  [DONE] Build complete!" -ForegroundColor Green
        Write-Host "  Your GPU-enabled Blender is in: $BuildDir\bin\Release" -ForegroundColor Cyan
    } else {
        Write-Host ""
        Write-Host "  [ERROR] Build failed (exit code $LASTEXITCODE)." -ForegroundColor Red
    }
} else {
    Write-Host ""
    Write-Host "  To build manually, run:" -ForegroundColor White
    Write-Host "  cmake --build '$BuildDir' --config Release --target install" -ForegroundColor Cyan
}

Write-Host ""
