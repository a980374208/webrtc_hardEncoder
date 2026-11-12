#!/usr/bin/env python3
"""Capture prebuild inputs (or recheck a prebuild ledger), then freeze QSV v3.

Task-specific dependency metadata comes from the immutable 20261005-v2 package;
all dependency bytes, source Git state and COFF CRT directives are rechecked.
This tool never builds, tests, runs hardware, downloads or modifies SDK sources.
"""

import argparse
import copy
import hashlib
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from datetime import datetime, timezone
from pathlib import Path


VERSION = "20261005-v3"
EXTERNAL = Path(__file__).resolve().parent
ROOT = EXTERNAL.parents[3]
BUILD = ROOT / "out/ReleasePrefixed"
V2 = ROOT / "out/qsv-h264-validation-20261005-v2"
BINARIES = ("backend_video_encoder_unittests.exe", "qsv_h264_smoke.exe")
SOURCE_SUFFIXES = {".cc", ".h", ".ps1", ".gn", ".gni", ".py"}
MAX_EVIDENCE_BYTES = 8 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def utc_now():
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def record(path, name):
    require(path.is_file() and not path.is_symlink(), "Missing/linked input: " + str(path))
    return {"file": name, "bytes": path.stat().st_size, "sha256": sha256(path)}


def read_json(path):
    with path.open(encoding="utf-8-sig") as stream:
        return json.load(stream)


def write_json(path, value):
    # Exclusive creation protects prior captures and frozen evidence.
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=False)
        stream.write("\n")


def git(directory, *arguments):
    result = subprocess.run(
        ["git", "-C", str(directory), *arguments], check=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8")
    return result.stdout.rstrip("\r\n")


def source_records():
    paths = {ROOT / name for name in (
        "BUILD.gn", "webrtc.gni", "media/BUILD.gn",
        "media/engine/internal_encoder_factory.cc",
        "tools_webrtc/libs/generate_licenses.py", "LICENSE", "PATENTS")}
    paths.update(path for path in EXTERNAL.rglob("*") if path.is_file() and (
        path.suffix.lower() in SOURCE_SUFFIXES or
        path.name.lower().startswith(("readme", "license", "copying", "notice", "patents"))))
    return [record(path, path.relative_to(ROOT).as_posix())
            for path in sorted(paths, key=lambda item: item.relative_to(ROOT).as_posix())]


def coff_crt(library):
    """Read actual .drectve sections in a Microsoft COFF archive, without dumpbin."""
    data = library.read_bytes()
    require(data.startswith(b"!<arch>\n"), "oneVPL is not a COFF static archive")
    offset = 8
    directives = []
    while offset < len(data):
        header = data[offset:offset + 60]
        require(len(header) == 60 and header[58:60] == b"`\n", "Invalid COFF archive header")
        size = int(header[48:58].strip())
        member = data[offset + 60:offset + 60 + size]
        require(len(member) == size, "Truncated COFF archive member")
        name = header[:16].strip()
        if name not in (b"/", b"//", b"/SYM64/") and len(member) >= 20:
            machine, section_count = struct.unpack_from("<HH", member)
            # Skip COFF import objects; normal x64 objects have machine 0x8664.
            if machine == 0x8664:
                optional_size = struct.unpack_from("<H", member, 16)[0]
                table = 20 + optional_size
                require(table + 40 * section_count <= len(member), "Invalid COFF section table")
                for index in range(section_count):
                    section = table + index * 40
                    if member[section:section + 8].rstrip(b"\0") == b".drectve":
                        length, pointer = struct.unpack_from("<II", member, section + 16)
                        require(pointer + length <= len(member), "Invalid .drectve extent")
                        directives.append(member[pointer:pointer + length])
        offset += 60 + size + size % 2
    require(offset == len(data) and directives, "No actual COFF .drectve sections")
    text = b"\n".join(directives).decode("ascii")
    runtimes = sorted(set(re.findall(
        r'(?:/|-)FAILIFMISMATCH:"?RuntimeLibrary=([^"\s]+)', text, re.I)))
    defaults = sorted(set(value.upper() for value in re.findall(
        r'(?:/|-)DEFAULTLIB:"?([^"\s]+)', text, re.I)))
    require(runtimes == ["MT_StaticRelease"], "oneVPL RuntimeLibrary is not /MT Release")
    require("LIBCMT" in defaults or "LIBCMT.LIB" in defaults, "oneVPL lacks LIBCMT")
    require(not {"MSVCRT", "MSVCRTD", "LIBCMTD", "MSVCRT.LIB", "MSVCRTD.LIB", "LIBCMTD.LIB"}
            .intersection(defaults), "oneVPL has conflicting CRT default libraries")
    return {"method": "Python stdlib COFF archive .drectve parser",
            "sectionCount": len(directives), "runtimeLibraries": runtimes,
            "defaultLibraries": defaults,
            "directivesSha256": hashlib.sha256(b"\n".join(directives)).hexdigest().upper()}


def gn_args():
    args_file = BUILD / "args.gn"
    values = {}
    for line in args_file.read_text(encoding="utf-8-sig").splitlines():
        match = re.fullmatch(r'\s*(\w+)\s*=\s*("[^"\r\n]*"|true|false)\s*(?:#.*)?', line)
        if match:
            values[match[1]] = match[2].strip('"')
    for flag in ("rtc_use_qsv", "rtc_qsv_build_tests", "rtc_use_h264"):
        require(values.get(flag) == "true", "Required GN flag: " + flag)
    require(values.get("is_debug") == "false" and values.get("target_cpu") == "x64",
            "Package requires the existing x64 Release build")
    return record(args_file, "out/ReleasePrefixed/args.gn"), values


def dependencies(values):
    previous_path = V2 / "package-manifest.json"
    previous = read_json(previous_path)
    require(previous.get("packageVersion") == "20261005-v2", "Wrong dependency metadata version")
    metadata = copy.deepcopy(previous["dependencies"])
    vpl = metadata["oneVpl"]
    require(vpl.get("sourceGitClean") is True and vpl.get("sourceGitStatus") == [],
            "Reference oneVPL source was not clean")
    source = Path(vpl["sourceDirectory"])
    require(git(source, "rev-parse", "HEAD") == vpl["commit"], "oneVPL commit changed")
    require(git(source, "status", "--porcelain", "--untracked-files=all") == "", "oneVPL source is dirty")
    for item in metadata["files"]:
        actual = record(Path(item["path"]), item["file"])
        require(actual["bytes"] == item["bytes"] and actual["sha256"] == item["sha256"].upper(),
                "Dependency changed since v2: " + item["file"])
        item["sha256"] = actual["sha256"]
    headers = {path.resolve() for path in (source / "api").rglob("*.h")}
    listed_headers = {Path(item["path"]).resolve() for item in metadata["files"]
                      if item["file"].startswith("oneVPL/api/")}
    require(headers == listed_headers, "oneVPL header inventory changed since v2")
    require(Path(values.get("rtc_qsv_include_dir", "")).resolve() == (source / "api").resolve(),
            "GN oneVPL include directory differs from checked SDK")
    library = Path(vpl["libraryPath"])
    require(Path(values.get("rtc_qsv_vpl_library", "")).resolve() == library.resolve(),
            "GN oneVPL library differs from checked SDK")
    if values.get("rtc_use_nvenc") == "true":
        nvenc = next(item for item in metadata["files"] if item["file"] == "NVENC/nvEncodeAPI.h")
        require((Path(values.get("rtc_nvenc_include_dir", "")) / "nvEncodeAPI.h").resolve()
                == Path(nvenc["path"]).resolve(), "GN NVENC header differs from checked SDK")
    require(vpl.get("crt") == "MT_StaticRelease" and vpl.get("staticDispatcher") is True,
            "Reference dispatcher metadata is not static /MT Release")
    metadata["metadataReuse"] = {
        "scope": "Task-specific metadata only; no previous binaries or validation PASS reused",
        "sourceManifest": previous_path.relative_to(ROOT).as_posix(),
        "sourceManifestSha256": sha256(previous_path),
        "rechecked": "Every listed path/bytes/SHA256, all API header paths, Git HEAD/clean and actual COFF CRT directives",
        "coffDirectives": coff_crt(library)}
    return metadata, previous


def license_records(previous):
    originals = {
        "licenses/WebRTC-LICENSE": ROOT / "LICENSE",
        "licenses/WebRTC-PATENTS": ROOT / "PATENTS",
        "licenses/GOOGLETEST-LICENSE": ROOT / "third_party/googletest/src/LICENSE",
        "licenses/oneVPL-LICENSE": Path(previous["dependencies"]["oneVpl"]["sourceDirectory"]) / "LICENSE",
        "licenses/oneVPL-THIRD-PARTY-NOTICES.txt":
            Path(previous["dependencies"]["oneVpl"]["sourceDirectory"]) / "third-party-programs.txt"}
    result = []
    for item in previous["files"]:
        if not item["file"].startswith("licenses/"):
            continue
        path = V2 / item["file"]
        actual = record(path, item["file"])
        require(actual["bytes"] == item["bytes"] and actual["sha256"] == item["sha256"].upper(),
                "Prior license copy changed: " + item["file"])
        actual["sourcePath"] = str(path)
        if item["file"] in originals:
            original = originals[item["file"]]
            origin_record = record(original, item["file"])
            require(origin_record["sha256"] == actual["sha256"], "Current license differs: " + str(original))
            actual["verifiedOriginalPath"] = str(original)
        result.append(actual)
    require(len(result) == 7, "Expected seven v2 license/notice files")
    return sorted(result, key=lambda item: item["file"])


def current_inputs(version):
    args_record, values = gn_args()
    sdk, previous = dependencies(values)
    inputs = {"schemaVersion": 2, "packageVersion": version,
            "repositoryHead": git(ROOT, "rev-parse", "HEAD"),
            "buildDirectory": "out/ReleasePrefixed", "gnArgsSha256": args_record["sha256"],
            "gnArgs": args_record, "files": source_records(), "dependencies": sdk,
            "licenses": license_records(previous)}
    compile_path = ROOT / ("out/qsv-h264-local-validation-" + version + "/compile-inputs.json")
    if compile_path.exists():
        # This task may freeze compilation inputs first while finalizing only
        # README/runner/packaging inputs. Require full compilation coverage.
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
                "Prebuild compilation snapshot omits a current C++/GN/args/SDK input")
        inputs["compileInputs"] = record(compile_path, compile_path.relative_to(ROOT).as_posix())
    return inputs


def check_outputs_absent(package, archive, checksum):
    for path in (package, archive, checksum):
        require(not path.exists(), "Refusing to overwrite frozen output: " + str(path))


def capture(version, gate, package, archive, checksum):
    check_outputs_absent(package, archive, checksum)
    snapshot = gate / "build-inputs.json"
    require(not snapshot.exists() and not (gate / "result.json").exists(),
            "Capture/result already exists; this version is immutable")
    inputs = current_inputs(version)
    inputs["capturedAtUtc"] = utc_now()
    inputs["captureBoundary"] = (
        "Compilation inputs frozen separately before build and rechecked; final README/runner/package inputs frozen here"
        if "compileInputs" in inputs else
        "Captured before build; source inventory/args/SDK/licenses must remain unchanged through freeze")
    gate.mkdir(parents=True, exist_ok=True)
    write_json(snapshot, inputs)
    print("Captured:", snapshot)
    print("buildInputSnapshotSha256=" + sha256(snapshot))


def copy_checked(source, destination, expected):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    actual = record(destination, expected["file"])
    require(actual["bytes"] == expected["bytes"] and actual["sha256"] == expected["sha256"],
            "Copy changed: " + expected["file"])


def verify_zip(archive, stage, package_name):
    expected = {package_name + "/" + path.relative_to(stage).as_posix():
                record(path, path.relative_to(stage).as_posix())
                for path in stage.rglob("*") if path.is_file()}
    with zipfile.ZipFile(archive) as bundle:
        entries = bundle.infolist()
        require(len(entries) == len(expected) and len({item.filename for item in entries}) == len(entries),
                "ZIP entry count/uniqueness differs")
        require({item.filename for item in entries} == set(expected), "ZIP inventory differs")
        for item in entries:
            require("\\" not in item.filename, "ZIP path is not portable")
            digest = hashlib.sha256()
            size = 0
            with bundle.open(item) as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
                    size += len(block)
            reference = expected[item.filename]
            require(size == item.file_size == reference["bytes"] and
                    digest.hexdigest().upper() == reference["sha256"],
                    "ZIP entry bytes/hash differs: " + item.filename)


def freeze(version, gate, package, archive, checksum):
    check_outputs_absent(package, archive, checksum)
    snapshot_path = gate / "build-inputs.json"
    snapshot = read_json(snapshot_path)
    current = current_inputs(version)
    saved = {key: value for key, value in snapshot.items()
             if key not in ("capturedAtUtc", "captureBoundary")}
    require(current == saved, "Build inputs changed after capture (inventory/hash/args/SDK/license/HEAD)")
    result_path = gate / "result.json"
    result = read_json(result_path)
    require(result.get("status") == "PASS" and result.get("hardwareStatus") == "NOT_RUN",
            "Gate must be local PASS with hardwareStatus NOT_RUN")
    require(result.get("buildInputSnapshotSha256", "").upper() == sha256(snapshot_path),
            "Gate result is not bound to this prebuild snapshot")
    require(result_path.stat().st_mtime_ns >= snapshot_path.stat().st_mtime_ns,
            "Gate result predates the input snapshot")
    freshness_ns = snapshot_path.stat().st_mtime_ns
    if "compileInputs" in snapshot:
        require(result.get("compileInputsSha256", "").upper() == snapshot["compileInputs"]["sha256"],
                "Gate lacks matching prebuild compilation snapshot hash")
        freshness_ns = (ROOT / snapshot["compileInputs"]["file"]).stat().st_mtime_ns
    bound_binaries = {item["file"]: item["sha256"].upper() for item in result["binaries"]}
    require(all(name in bound_binaries for name in BINARIES), "Gate lacks required two executable hashes")
    binary_records = [record(BUILD / name, name) for name in BINARIES]
    for item in binary_records:
        require(item["sha256"] == bound_binaries[item["file"]], "Gate binary hash differs: " + item["file"])
    evidence = [(result_path, record(result_path, "local-verification/result.json")),
                (snapshot_path, record(snapshot_path, "build-inputs.json")),
                (EXTERNAL / "README.md", record(EXTERNAL / "README.md", "README.md")),
                (EXTERNAL / "run_qsv_h264_validation.ps1",
                 record(EXTERNAL / "run_qsv_h264_validation.ps1", "run_qsv_h264_validation.ps1"))]
    for path in sorted(gate.rglob("*")):
        if not path.is_file() or path in (result_path, snapshot_path) or path.suffix.lower() not in (".json", ".log", ".txt"):
            continue
        require(path.stat().st_size <= MAX_EVIDENCE_BYTES, "Evidence exceeds 8 MiB: " + str(path))
        require(path.stat().st_mtime_ns >= freshness_ns,
                "Local evidence predates capture: " + str(path))
        evidence.append((path, record(path, "local-verification/" + path.relative_to(gate).as_posix())))
    with tempfile.TemporaryDirectory(prefix="qsv-v3-freeze-", dir=ROOT / "out") as temporary:
        stage = Path(temporary) / package.name
        stage.mkdir()
        for item in binary_records:
            copy_checked(BUILD / item["file"], stage / item["file"], item)
        for source, item in evidence:
            copy_checked(source, stage / item["file"], item)
        for item in snapshot["licenses"]:
            copy_checked(Path(item["sourcePath"]), stage / item["file"], item)
        manifest = {
            "schemaVersion": 2, "packageVersion": version, "createdAtUtc": utc_now(),
            "scope": "Intel QSV H264 Baseline local encode/RTP/builtin FFmpeg H264 decode validation",
            "repositoryHead": snapshot["repositoryHead"], "sourceInputs": snapshot["files"],
            "gnArgsSha256": snapshot["gnArgsSha256"],
            "buildInputSnapshotSha256": sha256(snapshot_path), "dependencies": snapshot["dependencies"],
            "localVerification": result, "localVerificationEvidenceSha256": sha256(result_path),
            "hardwareAcceptance": {"status": "NOT_RUN", "requires": "Fresh v3 Intel host evidence with matching package/runner/EXE hashes"},
            "historyBoundary": "v2 UHD630/API1.35 failed after initialization with 0xC0000005 and no dump; no v1/v2 PASS is reused",
            "runtimeDeployment": "Two EXEs, static /MT dispatcher and builtin FFmpeg H264 decoder; no SDK lib or HEVC CLI",
            "files": sorted([record(path, path.relative_to(stage).as_posix())
                             for path in stage.rglob("*") if path.is_file()], key=lambda item: item["file"])}
        write_json(stage / "package-manifest.json", manifest)
        # Recheck inputs and hashes immediately before publishing immutable outputs.
        require(current_inputs(version) == current, "Inputs changed during packaging")
        for source, item in evidence:
            require(record(source, item["file"]) == item, "Evidence changed during packaging")
        for item in binary_records:
            require(record(BUILD / item["file"], item["file"]) == item, "Binary changed during packaging")
        check_outputs_absent(package, archive, checksum)
        temporary_zip = Path(temporary) / archive.name
        with zipfile.ZipFile(temporary_zip, "x", compression=zipfile.ZIP_DEFLATED) as bundle:
            for path in sorted(stage.rglob("*")):
                if path.is_file():
                    bundle.write(path, package.name + "/" + path.relative_to(stage).as_posix())
        verify_zip(temporary_zip, stage, package.name)
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
    print("Local PASS; Intel hardware acceptance NOT_RUN")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true", help="Capture before build, or finish metadata capture with a verified prebuild compile-inputs.json ledger")
    parser.add_argument("--package-version", default=VERSION, choices=[VERSION])
    args = parser.parse_args()
    gate = ROOT / ("out/qsv-h264-local-validation-" + args.package_version)
    package = ROOT / ("out/qsv-h264-validation-" + args.package_version)
    archive = package.with_name(package.name + ".zip")
    checksum = archive.with_name(archive.name + ".sha256")
    if args.capture:
        capture(args.package_version, gate, package, archive, checksum)
    else:
        freeze(args.package_version, gate, package, archive, checksum)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        print("ERROR:", error, file=sys.stderr)
        sys.exit(1)
