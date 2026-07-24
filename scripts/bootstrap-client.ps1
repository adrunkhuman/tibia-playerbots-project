param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$clientRoot = Join-Path $projectRoot "client"
$thingsRoot = Join-Path $clientRoot "data\things\860"
$executable = Join-Path $clientRoot "otclient_gl_x64.exe"
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) "angelion-client-$PID"

$expectedExecutableHash = "3054ec603454cc71536851da979d11a02546f0348473b1f2a931ae4c10bf6b55"
$expectedArchiveHash = "b1bc73c3b1fb8989fee17498d3189968374a97836278314c18f058935723e6be"
$expectedDatHash = "38e4e23ee550503842388e3aa3119125692316facf5adf97f4c24385735ddaee"
$expectedSprHash = "672351b1a3dd0f45da7c76bca35a1d8f7fbf1db3be65e080aeb1d8789c61a575"

function Test-ExpectedHash {
    param(
        [string]$Path,
        [string]$Expected
    )

    return (Test-Path -LiteralPath $Path) -and
        ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Expected)
}

if (-not (Test-Path -LiteralPath $clientRoot)) {
    throw "Client source directory not found: $clientRoot"
}

New-Item -ItemType Directory -Path $tempRoot | Out-Null

try {
    if ($Force -or -not (Test-ExpectedHash -Path $executable -Expected $expectedExecutableHash)) {
        if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
            throw "GitHub CLI is required to download the verified Redemption artifact."
        }

        $artifactRoot = Join-Path $tempRoot "redemption"
        & gh run download 29866426953 `
            --repo opentibiabr/otclient `
            --name windows-solution-opengl `
            --dir $artifactRoot

        if ($LASTEXITCODE -ne 0) {
            throw "Failed to download the Redemption GitHub Actions artifact."
        }

        $downloadedExecutable = Join-Path $artifactRoot "otclient_gl_x64.exe"
        if (-not (Test-ExpectedHash -Path $downloadedExecutable -Expected $expectedExecutableHash)) {
            throw "Redemption executable SHA-256 mismatch."
        }

        Copy-Item -LiteralPath $downloadedExecutable -Destination $executable -Force
    }

    $datFile = Join-Path $thingsRoot "Tibia.dat"
    $sprFile = Join-Path $thingsRoot "Tibia.spr"
    $assetsValid = (Test-ExpectedHash -Path $datFile -Expected $expectedDatHash) -and
        (Test-ExpectedHash -Path $sprFile -Expected $expectedSprHash)

    if ($Force -or -not $assetsValid) {
        $session = New-Object Microsoft.PowerShell.Commands.WebRequestSession
        Invoke-WebRequest -Uri "https://api.otservers.online/sanctum/csrf-cookie" -WebSession $session | Out-Null
        $xsrfCookie = $session.Cookies.GetCookies("https://api.otservers.online") |
            Where-Object Name -eq "XSRF-TOKEN"
        $xsrfToken = [System.Uri]::UnescapeDataString($xsrfCookie.Value)
        $headers = @{
            Accept = "application/json"
            Origin = "https://otservers.online"
            Referer = "https://otservers.online/tibia-clients/860"
            "X-XSRF-TOKEN" = $xsrfToken
        }

        $response = Invoke-WebRequest `
            -Uri "https://api.otservers.online/downloads/tibia-client/860?type=assets" `
            -Method Post `
            -Headers $headers `
            -WebSession $session
        $signedUrl = ($response.Content | ConvertFrom-Json).url
        $archive = Join-Path $tempRoot "tibia-860-assets.zip"
        Invoke-WebRequest -Uri $signedUrl -OutFile $archive

        if (-not (Test-ExpectedHash -Path $archive -Expected $expectedArchiveHash)) {
            throw "Tibia 8.60 asset archive SHA-256 mismatch."
        }

        New-Item -ItemType Directory -Path $thingsRoot -Force | Out-Null
        Expand-Archive -LiteralPath $archive -DestinationPath $thingsRoot -Force

        if (-not (Test-ExpectedHash -Path $datFile -Expected $expectedDatHash) -or
            -not (Test-ExpectedHash -Path $sprFile -Expected $expectedSprHash)) {
            throw "Extracted Tibia 8.60 asset SHA-256 mismatch."
        }
    }
}
finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}

"OTClient Redemption and Tibia 8.60 assets are ready."
