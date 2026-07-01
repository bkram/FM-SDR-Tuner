# Release process

How a versioned release of `fm-sdr-tuner` is cut. This is the flow used for the
v1.7.x line; follow it for future releases.

## Branch & version model

- Each release is prepared on a branch named after its version, e.g. `v1.7.0`.
- The version lives in one place: `project(fm-sdr-tuner VERSION X.Y.Z ...)` in
  [CMakeLists.txt](CMakeLists.txt). Bump it on the release branch.
- **Patch releases** branch from the current release branch, carry the fix
  (cherry-pick or commit), and bump only the patch component. Example: v1.7.1
  was `v1.7.0` + the auto-gain fix + `VERSION 1.7.1`. When a fix belongs to the
  patch and not the base release, remove it from the base branch
  (`git reset --hard`, force-push) so each line is clean.

## 1. Prepare the release branch

```bash
git checkout -b vX.Y.Z            # or branch from the previous release
# ... changes ...
# bump project(... VERSION X.Y.Z ...) in CMakeLists.txt
git push -u origin vX.Y.Z
```

## 2. Open a PR to main → CI builds & verifies all platforms

CI only runs on PRs to `main` (and pushes to `main`), so open a PR:

```bash
gh pr create --base main --head vX.Y.Z --title "vX.Y.Z — <summary>" --body "..."
```

This runs three workflows ([.github/workflows/](.github/workflows/)):

- **Build (Linux)** — coverage job + `.deb` (Ubuntu 24.04, Debian trixie) and
  `.rpm` (Fedora 42/43) for `x86_64` **and** `aarch64`, plus container
  install smoke-tests. These jobs build **SDRplay-enabled** (they fetch the
  vendor's public API headers at build time; the lib is `dlopen`'d, nothing
  proprietary is bundled).
- **Build (macOS)** — SDRplay-enabled arm64 build + tests.
- **Build (Windows / MinGW-w64)** — RTL-only (no SDRplay loader on Windows).

Each build runs `ctest` and uploads a platform artifact. Wait until all three
are green (`gh run list --branch vX.Y.Z`).

## 3. Merge to main

```bash
gh pr merge <pr#> --merge
```

If a patch release supersedes an in-flight base PR, close the base PR with a
note (e.g. v1.7.0's PR was closed in favour of v1.7.1).

## 4. Publish the GitHub release with binaries

The CI artifacts are the release binaries. Download them from the green run,
give them distinct per-distro/arch names (the raw CPack names collide — Ubuntu
and Debian both emit `fm-sdr-tuner_X.Y.Z_amd64.deb`, Fedora 42 and 43 both emit
`...x86_64.rpm`), then attach everything to the tagged release.

```bash
V=X.Y.Z; A=$(mktemp -d)/assets; mkdir -p "$A"

# Download each platform workflow run's artifacts (find run IDs via `gh run list`)
gh run download <macos-run>   -D macos
gh run download <windows-run> -D windows
gh run download <linux-run>   -D linux

# Zip the macOS/Windows folders (binary + docs + configs + REST panel)
( cd macos/fm-sdr-tuner-macos            && zip -r -X "$A/fm-sdr-tuner-$V-macos-arm64.zip" . )
( cd windows/fm-sdr-tuner-windows-mingw  && zip -r -X "$A/fm-sdr-tuner-$V-windows-x64.zip" . )

# Copy + rename the Linux packages so they don't collide:
#   fm-sdr-tuner-$V-ubuntu24.04-amd64.deb   / -arm64.deb
#   fm-sdr-tuner-$V-debian-trixie-amd64.deb / -arm64.deb
#   fm-sdr-tuner-$V-fedora42-x86_64.rpm     / -aarch64.rpm
#   fm-sdr-tuner-$V-fedora43-x86_64.rpm     / -aarch64.rpm

gh release create "v$V" --target main --latest \
  --title "v$V" --notes "..." "$A"/*
```

Verify: `gh release view v$V --json isDraft,isPrerelease,assets`.

## Notes

- Prebuilt macOS/Linux binaries are SDRplay-capable but require the SDRplay API
  service installed at runtime; Windows prebuilts are RTL-only. See
  [USER-MANUAL.md](USER-MANUAL.md) and [README.md](README.md).
- Every archive/package bundles `README.md`, `USER-MANUAL.md`, the ready-to-use
  `fm-sdr-tuner.ini`, and `rest_test_panel.py` (CI copies these in; see the
  workflow files and the CMake `install()` rules). macOS and Linux additionally
  ship the `fm-sdr-tuner-sdrplay.ini.example` template and `run_rest_panel.sh`;
  the Windows archive ships only `fm-sdr-tuner.ini.example` (SDRplay is
  unsupported on Windows and `run_rest_panel.sh` is a POSIX shell launcher).
- There is no automated release workflow yet; the publish step (4) is manual.
  A future improvement is a tag-triggered workflow that builds and attaches the
  assets automatically.
