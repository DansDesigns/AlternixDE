#!/bin/bash
# AlternixOS sudo -> PAM -> osm-lock pattern/PIN authentication setup.
# Run as root from the existing installer or the existing updater.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
if (( EUID != 0 )) || (( $# != 2 )); then
    echo 'Usage (as root): alternix-auth-setup.sh USERNAME ABSOLUTE_APPS_SOURCE_DIRECTORY' >&2
    exit 2
fi
LOGIN_USER="$1"
APPS_DIR="$2"
[[ "$LOGIN_USER" =~ ^[a-z_][a-z0-9_-]*$ ]] || exit 2
[[ "$APPS_DIR" = /* ]] || exit 2
id "$LOGIN_USER" >/dev/null
USER_UID="$(id -u "$LOGIN_USER")"
if (( USER_UID < 100 )); then
    echo 'ERROR: Pattern sudo authentication can only be configured for regular user accounts.' >&2
    exit 1
fi
USER_HOME="$(getent passwd "$LOGIN_USER" | cut -d: -f6)"
if [[ -z "$USER_HOME" || ! -d "$USER_HOME" || ! -f "$APPS_DIR/alternix-auth.c" || ! -f "$APPS_DIR/osm-lock.cpp" ]]; then
    echo 'ERROR: Missing authentication sources or target user home.' >&2; exit 1
fi
if ! grep -Fq '"--pam-prompt"' "$APPS_DIR/osm-lock.cpp"; then
    echo 'ERROR: osm-lock.cpp has not been updated for PAM authentication.' >&2; exit 1
fi
for dep in cc g++ pkg-config visudo python3; do
    command -v "$dep" >/dev/null || { echo "ERROR: Missing dependency: $dep" >&2; exit 1; }
done
if ! pkg-config --exists Qt5Widgets; then
    echo 'ERROR: Missing Qt5Widgets development package.' >&2; exit 1
fi
if ! test -f /usr/include/openssl/evp.h; then
    echo 'ERROR: libssl-dev is needed to build the verifier.' >&2; exit 1
fi
if ! find /lib /usr/lib -name pam_exec.so -type f -print -quit 2>/dev/null | grep -q .; then
    echo 'ERROR: pam_exec.so is not installed; refusing to modify sudo PAM.' >&2; exit 1
fi
[[ -f /etc/pam.d/sudo ]] || { echo 'ERROR: /etc/pam.d/sudo absent.' >&2; exit 1; }
visudo -c -q
umask 077
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
# Build and validate BEFORE touching authentication policy.
cc -O2 -std=c11 -Wall -Wextra -Werror -fPIE -pie -fstack-protector-strong \
   -D_FORTIFY_SOURCE=2 "$APPS_DIR/alternix-auth.c" -o "$TMP/alternix-auth" -lcrypto
if ! g++ -O2 -fPIC "$APPS_DIR/osm-lock.cpp" -o "$TMP/osm-lock" $(pkg-config --cflags --libs Qt5Widgets); then
    echo 'ERROR: osm-lock build failed; no PAM changes applied.' >&2; exit 1
fi
mkdir -p /usr/local/libexec /var/lib/alternix-auth
chown root:root /var/lib/alternix-auth
chmod 0700 /var/lib/alternix-auth
install -o root -g root -m 0755 "$TMP/osm-lock" /usr/local/bin/osm-lock
install -o root -g root -m 0755 "$TMP/alternix-auth" /usr/local/libexec/alternix-auth
chmod 4755 /usr/local/libexec/alternix-auth

# Migrate already-configured user's shape/PIN values to a root-only salted KDF.
# Do not make the editable HOME configuration authoritative for sudo.
python3 - "$LOGIN_USER" "$USER_UID" "$USER_HOME/.config/Alternix/.osm_lockdata" <<'PY'
import os, pathlib, pwd, re, stat, subprocess, sys
username, uid_s, file_path = sys.argv[1:]
uid = int(uid_s)
path = pathlib.Path(file_path)
cred = pathlib.Path('/var/lib/alternix-auth/credential-' + uid_s)
if cred.exists():
    print('[Auth] Protected credential already present; preserving it.')
    # Clear the old public hashes, using O_NOFOLLOW to avoid symlink races.
    if path.exists() and not path.is_symlink():
        s = path.stat()
        if s.st_uid == uid and stat.S_ISREG(s.st_mode):
            fd = os.open(str(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
            try:
                old = os.read(fd, 4096).decode('ascii')
                st = os.fstat(fd)
                if st.st_uid != uid or not stat.S_ISREG(st.st_mode):
                    raise ValueError('Unsafe legacy credential owner')
            finally:
                os.close(fd)
            if 'pattern=' in old or 'password=' in old:
                fd = os.open(str(path), os.O_WRONLY | os.O_TRUNC | os.O_NOFOLLOW | os.O_CLOEXEC)
                try:
                    os.write(fd, ('enhanced=' + ('1' if 'enhanced=1' in old else '0') + '\n').encode('ascii'))
                    os.fsync(fd)
                finally:
                    os.close(fd)
    sys.exit(0)
try:
    fd = os.open(str(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
except FileNotFoundError:
    print('[Auth] No existing pattern: user will enrol through osm-lock at first login.')
    sys.exit(0)
except OSError as e:
    print('[Auth] Legacy credential cannot be opened safely:', e, file=sys.stderr)
    sys.exit(1)
try:
    st = os.fstat(fd)
    if st.st_uid != uid or not stat.S_ISREG(st.st_mode):
        raise ValueError('unsafe owner or file type')
    old = os.read(fd, 4096).decode('ascii')
finally:
    os.close(fd)
values = dict(line.split('=', 1) for line in old.splitlines() if '=' in line)
enh = values.get('enhanced', '0')
pattern = values.get('pattern', '')
pin = values.get('password', '')
if enh not in ('0', '1') or not re.fullmatch(r'[a-f0-9]{64}', pin):
    print('[Auth] Incomplete legacy pattern/PIN. User must enrol; no sudo activation until then.')
    sys.exit(0)
shapes = pattern.split(',')
if len(shapes) != (5 if enh == '1' else 4) or not all(re.fullmatch(r'[a-f0-9]{64}', item) for item in shapes):
    print('[Auth] Incomplete legacy pattern. User must enrol; no sudo activation until then.')
    sys.exit(0)
payload = ('P:' + pattern + '\nI:' + pin + '\nE:' + enh + '\n').encode('ascii')
proc = subprocess.run(['/usr/local/libexec/alternix-auth', '--import', uid_s],
                      input=payload, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=30)
if proc.returncode != 0:
    raise RuntimeError('protected credential import failed: ' + proc.stderr.decode(errors='replace'))
# Strip credential material now that the protected copy has been created.
fd = os.open(str(path), os.O_WRONLY | os.O_TRUNC | os.O_NOFOLLOW | os.O_CLOEXEC)
try:
    os.write(fd, ('enhanced=' + enh + '\n').encode('ascii'))
    os.fsync(fd)
finally:
    os.close(fd)
print('[Auth] Migrated existing lockscreen credential to protected storage.')
PY

# Fresh installations have no legacy pattern. The root installer must
# provision one interactively BEFORE PAM replaces the Unix-password flow.
# An unprivileged account is deliberately NOT allowed to self-enrol.
python3 - "$USER_UID" <<'PY_ENROL'
import getpass, hashlib, os, pathlib, re, subprocess, sys
uid = sys.argv[1]
cred = pathlib.Path('/var/lib/alternix-auth/credential-' + uid)
if cred.exists():
    sys.exit(0)
if not sys.stdin.isatty():
    raise SystemExit('ERROR: No pattern is enrolled and no interactive root terminal is available. Sudo PAM was not changed.')
print('\n[Auth] Set the Alternix administrator pattern (one-time root-approved setup).')
print('Each grid item consists of a shape and colour; the positions are randomised at login.')
shapes = ('circle', 'triangle', 'square', 'pentagon')
colors = ('red', 'blue', 'green', 'white')
choices = [f'{shape}-{color}' for shape in shapes for color in colors]
for i, item in enumerate(choices, 1):
    print(f' {i:2}) {item}', end='\n' if i % 4 == 0 else '   ')
while True:
    enhanced = input('Enhanced Security (5 shapes / 5-character PIN)? [y/N]: ').strip().lower() in ('y', 'yes')
    count = 5 if enhanced else 4
    chosen = input(f'Enter {count} shape numbers in order, separated by spaces: ').split()
    if len(chosen) != count or not all(x.isdigit() and 1 <= int(x) <= 16 for x in chosen):
        print('Invalid shape sequence; try again.'); continue
    pin = getpass.getpass(f'Choose a {count}-character fallback PIN: ')
    pin2 = getpass.getpass('Confirm fallback PIN: ')
    charset = '0123456789!?<>' if enhanced else '0123456789'
    if len(pin) != count or any(c not in charset for c in pin) or pin != pin2:
        print('PIN mismatch or invalid characters; try again.'); continue
    phashes = ','.join(hashlib.sha256(choices[int(x)-1].encode()).hexdigest() for x in chosen)
    pinhash = hashlib.sha256(pin.encode()).hexdigest()
    payload = f'P:{phashes}\nI:{pinhash}\nE:{int(enhanced)}\n'.encode()
    result = subprocess.run(['/usr/local/libexec/alternix-auth', '--import', uid], input=payload,
                            capture_output=True, timeout=30)
    if result.returncode != 0:
        raise SystemExit('ERROR: Protected root credential could not be created; sudo PAM was not changed.')
    print('[Auth] Pattern and fallback PIN securely enrolled.')
    break
PY_ENROL

# Check as the actual target user BEFORE making any PAM or sudoers changes.
STATUS="$(/usr/bin/sudo -u "$LOGIN_USER" -H /usr/local/libexec/alternix-auth --status 2>/dev/null)" || {
    echo 'ERROR: Protected credential cannot be read by the target user. PAM unchanged.' >&2
    exit 1
}
if [[ "$STATUS" != READY:0 && "$STATUS" != READY:1 ]]; then
    echo 'ERROR: Unexpected protected credential status. PAM unchanged.' >&2
    exit 1
fi

# Keep the user's normal sudoers command policy, but require authentication.
# Remove the two Alternix-specific passwordless exceptions after PAM is ready.
mkdir -p /etc/sudoers.d
printf '%s ALL=(ALL:ALL) ALL\n' "$LOGIN_USER" > "$TMP/alternix-pattern"
visudo -cf "$TMP/alternix-pattern" >/dev/null
# Reject unrelated passwordless sudo policies instead of claiming that
# every sudo request reaches PAM when some sudoers rule could skip it.
python3 - <<'PY_POLICY'
from pathlib import Path
import re
files = [Path('/etc/sudoers')]
directory = Path('/etc/sudoers.d')
if directory.exists():
    files += [p for p in directory.iterdir() if p.is_file() and p.name not in
              ('alternix-nopasswd', 'zz-alternix-time', 'alternix-pattern')]
for p in files:
    try:
        data = p.read_text()
    except (OSError, UnicodeError):
        raise SystemExit('ERROR: Cannot inspect sudoers source: ' + str(p))
    for line in data.splitlines():
        rule = line.split('#', 1)[0].strip()
        if re.search(r'\bNOPASSWD\s*:', rule, re.I) or re.search(r'\b!authenticate\b', rule, re.I):
            raise SystemExit('ERROR: Another passwordless sudo rule exists in ' + str(p) +
                             '. Remove or scope that rule first; PAM was not changed.')
PY_POLICY
# Backup the original sudo PAM service exactly once, for root-shell recovery.
if [[ ! -e /etc/pam.d/sudo.alternix-original ]]; then
    install -o root -g root -m 0600 /etc/pam.d/sudo /etc/pam.d/sudo.alternix-original
fi
# If re-running, derive from original, never stack duplicate auth statements.
python3 - /etc/pam.d/sudo.alternix-original "$TMP/sudo-pam" <<'PY'
import pathlib, re, sys
old = pathlib.Path(sys.argv[1]).read_text()
# A standard Debian/Devuan sudo PAM file must include common-auth.
if not re.search(r'^\s*@include\s+common-auth\s*$', old, re.M):
    raise SystemExit('ERROR: Unsupported sudo PAM configuration; refusing to override it.')
extra_auth = [line for line in old.splitlines()
              if re.match(r'^\s*auth\s+', line)]
if extra_auth or len(re.findall(r'^\s*@include\s+common-auth\s*$', old, re.M)) != 1:
    raise SystemExit('ERROR: Nonstandard sudo PAM auth rules; refusing to replace them.')
clean = []
for line in old.splitlines():
    if re.match(r'^\s*@include\s+common-auth\s*$', line):
        clean.append('auth requisite pam_exec.so seteuid quiet /usr/local/libexec/alternix-auth --pam')
    else:
        clean.append(line)
pathlib.Path(sys.argv[2]).write_text('\n'.join(clean) + '\n')
PY
install -o root -g root -m 0440 "$TMP/alternix-pattern" /etc/sudoers.d/alternix-pattern
install -o root -g root -m 0644 "$TMP/sudo-pam" /etc/pam.d/sudo
# Remove old Alternix NOPASSWD entries that defeated pattern authentication.
rm -f /etc/sudoers.d/alternix-nopasswd /etc/sudoers.d/zz-alternix-time
visudo -c -q || { echo 'ERROR: Invalid sudoers. Recover using root shell.' >&2; exit 1; }
echo '[Auth] sudo now uses Alternix PAM pattern/PIN authentication.'
echo '[Auth] PAM backup: /etc/pam.d/sudo.alternix-original'
echo '[Auth] For root-shell recovery: cp /etc/pam.d/sudo.alternix-original /etc/pam.d/sudo'
echo '[Auth] Any user without a pattern must first configure it via osm-lock.'
