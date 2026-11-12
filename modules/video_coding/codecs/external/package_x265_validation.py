#!/usr/bin/env python3
"""Freeze a hash-bound x265 software HEVC validation package.

This tool reuses the existing QSV packaging primitives and current combined-build
SDK/license metadata, then verifies the real task-owned x265 static dependency.
It never builds, tests, downloads, or modifies external sources. A local runner
stage is explicitly separate from immutable final package outputs.
"""

import argparse
import importlib.util
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


EXTERNAL = Path(__file__).resolve().parent
ROOT = EXTERNAL.parents[3]
BUILD = ROOT / "out/ReleasePrefixed"
VERSION = "20261006-v1"
SCOPE = "x265 H265 Main software encode/RTP/independent FFmpeg HEVC decode and OpenH264 regression"
BINARIES = ("backend_video_encoder_unittests.exe", "nvenc_h264_smoke.exe", "x265_h265_smoke.exe")
RUNNER = "run_x265_validation.ps1"
MAX_EVIDENCE_BYTES = 8 * 1024 * 1024
FFMPEG = None
X265_METADATA = None

_spec = importlib.util.spec_from_file_location(
    "_qsv_package_primitives", EXTERNAL / "package_qsv_h265_validation.py")
common = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(common)
require = common.require
sha256 = common.sha256
record = common.record
read_json = common.read_json
write_json = common.write_json
utc_now = common.utc_now


def x265_metadata(values):
    require(X265_METADATA is not None and X265_METADATA.is_file(),
            "Set --x265-metadata to the verified real static dependency metadata")
    metadata = read_json(X265_METADATA)
    require(metadata.get("schemaVersion") == 1 and metadata.get("name") == "x265",
            "Unexpected x265 dependency metadata schema/name")
    require(metadata.get("staticLibrary") is True and metadata.get("crt") == "MT_StaticRelease"
            and metadata.get("bitDepth") == 8,
            "x265 must be an 8-bit static /MT Release library")
    require(metadata.get("sourceGitClean") is True and metadata.get("sourceGitStatus") == [],
            "x265 source metadata is not clean")
    source = Path(metadata["sourceDirectory"])
    require(common.git(source, "rev-parse", "HEAD") == metadata["sourceGitCommit"],
            "x265 source commit changed")
    require(common.git(source, "status", "--porcelain", "--untracked-files=all") == "",
            "x265 external source is dirty")
    require(isinstance(metadata.get("version"), str) and metadata["version"].strip(),
            "x265 dependency has no recorded version")
    include = Path(metadata["includeDirectory"]).resolve()
    library = Path(metadata["libraryPath"]).resolve()
    require(Path(values.get("rtc_x265_include_dir", "")).resolve() == include,
            "GN x265 include directory differs from the verified dependency")
    require(Path(values.get("rtc_x265_library", "")).resolve() == library,
            "GN x265 library differs from the verified dependency")
    headers = {Path(path).resolve() for path in metadata["headerPaths"]}
    require(headers == {include / "x265.h", include / "x265_config.h"},
            "x265 header inventory must contain exactly x265.h and x265_config.h")
    files = [dict(record(path, "x265/include/" + path.name), path=str(path))
             for path in sorted(headers)]
    files.append(dict(record(library, "x265/lib/" + library.name), path=str(library)))
    config = (include / "x265_config.h").read_text(encoding="utf-8-sig")
    match = re.search(r"^\s*#\s*define\s+X265_BUILD\s+(\d+)\s*$", config, re.M)
    require(match is not None, "Missing x265 API build in x265_config.h")
    if "apiBuild" in metadata:
        require(metadata["apiBuild"] == int(match.group(1)), "x265 metadata/API build mismatch")
    license_path = Path(metadata["licensePath"]).resolve()
    require(Path(values.get("rtc_x265_license_file", "")).resolve() == license_path,
            "GN x265 license differs from the original source COPYING")
    require(metadata.get("license") == "GPL-2.0-or-later" and
            license_path == (source / "COPYING").resolve(),
            "x265 license must be the original source COPYING (GPL-2.0-or-later)")
    license_record = dict(record(license_path, "licenses/x265-COPYING"), sourcePath=str(license_path))
    metadata = dict(metadata)
    metadata["apiBuild"] = int(match.group(1))
    metadata["metadataFile"] = dict(record(X265_METADATA, "x265-dependency-metadata.json"),
                                    path=str(X265_METADATA))
    metadata["coffDirectives"] = common.coff_crt(library)
    metadata["verification"] = "Rechecked metadata SHA, Git HEAD/clean, exact API headers, library SHA/COFF CRT and original COPYING"
    return metadata, files, license_record


def current_inputs(version):
    common.FFMPEG = FFMPEG
    args_record, values = common.gn_args()
    require(values.get("rtc_use_x265") == "true" and values.get("rtc_x265_build_tests") == "true",
            "x265 backend and validation GN flags must be enabled")
    sdk, previous = common.dependencies(values)
    x265, x265_files, x265_license = x265_metadata(values)
    sdk["x265"] = x265
    sdk["files"].extend(x265_files)
    licenses = common.license_records(previous) + [x265_license]
    notice = ROOT / ("out/x265-local-validation-" + version + "/product-licenses/LICENSE.md")
    if notice.is_file():
        licenses.append(dict(record(notice, "licenses/Current-WebRTC-THIRD-PARTY-NOTICES.md"),
                             sourcePath=str(notice)))
    inputs = {
        "schemaVersion": 2, "packageVersion": version,
        "repositoryHead": common.git(ROOT, "rev-parse", "HEAD"),
        "buildDirectory": "out/ReleasePrefixed", "gnArgsSha256": args_record["sha256"],
        "gnArgs": args_record, "files": common.source_records(), "dependencies": sdk,
        "licenses": licenses}
    compile_path = ROOT / ("out/x265-local-validation-" + version + "/compile-inputs.json")
    if compile_path.exists():
        frozen = read_json(compile_path)
        listed = set()
        for item in frozen["files"]:
            path = Path(item["file"])
            require(sha256(path) == item["sha256"].upper(), "Prebuild compilation input changed: " + str(path))
            listed.add(path.resolve())
        required = {ROOT / item["file"] for item in inputs["files"]
                    if Path(item["file"]).suffix.lower() in (".cc", ".h", ".gn", ".gni")}
        required.update(Path(item["path"]) for item in sdk["files"])
        required.add(BUILD / "args.gn")
        require({path.resolve() for path in required}.issubset(listed),
                "Prebuild ledger omits a current C++/GN/args/SDK input")
        inputs["compileInputs"] = record(compile_path, compile_path.relative_to(ROOT).as_posix())
    return inputs


def outputs(version):
    gate = ROOT / ("out/x265-local-validation-" + version)
    package = ROOT / ("out/x265-validation-" + version)
    archive = package.with_name(package.name + ".zip")
    checksum = archive.with_name(archive.name + ".sha256")
    return gate, package, archive, checksum


def capture(version):
    gate, package, archive, checksum = outputs(version)
    common.check_outputs_absent(package, archive, checksum)
    snapshot = gate / "build-inputs.json"
    require(not snapshot.exists() and not (gate / "result.json").exists(),
            "Final capture/result already exists; this version is immutable")
    inputs = current_inputs(version)
    inputs["capturedAtUtc"] = utc_now()
    inputs["captureBoundary"] = (
        "Compilation inputs frozen before build and rechecked; final metadata frozen here"
        if "compileInputs" in inputs else "Prebuild inputs frozen here; require unchanged through freeze")
    gate.mkdir(parents=True, exist_ok=True)
    write_json(snapshot, inputs)
    print("Captured:", snapshot)
    print("buildInputSnapshotSha256=" + sha256(snapshot))


def prepare_local_runner(version):
    gate, package, archive, checksum = outputs(version)
    common.check_outputs_absent(package, archive, checksum)
    stage = gate / "local-runner"
    require(not stage.exists(), "Refusing to overwrite local runner stage")
    inputs = current_inputs(version)
    require("compileInputs" in inputs, "Local gate preparation requires the verified prebuild compilation ledger")
    inputs["capturedAtUtc"] = utc_now()
    inputs["captureBoundary"] = "Local build gate input snapshot; final metadata capture is a separate immutable boundary"
    gate.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="x265-local-runner-", dir=gate) as temporary:
        temporary_stage = Path(temporary) / "local-runner"
        temporary_stage.mkdir()
        for name in BINARIES:
            common.copy_checked(BUILD / name, temporary_stage / name, record(BUILD / name, name))
        common.copy_checked(EXTERNAL / RUNNER, temporary_stage / RUNNER, record(EXTERNAL / RUNNER, RUNNER))
        ffmpeg = inputs["dependencies"]["referenceFfmpeg"]
        common.copy_checked(Path(ffmpeg["path"]), temporary_stage / ffmpeg["file"]["file"], ffmpeg["file"])
        write_json(temporary_stage / "build-inputs.json", inputs)
        manifest = {
            "schemaVersion": 2, "packageVersion": version, "scope": SCOPE,
            "packageStage": "LOCAL_BUILD_GATE", "createdAtUtc": utc_now(),
            "buildInputSnapshotSha256": sha256(temporary_stage / "build-inputs.json"),
            "files": sorted([record(path, path.relative_to(temporary_stage).as_posix())
                             for path in temporary_stage.rglob("*") if path.is_file()],
                            key=lambda item: item["file"])}
        write_json(temporary_stage / "package-manifest.json", manifest)
        require(current_inputs(version) == {key: value for key, value in inputs.items()
                                           if key not in ("capturedAtUtc", "captureBoundary")},
                "Inputs changed while preparing the local gate")
        temporary_stage.rename(stage)
    print("Prepared:", stage)
    print("Run once:", stage / RUNNER)


def freeze(version):
    gate, package, archive, checksum = outputs(version)
    common.check_outputs_absent(package, archive, checksum)
    snapshot_path = gate / "build-inputs.json"
    snapshot = read_json(snapshot_path)
    current = current_inputs(version)
    saved = {key: value for key, value in snapshot.items() if key not in ("capturedAtUtc", "captureBoundary")}
    require(current == saved, "Inputs changed after final capture")
    require(any(item["file"] == "licenses/Current-WebRTC-THIRD-PARTY-NOTICES.md"
                for item in snapshot["licenses"]), "Missing current product GN license notice")
    result_path = gate / "result.json"
    result = read_json(result_path)
    require(result.get("status") == "PASS" and result.get("softwareEncoderStatus") == "PASS" and
            result.get("hardwareStatus") == "NOT_APPLICABLE", "Gate must pass real x265 software encoding; hardware is not applicable")
    require(result.get("buildInputSnapshotSha256", "").upper() == sha256(snapshot_path),
            "Result is not bound to the final input snapshot")
    require(result_path.stat().st_mtime_ns >= snapshot_path.stat().st_mtime_ns,
            "Gate result predates final capture")
    freshness_ns = snapshot_path.stat().st_mtime_ns
    if "compileInputs" in snapshot:
        require(result.get("compileInputsSha256", "").upper() == snapshot["compileInputs"]["sha256"],
                "Result lacks matching compilation ledger hash")
        freshness_ns = (ROOT / snapshot["compileInputs"]["file"]).stat().st_mtime_ns
    bound = {item["file"]: item["sha256"].upper() for item in result["binaries"]}
    require(all(name in bound for name in BINARIES), "Result lacks all three executable hashes")
    binary_records = [record(BUILD / name, name) for name in BINARIES]
    for item in binary_records:
        require(item["sha256"] == bound[item["file"]], "Gate executable changed: " + item["file"])
    ffmpeg = snapshot["dependencies"]["referenceFfmpeg"]
    require(result.get("referenceFfmpegSha256", "").upper() == ffmpeg["file"]["sha256"],
            "Gate must bind the actual independent FFmpeg executable")
    runner_result_path = gate / result["runnerResultFile"]
    require(runner_result_path.resolve().is_relative_to(gate.resolve()), "Runner result leaves the local gate")
    runner_result = read_json(runner_result_path)
    require(runner_result.get("overallStatus") == "X265_PASS" and
            runner_result.get("softwareEncoderStatus") == "PASS" and
            runner_result.get("hardwareStatus") == "NOT_APPLICABLE" and
            runner_result.get("evidenceBindingErrors") == [], "Real hash-bound x265 runner gate did not pass")
    require(result.get("runnerResultSha256", "").upper() == sha256(runner_result_path),
            "Result does not bind the actual x265 runner report")
    for key, name in (("unitTests", BINARIES[0]), ("h264Regression", BINARIES[1]), ("h265Smoke", BINARIES[2])):
        require(runner_result[key]["status"] == "PASS" and runner_result[key]["sha256"].upper() == bound[name],
                "Runner report does not bind a passing executable: " + name)
    require(runner_result["referenceDecoder"]["status"] == "PASS" and
            runner_result["referenceDecoder"]["version"]["sha256"].upper() == ffmpeg["file"]["sha256"],
            "Runner did not pass the bound independent decoder gate")
    require(runner_result["runnerSha256"].upper() == sha256(EXTERNAL / RUNNER),
            "Runner source changed after the actual gate")
    evidence = [(result_path, record(result_path, "local-verification/result.json")),
                (snapshot_path, record(snapshot_path, "build-inputs.json")),
                (EXTERNAL / "README.md", record(EXTERNAL / "README.md", "README.md")),
                (EXTERNAL / RUNNER, record(EXTERNAL / RUNNER, RUNNER)),
                (X265_METADATA, record(X265_METADATA, "x265-dependency-metadata.json"))]
    for path in sorted(gate.rglob("*")):
        if (not path.is_file() or path in (result_path, snapshot_path) or
                "local-runner" in path.relative_to(gate).parts or
                path.suffix.lower() not in (".json", ".log", ".txt")):
            continue
        require(path.stat().st_size <= MAX_EVIDENCE_BYTES, "Evidence exceeds 8 MiB: " + str(path))
        require(path.stat().st_mtime_ns >= freshness_ns, "Evidence predates the compilation boundary: " + str(path))
        evidence.append((path, record(path, "local-verification/" + path.relative_to(gate).as_posix())))
    with tempfile.TemporaryDirectory(prefix="x265-" + version + "-freeze-", dir=ROOT / "out") as temporary:
        stage = Path(temporary) / package.name
        stage.mkdir()
        for item in binary_records:
            common.copy_checked(BUILD / item["file"], stage / item["file"], item)
        common.copy_checked(Path(ffmpeg["path"]), stage / ffmpeg["file"]["file"], ffmpeg["file"])
        for source, item in evidence:
            common.copy_checked(source, stage / item["file"], item)
        for item in snapshot["licenses"]:
            common.copy_license(item, stage)
        manifest = {
            "schemaVersion": 2, "packageVersion": version, "createdAtUtc": utc_now(),
            "packageStage": "FROZEN_PACKAGE", "scope": SCOPE,
            "repositoryHead": snapshot["repositoryHead"], "sourceInputs": snapshot["files"],
            "gnArgsSha256": snapshot["gnArgsSha256"],
            "buildInputSnapshotSha256": sha256(snapshot_path), "dependencies": snapshot["dependencies"],
            "localVerification": result, "localVerificationEvidenceSha256": sha256(result_path),
            "softwareEncoderAcceptance": {"status": "PASS", "scope": "Real x265 encoder/adapter/RTP plus fresh independent HEVC decoding; no replay fixture"},
            "hardwareAcceptance": {"status": "NOT_APPLICABLE"},
            "networkAndRtcpPli": "NOT_RUN", "performanceAndStability": "NOT_RUN",
            "historyBoundary": "NVENC/QSV hardware results remain bound to their own immutable packages; none are reused as x265 acceptance",
            "runtimeDeployment": "Three EXEs with static /MT x265/dispatcher, builtin H264 decoder and independent reference FFmpeg HEVC CLI; no SDK lib or driver DLL",
            "files": sorted([record(path, path.relative_to(stage).as_posix()) for path in stage.rglob("*") if path.is_file()],
                            key=lambda item: item["file"])}
        write_json(stage / "package-manifest.json", manifest)
        require(current_inputs(version) == current, "Inputs changed during packaging")
        for source, item in evidence:
            require(record(source, item["file"]) == item, "Evidence changed during packaging")
        for item in binary_records:
            require(record(BUILD / item["file"], item["file"]) == item, "Executable changed during packaging")
        common.check_outputs_absent(package, archive, checksum)
        temporary_zip = Path(temporary) / archive.name
        with zipfile.ZipFile(temporary_zip, "x", compression=zipfile.ZIP_DEFLATED) as bundle:
            for path in sorted(stage.rglob("*")):
                if path.is_file():
                    bundle.write(path, package.name + "/" + path.relative_to(stage).as_posix())
        common.verify_zip(temporary_zip, stage, package.name)
        archive_hash = sha256(temporary_zip)
        temporary_sha = Path(temporary) / checksum.name
        with temporary_sha.open("x", encoding="ascii", newline="\n") as stream:
            stream.write(archive_hash + "  " + archive.name + "\n")
        stage.rename(package)
        temporary_zip.rename(archive)
        temporary_sha.rename(checksum)
    print("Frozen:", package)
    print("ZIP:", archive)
    print("SHA256=" + archive_hash)
    print("Real x265 local media PASS; hardware NOT_APPLICABLE; network/stability NOT_RUN")


def main():
    global FFMPEG, X265_METADATA
    parser = argparse.ArgumentParser(description=__doc__)
    operations = parser.add_mutually_exclusive_group()
    operations.add_argument("--capture", action="store_true")
    operations.add_argument("--prepare-local-runner", action="store_true")
    parser.add_argument("--package-version", default=VERSION, choices=(VERSION,))
    parser.add_argument("--ffmpeg-path", type=Path, required=True)
    parser.add_argument("--x265-metadata", type=Path, required=True)
    args = parser.parse_args()
    FFMPEG = args.ffmpeg_path.resolve()
    X265_METADATA = args.x265_metadata.resolve()
    if args.capture:
        capture(args.package_version)
    elif args.prepare_local_runner:
        prepare_local_runner(args.package_version)
    else:
        freeze(args.package_version)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        print("ERROR:", error, file=sys.stderr)
        sys.exit(1)
