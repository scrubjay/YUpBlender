# =============================================================================
# publish_release.ps1
# Zips the Blender Release folder and publishes it as a GitHub Release,
# uploading the zip as a downloadable release asset.
#
# PREREQUISITES:
#   A GitHub Personal Access Token (PAT) with 'repo' scope.
#   Create one at: https://github.com/settings/tokens/new
# =============================================================================

$ErrorActionPreference = "Stop"

# --- Config ------------------------------------------------------------------
$Repo        = "scrubjay/YUpBlender"
$ReleaseDir  = "$PSScriptRoot\build_windows_x64_vc17_Release\bin\Release"
$ZipOut      = "$PSScriptRoot\blender_yup_gpu_win64.zip"
$TagName     = "v5.2-yup-gpu"
$ReleaseName = "Blender 5.2 Y-Up + GPU (CUDA/OptiX RTX 3090)"
$ReleaseBody = @"
## Blender 5.2 - Y-Up Orientation + GPU Rendering (Windows x64)

Custom build of Blender with two modifications:

### Changes
- **Y-Up viewport orientation** — Y axis is up, Z is forward (matches game engines like Unity/Unreal)
- **CUDA + OptiX GPU support** — compiled with CUDA 13.2 and OptiX 9.1 for NVIDIA RTX cards

### GPU Setup
After first launch, enable your GPU under:
**Edit → Preferences → System → Cycles Render Devices**
Select **OptiX** (fastest) or **CUDA** and check your RTX card.

### Requirements
- Windows 10/11 x64
- NVIDIA RTX GPU (tested on RTX 3090)
- Latest NVIDIA drivers
"@
# -----------------------------------------------------------------------------

# Prompt for PAT
Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  Blender GitHub Release Publisher" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "  Repo    : $Repo" -ForegroundColor White
Write-Host "  Tag     : $TagName" -ForegroundColor White
Write-Host "  Source  : $ReleaseDir" -ForegroundColor White
Write-Host "  Zip out : $ZipOut" -ForegroundColor White
Write-Host ""
Write-Host "  You need a GitHub PAT with 'repo' scope." -ForegroundColor Yellow
Write-Host "  Create one at: https://github.com/settings/tokens/new" -ForegroundColor Cyan
Write-Host ""

$PatSecure = Read-Host "Enter your GitHub PAT" -AsSecureString
$Pat = [System.Runtime.InteropServices.Marshal]::PtrToStringAuto(
           [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($PatSecure))

$Headers = @{
    "Authorization" = "Bearer $Pat"
    "Accept"        = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
}

# --- Step 1: Zip the release folder ------------------------------------------
if (Test-Path $ZipOut) {
    Write-Host ""
    Write-Host "  Found existing zip, deleting..." -ForegroundColor Yellow
    Remove-Item $ZipOut -Force
}

Write-Host ""
Write-Host "  [1/4] Zipping release folder..." -ForegroundColor Cyan
Write-Host "        This may take a few minutes (~942 MB)..." -ForegroundColor White

Compress-Archive -Path "$ReleaseDir\*" -DestinationPath $ZipOut -CompressionLevel Optimal

$ZipSizeMB = [math]::Round((Get-Item $ZipOut).Length / 1MB, 1)
Write-Host "        Done! Zip size: $ZipSizeMB MB" -ForegroundColor Green

# --- Step 2: Create the GitHub release ---------------------------------------
Write-Host ""
Write-Host "  [2/4] Creating GitHub release '$TagName'..." -ForegroundColor Cyan

$ReleasePayload = @{
    tag_name         = $TagName
    target_commitish = "main"
    name             = $ReleaseName
    body             = $ReleaseBody
    draft            = $false
    prerelease       = $false
} | ConvertTo-Json

try {
    $ReleaseResp = Invoke-RestMethod `
        -Uri "https://api.github.com/repos/$Repo/releases" `
        -Method POST `
        -Headers $Headers `
        -Body $ReleasePayload `
        -ContentType "application/json"
    Write-Host "        Release created: $($ReleaseResp.html_url)" -ForegroundColor Green
}
catch {
    # If tag already exists, fetch the existing release
    if ($_.Exception.Response.StatusCode -eq 422) {
        Write-Host "        Release tag already exists, fetching existing release..." -ForegroundColor Yellow
        $ReleaseResp = Invoke-RestMethod `
            -Uri "https://api.github.com/repos/$Repo/releases/tags/$TagName" `
            -Method GET `
            -Headers $Headers
        Write-Host "        Using existing release: $($ReleaseResp.html_url)" -ForegroundColor Green
    } else {
        throw
    }
}

$UploadUrl = $ReleaseResp.upload_url -replace '\{.*\}', ''
$ReleaseId = $ReleaseResp.id

# --- Step 3: Delete existing asset if re-uploading ---------------------------
$AssetName = [System.IO.Path]::GetFileName($ZipOut)
$ExistingAssets = Invoke-RestMethod `
    -Uri "https://api.github.com/repos/$Repo/releases/$ReleaseId/assets" `
    -Method GET `
    -Headers $Headers

$Existing = $ExistingAssets | Where-Object { $_.name -eq $AssetName }
if ($Existing) {
    Write-Host ""
    Write-Host "  [3/4] Removing old asset '$AssetName'..." -ForegroundColor Yellow
    Invoke-RestMethod `
        -Uri "https://api.github.com/repos/$Repo/releases/assets/$($Existing.id)" `
        -Method DELETE `
        -Headers $Headers | Out-Null
} else {
    Write-Host ""
    Write-Host "  [3/4] No existing asset to remove." -ForegroundColor White
}

# --- Step 4: Upload the zip --------------------------------------------------
Write-Host ""
Write-Host "  [4/4] Uploading $AssetName ($ZipSizeMB MB)..." -ForegroundColor Cyan
Write-Host "        This will take a while on large files..." -ForegroundColor White

$UploadHeaders = $Headers.Clone()
$UploadHeaders["Content-Type"] = "application/zip"

$ZipBytes = [System.IO.File]::ReadAllBytes($ZipOut)

$AssetResp = Invoke-RestMethod `
    -Uri "${UploadUrl}?name=$AssetName&label=Windows+x64+Installer" `
    -Method POST `
    -Headers $UploadHeaders `
    -Body $ZipBytes

Write-Host ""
Write-Host "============================================================" -ForegroundColor Green
Write-Host "  Release published!" -ForegroundColor Green
Write-Host "============================================================" -ForegroundColor Green
Write-Host ""
Write-Host "  Release page : $($ReleaseResp.html_url)" -ForegroundColor Cyan
Write-Host "  Download URL : $($AssetResp.browser_download_url)" -ForegroundColor Cyan
Write-Host ""
