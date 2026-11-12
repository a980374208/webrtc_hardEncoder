#!/usr/bin/env python3
"""Capture prebuild inputs (or recheck a prebuild ledger), then freeze QSV H.265.

Task-specific dependency metadata comes from the immutable H.264 v3 ZIP, or
the hash-pinned H.265 v1 ZIP when that original metadata archive is unavailable.
All dependency bytes, source Git state and COFF CRT directives are rechecked.
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
SUPPORTED_VERSIONS = ("20261005-v1", "20261005-v2", VERSION)
EXTERNAL = Path(__file__).resolve().parent
ROOT = EXTERNAL.parents[3]
BUILD = ROOT / "out/ReleasePrefixed"
PRIMARY_REFERENCE = ROOT / "out/qsv-h264-validation-20261005-v3.zip"
REFERENCE_IS_H265_FALLBACK = not PRIMARY_REFERENCE.is_file()
REFERENCE = (ROOT / "out/qsv-h265-validation-20261005-v1.zip"
             if REFERENCE_IS_H265_FALLBACK else PRIMARY_REFERENCE)
REFERENCE_VERSION = "20261005-v1" if REFERENCE_IS_H265_FALLBACK else "20261005-v3"
REFERENCE_PREFIX = ("qsv-h265-validation-" if REFERENCE_IS_H265_FALLBACK
                    else "qsv-h264-validation-") + REFERENCE_VERSION + "/"
FALLBACK_ARCHIVE_SHA256 = "AC3F4A52F11CFCDB6698A79DF1BB0DC6DED53E3EE5D07046350DC5D91FFC56C9"
SDK_LICENSE_FILES = {
    "licenses/GOOGLETEST-LICENSE", "licenses/NvEncodeAPI-header-LICENSE.txt",
    "licenses/WebRTC-LICENSE", "licenses/WebRTC-PATENTS",
    "licenses/WebRTC-THIRD-PARTY-NOTICES.md", "licenses/oneVPL-LICENSE",
    "licenses/oneVPL-THIRD-PARTY-NOTICES.txt"}
BINARIES = ("backend_video_encoder_unittests.exe", "qsv_h264_smoke.exe", "qsv_h265_smoke.exe")
SOURCE_SUFFIXES = {".cc", ".h", ".ps1", ".gn", ".gni", ".py"}
MAX_EVIDENCE_BYTES = 8 * 1024 * 1024
FFMPEG = None


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


def reference_member(name):
    with zipfile.ZipFile(REFERENCE) as bundle:
        member = REFERENCE_PREFIX + name
        require(bundle.namelist().count(member) == 1, "Missing/duplicate reference ZIP member: " + member)
        return bundle.read(member)


def data_record(data, name):
    return {"file": name, "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest().upper()}


def reference_manifest():
    if REFERENCE_IS_H265_FALLBACK:
        require(sha256(REFERENCE) == FALLBACK_ARCHIVE_SHA256,
                "H265 v1 dependency metadata archive differs from the frozen expected SHA256")
    data = reference_member("package-manifest.json")
    previous = json.loads(data.decode("utf-8-sig"))
    require(previous.get("packageVersion") == REFERENCE_VERSION, "Wrong dependency metadata version")
    require(previous.get("scope", "").startswith(
        "Intel QSV H265" if REFERENCE_IS_H265_FALLBACK else "Intel QSV H264"),
        "Wrong dependency metadata scope")
    return previous, data


def ffmpeg_metadata():
    require(FFMPEG is not None and FFMPEG.is_file(), "Set --ffmpeg-path or provide ffmpeg.exe on PATH")
    executable = record(FFMPEG, "reference-ffmpeg/ffmpeg.exe")
    result = subprocess.run([str(FFMPEG), "-version"], check=True, timeout=10,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8")
    version = result.stdout.splitlines()[0]
    require(version.startswith("ffmpeg version ") and "--enable-static" in result.stdout,
            "Reference FFmpeg must be the verified standalone static CLI build")
    directory = FFMPEG.parent.parent
    license_file = record(directory / "LICENSE", "licenses/Reference-FFmpeg-LICENSE")
    readme_file = record(directory / "README.txt", "licenses/Reference-FFmpeg-README.txt")
    return {"scope": "Independent software HEVC reference decoder/fixture tool; not a product linked dependency",
            "path": str(FFMPEG), "file": executable, "version": version,
            "versionOutput": result.stdout, "versionStderr": result.stderr,
            "license": "GPL v3" if "--enable-gpl" in result.stdout and "--enable-version3" in result.stdout else "See license",
            "licenseFile": license_file, "readmeFile": readme_file,
            "origin": "Existing local CLI; no download/install/SDK modification by this tool"}


def dependencies(values):
    previous, previous_data = reference_manifest()
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
                "Dependency changed since H264 v3: " + item["file"])
        item["sha256"] = actual["sha256"]
    headers = {path.resolve() for path in (source / "api").rglob("*.h")}
    listed_headers = {Path(item["path"]).resolve() for item in metadata["files"]
                      if item["file"].startswith("oneVPL/api/")}
    require(headers == listed_headers, "oneVPL header inventory changed since H264 v3")
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
    upstream_metadata = copy.deepcopy(metadata.get("metadataReuse"))
    metadata["metadataReuse"] = {
        "scope": "Task-specific metadata only; no previous binaries or validation PASS reused",
        "sourceArchive": REFERENCE.relative_to(ROOT).as_posix(),
        "sourceArchiveSha256": sha256(REFERENCE),
        "sourceManifestEntry": REFERENCE_PREFIX + "package-manifest.json",
        "sourceManifestSha256": data_record(previous_data, "manifest")["sha256"],
        "referenceSelection": "Hash-pinned H265 v1 fallback; original H264 v3 archive absent"
        if REFERENCE_IS_H265_FALLBACK else "Original H264 v3 metadata archive",
        "rechecked": "Every listed path/bytes/SHA256, all API header paths, Git HEAD/clean and actual COFF CRT directives",
        "coffDirectives": coff_crt(library)}
    if upstream_metadata:
        metadata["metadataReuse"]["upstreamMetadataProvenance"] = upstream_metadata
    if REFERENCE_IS_H265_FALLBACK:
        metadata["metadataReuse"]["expectedSourceArchiveSha256"] = FALLBACK_ARCHIVE_SHA256
    metadata["referenceFfmpeg"] = ffmpeg_metadata()
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
        if item["file"] not in SDK_LICENSE_FILES:
            continue
        actual = data_record(reference_member(item["file"]), item["file"])
        require(actual["bytes"] == item["bytes"] and actual["sha256"] == item["sha256"].upper(),
                "Prior license copy changed: " + item["file"])
        actual["sourceArchive"] = str(REFERENCE)
        actual["sourceEntry"] = REFERENCE_PREFIX + item["file"]
        if item["file"] in originals:
            original = originals[item["file"]]
            origin_record = record(original, item["file"])
            require(origin_record["sha256"] == actual["sha256"], "Current license differs: " + str(original))
            actual["verifiedOriginalPath"] = str(original)
        result.append(actual)
    require({item["file"] for item in result} == SDK_LICENSE_FILES and len(result) == 7,
            "Expected exactly seven original SDK/runtime license/notice files")
    for file_name, source_name in (("licenses/Reference-FFmpeg-LICENSE", "LICENSE"),
                                   ("licenses/Reference-FFmpeg-README.txt", "README.txt")):
        source = FFMPEG.parent.parent / source_name
        item = record(source, file_name)
        item["sourcePath"] = str(source)
        result.append(item)
    return sorted(result, key=lambda item: item["file"])


def current_inputs(version):
    args_record, values = gn_args()
    sdk, previous = dependencies(values)
    inputs = {"schemaVersion": 2, "packageVersion": version,
            "repositoryHead": git(ROOT, "rev-parse", "HEAD"),
            "buildDirectory": "out/ReleasePrefixed", "gnArgsSha256": args_record["sha256"],
            "gnArgs": args_record, "files": source_records(), "dependencies": sdk,
            "licenses": license_records(previous)}
    compile_path = ROOT / ("out/qsv-h265-local-validation-" + version + "/compile-inputs.json")
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


def copy_license(item, stage):
    destination = stage / item["file"]
    if "sourcePath" in item:
        copy_checked(Path(item["sourcePath"]), destination, item)
        return
    require(Path(item["sourceArchive"]) == REFERENCE, "Unexpected license source archive")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(REFERENCE) as bundle:
        data = bundle.read(item["sourceEntry"])
    require(data_record(data, item["file"])["sha256"] == item["sha256"], "Reference license bytes changed")
    destination.write_bytes(data)
    actual = record(destination, item["file"])
    require(actual["bytes"] == item["bytes"] and actual["sha256"] == item["sha256"],
            "License copy changed: " + item["file"])


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
    require(all(name in bound_binaries for name in BINARIES), "Gate lacks required three executable hashes")
    binary_records = [record(BUILD / name, name) for name in BINARIES]
    for item in binary_records:
        require(item["sha256"] == bound_binaries[item["file"]], "Gate binary hash differs: " + item["file"])
    require(result.get("referenceFfmpegSha256", "").upper() ==
            snapshot["dependencies"]["referenceFfmpeg"]["file"]["sha256"],
            "Gate must bind the independent reference FFmpeg executable hash")
    evidence = [(result_path, record(result_path, "local-verification/result.json")),
                (snapshot_path, record(snapshot_path, "build-inputs.json")),
                (EXTERNAL / "README.md", record(EXTERNAL / "README.md", "README.md")),
                (EXTERNAL / "run_qsv_h265_validation.ps1",
                 record(EXTERNAL / "run_qsv_h265_validation.ps1", "run_qsv_h265_validation.ps1"))]
    for path in sorted(gate.rglob("*")):
        if not path.is_file() or path in (result_path, snapshot_path) or path.suffix.lower() not in (".json", ".log", ".txt"):
            continue
        require(path.stat().st_size <= MAX_EVIDENCE_BYTES, "Evidence exceeds 8 MiB: " + str(path))
        require(path.stat().st_mtime_ns >= freshness_ns,
                "Local evidence predates capture: " + str(path))
        evidence.append((path, record(path, "local-verification/" + path.relative_to(gate).as_posix())))
    with tempfile.TemporaryDirectory(prefix="qsv-h265-" + version + "-freeze-", dir=ROOT / "out") as temporary:
        stage = Path(temporary) / package.name
        stage.mkdir()
        for item in binary_records:
            copy_checked(BUILD / item["file"], stage / item["file"], item)
        ffmpeg = snapshot["dependencies"]["referenceFfmpeg"]
        copy_checked(Path(ffmpeg["path"]), stage / ffmpeg["file"]["file"], ffmpeg["file"])
        for source, item in evidence:
            copy_checked(source, stage / item["file"], item)
        for item in snapshot["licenses"]:
            copy_license(item, stage)
        manifest = {
            "schemaVersion": 2, "packageVersion": version, "createdAtUtc": utc_now(),
            "scope": "Intel QSV H265 Main local encode/RTP/independent FFmpeg HEVC decode and current H264 regression",
            "repositoryHead": snapshot["repositoryHead"], "sourceInputs": snapshot["files"],
            "gnArgsSha256": snapshot["gnArgsSha256"],
            "buildInputSnapshotSha256": sha256(snapshot_path), "dependencies": snapshot["dependencies"],
            "localVerification": result, "localVerificationEvidenceSha256": sha256(result_path),
            "hardwareAcceptance": {"status": "NOT_RUN", "requires": "Fresh H265 " + version + " Intel host evidence with matching package/runner/three EXE hashes"},
            "historyBoundary": "QSV H265 v1 initialization FAIL and v2 Reset(-14) hardware FAIL remain bound to their frozen packages, as do each version's H264/unit PASS; QSV H264 v3 PASS remains historical; no prior hardware PASS is reused for this source/binary version",
            "runtimeDeployment": "Three EXEs, static /MT dispatcher, builtin FFmpeg H264 decoder and independent reference FFmpeg HEVC CLI; no SDK lib or driver DLL",
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
    global FFMPEG
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true", help="Capture before build, or finish metadata capture with a verified prebuild compile-inputs.json ledger")
    parser.add_argument("--package-version", default=VERSION, choices=SUPPORTED_VERSIONS,
                        help="Defaults to v3; all existing captures/packages remain immutable")
    parser.add_argument("--ffmpeg-path", type=Path,
                        help="Existing standalone static FFmpeg CLI; its installation root must contain LICENSE and README.txt")
    args = parser.parse_args()
    default_ffmpeg = shutil.which("ffmpeg.exe")
    FFMPEG = (args.ffmpeg_path or (Path(default_ffmpeg) if default_ffmpeg else Path())).resolve()
    gate = ROOT / ("out/qsv-h265-local-validation-" + args.package_version)
    package = ROOT / ("out/qsv-h265-validation-" + args.package_version)
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
