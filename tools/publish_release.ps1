# Publish only a clean, fast-forward commit and a new release after CI succeeds.
# Existing releases and assets are never edited, overwritten or deleted.
param([string]$ReleaseDirectory = "")
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $ReleaseDirectory) { $ReleaseDirectory = Join-Path $repoRoot "dist\release" }
$repository = "earendel-dev/TCall_Pager"
$tag = "v1.1.0-rc.2"
Push-Location $repoRoot
try {
    $remote = & git remote get-url origin
    if ($LASTEXITCODE -ne 0 -or $remote -notmatch '^(https://github\.com/earendel-dev/TCall_Pager(\.git)?|git@github\.com:earendel-dev/TCall_Pager(\.git)?)$') {
        throw "origin does not point to the intended TCall_Pager repository."
    }
    & gh api "repos/$repository" --silent
    if ($LASTEXITCODE -ne 0) { throw "GitHub authentication or network access failed." }
    $changes = & git status --porcelain
    if ($changes) { throw "Commit or stash local changes before publishing." }
    $commit = & git rev-parse HEAD
    & git -c credential.helper= -c "credential.helper=!gh auth git-credential" fetch origin main
    if ($LASTEXITCODE -ne 0) { throw "Could not fetch origin/main." }
    & git merge-base --is-ancestor origin/main HEAD
    if ($LASTEXITCODE -ne 0) { throw "Remote main is not an ancestor; resolve divergence without force-pushing." }
    & gh release view $tag --repo $repository --json tagName 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) { throw "Release $tag already exists; it will not be overwritten." }
    $assets = @("TCallPager-1.1.0-rc.2-windows.zip", "TCall_Pager-1.1.0-rc.2-source.zip", "SHA256SUMS.txt") | ForEach-Object {
        $asset = Join-Path $ReleaseDirectory $_
        if (-not (Test-Path -LiteralPath $asset -PathType Leaf)) { throw "Missing release asset: $_" }
        $asset
    }
    & git -c credential.helper= -c "credential.helper=!gh auth git-credential" push origin HEAD:main
    if ($LASTEXITCODE -ne 0) { throw "Push failed." }
    $run = $null
    for ($attempt = 0; $attempt -lt 12; $attempt++) {
        $runs = & gh run list --repo $repository --commit $commit --workflow ci.yml --json databaseId | ConvertFrom-Json
        if ($LASTEXITCODE -ne 0) { throw "Could not inspect CI runs." }
        if ($runs.Count) { $run = $runs[0].databaseId; break }
        Start-Sleep -Seconds 5
    }
    if (-not $run) { throw "CI did not start; no release was created." }
    & gh run watch $run --repo $repository --exit-status
    if ($LASTEXITCODE -ne 0) { throw "CI did not pass; no release was created." }
    & gh release create $tag @assets --repo $repository --target $commit --prerelease --title "TCall Pager 1.1.0-rc.2" --notes-file (Join-Path $repoRoot "docs\RELEASE_NOTES.md")
    if ($LASTEXITCODE -ne 0) { throw "Release creation failed." }
    & gh release view $tag --repo $repository --json url,isPrerelease,assets
} finally { Pop-Location }
