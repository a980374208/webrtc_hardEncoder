#!/usr/bin/env python3
"""Measure the product x265 factory on this Windows host, without rebuilding.

Serial repeats use pre-generated synthetic I420 inputs. The probe measures the
complete synchronous Encode call, and reports process CPU normalized to all
logical CPUs. A measured result is not network or production qualification.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


EXTERNAL = Path(__file__).resolve().parent
ROOT = EXTERNAL.parents[3]
RESOLUTIONS = ((640, 360, 600000), (1280, 720, 2000000), (1920, 1080, 4000000))


def timestamp():
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def record(path, name=None):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return {"file": str(path) if name is None else name,
            "bytes": path.stat().st_size, "sha256": digest.hexdigest().upper()}


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def inputs(executable, build_evidence):
    paths = {ROOT / name for name in ("BUILD.gn", "media/BUILD.gn", "webrtc.gni",
             "media/engine/internal_encoder_factory.cc", "out/ReleasePrefixed/args.gn")}
    paths.update(path for path in EXTERNAL.iterdir()
                 if path.is_file() and path.suffix in (".cc", ".h", ".gn"))
    paths.add(Path(__file__).resolve())
    metadata_path = ROOT / "out/x265-dependency-metadata.json"
    metadata = read_json(metadata_path)
    paths.add(metadata_path)
    paths.update(Path(path) for path in metadata["headerPaths"])
    paths.add(Path(metadata["libraryPath"]))
    paths.add(Path(metadata["licensePath"]))
    # Rebind the combined build's SDK/dispatcher and production source inventory
    # to current bytes; the old functional PASS remains at its frozen version.
    original_ledger = ROOT / "out/x265-local-validation-20261006-v1/compile-inputs.json"
    paths.update(Path(item["file"]) for item in read_json(original_ledger)["files"])
    build_result = read_json(build_evidence / "result.json")
    if build_result["status"] != "PASS" or build_result["executable"] != record(executable):
        raise ValueError("Executable does not match its successful current build")
    build_inputs = read_json(build_evidence / "inputs.json")
    for item in build_inputs["files"]:
        if record(Path(item["file"])) != item:
            raise ValueError("Build input changed before measurement: " + item["file"])
    paths.update(build_evidence / name for name in ("result.json", "inputs.json", "build.log"))
    args = (ROOT / "out/ReleasePrefixed/args.gn").read_text()
    if not re.search(r"^\s*is_debug\s*=\s*false\s*$", args, re.M):
        raise ValueError("Performance measurements require the existing optimized Release build")
    if not re.search(r"^\s*rtc_use_x265\s*=\s*true\s*$", args, re.M):
        raise ValueError("x265 is not enabled in the build")
    return {"captured_at_utc": timestamp(), "repository_head": subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
            "executable": record(executable), "dependency": metadata,
            "files": [record(path) for path in sorted(paths, key=str)]}


def host_metadata():
    command = r"""
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object System.Text.UTF8Encoding
$cpu=@(Get-CimInstance Win32_Processor | Select-Object Name,Manufacturer,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed)
$computer=Get-CimInstance Win32_ComputerSystem | Select-Object TotalPhysicalMemory,NumberOfLogicalProcessors
[ordered]@{cpu=$cpu;computer=$computer;power_scheme=@(powercfg /getactivescheme);operating_system=[Environment]::OSVersion.VersionString;measurement_scope='local machine, unchanged power/priority/affinity'} | ConvertTo-Json -Depth 5
"""
    env = os.environ.copy()
    env["PSModulePath"] = str(Path(env.get("SystemRoot", "C:/Windows")) /
                               "System32/WindowsPowerShell/v1.0/Modules")
    powershell = Path(env.get("SystemRoot", "C:/Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    result = subprocess.run([str(powershell), "-NoProfile", "-Command", command],
                            capture_output=True, timeout=30, encoding="utf-8", errors="replace",
                            env=env, creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        return {"status": "UNKNOWN", "error": result.stderr, "exit_code": result.returncode}
    return {"status": "RECORDED", "values": json.loads(result.stdout)}


def verify_inputs(snapshot):
    for item in snapshot["files"] + [snapshot["executable"]]:
        if record(Path(item["file"])) != item:
            raise ValueError("Measurement input changed: " + item["file"])


def run_case(executable, output, case, timeout):
    name = (f"r{case['round']}-{case['width']}x{case['height']}-"
            f"{case['workload']}-{case['mode']}")
    result_file = output / (name + ".json")
    command = [str(executable)]
    for key in ("width", "height", "bitrate", "fps", "seconds", "warmup_seconds", "mode", "workload"):
        command.extend(["--" + key.replace("_", "-"), str(case[key])])
    command.extend(["--output", str(result_file)])
    started = timestamp()
    process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               creationflags=subprocess.CREATE_NO_WINDOW)
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        process.kill()
        stdout, stderr = process.communicate(timeout=15)
    for suffix, data in (("stdout.txt", stdout), ("stderr.txt", stderr)):
        with (output / (name + "." + suffix)).open("xb") as stream:
            stream.write(data)
    result = {"case": case, "command": command, "started_at_utc": started,
              "finished_at_utc": timestamp(), "exit_code": process.returncode,
              "timed_out": timed_out, "result_file": result_file.name, "status": "FAIL"}
    if result_file.is_file():
        try:
            result["measurement"] = read_json(result_file)
            result["result_sha256"] = record(result_file)["sha256"]
            if not timed_out and process.returncode == 0 and result["measurement"].get("status") == "MEASURED":
                result["status"] = "MEASURED"
        except (ValueError, OSError) as error:
            result["parse_error"] = str(error)
    write_json(output / (name + ".process.json"), result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "out/ReleasePrefixed/x265_performance_probe.exe")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--build-evidence", type=Path, default=ROOT / "out/x265-performance-build-20261006-v2")
    parser.add_argument("--rounds", type=int, default=3, choices=range(1, 6))
    parser.add_argument("--paced-seconds", type=int, default=20, choices=range(5, 121))
    parser.add_argument("--capacity-seconds", type=int, default=5, choices=range(5, 121))
    parser.add_argument("--warmup-seconds", type=int, default=2, choices=range(1, 11))
    args = parser.parse_args()
    executable = args.exe.resolve()
    output = (args.output or ROOT / ("out/x265-performance-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))).resolve()
    output.mkdir(parents=True, exist_ok=False)
    snapshot = inputs(executable, args.build_evidence.resolve())
    write_json(output / "inputs.json", snapshot)
    write_json(output / "host.json", host_metadata())
    results = []
    print("Evidence:", output, flush=True)
    for round_index in range(1, args.rounds + 1):
        # Rotate resolution/workload order between rounds to expose drift.
        scenarios = [(width, height, bitrate, workload)
                     for width, height, bitrate in RESOLUTIONS for workload in ("screen", "texture")]
        shift = (round_index - 1) * 2
        scenarios = scenarios[shift:] + scenarios[:shift]
        for width, height, bitrate, workload in scenarios:
            for mode, seconds in (("paced", args.paced_seconds), ("capacity", args.capacity_seconds)):
                case = {"round": round_index, "width": width, "height": height,
                        "bitrate": bitrate, "fps": 30, "workload": workload, "mode": mode,
                        "seconds": seconds, "warmup_seconds": args.warmup_seconds}
                verify_inputs(snapshot)
                result = run_case(executable, output, case, timeout=seconds + args.warmup_seconds + 90)
                results.append(result)
                print(f"[{len(results)}/{args.rounds * 12}] r{round_index} {width}x{height} {workload} {mode}: {result['status']}", flush=True)
                if result["status"] != "MEASURED":
                    write_json(output / "result.json", {"status": "FAIL", "scope": __doc__, "cases": results,
                        "reason": "A required probe failed; samples and logs retained without excluding anomalies"})
                    return 1
    verify_inputs(snapshot)
    write_json(output / "result.json", {"schema_version": 1, "status": "MEASURED",
        "scope": "Local real x265 builtin-factory synthetic input performance; no network, receiver or release qualification",
        "rounds": args.rounds, "cases": results, "recorded_at_utc": timestamp(),
        "input_snapshot_sha256": record(output / "inputs.json")["sha256"],
        "host_metadata_sha256": record(output / "host.json")["sha256"]})
    write_json(output / "artifact-hashes.json", {"recorded_at_utc": timestamp(),
        "files": [record(path, path.relative_to(output).as_posix()) for path in sorted(output.rglob("*")) if path.is_file()]})
    print("Measurement: MEASURED (not qualification PASS)", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print("ERROR:", error, file=sys.stderr, flush=True)
        sys.exit(1)
