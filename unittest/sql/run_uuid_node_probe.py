#!/usr/bin/env python3
"""Build and run the production UUID node component probe on one Linux CPU."""

import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--real", action="store_true", help="use the actual OS network interfaces")
    modes.add_argument("--real-permission", action="store_true", help="kernel-denied discovery in this child only")
    modes.add_argument("--real-device", action="store_true", help="kernel-denied discovery/getrandom; real urandom")
    parser.add_argument("--sanitize", action="store_true", help="enable AddressSanitizer and UndefinedBehaviorSanitizer")
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        parser.error("the syscall probe requires Linux; use WSL on Windows")

    # The compiler and probe inherit this affinity. No concurrent build jobs.
    cpu = min(os.sched_getaffinity(0))
    os.sched_setaffinity(0, {cpu})
    root = Path(__file__).resolve().parents[2]
    output = root / "build_bugfix" / ("uuid_node_probe_sanitized" if args.sanitize else "uuid_node_probe")
    output.parent.mkdir(parents=True, exist_ok=True)
    print(f"CPU={cpu} compiler=g++ output={output}", flush=True)
    command = [
        "g++", "-std=gnu++20", "-O1", "-Isrc", "-Isrc/oblib",
        "unittest/sql/test_uuid_node_fallback.cpp", "-Wl,--wrap=socket",
        "-Wl,--wrap=ioctl", "-Wl,--wrap=close", "-Wl,--wrap=getrandom",
        "-Wl,--wrap=open", "-Wl,--wrap=read", "-Wl,--wrap=__read_chk", "-o", str(output),
    ]
    if args.sanitize:
        command.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"])
    subprocess.run(command, cwd=root, check=True)
    mode = (["--real"] if args.real else ["--real-permission"] if args.real_permission
            else ["--real-device"] if args.real_device else [])
    return subprocess.run([str(output), *mode], cwd=root).returncode


if __name__ == "__main__":
    sys.exit(main())
