$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$serverPath = Join-Path $projectRoot "build\bin\server.exe"

if (-not (Test-Path $serverPath)) {
    throw "build\bin\server.exe not found. Build the server target first."
}

function Invoke-HttpJson {
    param(
        [string]$Method,
        [string]$Uri,
        [string]$Body = $null
    )

    try {
        if ($null -ne $Body) {
            $response = Invoke-WebRequest -Method $Method -Uri $Uri -UseBasicParsing -ContentType "application/json" -Body $Body -TimeoutSec 5
        }
        else {
            $response = Invoke-WebRequest -Method $Method -Uri $Uri -UseBasicParsing -TimeoutSec 5
        }

        return [PSCustomObject]@{
            StatusCode = [int]$response.StatusCode
            Content = $response.Content
        }
    }
    catch {
        if ($_.Exception.Response) {
            $stream = $_.Exception.Response.GetResponseStream()
            $reader = New-Object System.IO.StreamReader($stream)
            $content = $reader.ReadToEnd()
            $reader.Dispose()

            return [PSCustomObject]@{
                StatusCode = [int]$_.Exception.Response.StatusCode
                Content = $content
            }
        }

        throw
    }
}

function Wait-ForHealth {
    param([int]$Port)

    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        try {
            $response = Invoke-HttpJson -Method "GET" -Uri "http://127.0.0.1:$Port/health"
            if ($response.StatusCode -eq 200) {
                return
            }
        }
        catch {
        }

        Start-Sleep -Milliseconds 100
    }

    throw "HTTP server on port $Port did not become healthy in time."
}

function Start-TestServer {
    param(
        [int]$Port,
        [string[]]$Arguments
    )

    $stdout = Join-Path $PSScriptRoot "server-http-$Port.out.log"
    $stderr = Join-Path $PSScriptRoot "server-http-$Port.err.log"

    if (Test-Path $stdout) { Remove-Item $stdout -Force }
    if (Test-Path $stderr) { Remove-Item $stderr -Force }

    $process = Start-Process -FilePath $serverPath `
        -ArgumentList $Arguments `
        -PassThru `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr

    Wait-ForHealth -Port $Port
    return $process
}

function Assert-Contains {
    param(
        [string]$Text,
        [string]$Needle
    )

    if (-not $Text.Contains($Needle)) {
        throw "Expected response to contain: $Needle`nActual response: $Text"
    }
}

$phase1Port = 18080
$phase1 = $null

try {
    $phase1 = Start-TestServer -Port $phase1Port -Arguments @(
        "--serve",
        "--port", "$phase1Port",
        "--workers", "2",
        "--queue", "4",
        "--max-requests", "6"
    )

    $insert = Invoke-HttpJson -Method "POST" -Uri "http://127.0.0.1:$phase1Port/query" -Body '{"query":"INSERT INTO users VALUES (''Alice'', 20);"}'
    if ($insert.StatusCode -ne 200) { throw "Insert request failed: $($insert.Content)" }
    Assert-Contains -Text $insert.Content -Needle '"action":"insert"'
    Assert-Contains -Text $insert.Content -Needle '"insertedId":1'

    $select = Invoke-HttpJson -Method "POST" -Uri "http://127.0.0.1:$phase1Port/query" -Body '{"query":"SELECT * FROM users WHERE id = 1;"}'
    if ($select.StatusCode -ne 200) { throw "Select request failed: $($select.Content)" }
    Assert-Contains -Text $select.Content -Needle '"usedIndex":true'
    Assert-Contains -Text $select.Content -Needle '"name":"Alice"'

    $empty = Invoke-HttpJson -Method "POST" -Uri "http://127.0.0.1:$phase1Port/query" -Body '{"query":"SELECT * FROM users WHERE id = 999;"}'
    if ($empty.StatusCode -ne 200) { throw "Empty select request failed: $($empty.Content)" }
    Assert-Contains -Text $empty.Content -Needle '"rowCount":0'
    Assert-Contains -Text $empty.Content -Needle '"rows":[]'

    $syntax = Invoke-HttpJson -Method "POST" -Uri "http://127.0.0.1:$phase1Port/query" -Body '{"query":"SELECT * FORM users;"}'
    if ($syntax.StatusCode -ne 400) { throw "Syntax error request should return 400: $($syntax.Content)" }
    Assert-Contains -Text $syntax.Content -Needle '"syntax_error"'

    $metrics = Invoke-HttpJson -Method "GET" -Uri "http://127.0.0.1:$phase1Port/metrics"
    if ($metrics.StatusCode -ne 200) { throw "Metrics request failed: $($metrics.Content)" }
    Assert-Contains -Text $metrics.Content -Needle '"totalHealthRequests":1'
    Assert-Contains -Text $metrics.Content -Needle '"totalMetricsRequests":1'
    Assert-Contains -Text $metrics.Content -Needle '"totalQueryRequests":4'
    Assert-Contains -Text $metrics.Content -Needle '"totalInsertRequests":1'
    Assert-Contains -Text $metrics.Content -Needle '"totalSelectRequests":3'
    Assert-Contains -Text $metrics.Content -Needle '"totalSyntaxErrors":1'
    Assert-Contains -Text $metrics.Content -Needle '"totalNotFoundResults":1'

    Wait-Process -Id $phase1.Id -Timeout 10
    if ($phase1.ExitCode -ne 0) {
        throw "Phase 1 server exited with code $($phase1.ExitCode)"
    }
}
finally {
    if ($phase1 -and -not $phase1.HasExited) {
        Stop-Process -Id $phase1.Id -Force
    }
}

$phase2Port = 18081
$phase2 = $null

try {
    $phase2 = Start-TestServer -Port $phase2Port -Arguments @(
        "--serve",
        "--port", "$phase2Port",
        "--workers", "1",
        "--queue", "1",
        "--simulate-read-delay-ms", "300",
        "--max-requests", "6"
    )

    $seed = Invoke-HttpJson -Method "POST" -Uri "http://127.0.0.1:$phase2Port/query" -Body '{"query":"INSERT INTO users VALUES (''Queue Seed'', 10);"}'
    if ($seed.StatusCode -ne 200) { throw "Seed insert failed: $($seed.Content)" }

    $jobs = 1..4 | ForEach-Object {
        Start-Job -ScriptBlock {
            param($Port)

            try {
                $response = Invoke-WebRequest -Method "POST" -Uri "http://127.0.0.1:$Port/query" -UseBasicParsing -ContentType "application/json" -Body '{"query":"SELECT * FROM users WHERE id = 1;"}' -TimeoutSec 5
                [PSCustomObject]@{
                    StatusCode = [int]$response.StatusCode
                    Content = $response.Content
                }
            }
            catch {
                if ($_.Exception.Response) {
                    $stream = $_.Exception.Response.GetResponseStream()
                    $reader = New-Object System.IO.StreamReader($stream)
                    $content = $reader.ReadToEnd()
                    $reader.Dispose()

                    [PSCustomObject]@{
                        StatusCode = [int]$_.Exception.Response.StatusCode
                        Content = $content
                    }
                }
                else {
                    throw
                }
            }
        } -ArgumentList $phase2Port
    }

    $jobResults = $jobs | Receive-Job -Wait -AutoRemoveJob
    $successCount = ($jobResults | Where-Object { $_.StatusCode -eq 200 }).Count
    $queueFullResults = $jobResults | Where-Object { $_.StatusCode -eq 503 -and $_.Content.Contains('"queue_full"') }

    if ($successCount -lt 2) {
        throw "Expected at least 2 successful queued SELECT responses. Results: $($jobResults | ConvertTo-Json -Compress)"
    }

    if ($queueFullResults.Count -lt 1) {
        throw "Expected at least one queue_full response. Results: $($jobResults | ConvertTo-Json -Compress)"
    }

    Wait-Process -Id $phase2.Id -Timeout 10
    if ($phase2.ExitCode -ne 0) {
        throw "Phase 2 server exited with code $($phase2.ExitCode)"
    }
}
finally {
    if ($phase2 -and -not $phase2.HasExited) {
        Stop-Process -Id $phase2.Id -Force
    }
}

Write-Host "server HTTP smoke test passed."
