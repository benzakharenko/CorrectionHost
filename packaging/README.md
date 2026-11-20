# macOS signing identity

`macos-signing-cert.pem` is the public half of a self-signed code-signing certificate
("Correction Host Signing"). The release workflow signs the Mac app with it when the
repo has these Actions secrets:

| Secret | Contents |
|---|---|
| `MACOS_SIGNING_P12` | The certificate and private key as a base64-encoded .p12 |
| `MACOS_SIGNING_PASSWORD` | The .p12 password |

Why: macOS ties the microphone permission (needed to capture from BlackHole) to the
app's signature. Signing every build with the same identity means the permission
survives automatic updates. Without the secrets the workflow falls back to an ad-hoc
signature, and macOS asks for the permission again after each update.

This is not an Apple Developer ID. It doesn't notarise the app, and Gatekeeper still
treats a *browser-downloaded* copy as unidentified. The install script and the
updater download with tools that don't add the quarantine flag, so that never comes up.

**To set it up (or replace it),** run `bash packaging/make-mac-signing-identity.sh`
from the repo root. You need openssl (Git Bash on Windows has it) and `gh auth login`.
The script creates the identity, stores both secrets, and writes the new public
certificate here. Commit and push that file. macOS asks for the microphone once more
after that update, then remembers it.
