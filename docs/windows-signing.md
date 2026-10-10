# Windows code signing (Authenticode, Azure Artifact Signing)

EdgeSlicer's Windows release builds can be Authenticode-signed with
[Azure Artifact Signing](https://learn.microsoft.com/azure/artifact-signing/) (the service formerly
called Trusted Signing). Signing is what lets Windows 11 Smart App Control load our exe and DLLs
without a reputation history (the "Bad Image ... 0xc0e90002" reports), and it shows a verified
publisher name in UAC and SmartScreen prompts. The research and the reasoning behind every choice
here are in `tests/plan_windows_code_signing.md` (owner's copy).

**Signing is off by default.** Until the repository variable `EDGESLICER_SIGN_WINDOWS` exists with
the value `1`, the self-hosted Windows workflow builds exactly what it built before: no GitHub
environment, every `Signing -` step skipped, the CMake option off, and the generated NSIS script
byte-for-byte the same.

## What gets signed

One signing identity (the owner's validated identity, Public Trust certificate profile) signs, in
one run of `build_windows_selfhosted.yml`:

| File | How |
|---|---|
| `EdgeSlicer.exe`, `EdgeSlicer.dll` | `scripts/windows_sign.ps1` on `build\EdgeSlicer` |
| OCCT `TK*.dll`, `freetype.dll`, `libgmp-10.dll`, `libmpfr-4.dll` | same |
| `sentry.dll`, `crashpad_handler.exe` (and `crashpad_wer.dll` when built) | same |
| `resources\tools\go2rtc\go2rtc.exe`, `resources\tools\go2rtc\ffmpeg.exe` | same (unmodified apart from the appended signature) |
| `ultranet\bambu_networking.dll`, `ultranet\BambuSource.dll` | same (so the network plug-in zip cut from the portable zip is signed too) |
| `Uninstall.exe` | makensis `!uninstfinalize`, before it is embedded in the installer |
| `EdgeSlicer_Windows_Installer_V<ver>.exe` | makensis `!finalize` |

Not re-signed: any file that already has a valid signature, which today means Microsoft's VC runtime
(`msvcp140*.dll`, `vcruntime140*.dll`, `concrt140.dll`) and `WebView2Loader.dll`. We never dual-sign
or replace an existing signature.

Not signed unless the owner opts in: `FlashNetwork.dll`. Its redistribution terms
(`docs/superpowers/specs/2026-09-11-flashforge-lan-ship.md`) have not been checked for byte changes.
Set the variable `EDGESLICER_SIGN_FLASHNETWORK` to `1` to sign it; until then it ships unsigned and
every verifier lists it as an allowed exception.

Not signable by us: the stock NSIS plug-in DLLs (`System.dll`, `StartMenu.dll`, `UserInfo.dll`,
`InstallOptions.dll`) that the installer unpacks to `%TEMP%` while it runs. They are reported as
allowed exceptions (`$PLUGINSDIR\*`). Whether Smart App Control blocks them inside a signed installer
needs a test on a machine with SAC enforcing (plan, risk 3).

About 45 signatures per release; the Basic SKU includes 5,000 a month.

The portable zip and the installer contain byte-identical files: CPack re-installs the build into
its own staging folder, and a pre-build hook (`cmake/signing/cpack_sign_staging.cmake`) copies the
signed files over that copy and verifies it before NSIS packs it.

## The pieces

- `scripts/windows_sign.ps1 -Path <folder|file>[;...]`: signs PE files with signtool and the
  Artifact Signing dlib (SHA256 digest, RFC 3161 timestamp from `http://timestamp.acs.microsoft.com`
  with a SHA256 digest), in batches with retries. Does nothing and exits 0 unless the environment
  variable `EDGESLICER_SIGN_WINDOWS` is `1`. Fails (non-zero) if any file ends up unsigned.
  `-ManifestPath` writes each file's sha256 before signing.
- `scripts/windows_verify_signatures.ps1 -Path <folder|zip|exe>[;...]`: checks every PE file is
  validly signed with an embedded signature, timestamped, and signed by Microsoft or by
  `-ExpectedSubject` (default: the `WINDOWS_SIGNER_SUBJECT` environment variable). Prints a table,
  exits 1 on any miss. `-ExtractInstaller` also checks what 7-Zip unpacks from an installer;
  `-AllowUnsigned` lists exceptions; `-HashOnly` prints signature-independent Authenticode
  hashes (see "Pinned hashes" below).
- `cmake/signing/WindowsSigning.cmake`: the CMake option `EDGESLICER_SIGN_WINDOWS` (default OFF).
  ON adds the two makensis hooks through `CPACK_NSIS_DEFINES` and the CPack pre-build script.
  OFF adds nothing.
- `.github/workflows/build_windows_selfhosted.yml`: the `Signing -` steps.

## How to turn it on

Everything is by name only; values go straight into GitHub's and Azure's own UIs, never into
chat, commits or logs.

1. Azure (owner, plan section 5): Artifact Signing account (Basic), completed individual identity
   validation, a Public Trust certificate profile, an Entra app registration with a federated
   credential for `repo:aceRage/EdgeSlicer:environment:windows-signing`, and the role
   *Artifact Signing Certificate Profile Signer* for that app on the certificate profile.
2. Build VM: the .NET 8 runtime (`C:\Program Files\dotnet`) and the Azure CLI
   (`C:\Program Files\Microsoft SDKs\Azure\CLI2\wbin\az.cmd`). The workflow downloads and caches
   signtool (`Microsoft.Windows.SDK.BuildTools`) and the dlib (`Microsoft.ArtifactSigning.Client`)
   under `C:\Dev\tools\signing` itself, pinned by version and sha256.
3. GitHub, repository settings, Environments: create `windows-signing`. Deployment branches and
   tags: `ci/**`, `release/**`, `main`, and tags (plus any branch you will dispatch with
   `sign=true`; a run whose ref the environment refuses fails before it starts).
   - Environment secrets: `AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, `AZURE_SUBSCRIPTION_ID`.
   - Environment variables: `ARTIFACT_SIGNING_ENDPOINT` (the account's regional endpoint, e.g. the
     East US one), `ARTIFACT_SIGNING_ACCOUNT`, `ARTIFACT_SIGNING_PROFILE`, and `WINDOWS_SIGNER_SUBJECT` (preferably as an environment secret, so the name and location stay out of public logs; a variable also works)
     (the certificate's CN, i.e. the validated legal name), optionally `EDGESLICER_SIGN_FLASHNETWORK`.
4. GitHub, repository settings, Variables (repository level, not the environment):
   `EDGESLICER_SIGN_WINDOWS` = `1`. This is the switch. Deleting it turns signing off again.

From then on every self-hosted Windows run on `ci/**`, `release/**`, `main` or a tag (or a manual
dispatch with `sign=true`) signs. Such a run fails, and uploads nothing, if any setting is missing,
the login fails, any file fails to sign, or any verification fails. Other branches build unsigned
as before.

Order inside the job: configuration check and tool install (before the build, so a mistake costs
a minute, not 25), build and tests, install, `azure/login` (OIDC), sign the portable tree and
verify it, Sentry symbols, `cpack` (which signs `Uninstall.exe` and the installer through the
hooks), zip, verify the zip and the installer, upload. The run also uploads
`SigningManifest_Windows_V<ver>` (the pre-signing sha256 of every file it signed).

## Verify locally

No SDK needed; Windows PowerShell 5.1 is enough:

```
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\windows_verify_signatures.ps1 ^
  -Path "C:\path\EdgeSlicer_Windows_V2.4.6.0_portable.zip;C:\path\EdgeSlicer_Windows_Installer_V2.4.6.0.exe" ^
  -ExtractInstaller -ExpectedSubject "<certificate CN>" -AllowUnsigned "$PLUGINSDIR\*;FlashNetwork.dll"
```

Exit 0 means every PE file passed; the table names any that did not. On an unsigned build it lists
the 38 unsigned files and exits 1. `signtool verify /pa /v <file>` gives the same verdict per file
if the SDK is installed.

### Exercising the scripts without Azure

`windows_sign.ps1` and `windows_verify_signatures.ps1` have a local test mode that signs with a
throwaway self-signed certificate from `Cert:\CurrentUser\My` instead of Artifact Signing (no dlib,
no Azure, signtool from the Windows SDK). Set `EDGESLICER_SIGN_TEST_THUMBPRINT` to the certificate's
thumbprint together with `EDGESLICER_SIGN_WINDOWS=1`; both scripts and the CPack hooks then accept
that certificate and say TEST MODE. The workflow refuses to run with that variable set. Remove the
certificate afterwards (`Remove-Item Cert:\CurrentUser\My\<thumbprint>`).

## Pinned hashes

Signing changes a file's bytes, so a plain sha256 pinned for an unsigned payload (the publisher pins
`ultranet\bambu_networking.dll`) no longer matches the signed copy. Two ways to keep the check:

- Compare Authenticode hashes, which exclude the signature: `windows_verify_signatures.ps1 -HashOnly
  -Path <file>` prints the same value for the unsigned original and the signed copy.
- Or read the run's `SigningManifest_Windows_V<ver>` artifact, which records each file's sha256
  before it was signed.

## Renewals and rotation

- Certificates: nothing to do. Artifact Signing issues a fresh 3-day certificate every day; the
  RFC 3161 timestamp keeps every signature valid after its certificate expires.
- Identity validation: it expires; Microsoft emails reminders from 60 days before. Renew it in the
  portal (Identity validations) before it lapses, or every signing run fails (by design, the job
  stops rather than ship unsigned). Keep the same identity and certificate profile: SmartScreen
  reputation follows the identity, and deleting and recreating a profile changes it.
- A changed legal name or a move to an organization identity means a new identity, a new
  `WINDOWS_SIGNER_SUBJECT`, and SmartScreen reputation starting over.
- Entra app registration: OIDC federation has no secret to expire. If the fallback client secret
  (`AZURE_CLIENT_SECRET`, picked up by the dlib's EnvironmentCredential) is ever used instead, it
  expires within 24 months; rotate it in Entra and update the environment secret.
- Tool updates: bump `SDK_BUILDTOOLS_VERSION` / `ARTIFACT_SIGNING_CLIENT_VERSION` and their sha256
  in the workflow's `env:` block (the sha256 of the `.nupkg` from api.nuget.org). The new version
  unpacks beside the old one under `C:\Dev\tools\signing`.
- Billing: a lapsed card stops signing the same way an expired validation does.

## Turning it off

Delete the repository variable `EDGESLICER_SIGN_WINDOWS` (or set it to anything but `1`). The next
run builds unsigned, uses no environment, and touches no Azure resource.
