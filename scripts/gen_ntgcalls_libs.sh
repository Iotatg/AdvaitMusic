#!/usr/bin/env bash
# Regenerate the two link artifacts derived from lib/libntgcalls.a:
#   libntgcalls_trimmed.a         - base archive minus the BoringSSL crypto members
#   libntgcalls_boringssl_forced.a- those members only (renamed bs_*), force-linked
# The CMake target advait_ntg links trimmed normally and the forced archive with
# --whole-archive so the whole app binds to the bundled BoringSSL instead of the
# system libcrypto (fixes an ASN1 SIGSEGV during WebRTC DTLS).
#
# Trimmed archive >100MB so it is git-ignored; run this after cloning.
set -euo pipefail
cd "$(dirname "$0")/.."

BASE=lib/libntgcalls.a
LIST=scripts/boringssl_crypto_objs.json
TRIMMED=lib/libntgcalls_trimmed.a
FORCED=lib/libntgcalls_boringssl_forced.a

[ -f "$BASE" ] || { echo "missing $BASE"; exit 1; }

# member names, one per line
mapfile -t MEMBERS < <(python3 -c "import json;print('\n'.join(json.load(open('$LIST'))))")

rm -rf lib/.gen_tmp
mkdir -p lib/.gen_tmp
cp "$BASE" "$TRIMMED"
ar d "$TRIMMED" "${MEMBERS[@]}"

# forced archive: extract the crypto members with a bs_ prefix
BASE_ABS="$PWD/$BASE"
OUT_ABS="$PWD/lib/.gen_tmp"
for m in "${MEMBERS[@]}"; do
  ar x "$BASE_ABS" "$m" --output="$OUT_ABS" 2>/dev/null || (cd "$OUT_ABS" && ar x "$BASE_ABS" "$m")
  mv "$OUT_ABS/$m" "$OUT_ABS/bs_$m"
done

# drop members whose exported symbols duplicate another member's (whole-archive
# linking pulls every object in, duplicates would be redefinition errors)
python3 - <<'EOF'
import os, subprocess, glob
seen = {}
drop = []
for obj in sorted(glob.glob("lib/.gen_tmp/bs_*.o")):
    out = subprocess.run(["nm", "--defined-only", "-g", obj], capture_output=True, text=True).stdout
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ("T", "D", "B", "R", "r"):
            syms.add(parts[2])
    clash = {s for s in syms if s in seen and seen[s] != obj}
    if clash and not syms - {s for s in syms if s in seen and seen[s] != obj}:
        drop.append(obj)
        continue
    for s in syms:
        seen.setdefault(s, obj)
for obj in drop:
    os.remove(obj)

# remove symbols already fully defined by other kept members of the forced set
ar = ["ar", "rc", "lib/libntgcalls_boringssl_forced.a"] + sorted(glob.glob("lib/.gen_tmp/bs_*.o"))
subprocess.run(["rm", "-f", "lib/libntgcalls_boringssl_forced.a"], check=False)
subprocess.run(ar, check=True)
EOF

rm -rf lib/.gen_tmp
ranlib "$FORCED" 2>/dev/null || true
ranlib "$TRIMMED" 2>/dev/null || true
echo "generated $TRIMMED and $FORCED"
