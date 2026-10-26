# Copyright (c) 2026 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Run the native UUID-node methods with syscall fault injection and sanitizers.

The normal Bazel target compiles the complete production translation unit. This
focused fallback reuses an existing release build's native OBLib and toolchain,
compiling the constructor/init method definitions verbatim from production. It
does not reconstruct the original cloud sandbox or run a complete SQL server.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiled-tree", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("build_uuid_fault"))
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--baseline-ref")
    args = parser.parse_args()
    if args.baseline and not args.baseline_ref:
        parser.error("--baseline requires an explicit --baseline-ref commit")
    root = Path(__file__).resolve().parents[4]
    cached = args.compiled_tree.resolve()
    header_path = "src/sql/engine/expr/ob_expr_uuid.h"

    def node_declaration(path):
        header = path.read_text()
        begin = header.index("struct ObUUIDNode")
        return header[begin:header.index("};", begin) + 2]

    declaration = node_declaration(root / header_path)
    if declaration != node_declaration(cached / header_path):
        raise RuntimeError("compiled-tree ObUUIDNode declaration does not match target source")
    args.output.mkdir(parents=True, exist_ok=True)
    output = args.output.resolve()
    source_path = "src/sql/engine/expr/ob_expr_uuid.cpp"
    source = subprocess.check_output(["git", "show", args.baseline_ref + ":" + source_path], cwd=root, text=True) if args.baseline else (root / source_path).read_text()
    methods = source[source.index("ObUUIDNode::ObUUIDNode()"):source.index("int ObUUIDTime::get_time(")]
    prefix = """#define USING_LOG_PREFIX SQL_ENG
#include <sys/ioctl.h>
#include <net/if.h>
#include <openssl/rand.h>
#include "sql/engine/expr/ob_expr_uuid.h"
using namespace oceanbase::common;
namespace oceanbase { namespace sql {
"""
    native_source = output / "uuid_node_native.cpp"
    native_source.write_text(prefix + methods + "\n} }\n")
    database = json.loads((cached / "build_release/compile_commands.json").read_text())
    entry = next(item for item in database if "/src/sql/" in item["file"])
    original = shlex.split(entry["command"])
    compiler = original[0]
    flags = []
    iterator = iter(original[1:])
    for value in iterator:
        if value in {"-o", "-c"}:
            next(iterator)
            continue
        if value == "-Werror":
            continue
        # Keep the native cached header/library pair together. The target node
        # declaration was checked byte-for-byte above; do not invent a fake class.
        flags.append(value)
    flags += ["-I" + str(root / "src"), "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize=vptr", "-fno-sanitize-recover=all"]
    records = []

    def run(command):
        result = subprocess.run(command, capture_output=True, text=True, cwd=output,
                                env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1", ASAN_OPTIONS="detect_leaks=0"))
        records.append({"command": command, "exit_code": result.returncode,
                        "stdout": result.stdout, "stderr": result.stderr})
        print(result.stdout + result.stderr, end="", flush=True)
        return result.returncode

    try:
        for source_file, target in [(native_source, output / "node.o"),
                                    (root / "unittest/sql/engine/expr/test_uuid_node.cpp", output / "test.o")]:
            if run([compiler, *flags, "-c", str(source_file), "-o", str(target)]):
                raise RuntimeError("native compilation failed")
        development = cached / "deps/3rd/usr/local/oceanbase/deps/devel"
        malloc_objects = sorted((cached / "build_release/src/oblib/lib/CMakeFiles/ob_malloc_object.dir/Unity").glob("*.o"))
        if not malloc_objects:
            raise RuntimeError("compiled-tree is missing native malloc object files")
        command = [compiler, *[flag for flag in flags if flag.startswith("--gcc-toolchain=")],
                   "-fsanitize=address,undefined", "-fno-sanitize=vptr", "-fuse-ld=lld", "-Wl,--gc-sections", "-o", str(output / "test_uuid_node"),
                   str(output / "node.o"), str(output / "test.o"),
                   *map(str, malloc_objects),
                   str(development / "lib/libgtest.a"),
                   str(cached / "build_release/src/oblib/liboblib.a"),
                   str(development / "lib/libjemalloc.a"),
                   str(development / "lib/libunwind.a"),
                   "-L" + str(development / "lib/vsag_lib"), "-lvsag",
                   "-Wl,-rpath," + str(development / "lib/vsag_lib"),
                   "-lcrypto", "-lz", "-ldl", "-lpthread", "-lrt", "-lm",
                   "-Wl,--wrap=socket", "-Wl,--wrap=ioctl", "-Wl,--wrap=close", "-Wl,--wrap=RAND_bytes"]
        if run(command):
            raise RuntimeError("native link failed")
        result = run([str(output / "test_uuid_node")])
    finally:
        archive = cached / "build_release/src/oblib/liboblib.a"
        digest = hashlib.sha256()
        with archive.open("rb") as stream:
            while data := stream.read(1024 * 1024):
                digest.update(data)
        report = {"baseline": args.baseline, "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
                  "baseline_ref": args.baseline_ref if args.baseline else None,
                  "methods_sha256": hashlib.sha256(methods.encode()).hexdigest(),
                  "native_node_declaration_identical": True,
                  "native_node_declaration_sha256": hashlib.sha256(declaration.encode()).hexdigest(),
                  "native_header_and_library_tree": str(cached),
                  "native_header_tree_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=cached, text=True).strip(),
                  "native_oblib_archive_sha256": digest.hexdigest(),
                  "native_oblib_build_commit": "pre-existing archive; build-commit provenance not independently reconstructed",
                  "runtime_library_path": os.environ.get("LD_LIBRARY_PATH", ""),
                  "scope": "actual production ObUUIDNode constructor/init + native OB class/header/library; injected OS/entropy boundary; no complete server/sandbox claim",
                  "commands": records}
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    raise SystemExit(result)
