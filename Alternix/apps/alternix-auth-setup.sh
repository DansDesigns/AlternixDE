
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
    echo 'ERROR: Missing authentication sources or target user home.' >&2
    exit 1
fi

if ! grep -Fq '"--pam-prompt"' "$APPS_DIR/osm-lock.cpp"; then
    echo 'ERROR: osm-lock.cpp has not been updated for PAM authentication.' >&2
    exit 1
fi

for dep in cc g++ pkg-config visudo python3; do
    command -v "$dep" >/dev/null || {
        echo "ERROR: Missing dependency: $dep" >&2
        exit 1
    }
done

if ! pkg-config --exists Qt5Widgets; then
    echo 'ERROR: Missing Qt5Widgets development package.' >&2
    exit 1
fi

if ! test -f /usr/include/openssl/evp.h; then
    echo 'ERROR: libssl-dev is needed to build the verifier.' >&2
    exit 1
fi

if ! find /lib /usr/lib -name pam_exec.so -type f -print -quit 2>/dev/null | grep -q .; then
    echo 'ERROR: pam_exec.so is not installed; refusing to modify sudo PAM.' >&2
    exit 1
fi

[[ -f /etc/pam.d/sudo ]] || {
    echo 'ERROR: /etc/pam.d/sudo absent.' >&2
    exit 1
}

visudo -c -q
umask 077
TMP="$(mktemp -d)"

# Retain root-level rollback capability, independently of sudo.
ACTIVATION_IN_PROGRESS=0

rollback_auth_policy() {
    if [[ -f "$TMP/pam-before" ]]; then
        install -o root -g root -m 0644 \
            "$TMP/pam-before" \
            /etc/pam.d/.sudo.alternix-rollback
        mv -f /etc/pam.d/.sudo.alternix-rollback /etc/pam.d/sudo
    fi

    if [[ -f "$TMP/power-before" ]]; then
        install -o root -g root -m 0440 \
            "$TMP/power-before" \
            /etc/sudoers.d/90-alternix-power
    fi

    if [[ -f "$TMP/pattern-before" ]]; then
        install -o root -g root -m 0440 \
            "$TMP/pattern-before" \
            /etc/sudoers.d/alternix-pattern
    else
        rm -f /etc/sudoers.d/alternix-pattern
    fi

    for old_rule in alternix-nopasswd zz-alternix-time; do
        if [[ -f "$TMP/$old_rule.before" ]]; then
            install -o root -g root -m 0440 \
                "$TMP/$old_rule.before" \
                "/etc/sudoers.d/$old_rule"
        fi
    done

    if ! visudo -c -q; then
        echo 'CRITICAL: Rollback restored files, but visudo reports an error; use the root console.' >&2
    fi
}

auth_setup_exit() {
    local exit_code=$?
    trap - EXIT INT TERM

    if [[ "$ACTIVATION_IN_PROGRESS" = 1 ]]; then
        echo 'ERROR: Alternix sudo/PAM verification failed. Restoring previous configuration...' >&2
        rollback_auth_policy || true
        echo 'Previous sudo/PAM configuration restored. Graphical authentication was NOT enabled.' >&2
        exit_code=1
    fi

    rm -rf -- "$TMP"
    exit "$exit_code"
}

trap auth_setup_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# Compile both authentication components before modifying policy.
cc -O2 -std=c11 -Wall -Wextra -Werror -fPIE -pie \
   -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
   "$APPS_DIR/alternix-auth.c" \
   -o "$TMP/alternix-auth" -lcrypto

if ! g++ -O2 -fPIC \
    "$APPS_DIR/osm-lock.cpp" \
    -o "$TMP/osm-lock" \
    $(pkg-config --cflags --libs Qt5Widgets); then
    echo 'ERROR: osm-lock build failed; no PAM changes applied.' >&2
    exit 1
fi

mkdir -p /usr/local/libexec /var/lib/alternix-auth
chown root:root /var/lib/alternix-auth
chmod 0700 /var/lib/alternix-auth

install -o root -g root -m 0755 \
    "$TMP/osm-lock" /usr/local/bin/osm-lock

install -o root -g root -m 0755 \
    "$TMP/alternix-auth" /usr/local/libexec/alternix-auth

chmod 4755 /usr/local/libexec/alternix-auth

# Migrate existing pattern/PIN into protected storage.
python3 - "$LOGIN_USER" "$USER_UID" \
    "$USER_HOME/.config/Alternix/.osm_lockdata" <<'PY'
import os
import pathlib
import re
import stat
import subprocess
import sys

username, uid_s, file_path = sys.argv[1:]
uid = int(uid_s)
path = pathlib.Path(file_path)
cred = pathlib.Path('/var/lib/alternix-auth/credential-' + uid_s)

if cred.exists():
    print('[Auth] Protected credential already present; preserving it.')
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
                fd = os.open(
                    str(path),
                    os.O_WRONLY | os.O_TRUNC | os.O_NOFOLLOW | os.O_CLOEXEC
                )
                try:
                    os.write(
                        fd,
                        ('enhanced=' + ('1' if 'enhanced=1' in old else '0') + '\n').encode('ascii')
                    )
                    os.fsync(fd)
                finally:
                    os.close(fd)
    sys.exit(0)

try:
    fd = os.open(str(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
except FileNotFoundError:
    print('[Auth] No existing pattern; enrolment required.')
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

values = dict(
    line.split('=', 1)
    for line in old.splitlines()
    if '=' in line
)

enh = values.get('enhanced', '0')
pattern = values.get('pattern', '')
pin = values.get('password', '')

if enh not in ('0', '1') or not re.fullmatch(r'[a-f0-9]{64}', pin):
    print('[Auth] Incomplete legacy credential. Enrolment required.')
    sys.exit(0)

shapes = pattern.split(',')
expected = 5 if enh == '1' else 4

if len(shapes) != expected or not all(
    re.fullmatch(r'[a-f0-9]{64}', item) for item in shapes
):
    print('[Auth] Incomplete legacy pattern. Enrolment required.')
    sys.exit(0)

payload = ('P:' + pattern + '\nI:' + pin + '\nE:' + enh + '\n').encode('ascii')

proc = subprocess.run(
    ['/usr/local/libexec/alternix-auth', '--import', uid_s],
    input=payload,
    stdout=subprocess.DEVNULL,
    stderr=subprocess.PIPE,
    timeout=30
)

if proc.returncode != 0:
    raise RuntimeError(
        'Protected credential import failed: ' +
        proc.stderr.decode(errors='replace')
    )

fd = os.open(
    str(path),
    os.O_WRONLY | os.O_TRUNC | os.O_NOFOLLOW | os.O_CLOEXEC
)
try:
    os.write(fd, ('enhanced=' + enh + '\n').encode('ascii'))
    os.fsync(fd)
finally:
    os.close(fd)

print('[Auth] Migrated existing lockscreen credential to protected storage.')
PY

# First-time enrolment from the controlling terminal.
python3 - "$USER_UID" <<'PY_ENROL'
import getpass
import hashlib
import pathlib
import subprocess
import sys

uid = sys.argv[1]
cred = pathlib.Path('/var/lib/alternix-auth/credential-' + uid)

if cred.exists():
    sys.exit(0)

try:
    tty_input = open('/dev/tty', 'r', encoding='utf-8', buffering=1)
    tty_output = open('/dev/tty', 'w', encoding='utf-8', buffering=1)
except OSError:
    raise SystemExit(
        'ERROR: No enrolled credential and no interactive terminal. PAM unchanged.'
    )

def ask(prompt):
    tty_output.write(prompt)
    tty_output.flush()
    answer = tty_input.readline()
    if not answer:
        raise SystemExit('ERROR: Enrolment cancelled. PAM unchanged.')
    return answer.strip()

print('\n[Auth] Set the Alternix administrator pattern.')
shapes = ('circle', 'triangle', 'square', 'pentagon')
colors = ('red', 'blue', 'green', 'white')
choices = [
    f'{shape}-{color}'
    for shape in shapes
    for color in colors
]

for i, item in enumerate(choices, 1):
    print(f' {i:2}) {item}', end='\n' if i % 4 == 0 else '   ')

while True:
    enhanced = ask(
        'Enhanced Security (5 shapes / 5-character PIN)? [y/N]: '
    ).lower() in ('y', 'yes')

    count = 5 if enhanced else 4
    chosen = ask(
        f'Enter {count} shape numbers in order, separated by spaces: '
    ).split()

    if len(chosen) != count or not all(
        x.isdigit() and 1 <= int(x) <= 16 for x in chosen
    ):
        print('Invalid shape sequence; try again.')
        continue

    pin = getpass.getpass(
        f'Choose a {count}-character fallback PIN: ',
        stream=tty_output
    )
    pin2 = getpass.getpass(
        'Confirm fallback PIN: ',
        stream=tty_output
    )

    charset = '0123456789!?<>' if enhanced else '0123456789'

    if len(pin) != count or any(c not in charset for c in pin) or pin != pin2:
        print('PIN mismatch or invalid characters; try again.')
        continue

    phashes = ','.join(
        hashlib.sha256(choices[int(x)-1].encode()).hexdigest()
        for x in chosen
    )
    pinhash = hashlib.sha256(pin.encode()).hexdigest()

    payload = (
        f'P:{phashes}\n'
        f'I:{pinhash}\n'
        f'E:{int(enhanced)}\n'
    ).encode()

    result = subprocess.run(
        ['/usr/local/libexec/alternix-auth', '--import', uid],
        input=payload,
        capture_output=True,
        timeout=30
    )

    if result.returncode != 0:
        raise SystemExit(
            'ERROR: Protected credential could not be created; PAM unchanged.'
        )

    print('[Auth] Pattern and fallback PIN enrolled.')
    break

tty_input.close()
tty_output.close()
PY_ENROL

# Verify the protected credential before touching PAM.
STATUS="$(/usr/bin/sudo -u "$LOGIN_USER" -H \
    /usr/local/libexec/alternix-auth --status 2>/dev/null)" || {
    echo 'ERROR: Protected credential cannot be read. PAM unchanged.' >&2
    exit 1
}

if [[ "$STATUS" != READY:0 && "$STATUS" != READY:1 ]]; then
    echo 'ERROR: Unexpected credential status. PAM unchanged.' >&2
    exit 1
fi

# Ensure ordinary sudo-group policy controls privileges.
mkdir -p /etc/sudoers.d

case " $(id -nG "$LOGIN_USER") " in
    *" sudo "*) ;;
    *)
        echo 'ERROR: User is not in the sudo group.' >&2
        exit 1
        ;;
esac

# Stage conversion of the known legacy power rule.
python3 - "$TMP" <<'PY_POLICY'
from pathlib import Path
import re
import stat
import subprocess
import sys

stage = Path(sys.argv[1])
power = Path('/etc/sudoers.d/90-alternix-power')
modified_power = None

if power.is_symlink():
    raise SystemExit('ERROR: Unsafe power sudoers symlink; PAM unchanged.')

if power.exists():
    st = power.stat()

    if not stat.S_ISREG(st.st_mode) or st.st_uid != 0 or (st.st_mode & 0o022):
        raise SystemExit('ERROR: Unsafe power sudoers ownership; PAM unchanged.')

    original = power.read_text(encoding='utf-8')
    rewritten = []

    for line in original.splitlines(keepends=True):
        rule, comment_mark, comment = line.partition('#')

        if re.search(r'(?<!\w)!authenticate\b', rule, re.I):
            raise SystemExit('ERROR: Authentication bypass in power rule.')

        rule = re.sub(
            r'\bNOPASSWD\s*:',
            'PASSWD:',
            rule,
            flags=re.I
        )
        rewritten.append(rule + comment_mark + comment)

    normalized = ''.join(rewritten)

    if normalized != original:
        dest = stage / '90-alternix-power.password-required'
        dest.write_text(normalized, encoding='utf-8')

        cmd = subprocess.run(
            ['visudo', '-cf', str(dest)],
            capture_output=True,
            text=True
        )

        if cmd.returncode:
            raise SystemExit('ERROR: Invalid converted power sudoers rule.')

        modified_power = normalized
        print('[Auth] Power rule will require authentication.')

files = [Path('/etc/sudoers')]
directory = Path('/etc/sudoers.d')

if directory.exists():
    files += [
        p for p in directory.iterdir()
        if p.is_file() and p.name not in
        ('alternix-nopasswd', 'zz-alternix-time', 'alternix-pattern')
    ]

for p in files:
    data = (
        modified_power
        if p == power and modified_power is not None
        else p.read_text(encoding='utf-8')
    )

    for line in data.splitlines():
        rule = line.split('#', 1)[0]

        if (
            re.search(r'\bNOPASSWD\s*:', rule, re.I)
            or re.search(r'(?<!\w)!authenticate\b', rule, re.I)
        ):
            raise SystemExit(
                'ERROR: Another passwordless sudo rule exists in ' +
                str(p) + '. PAM unchanged.'
            )
PY_POLICY

# Verify the actual graphical session before changing PAM.
if [[ -z "${DISPLAY:-}" ]]; then
    echo 'ERROR: No graphical DISPLAY. sudo PAM was NOT changed.' >&2
    exit 1
fi

for dep in runuser timeout; do
    command -v "$dep" >/dev/null || {
        echo "ERROR: Missing dependency: $dep. PAM unchanged." >&2
        exit 1
    }
done

AUTH_SESSION_ENV=(
    "HOME=$USER_HOME"
    "USER=$LOGIN_USER"
    "LOGNAME=$LOGIN_USER"
    'PATH=/usr/local/bin:/usr/bin:/bin'
    "DISPLAY=$DISPLAY"
)

if [[ -n "${XAUTHORITY:-}" ]]; then
    AUTH_SESSION_ENV+=("XAUTHORITY=$XAUTHORITY")
elif [[ -f "$USER_HOME/.Xauthority" ]]; then
    AUTH_SESSION_ENV+=("XAUTHORITY=$USER_HOME/.Xauthority")
fi

for key in XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS; do
    if [[ -n "${!key:-}" ]]; then
        AUTH_SESSION_ENV+=("$key=${!key}")
    fi
done

echo '[Auth] Testing pattern/PIN window before changing sudo PAM...'
echo '[Auth] Enter your existing pattern or fallback PIN.'

if ! timeout --kill-after=5s 150s \
    runuser -u "$LOGIN_USER" -- \
    env -i "${AUTH_SESSION_ENV[@]}" \
    /usr/local/libexec/alternix-auth --pam; then
    echo 'ERROR: Graphical pattern verification failed. PAM unchanged.' >&2
    exit 1
fi

# Preserve original PAM policy for emergency recovery.
if [[ ! -e /etc/pam.d/sudo.alternix-original ]]; then
    install -o root -g root -m 0600 \
        /etc/pam.d/sudo \
        /etc/pam.d/sudo.alternix-original
fi

# Stage the new PAM policy.
python3 - /etc/pam.d/sudo.alternix-original "$TMP/sudo-pam" <<'PY_PAM'
import pathlib
import re
import sys

old = pathlib.Path(sys.argv[1]).read_text()

if not re.search(r'^\s*@include\s+common-auth\s*$', old, re.M):
    raise SystemExit('ERROR: Unsupported sudo PAM configuration.')

extra_auth = [
    line for line in old.splitlines()
    if re.match(r'^\s*auth\s+', line)
]

if extra_auth or len(
    re.findall(r'^\s*@include\s+common-auth\s*$', old, re.M)
) != 1:
    raise SystemExit('ERROR: Nonstandard PAM rules; refusing replacement.')

clean = []

for line in old.splitlines():
    if re.match(r'^\s*@include\s+common-auth\s*$', line):
        clean.append(
            'auth requisite pam_exec.so seteuid quiet '
            '/usr/local/libexec/alternix-auth --pam'
        )
    else:
        clean.append(line)

pathlib.Path(sys.argv[2]).write_text('\n'.join(clean) + '\n')
PY_PAM

# Snapshot all existing policy files before provisional activation.
install -o root -g root -m 0600 \
    /etc/pam.d/sudo "$TMP/pam-before"

if [[ -f /etc/sudoers.d/alternix-pattern ]]; then
    install -o root -g root -m 0600 \
        /etc/sudoers.d/alternix-pattern \
        "$TMP/pattern-before"
fi

if [[ -f "$TMP/90-alternix-power.password-required" ]]; then
    install -o root -g root -m 0600 \
        /etc/sudoers.d/90-alternix-power \
        "$TMP/power-before"
fi

for old_rule in alternix-nopasswd zz-alternix-time; do
    if [[ -f "/etc/sudoers.d/$old_rule" ]]; then
        install -o root -g root -m 0600 \
            "/etc/sudoers.d/$old_rule" \
            "$TMP/$old_rule.before"
    fi
done

# Begin provisional PAM activation with automatic rollback.
ACTIVATION_IN_PROGRESS=1

if [[ -f "$TMP/90-alternix-power.password-required" ]]; then
    install -d -o root -g root -m 0700 /var/backups/alternix-auth

    if [[ ! -e /var/backups/alternix-auth/90-alternix-power.before-pattern ]]; then
        install -o root -g root -m 0600 \
            "$TMP/power-before" \
            /var/backups/alternix-auth/90-alternix-power.before-pattern
    fi

    install -o root -g root -m 0440 \
        "$TMP/90-alternix-power.password-required" \
        /etc/sudoers.d/90-alternix-power
fi

# Remove earlier unrestricted rule and rely on sudo-group membership.
rm -f /etc/sudoers.d/alternix-pattern

# Atomically replace sudo PAM service.
install -o root -g root -m 0644 \
    "$TMP/sudo-pam" \
    /etc/pam.d/.sudo.alternix-candidate

mv -f /etc/pam.d/.sudo.alternix-candidate /etc/pam.d/sudo

# Remove obsolete passwordless Alternix policies.
for old_rule in alternix-nopasswd zz-alternix-time; do
    rm -f "/etc/sudoers.d/$old_rule"
done

if ! visudo -c -q; then
    echo 'ERROR: New sudoers configuration invalid.' >&2
    exit 1
fi

# Test the real sudo/PAM authentication path, not only helper status.
echo '[Auth] Testing an actual sudo request through Alternix PAM...'
echo '[Auth] Confirm your pattern/PIN again to finish activation.'

if ! timeout --kill-after=5s 150s \
    runuser -u "$LOGIN_USER" -- \
    env -i "${AUTH_SESSION_ENV[@]}" \
    /bin/sh -c '/usr/bin/sudo -k && exec /usr/bin/sudo -v'; then
    echo 'ERROR: Real sudo/PAM challenge failed. Rolling back.' >&2
    exit 1
fi

# Commit only after the real sudo challenge succeeds.
ACTIVATION_IN_PROGRESS=0

echo '[Auth] Live sudo/PAM challenge passed.'
echo '[Auth] Alternix pattern/PIN authentication is active.'
echo '[Auth] PAM backup: /etc/pam.d/sudo.alternix-original'
echo '[Auth] Root recovery: cp /etc/pam.d/sudo.alternix-original /etc/pam.d/sudo'
