$ErrorActionPreference = "Stop"

$serverPath = Join-Path $PSScriptRoot "server.exe"

if (-not (Test-Path $serverPath)) {
    throw "server.exe not found. Build the server target first."
}

$output = & $serverPath `
    --query "INSERT INTO users VALUES ('Alice', 20);" `
    --query "INSERT INTO users VALUES ('Bob', 30);" `
    --query "SELECT * FROM users WHERE id = 2;" `
    --query "SELECT * FROM users WHERE name = 'Alice';" `
    --query "QUIT"

if ($LASTEXITCODE -ne 0) {
    throw "server.exe exited with code $LASTEXITCODE"
}

$joined = $output -join "`n"
$expectedLines = @(
    "OK INSERT id=1 used_index=false",
    "OK INSERT id=2 used_index=false",
    "OK SELECT rows=1 used_index=true",
    "ROW id=2 name=Bob age=30",
    "OK SELECT rows=1 used_index=false",
    "ROW id=1 name=Alice age=20",
    "BYE"
)

foreach ($line in $expectedLines) {
    if (-not $joined.Contains($line)) {
        throw "Expected output line missing: $line"
    }
}

Write-Host "server CLI smoke test passed."
