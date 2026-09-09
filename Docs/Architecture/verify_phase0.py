"""Read-only comparison with Phase 0 source/config and tracked resource baseline."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[2]
    evidence = json.loads(Path(__file__).with_name("PHASE0_EVIDENCE.json").read_text(encoding="utf-8"))
    differences = []
    for name, expected in evidence["sha256"].items():
        path = root / name
        actual = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None
        if actual != expected:
            differences.append(name)
    # hash-object reads the working tree and respects Git clean filters, without writing objects.
    for name, expected in evidence["resourceGitBlobs"].items():
        path = root / name
        actual = subprocess.check_output(["git", "hash-object", "--", name], cwd=root).decode().strip() if path.is_file() else None
        if actual != expected:
            differences.append(name)
    print(f"Compared {len(evidence['sha256'])} source/config files and {len(evidence['resourceGitBlobs'])} resources.")
    for name in differences:
        print("CHANGED:", name)
    print("PASS: baseline bytes unchanged" if not differences else "DIFF: review changes; this is not a behavior-equivalence test")
    return int(bool(differences))


if __name__ == "__main__":
    sys.exit(main())
