# Vita self-updater

## Feasibility result

The requested **downloaded, non-installed helper** is not possible for an
ordinary Vita homebrew title. VitaSDK requires the SELF passed to
`sceAppMgrLoadExec` to live under `app0:`, and a SUPRX loaded with
`sceKernelLoadStartModule` remains inside Melee's process and disappears when
that process exits.

The implemented opt-in proof of concept instead follows VitaShell's real
design:

1. Melee checks GitHub's latest published release on a background thread.
2. Melee verifies the release metadata and signed manifest, then prompts the
   player.
3. Melee downloads and hashes the VPK under `ux0:/data/melee/update/`.
4. Melee promotes an embedded temporary title, `MLVUPD001`, and launches it.
5. The helper re-verifies the manifest and VPK, extracts the VPK safely, backs
   up `ux0:/app/MLVITA002`, and asks the Vita package promoter to replace Melee.
6. On success, the helper writes an `awaiting-health` journal and launches
   Melee without ever initializing PGF, `vita2d`, or GXM.
7. After 300 successful game frames, Melee deletes `MLVUPD001`, the backup,
   the journal, and all staging files.

The helper is therefore temporarily installed and can briefly appear as a
LiveArea bubble. It is not a permanently installed second application.

## Required privileges and dependencies

This is not an ordinary safe-homebrew build. Enabling the updater gives Melee
the same unsafe auth ID used by VitaShell (`0x2808000000000000`) so it can load
the internal PAF/promoter modules, promote the temporary helper, and delete it
after startup. It requires a HENkaku/taiHEN environment that permits those
operations. A stock retail environment is unsupported.

The optional build needs the VitaSDK packages:

```powershell
vdpm install libsodium curl-mbedtls libarchive
```

`curl-mbedtls` uses the Vita CA bundle at
`vs0:data/external/cert/CA_LIST.cer`. The runtime explicitly keeps peer and
hostname verification enabled. It never embeds a GitHub token.

The default `build-full.ps1` path remains unchanged and does not include or
enable updater code. To build the opt-in Release variant:

```powershell
.\platforms\vita\build-full.ps1 -Configuration Release -EnableUpdater `
  -UpdaterPublicKeyHex '<64 hex characters>'
```

`-EnableUpdater` is rejected for Debug builds. The public key is Ed25519 and is
compiled into both Melee and the helper. Keep the corresponding private key
offline and outside the repository, CI logs, and release artifacts.

Before contacting GitHub, an updater-capable build queries the upcoming
Homebrew Update shell service with the main application's title ID,
`MLVITA002`. A response of exactly `HOMEBREW_UPDATE_READY` delegates update
discovery to that service. If the service is absent, disabled, reports a hook
error, times out, or returns an invalid response, the existing built-in check
continues unchanged. Transaction recovery and cleanup are always handled
before this query.

Every Melee VPK advertises the same title-scoped feed in
`sce_sys/homebrew_update.ini`:

```text
https://github.com/zm2283145/melee-Vita/releases/latest/download/MLVITA002-ver.xml
```

The release workflow generates `MLVITA002-ver.xml` and
`MLVITA002-changeinfo.xml` from the final versioned VPK. The feed contains the
VPK's exact byte size and SHA-1, the package APP_VER and content ID, and stable
`releases/latest/download` URLs. These assets are separate from the signed
manifest consumed by Melee's built-in updater.

The `vita-release.yml` workflow reads the checked-in public key from
`platforms/vita/updater/public-key.hex` and the matching PEM private key from
the encrypted `VITA_UPDATER_PRIVATE_KEY_PEM` repository secret. Branch and
pull-request builds compile and test the updater, but only trusted branch
builds receive the private key and create signed release assets.

## Release and network policy

The check uses:

```text
https://api.github.com/repos/zm2283145/melee-Vita/releases/latest
```

It runs asynchronously with a three-second connect timeout and an eight-second
overall metadata timeout. No network connection, DNS failure, connection
failure, and timeout are treated as offline and do not interrupt startup.
Certificate failures, malformed responses, and security-policy failures are
shown explicitly. GitHub's `latest` endpoint hides drafts and prereleases; the
parser also rejects drafts and the runtime ignores prereleases.

The player sees the release version, release notes, package size, and
**Update** / **Later** buttons. Package download and verification run off the
game thread with a progress dialog. The package request has bounded low-speed
and overall timeouts.

Limits are enforced before allocation or staging:

| Input | Limit |
|---|---:|
| Release metadata | 64 KiB |
| Release notes | 8 KiB |
| Signed manifest | 4 KiB |
| Downloaded VPK | 256 MiB |
| Extracted package | 512 MiB |
| Archive entries | 16,384 |
| Additional storage reserve | 16 MiB |

The free-space requirement includes the VPK, the current installed
application tree used for rollback, the full 512 MiB extraction ceiling, and
the reserve. This is intentionally conservative because the compact manifest
does not trust an unsigned expanded-size estimate.

## Signed manifest

The release must contain exactly one asset named
`melee-update-manifest.txt`. The manifest has six ordered fields:

```text
format=1
version=0.8.5
asset=SmashMeleevita-0.8.5.vpk
size=1048576
sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
signature=<128 hexadecimal characters>
```

The Ed25519 signature covers the canonical LF form of the first five lines,
including the final LF. Validation requires:

- a valid Ed25519 signature;
- a complete-file SHA-256 match;
- exact equality between manifest, GitHub, and downloaded sizes;
- exact semantic-version equality between the release tag and manifest;
- a version newer than the compiled `version.json` release;
- exact release URLs under the configured GitHub repository and tag;
- uploaded, uniquely named assets; and
- a safe package filename.

The helper repeats signature, size, and SHA-256 verification after Melee exits.
This prevents a staged file from being replaced between the first verification
and package promotion.

The release workflow creates an unpublished, non-prerelease `v<version>`
draft, downloads the VPK and manifest back from GitHub, and requires byte-for-
byte equality before completing. Publishing that reviewed draft is the update
activation step; drafts remain invisible to installed games.

## Extraction, replacement, and rollback

VPK extraction rejects absolute paths, `..`, backslashes, device prefixes,
duplicate names, encryption, links, special files, excessive depth, excessive
entry counts, and excessive expanded size. The extracted package must contain:

```text
eboot.bin
sce_sys/param.sfo
sce_sys/package/head.bin
```

The SFO `TITLE_ID` must be `MLVITA002`. `head.bin` is generated at build time
from VitaShell's GPL-3.0-or-later fake-package header algorithm and template.
The generated header is included in every updater-capable Melee VPK so both
the new package and the local rollback copy are promoter-ready.
The helper also requires the replacement executable to carry the updater auth
ID and the package to contain a correctly identified embedded `MLVUPD001`
helper, preventing an update that cannot later perform cleanup or recovery.

The helper never overwrites the running executable. It runs as `MLVUPD001`,
copies the stopped Melee application to
`ux0:/data/melee/update/backup-package`, writes and synchronizes a durable
journal, and uses `scePromoterUtilityPromotePkgWithRif` for replacement. A
failed promotion immediately attempts to promote the backup package. Errors
and rollback results are persisted in the journal for Melee to display;
recovery files are retained on failure. The helper contains no graphics or
dialog code. Hardware testing showed that tearing down GXM and immediately
URI-launching Melee can race the GPU driver, so the helper remains headless on
every path.

Vita applications do not provide a concurrent watchdog process. If the new
Melee installation cannot reach its 300-frame health acknowledgement, the
temporary helper deliberately remains installed. Launching its bubble offers
rollback to the preserved package. This manual relaunch is the remaining
recovery boundary; claiming fully automatic rollback after a hard launch crash
would be inaccurate.

Successful cleanup occurs only after the health threshold. These paths are
never part of package backup, extraction, replacement, or cleanup:

```text
ux0:/data/melee/save/
ux0:/data/melee/shadercache/
ux0:/data/melee/GALE01.iso
```

## Validation

Run focused host, cryptographic, package-header, and Vita compile checks:

```powershell
.\platforms\vita\test-updater.ps1
```

Run the complete opt-in Release build with a real release public key before
producing a distributable artifact. A build made with a public test-vector key
is suitable only for build inspection, not release.

Hardware validation is still required for:

- internal PAF/promoter behavior across supported HENkaku/taiHEN versions;
- replacement and rollback when the existing title is registered;
- power loss during backup and package promotion;
- helper deletion after health acknowledgement;
- LiveArea refresh timing and helper-bubble visibility; and
- CA bundle compatibility with current GitHub TLS endpoints.

No Vita deployment is performed by the build or test scripts.

## References

- [VitaSDK `sceAppMgrLoadExec`](https://github.com/vitasdk/vita-headers/blob/master/include/psp2/appmgr.h)
- [VitaSDK module manager](https://github.com/vitasdk/vita-headers/blob/master/include/psp2/kernel/modulemgr.h)
- [VitaShell package installer and `head.bin` generation](https://github.com/TheOfficialFloW/VitaShell/blob/master/package_installer.c)
- [VitaShell temporary updater](https://github.com/TheOfficialFloW/VitaShell/blob/master/updater/main.c)
- [HENlo bootstrap](https://github.com/SKGleba/henlo_jb/blob/master/bootstrap_lite/main.c)
- [GitHub latest-release API](https://docs.github.com/en/rest/releases/releases#get-the-latest-release)
