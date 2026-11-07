# winget manifest

Three files describe the portable ZIP release to the Windows Package Manager.
`joaopmatos.tinyAmbient` is the package identifier; `tinyambient` is the
moniker and the command it installs.

## Before opening the pull request

The release must be **published**, not a draft: the validation pipeline
downloads the installer anonymously. Fill in the hash from the published asset
and never re-upload that asset afterwards, because the manifest pins its bytes.

```sh
curl -LO https://github.com/joaopmatos/tinyAmbient/releases/download/v1.0.5/tinyambient-1.0.5-windows-x86_64.zip
sha256sum tinyambient-1.0.5-windows-x86_64.zip | cut -d' ' -f1 | tr 'a-f' 'A-F'
```

Replace the `InstallerSha256` value in
`joaopmatos.tinyAmbient.installer.yaml` with that string. The placeholder is
sixty-four zeros so a forgotten hash fails validation instead of pointing at
the wrong bytes.

## Submitting

```sh
gh repo fork microsoft/winget-pkgs --clone
cd winget-pkgs
git checkout -b joaopmatos.tinyAmbient-1.0.5
mkdir -p manifests/j/joaopmatos/tinyAmbient/1.0.5
cp <this-directory>/*.yaml manifests/j/joaopmatos/tinyAmbient/1.0.5/
git add manifests/j/joaopmatos/tinyAmbient
git commit -m "New package: joaopmatos.tinyAmbient version 1.0.5"
git push -u origin joaopmatos.tinyAmbient-1.0.5
gh pr create --repo microsoft/winget-pkgs \
  --title "New package: joaopmatos.tinyAmbient version 1.0.5" \
  --body "Portable ZIP release of tinyAmbient, a command-line white, pink, and brown noise generator."
```

The first contribution needs the Microsoft CLA (the bot comments with a link)
and a GitHub account with two-factor authentication. Validation installs the
package in a disposable VM and takes minutes to hours; a moderator merges after
the checks pass.

## Later versions

`wingetcreate update joaopmatos.tinyAmbient --urls <zip> --version <x.y.z>`
regenerates the manifests on Windows and opens the pull request. On any other
platform, copy this directory, bump `PackageVersion`, update `InstallerUrl`,
`ReleaseDate` and the hash, and open the pull request by hand.

## Why portable

The archive is a flat ZIP containing `tinyambient.exe`, `README.md`, and
`LICENSE`. A portable installer needs no elevation, no registry keys, and no
uninstaller, which matches a binary that writes a single `config.ini` under
`%APPDATA%`. winget places it under
`%LOCALAPPDATA%\Microsoft\WinGet\Packages\` and exposes `tinyambient` through a
link in `%LOCALAPPDATA%\Microsoft\WinGet\Links\`.

The background daemon copies itself out of that package directory; see
`AGENTS.md` section 5, "Windows daemon image".
