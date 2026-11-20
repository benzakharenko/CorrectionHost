#!/bin/bash
# Creates the self-signed "Correction Host Signing" identity and stores it as GitHub
# Actions secrets, so every Mac release is signed the same way and macOS keeps the
# microphone permission across updates. The private key never leaves this machine
# except encrypted into the repo's secrets.
#
# Needs: openssl (macOS, Linux, or Git Bash on Windows) and the GitHub CLI, logged in
# (gh auth login) with admin rights on the repo. Run from the repo root:
#   bash packaging/make-mac-signing-identity.sh
# Then commit the updated packaging/macos-signing-cert.pem and push.
set -euo pipefail
cd "$(dirname "$0")/.."

REPO="${CORRECTIONHOST_REPO:-$(gh repo view --json nameWithOwner -q .nameWithOwner)}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/cert.cnf" <<'CNF'
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = Correction Host Signing
[ext]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
subjectKeyIdentifier = hash
CNF

openssl req -x509 -newkey rsa:2048 -nodes -keyout "$WORK/key.pem" -out "$WORK/cert.pem" \
    -days 7300 -config "$WORK/cert.cnf" 2> /dev/null

PASSWORD="$(openssl rand -hex 16)"

# macOS's keychain only imports the older PKCS#12 encryption, hence -legacy (OpenSSL 3)
# with a fallback for OpenSSL 1.x / LibreSSL.
openssl pkcs12 -export -legacy -inkey "$WORK/key.pem" -in "$WORK/cert.pem" -name "Correction Host Signing" \
        -out "$WORK/identity.p12" -passout "pass:$PASSWORD" 2> /dev/null \
 || openssl pkcs12 -export -inkey "$WORK/key.pem" -in "$WORK/cert.pem" -name "Correction Host Signing" \
        -out "$WORK/identity.p12" -passout "pass:$PASSWORD"

base64 < "$WORK/identity.p12" | tr -d '\n' | gh secret set MACOS_SIGNING_P12 --repo "$REPO"
printf '%s' "$PASSWORD" | gh secret set MACOS_SIGNING_PASSWORD --repo "$REPO"
cp "$WORK/cert.pem" packaging/macos-signing-cert.pem

echo "Secrets MACOS_SIGNING_P12 and MACOS_SIGNING_PASSWORD set on $REPO."
echo "Now commit packaging/macos-signing-cert.pem and push; the next Mac build is signed with it."
echo "macOS asks for the microphone permission once more after that update, then remembers it."
