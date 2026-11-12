# =============================================================================
# add_sm89_kernel.ps1
#
# Adds RTX 4090 (sm_89) CUDA kernel support to an existing Blender build
# WITHOUT recompiling the C++ executable.
#
# Steps:
#   1. CMake reconfigure - adds sm_89 to CYCLES_CUDA_BINARIES_ARCH
#   2. Builds ONLY the cycles_kernel_cuda target (nvcc only, ~30-90 min)
#   3. Copies kernel_sm_86.cubin + kernel_sm_89.cubin into bin\Release\...
#   4. Patches the existing blender_yup_gpu_win64.zip with the new cubins
#
# NOTE: The previous GPU build shipped WITHOUT any .cubin files, so Blender
# was always falling back to runtime JIT (which fails with "driver too new"
# on newer cards). This script fixes sm_86 (RTX 3090) and sm_89 (RTX 4090).
#
# PREREQUISITES:
#   - CUDA Toolkit 12.x installed (nvcc must know sm_89)
#   - CMakeCache.txt must exist (run reconfigure_gpu.ps1 first if not)
# =============================================================================

$ErrorActionPreference = "Stop"

$BuildDir    = "$PSScriptRoot\build_windows_x64_vc17_Release"
$SourceDir   = "$PSScriptRoot\blender"
$CacheFile   = "$BuildDir\CMakeCache.txt"
$ZipPath     = "$PSScriptRoot\blender_yup_gpu_win64.zip"

# Where nvcc puts the compiled cubins (build intermediates)
$CubinSrcDir = "$BuildDir\intern\cycles\kernel\device\cuda"

# Where Blender looks for them at runtime
$ReleaseRoot = "$BuildDir\bin\Release"

# =============================================================================
# Validate prerequisites
# =============================================================================
Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  add_sm89_kernel.ps1 - CUDA Kernel Patch" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""

if (-not (Test-Path $CacheFile)) {
    Write-Host "  [ERROR] CMakeCache.txt not found at:" -ForegroundColor Red
    Write-Host "          $CacheFile" -ForegroundColor Yellow
    Write-Host "  Run reconfigure_gpu.ps1 first to set up the build." -ForegroundColor Yellow
    exit 1
}

# Find CUDA root
$CudaRoot = $env:CUDA_PATH
if (-not $CudaRoot) {
    $CudaRegKey = "HKLM:\SOFTWARE\NVIDIA Corporation\GPU Computing Toolkit\CUDA"
    if (Test-Path $CudaRegKey) {
        $ver = (Get-ChildItem $CudaRegKey | Sort-Object PSChildName -Descending | Select-Object -First 1).PSChildName
        $CudaRoot = (Get-ItemProperty "$CudaRegKey\$ver").InstallDir
    }
}
if (-not $CudaRoot) {
    $CudaRoot = Get-ChildItem "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA" -Directory -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending | Select-Object -ExpandProperty FullName -First 1
}

if (-not $CudaRoot) {
    Write-Host "  [ERROR] CUDA Toolkit not found. Install CUDA 12.x from:" -ForegroundColor Red
    Write-Host "          https://developer.nvidia.com/cuda-downloads" -ForegroundColor Cyan
    exit 1
}

# Check nvcc
$NvccExe = Join-Path $CudaRoot "bin\nvcc.exe"
if (-not (Test-Path $NvccExe)) {
    Write-Host "  [ERROR] nvcc.exe not found at: $NvccExe" -ForegroundColor Red
    exit 1
}
$NvccVersion = (& $NvccExe --version 2>&1 | Select-String "release" | Select-Object -First 1).ToString().Trim()
Write-Host "  CUDA root  : $CudaRoot" -ForegroundColor Green
Write-Host "  nvcc       : $NvccVersion" -ForegroundColor Green

# Warn if CUDA < 12
if ($NvccVersion -match "release (\d+)\." -and [int]$matches[1] -lt 12) {
    Write-Host ""
    $majVer = $matches[1]
    Write-Host "  [WARNING] CUDA $majVer.x detected. sm_89 (RTX 4090) requires CUDA 12.0+." -ForegroundColor Yellow
    Write-Host "            Compilation may fail. Upgrade to CUDA 12.x and retry." -ForegroundColor Yellow
    Write-Host ""
    $cont = Read-Host "  Continue anyway? [y/N]"
    if ($cont -notmatch '^[Yy]') { exit 0 }
}

# Pull OptiX path from existing cache
$OptiXLine = Select-String -Path $CacheFile -Pattern "OPTIX_ROOT_DIR:PATH=(.*)" | Select-Object -First 1
$OptiXRoot = if ($OptiXLine) { $OptiXLine.Matches[0].Groups[1].Value.Trim() } else { "" }

Write-Host "  Build dir  : $BuildDir" -ForegroundColor White
$optixDisplay = if ($OptiXRoot) { $OptiXRoot } else { "(not found in cache)" }
Write-Host "  OptiX SDK  : $optixDisplay" -ForegroundColor White
Write-Host "  Arch target: sm_86 (RTX 3090) + sm_89 (RTX 4090)" -ForegroundColor Green
Write-Host ""

$confirm = Read-Host "Proceed? This will take 30-90 minutes (nvcc only, no C++ recompile). [Y/n]"
if ($confirm -match '^[Nn]') { Write-Host "Aborted." -ForegroundColor Yellow; exit 0 }

# =============================================================================
# Step 1: CMake reconfigure
# =============================================================================
Write-Host ""
Write-Host "  [1/4] CMake reconfigure (adding sm_89)..." -ForegroundColor Cyan
Write-Host ""

$CudaRootFwd = $CudaRoot -replace '\\', '/'
$NvccFwd     = "$CudaRootFwd/bin/nvcc.exe"

$CmakeArgs = @(
    "-B", $BuildDir,
    "-S", $SourceDir,
    "-DWITH_CYCLES_CUDA_BINARIES=ON",
    "-DWITH_CYCLES_DEVICE_OPTIX=ON",
    "-DWITH_CYCLES_DEVICE_CUDA=ON",
    "-DCYCLES_CUDA_BINARIES_ARCH=sm_86;sm_89",
    "-DCUDA_TOOLKIT_ROOT_DIR=$CudaRootFwd",
    "-DCUDA_NVCC_EXECUTABLE=$NvccFwd"
)
if ($OptiXRoot) {
    $OptiXRootFwd = $OptiXRoot -replace '\\', '/'
    $CmakeArgs += "-DOPTIX_ROOT_DIR=$OptiXRootFwd"
}

cmake @CmakeArgs

if ($LASTEXITCODE -ne 0) {
    Write-Host "  [ERROR] CMake reconfigure failed (exit $LASTEXITCODE)." -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host "  [OK] CMake reconfigure done." -ForegroundColor Green

# =============================================================================
# Step 2: Build ONLY the CUDA kernel target
# =============================================================================
Write-Host ""
Write-Host "  [2/4] Building cycles_kernel_cuda (sm_86 + sm_89)..." -ForegroundColor Cyan
Write-Host "        This is the long step - grab a coffee." -ForegroundColor White
Write-Host ""

$Cores = (Get-CimInstance Win32_Processor).NumberOfLogicalProcessors
cmake --build $BuildDir --config Release --target cycles_kernel_cuda --parallel $Cores

if ($LASTEXITCODE -ne 0) {
    Write-Host "  [ERROR] Kernel build failed (exit $LASTEXITCODE)." -ForegroundColor Red
    exit $LASTEXITCODE
}

# Verify cubins were produced
$Cubins = Get-ChildItem -Path $CubinSrcDir -Filter "kernel_sm_*.cubin" -ErrorAction SilentlyContinue
if (-not $Cubins) {
    Write-Host "  [ERROR] No .cubin files found in: $CubinSrcDir" -ForegroundColor Red
    Write-Host "          Build may have succeeded but output path changed." -ForegroundColor Yellow
    exit 1
}

Write-Host ""
Write-Host "  [OK] Cubins compiled:" -ForegroundColor Green
foreach ($c in $Cubins) {
    $sizeMB = [math]::Round($c.Length / 1MB, 1)
    Write-Host "       $($c.Name)  ($sizeMB MB)" -ForegroundColor White
}

# =============================================================================
# Step 3: Copy cubins into the installed Release tree
# =============================================================================
Write-Host ""
Write-Host "  [3/4] Copying cubins into Release tree..." -ForegroundColor Cyan

$CyclesLibDirs = Get-ChildItem -Path $ReleaseRoot -Recurse -Directory -Filter "lib" -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -like "*cycles*lib*" }

if (-not $CyclesLibDirs) {
    $VersionDir = Get-ChildItem $ReleaseRoot -Directory -ErrorAction SilentlyContinue |
                  Where-Object { $_.Name -match '^\d+\.\d+$' } |
                  Sort-Object Name -Descending | Select-Object -First 1
    if (-not $VersionDir) {
        Write-Host "  [ERROR] Cannot find versioned Blender dir under: $ReleaseRoot" -ForegroundColor Red
        Write-Host "          Expected: $ReleaseRoot\5.2\scripts\addons_core\cycles\lib" -ForegroundColor Yellow
        exit 1
    }
    $CyclesLibPath = "$($VersionDir.FullName)\scripts\addons_core\cycles\lib"
    New-Item -ItemType Directory -Path $CyclesLibPath -Force | Out-Null
    Write-Host "       Created: $CyclesLibPath" -ForegroundColor Yellow
} else {
    $CyclesLibPath = $CyclesLibDirs[0].FullName
}

Write-Host "       Destination: $CyclesLibPath" -ForegroundColor White

foreach ($cubin in $Cubins) {
    $dest = Join-Path $CyclesLibPath $cubin.Name
    Copy-Item $cubin.FullName $dest -Force
    Write-Host "       Copied: $($cubin.Name)" -ForegroundColor Green
}

# =============================================================================
# Step 4: Patch the zip
# =============================================================================
Write-Host ""
$zipName = [System.IO.Path]::GetFileName($ZipPath)
Write-Host "  [4/4] Patching $zipName..." -ForegroundColor Cyan

if (-not (Test-Path $ZipPath)) {
    Write-Host "  [WARNING] Zip not found at: $ZipPath" -ForegroundColor Yellow
    Write-Host "            Skipping zip patch. Re-run publish_release.ps1 to rebuild it." -ForegroundColor Yellow
} else {
    Add-Type -Assembly System.IO.Compression.FileSystem

    $zipObj = [IO.Compression.ZipFile]::Open($ZipPath, [IO.Compression.ZipArchiveMode]::Update)

    # Find the cycles\lib prefix inside the zip
    $sampleEntry = $zipObj.Entries | Where-Object { $_.FullName -like "*cycles*lib*" } | Select-Object -First 1
    if ($sampleEntry) {
        $libPrefix = $sampleEntry.FullName -replace '([/\\]lib[/\\]).*$', '$1'
        $libPrefix = $libPrefix -replace '\\', '/'
    } else {
        $verEntry  = $zipObj.Entries | Where-Object { $_.FullName -match '^\d+\.\d+[/\\]' } | Select-Object -First 1
        $verPrefix = if ($verEntry) { ($verEntry.FullName -split '[/\\]')[0] } else { "5.2" }
        $libPrefix = "$verPrefix/scripts/addons_core/cycles/lib/"
    }

    Write-Host "       Zip lib prefix: $libPrefix" -ForegroundColor White

    foreach ($cubin in $Cubins) {
        $entryName = $libPrefix + $cubin.Name

        # Remove existing entry if present (must enumerate to array first to avoid mutation during iteration)
        $toDelete = @($zipObj.Entries | Where-Object { ($_.FullName -replace '\\','/') -eq $entryName })
        foreach ($e in $toDelete) {
            $e.Delete()
            Write-Host "       Replaced: $entryName" -ForegroundColor Yellow
        }
        if (-not $toDelete) {
            Write-Host "       Added   : $entryName" -ForegroundColor Green
        }

        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zipObj, $cubin.FullName, $entryName,
            [IO.Compression.CompressionLevel]::Optimal
        ) | Out-Null
    }

    $zipObj.Dispose()
    $zipSizeMB = [math]::Round((Get-Item $ZipPath).Length / 1MB, 1)
    Write-Host "       Zip updated: $zipSizeMB MB" -ForegroundColor Green
}

# =============================================================================
# Done
# =============================================================================
Write-Host ""
Write-Host "============================================================" -ForegroundColor Green
Write-Host "  Done! CUDA kernels patched." -ForegroundColor Green
Write-Host "============================================================" -ForegroundColor Green
Write-Host ""
Write-Host "  Next steps:" -ForegroundColor White
Write-Host "    1. Test locally: launch Blender from $ReleaseRoot" -ForegroundColor White
Write-Host "       Edit > Preferences > System > Cycles Render Devices" -ForegroundColor White
Write-Host "       RTX 3090 and RTX 4090 should both appear." -ForegroundColor White
Write-Host "    2. Re-publish: .\publish_release.ps1 -Pat <token>" -ForegroundColor White
Write-Host "       (the zip already has the cubins embedded)" -ForegroundColor White
Write-Host ""
