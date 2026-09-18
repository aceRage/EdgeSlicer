# Releasing on macOS: signing and notarization

CI builds the macOS dmg on `macos-14` (`.github/workflows/build_orca.yml`, called through
`build_all.yml`). When the repository holds the secrets listed below, the same job signs the app with
a Developer ID certificate, notarizes it with Apple, staples the ticket, and uploads the stapled
`EdgeSlicer_Mac_universal_V<version>.dmg` as the artifact the release scripts already download.
Without the secrets it builds exactly the unsigned dmg it always built: the signing steps are gated on
the secrets being present, never on the branch, so a fork, a pull request or a fresh clone never fails
for a missing secret.

The work is split between the workflow and two scripts you can also run on a Mac by hand:

- `scripts/macos_sign_app.sh <App.app> "<identity>" scripts/disable_validation.entitlements` signs
  every nested Mach-O (crashpad_handler, libsentry.dylib, whatever a later build adds) inside out with
  the hardened runtime and a secure timestamp, then seals the bundle with the entitlements.
- `scripts/macos_notarize.sh <zip|dmg> [staple-target]` submits, waits, prints Apple's log, fails when
  the verdict is anything but `Accepted`, and staples.

The app is notarized and stapled on its own (as a zip) before it goes into the dmg, so the copy a user
drags to `/Applications` carries its ticket and opens offline; the dmg is then signed, notarized and
stapled as well. Two submissions, a few minutes each.

## One-time setup on the Mac

Everything below happens once. You need the Apple Developer Program membership (team `57H3947FW2`),
a Mac with Xcode or the command-line tools, and the App Store Connect API key you already use for the
companion app's TestFlight upload.

### 1. Create a Developer ID Application certificate

This is **not** the App Store distribution certificate the iOS app uses. Developer ID certificates are
what Gatekeeper trusts for software distributed outside the App Store, and only the account holder
can create them.

1. On the Mac, open **Keychain Access** > menu **Keychain Access** > **Certificate Assistant** >
   **Request a Certificate From a Certificate Authority...**
   Enter your email and name, choose **Saved to disk**, and save the `.certSigningRequest`. This also
   creates the private key in your login keychain, which is why the export in step 2 has to happen
   on this same Mac.
2. Go to <https://developer.apple.com/account/resources/certificates/add>, choose
   **Developer ID Application** (under "Software"), pick the current profile type (G2 Sub-CA), upload
   the CSR, and download the resulting `developerID_application.cer`.
3. Double-click the `.cer` to install it. In Keychain Access > **login** > **My Certificates** it
   shows as `Developer ID Application: <Your Name> (57H3947FW2)` with a disclosure triangle revealing
   its private key. If there is no triangle, the CSR was made on another Mac: repeat step 1 there.

### 2. Export it as a .p12 and encode it

1. In Keychain Access, right-click the `Developer ID Application: ...` entry (the certificate, which
   takes the key with it) > **Export...** > file format **Personal Information Exchange (.p12)**.
   Give it a password when asked; that password becomes the `P12_PASSWORD` secret.
2. Base64 it onto the clipboard:

   ```sh
   base64 -i ~/Desktop/edgeslicer_developer_id.p12 | pbcopy
   ```

   That clipboard content is the `BUILD_CERTIFICATE_BASE64` secret. Line breaks and spaces are
   stripped by the workflow before decoding, so a paste from any terminal is fine.
3. Delete the `.p12` from the Desktop once the secret is stored; the key stays in your keychain.

### 3. Find the identity string

```sh
security find-identity -v -p codesigning
```

prints a line such as

```
1) 1234ABCD... "Developer ID Application: Ace Savell (57H3947FW2)"
```

The quoted part, exactly, is the `MACOS_CERTIFICATE_ID` secret. The workflow checks it against the
identities in the imported .p12 and fails with a clear message when they do not match.

### 4. The App Store Connect API key

Notarization authenticates with the same kind of key TestFlight uploads use, so reuse that key. It
needs the **Developer** role or higher (Admin works).

- **Key ID** and **Issuer ID**: App Store Connect > **Users and Access** > **Integrations** >
  **App Store Connect API** > **Team Keys**. The Issuer ID (a UUID) is at the top of the table, the
  Key ID (10 characters) is in the key's row.
- **The .p8 file**: `AuthKey_<KEY_ID>.p8`, downloadable exactly once when the key was created, so it
  is wherever you stored it for the companion app (the app repository's GitHub secrets hold a copy).
  Base64 it the same way: `base64 -i AuthKey_<KEY_ID>.p8 | pbcopy`. The workflow also accepts the
  raw PEM text (`-----BEGIN PRIVATE KEY-----` ...) pasted as-is.

If you would rather not use the API key, the Apple-ID fallback still works: an Apple ID, the team ID
and an app-specific password from <https://account.apple.com> > Sign-In and Security > App-Specific
Passwords. The API key is preferred because it never expires with a password change and has no 2FA.

### 5. Add the secrets

Repository > **Settings** > **Secrets and variables** > **Actions** > **New repository secret**, or
`gh secret set NAME -R aceRage/EdgeSlicer` (it reads the value from stdin or `--body`).

| Secret | Value | Required |
| --- | --- | --- |
| `BUILD_CERTIFICATE_BASE64` | the `.p12` from step 2, base64 | yes |
| `P12_PASSWORD` | the password given when exporting the `.p12` | yes |
| `MACOS_CERTIFICATE_ID` | `Developer ID Application: <Name> (57H3947FW2)` from step 3 | yes |
| `APP_STORE_CONNECT_KEY_ID` | the 10-character Key ID | yes, for notarization |
| `APP_STORE_CONNECT_ISSUER_ID` | the Issuer ID UUID | yes, for notarization |
| `APP_STORE_CONNECT_KEY_P8` | the `AuthKey_<KEY_ID>.p8`, base64 (or its PEM text) | yes, for notarization |
| `KEYCHAIN_PASSWORD` | any string; protects the throw-away build keychain | optional (generated when unset) |
| `APPLE_DEV_ACCOUNT`, `TEAM_ID`, `APP_PWD` | Apple ID, `57H3947FW2`, app-specific password | only as a fallback when no API key secrets are set |

What the gate does with them, in `Check for macOS signing secrets`:

- the three certificate secrets present: sign; otherwise the unsigned dmg, with a notice in the log;
- signing on and the three API-key secrets present: notarize with the key; else the Apple-ID trio if
  present; else sign only, with a warning (Gatekeeper still refuses a signed-but-unnotarized app).

The build keychain, the decoded `.p12` and the `.p8` are removed in an `always()` step at the end of
the job.

## Testing it

1. Dispatch the workflow on this branch (or any branch, the gate is the same everywhere):

   ```sh
   gh workflow run build_all.yml -R aceRage/EdgeSlicer --ref ci/macos-signing
   gh run list -R aceRage/EdgeSlicer --workflow build_all.yml --branch ci/macos-signing -L 1
   gh run watch <run-id> -R aceRage/EdgeSlicer
   ```

   The macOS job takes 40-60 minutes plus a few minutes per notarization. In the job log, the
   `Check for macOS signing secrets` step prints `sign=true notarize=api-key`, the signing step lists
   every nested binary it signed, and the notarize steps end with `notarized and stapled: ...`. A
   rejection fails the job and prints Apple's log with the offending file and reason.

2. Download the dmg and check it on a Mac:

   ```sh
   gh run download <run-id> -R aceRage/EdgeSlicer -n EdgeSlicer_Mac_universal_V<version>
   xcrun stapler validate EdgeSlicer_Mac_universal_V<version>.dmg
   spctl -a -vv -t open --context context:primary-signature EdgeSlicer_Mac_universal_V<version>.dmg
   hdiutil attach EdgeSlicer_Mac_universal_V<version>.dmg
   spctl -a -vv "/Volumes/EdgeSlicer/EdgeSlicer.app"
   xcrun stapler validate "/Volumes/EdgeSlicer/EdgeSlicer.app"
   codesign --verify --deep --strict --verbose=2 "/Volumes/EdgeSlicer/EdgeSlicer.app"
   codesign -d --entitlements - "/Volumes/EdgeSlicer/EdgeSlicer.app"
   hdiutil detach /Volumes/EdgeSlicer
   ```

   `spctl` must answer `accepted` with `source=Notarized Developer ID`, and both `stapler validate`
   calls `The validate action worked!`. The entitlements printed are the ones in
   `scripts/disable_validation.entitlements` (library validation off for the network plug-in and
   3Dconnexion drivers, JIT for the web views, no `get-task-allow`).

3. Install it the way a user would: drag to `/Applications`, double-click. macOS shows its usual
   "downloaded from the internet" prompt once and opens the app. No right-click > Open, no
   `xattr -dr com.apple.quarantine`.

## What users see afterwards

The dmg opens and the app launches like any other downloaded Mac app. The README's macOS lines
("unsigned", the right-click > Open workaround) are flipped after the first signed release ships, not
before, so the download instructions never promise a signed build that is not on the releases page
yet.

## Release day

Nothing changes in the orchestrator: `release_<version>.sh` pushes `ci/<version>` and dispatches
`build_all.yml` on it, `rel_<version>_ci_assets.sh` downloads the `EdgeSlicer_Mac_universal_V<version>`
artifact and uploads the dmg. The artifact name is the same and the file in it is the stapled dmg, so
the SHA256SUMS entry covers the notarized file.

## When it fails

- `MACOS_CERTIFICATE_ID does not match any identity` - the secret is not the full quoted string from
  `security find-identity`, or the .p12 holds a different certificate (the App Store one, say).
- `unable to build chain to self-signed root` - the .p12 lacks Apple's intermediate; the workflow
  imports `DeveloperIDG2CA.cer` from apple.com as a fallback, so this only appears when that download
  failed. Re-export the .p12 after installing the intermediate from
  <https://www.apple.com/certificateauthority/>.
- `Notarization ... was not accepted` - read the notary log printed just above it: each `issues`
  entry names the path inside the bundle and the reason (unsigned binary, missing timestamp, hardened
  runtime off). A new nested binary added to the bundle is covered by `macos_sign_app.sh` as long as
  it is a Mach-O file under `Contents/`.
- `APP_STORE_CONNECT_KEY_P8 does not decode to a .p8 private key` - the secret is neither the
  base64 of the file nor its PEM text; re-paste from `base64 -i AuthKey_<KEY_ID>.p8 | pbcopy`.
- The certificate expires after five years; notarization tickets stay valid, new builds need a new
  certificate and a new .p12.
