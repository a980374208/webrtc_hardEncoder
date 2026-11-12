#!/usr/bin/env python3
"""Run real local AMF/NVENC/QSV PeerConnections with immutable evidence.

No build, download, upload, power/priority/affinity change, or fake network is
performed. A 30-second run is functional smoke; 600/1800/3600-second runs apply
the same frozen FPS and resource gates. A 3600-second run records intermediate
checkpoints in the same process pair, and produces one final duration result.
"""

import argparse
import csv
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone

import psutil


EXTERNAL = Path(__file__).resolve().parent
ROOT = EXTERNAL.parents[3]
ROLES = ("receiver", "sender")
FPS_GATE = 27.0
PRIVATE_GROWTH_LIMIT = 32 * 1024 * 1024
HANDLE_GROWTH_LIMIT = 8
RESOURCE_WINDOW_SECONDS = 30
MINIMUM_WINDOW_SAMPLES = 25
PDH_MORE_DATA = 0x800007D2
PDH_FMT_DOUBLE = 0x200
PDH_FMT_NOCAP100 = 0x8000
PDH_COUNTERS = {
    "engine": r"\GPU Engine(*)\Utilization Percentage",
    "dedicated": r"\GPU Process Memory(*)\Dedicated Usage",
    "shared": r"\GPU Process Memory(*)\Shared Usage",
}
FROZEN_GATES = {
    "average_sender_encoded_fps_minimum": FPS_GATE,
    "average_receiver_decoded_fps_minimum": FPS_GATE,
    "packets_lost_maximum": 0,
    "disconnected_events_maximum": 0,
    "software_identity_events_maximum": 0,
    "codec_errors_maximum": 0,
    "encoder_drops_maximum": 0,
    "resource_head_window_media_seconds": [30, 60],
    "resource_tail_window_seconds": RESOURCE_WINDOW_SECONDS,
    "minimum_resource_samples_per_window": MINIMUM_WINDOW_SAMPLES,
    "private_bytes_tail_minus_head_median_maximum": PRIVATE_GROWTH_LIMIT,
    "absolute_handles_tail_minus_head_median_maximum": HANDLE_GROWTH_LIMIT,
    "resource_trend_minimum_duration_seconds": 600,
    "cpu_rss_threads_gpu_thresholds": "MEASURED; no invented thresholds",
    "latency_threshold": "MEASURED; no invented threshold",
    "encoder_pending_peak": 1,
    "codec_pending_final_maximum": 0,
    "codec_pending_after_release_maximum": 0,
    "decoder_pending_peak_range": [1, 256],
    "codec_release_calls_minimum": 1,
    "codec_release_errors_maximum": 0,
    "requested_backend_and_source_match": True,
    "builtin_advertised_encoder_format_verified": True,
    "single_frame_encode_callback_histograms": "Producer count > 0 required; no latency performance threshold",
    "vendor_runtime_binding": "Pre/post frozen runtime hash and actual sender loaded-module observation required",
}


def timestamp():
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def record(path):
    path = Path(path)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return {"file": str(path), "bytes": path.stat().st_size,
            "sha256": digest.hexdigest().upper()}


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write_json(path, value):
    with Path(path).open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def verify(records):
    for item in records:
        if record(Path(item["file"])) != item:
            raise ValueError("Fingerprint changed: " + item["file"])


def distribution(values):
    values = sorted(v for v in values if v is not None and math.isfinite(v))
    if not values:
        return {"status": "UNKNOWN", "samples": 0}
    return {"status": "MEASURED", "samples": len(values),
            "mean": statistics.mean(values), "median": statistics.median(values),
            "p95": values[math.ceil(len(values) * .95) - 1],
            "minimum": values[0], "maximum": values[-1]}


class CounterUnion(C.Union):
    _fields_ = [("long_value", W.LONG), ("double_value", C.c_double),
                ("large_value", C.c_longlong), ("ansi_value", C.c_char_p),
                ("wide_value", C.c_wchar_p)]


class CounterValue(C.Structure):
    _anonymous_ = ("value",)
    _fields_ = [("status", W.DWORD), ("value", CounterUnion)]


class CounterItem(C.Structure):
    _fields_ = [("name", W.LPWSTR), ("formatted", CounterValue)]


class GpuPdh:
    """English wildcard counters; preserve unavailable values and raw PID rows."""

    def __init__(self):
        self.library = C.WinDLL("pdh.dll", use_last_error=True)
        signatures = {
            "PdhOpenQueryW": [W.LPCWSTR, C.c_size_t, C.POINTER(W.HANDLE)],
            "PdhAddEnglishCounterW": [W.HANDLE, W.LPCWSTR, C.c_size_t,
                                     C.POINTER(W.HANDLE)],
            "PdhCollectQueryData": [W.HANDLE],
            "PdhGetFormattedCounterArrayW": [W.HANDLE, W.DWORD,
                C.POINTER(W.DWORD), C.POINTER(W.DWORD), C.c_void_p],
            "PdhCloseQuery": [W.HANDLE],
        }
        for name, arguments in signatures.items():
            function = getattr(self.library, name)
            function.argtypes = arguments
            function.restype = W.LONG
        self.query = W.HANDLE()
        self.counters, self.registration = {}, {}
        code = self.library.PdhOpenQueryW(None, 0, C.byref(self.query)) & 0xFFFFFFFF
        if code:
            raise OSError("PdhOpenQueryW: " + hex(code))
        for key, path in PDH_COUNTERS.items():
            handle = W.HANDLE()
            code = self.library.PdhAddEnglishCounterW(
                self.query, path, 0, C.byref(handle)) & 0xFFFFFFFF
            self.registration[key] = {"path": path, "return_code": hex(code)}
            if code == 0:
                self.counters[key] = handle
        self.library.PdhCollectQueryData(self.query)  # Ratio-counter baseline.

    def close(self):
        if self.query:
            self.library.PdhCloseQuery(self.query)
            self.query = W.HANDLE()

    def array(self, counter):
        size, count = W.DWORD(), W.DWORD()
        flags = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100
        code = self.library.PdhGetFormattedCounterArrayW(
            counter, flags, C.byref(size), C.byref(count), None) & 0xFFFFFFFF
        if code != PDH_MORE_DATA or not size.value:
            return {"status": "UNKNOWN", "return_code": hex(code)}, []
        for _ in range(3):
            buffer = C.create_string_buffer(size.value)
            code = self.library.PdhGetFormattedCounterArrayW(
                counter, flags, C.byref(size), C.byref(count), buffer) & 0xFFFFFFFF
            if code == PDH_MORE_DATA:
                continue
            if code:
                return {"status": "UNKNOWN", "return_code": hex(code)}, []
            items = C.cast(buffer, C.POINTER(CounterItem))
            rows = []
            for index in range(count.value):
                item = items[index]
                value = item.formatted.double_value
                if item.formatted.status in (0, 1) and math.isfinite(value) and value >= 0:
                    rows.append({"instance": item.name, "value": value,
                                 "counter_status": item.formatted.status})
            return {"status": "AVAILABLE", "valid_instances": len(rows)}, rows
        return {"status": "UNKNOWN", "reason": "wildcard buffer kept changing"}, []

    def sample(self, pids):
        code = self.library.PdhCollectQueryData(self.query) & 0xFFFFFFFF
        metadata, arrays = {}, {}
        for key in PDH_COUNTERS:
            if code:
                metadata[key], arrays[key] = {"status": "UNKNOWN",
                                              "return_code": hex(code)}, []
            elif key not in self.counters:
                metadata[key], arrays[key] = {"status": "UNKNOWN",
                    "reason": "counter registration failed"}, []
            else:
                metadata[key], arrays[key] = self.array(self.counters[key])
        selected = {}
        for pid in pids:
            by_category = {key: [row for row in rows
                if re.match(r"^pid_" + str(pid) + r"_", row["instance"])]
                for key, rows in arrays.items()}
            encode, codec = [], []
            for row in by_category["engine"]:
                kind = row["instance"].split("_engtype_", 1)[-1]
                normalized = re.sub(r"\s+", "", kind).casefold()
                if normalized.startswith("videoencode"):
                    encode.append(row)
                elif normalized.startswith("videocodec"):
                    codec.append(row)
            selected[pid] = {
                "gpu_encode_status": "MEASURED" if encode else "UNKNOWN",
                "gpu_encode_engine_peak_percent": max(
                    (row["value"] for row in encode), default=None),
                "gpu_video_codec_status": "MEASURED" if codec else "UNKNOWN",
                "gpu_video_codec_engine_peak_percent": max(
                    (row["value"] for row in codec), default=None),
                "gpu_dedicated_bytes": sum(row["value"] for row in
                    by_category["dedicated"]) if by_category["dedicated"] else None,
                "gpu_shared_bytes": sum(row["value"] for row in
                    by_category["shared"]) if by_category["shared"] else None,
                "counter_metadata": metadata,
                "pid_instances": by_category,
                "scope": "peak per video engine; combined Video Codec is not an "
                         "independent encode counter; GPU memory summed over "
                         "this PID's adapter instances; absent PID is UNKNOWN",
            }
        return selected


class CompleteLines:
    def __init__(self, path):
        self.path, self.position, self.remainder = Path(path), 0, b""

    def read(self):
        if not self.path.is_file():
            return []
        with self.path.open("rb") as stream:
            stream.seek(self.position)
            data = stream.read()
            self.position = stream.tell()
        parts = (self.remainder + data).split(b"\n")
        self.remainder = parts.pop()
        return [line.decode("utf-8").rstrip("\r") for line in parts if line]


class MediaEvidence:
    def __init__(self, directory, role):
        self.role = role
        self.counters = CompleteLines(directory / (role + "-counters.csv"))
        self.stats = CompleteLines(directory / (role + "-stats.jsonl"))
        self.header, self.rows, self.intervals, self.fps_intervals = None, [], [], []
        self.previous_outbound = {}
        self.latest_total_ms, self.latest_frames = None, None
        self.stats_count = 0

    def read(self):
        for line in self.counters.read():
            fields = next(csv.reader([line]))
            if self.header is None:
                self.header = fields
                continue
            if len(fields) != len(self.header):
                raise ValueError(self.role + " incomplete counter row")
            row = {key: float(value) for key, value in zip(self.header, fields)}
            if not all(math.isfinite(value) for value in row.values()):
                raise ValueError(self.role + " nonfinite counter value")
            if self.rows and row["elapsed_seconds"] <= self.rows[-1]["elapsed_seconds"]:
                raise ValueError(self.role + " counter times did not advance")
            if self.rows:
                previous = self.rows[-1]
                duration = row["elapsed_seconds"] - previous["elapsed_seconds"]
                field = "encoded_frames" if self.role == "sender" else "decoded_frames"
                frames = row[field] - previous[field]
                if frames < 0:
                    raise ValueError(self.role + " actual frame counters regressed")
                self.fps_intervals.append({"elapsed_seconds": row["elapsed_seconds"],
                    "interval_seconds": duration, "completed_frames": frames,
                    "actual_fps": frames / duration})
            self.rows.append(row)
        for line in self.stats.read():
            document = json.loads(line)
            self.stats_count += 1
            report = document["report"]
            objects = report if isinstance(report, list) else list(report.values())
            total_ms, frames, identified = 0.0, 0, False
            for obj in objects:
                if (obj.get("type") != "outbound-rtp" or
                    obj.get("kind", obj.get("mediaType")) != "video" or
                    "framesEncoded" not in obj or "totalEncodeTime" not in obj):
                    continue
                current_frames = int(obj["framesEncoded"])
                current_ms = float(obj["totalEncodeTime"]) * 1000
                if current_frames < 0 or not math.isfinite(current_ms) or current_ms < 0:
                    raise ValueError("Invalid RTCStats encoding counters")
                identified = True
                total_ms += current_ms
                frames += current_frames
                previous = self.previous_outbound.get(obj["id"])
                if previous is not None:
                    delta_frames = current_frames - previous[0]
                    delta_ms = current_ms - previous[1]
                    if delta_frames < 0 or delta_ms < -1e-6:
                        raise ValueError("RTCStats encoding counters regressed")
                    if delta_frames > 0:
                        self.intervals.append({
                            "elapsed_seconds": document.get("elapsed_seconds"),
                            "final_before_close": document.get("final_before_close", False),
                            "outbound_id": obj["id"], "frames": delta_frames,
                            "total_encode_ms": max(0.0, delta_ms),
                            "interval_mean_encode_ms": max(0.0, delta_ms) / delta_frames,
                        })
                self.previous_outbound[obj["id"]] = (current_frames, current_ms)
            if identified:
                self.latest_total_ms, self.latest_frames = total_ms, frames

    def summary(self, producer=None):
        histogram_fields = ("encode_latency", "key_encode_latency", "delta_encode_latency",
                            "encode_to_callback_latency", "encoded_callback_latency")
        histograms = {key: producer[key] for key in histogram_fields
                      if producer is not None and key in producer}
        pending = {key: value for key, value in (producer or {}).items()
                   if key.startswith(("encoder_pending", "decoder_pending",
                                      "encoder_release", "decoder_release"))}
        return {"counter_rows": len(self.rows), "stats_reports": self.stats_count,
                "last_counter": self.rows[-1] if self.rows else None,
                "real_time_fps": distribution([row["actual_fps"] for row in self.fps_intervals]),
                "fps_scope": "Actual encoder/decoder callbacks divided by producer "
                             "counter interval seconds; startup and teardown remain visible",
                "encode_latency_scope": "RTCStats totalEncodeTime; median/p95 "
                    "are distributions of interval mean Encode time, not "
                    "individual-frame percentiles or queue/end-to-end latency",
                "interval_mean_encode_ms": distribution([
                    row["interval_mean_encode_ms"] for row in self.intervals]),
                "cumulative_mean_encode_ms": self.latest_total_ms / self.latest_frames
                    if self.latest_frames else None,
                "queue_latency": "NOT_MEASURED",
                "frame_encode_p95": histograms.get("encode_latency", {}).get("p95_ms")
                    if histograms.get("encode_latency", {}).get("count", 0) else "NOT_MEASURED",
                "frame_encode_p95_ms": histograms.get("encode_latency", {}).get("p95_ms"),
                "single_frame_latency": {
                    "status": "MEASURED" if histograms.get("encode_latency", {}).get("count", 0) else "UNKNOWN",
                    "source": self.role + "-result.json" if producer is not None else None,
                    "scope": "Unmodified producer fixed 0.1ms histograms; overflow quantiles remain null. "
                             "Synchronous Encode/callback measurements, not queue or end-to-end latency.",
                    "histograms": histograms},
                "codec_pending_and_release": pending,
                "upstream_drop_accounting": "Only producer-reported encoder_drops and "
                    "skipped_capture_slots; source/Encode count differences are not "
                    "invented queue/drop measurements"}


def gn_path(text, key):
    match = re.search(r"^\s*" + re.escape(key) + r'\s*=\s*"([^"\r\n]+)"\s*$', text, re.M)
    if match is None:
        raise ValueError("GN args omit " + key)
    value = match.group(1)
    return (ROOT / value[2:] if value.startswith("//") else ROOT / value).resolve()


def vendor_binding(args, text, paths):
    """Bind only the requested vendor to its compiled SDK and host runtime."""
    backend = args.backend.lower()
    if not re.search(r"^\s*rtc_use_" + backend + r"\s*=\s*true\s*$", text, re.M):
        raise ValueError(args.backend + " build required")
    sdk = gn_path(text, "rtc_" + backend + "_include_dir")
    system32 = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32"
    if args.backend == "AMF":
        sdk_files = {sdk / "AMF/core/Version.h", sdk / "AMF/core/Factory.h",
                     sdk / ("AMF/components/VideoEncoderHEVC.h" if args.codec == "H265"
                            else "AMF/components/VideoEncoderVCE.h")}
        runtime = system32 / "amfrt64.dll"
        selection = "System32 AMF runtime shim; actual sender module observation required"
    elif args.backend == "NVENC":
        sdk_files = {sdk / "nvEncodeAPI.h"}
        runtime = system32 / "nvEncodeAPI64.dll"
        selection = "System32-only NVENC API load; actual sender module observation required"
    else:
        sdk_files = {sdk / "vpl" / name for name in
                     ("mfxvideo.h", "mfxdispatcher.h", "mfxdefs.h", "mfximplcaps.h")}
        sdk_files.add(gn_path(text, "rtc_qsv_vpl_library"))
        if args.qsv_runtime_dll is None or not args.qsv_runtime_dll.is_absolute():
            raise ValueError("QSV requires --qsv-runtime-dll with the absolute dispatcher-selected hardware runtime path")
        runtime = args.qsv_runtime_dll.resolve()
        selection = ("Operator declares the dispatcher-selected hardware runtime; its frozen "
                     "path must be observed in the sender. Module presence alone does not identify "
                     "MFX API version or selected adapter; retain QSV capability diagnostics.")
    if not sdk_files.issubset(paths):
        missing = sorted(str(path) for path in sdk_files - paths)
        raise ValueError("Build ledger omits requested " + args.backend + " SDK files: " + ", ".join(missing))
    if not runtime.is_file():
        raise ValueError("Requested " + args.backend + " runtime DLL is missing: " + str(runtime))
    return {"requested_backend": args.backend, "sdk_files": sorted(map(str, sdk_files)),
            "runtime_path": str(runtime.resolve()), "runtime_selection": selection}


def input_snapshot(args, executable):
    result_path, inputs_path = args.build_evidence / "result.json", args.build_evidence / "inputs.json"
    build_result = read_json(result_path)
    if build_result.get("status") != "PASS":
        raise ValueError("Build evidence is not PASS")
    build_inputs = read_json(inputs_path)
    if build_result.get("input_snapshot_sha256") != record(inputs_path)["sha256"]:
        raise ValueError("Build input ledger does not match build result")
    verify(build_inputs["files"])
    identity = record(executable)
    binaries = build_result.get("executables", [build_result.get("executable")])
    if identity not in binaries:
        raise ValueError("Executable does not match build evidence")
    paths = {Path(item["file"]) for item in build_inputs["files"]}
    expected = {EXTERNAL / name for name in (
        "amf_peer_connection_smoke.cc", "backend_video_encoder.cc",
        "backend_video_encoder.h", "video_encoder_backend.h", "BUILD.gn")}
    expected.update(EXTERNAL / (args.backend.lower() + suffix) for suffix in (
        "_video_encoder_backend.cc", "_video_encoder_backend.h",
        "_video_encoder.cc", "_video_encoder.h"))
    expected.add(ROOT / "media/engine/internal_encoder_factory.cc")
    if args.codec == "H265":
        expected.update(EXTERNAL / name for name in (
            "ffmpeg_h265_test_decoder.cc", "ffmpeg_h265_test_decoder.h"))
    if not expected.issubset(paths):
        raise ValueError("Build ledger omits current PC/" + args.backend + "/reference-decoder source")
    if args.codec == "H265" and not all(any(path.name == name and
        path.parent.name == directory for path in paths) for directory, name in (
            ("libavcodec", "avcodec.h"), ("libavcodec", "version.h"),
            ("libavutil", "frame.h"), ("libavutil", "version.h"))):
        raise ValueError("Build ledger omits the external FFmpeg decoder SDK headers")
    arguments = executable.parent / "args.gn"
    if arguments not in paths:
        raise ValueError("Build ledger omits the executable's GN args")
    text = arguments.read_text(encoding="utf-8")
    if not re.search(r"^\s*is_debug\s*=\s*false\s*$", text, re.M):
        raise ValueError("Optimized build required")
    binding = vendor_binding(args, text, paths)
    paths.update((executable, Path(__file__).resolve(), result_path, inputs_path,
                  args.build_evidence / "build.log", arguments, Path(binding["runtime_path"])))
    if args.codec == "H265":
        if args.ffmpeg_dll_dir is None or not args.ffmpeg_dll_dir.is_absolute():
            raise ValueError("HEVC requires an explicit absolute --ffmpeg-dll-dir")
        paths.update(args.ffmpeg_dll_dir / name for name in (
            "avcodec-61.dll", "avutil-59.dll", "swresample-5.dll", "libx264-164.dll", "zlib.dll"))
    files = [record(path) for path in sorted(paths, key=str)]
    binding["runtime"] = next(item for item in files if Path(item["file"]) == Path(binding["runtime_path"]))
    return {"recorded_at_utc": timestamp(), "executable": identity,
            "repository_head": subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
            "files": files, "vendor_binding": binding,
            "sdk_binding": "Successful build ledger binds the requested vendor's configured SDK "
                           "headers/static dispatcher; requested runtime and HEVC reference DLLs "
                           "are additionally frozen and verified after runtime."}


def resource_summary(samples, duration):
    selected = [row for row in samples if row.get("media_elapsed_seconds") is not None
                and 0 <= row["media_elapsed_seconds"] < duration
                and row.get("process_status") == "SAMPLED"]
    output = {"samples": len(selected), "scope": "media-active child process only"}
    fields = ("cpu_machine_percent", "working_set_bytes", "private_bytes", "handles", "threads",
              "gpu_encode_engine_peak_percent", "gpu_video_codec_engine_peak_percent",
              "gpu_dedicated_bytes", "gpu_shared_bytes")
    for field in fields:
        values = [(row.get("gpu", {}) if field.startswith("gpu_") else row).get(field)
                  for row in selected]
        output[field] = distribution(values)
        output[field]["coverage_fraction"] = sum(value is not None for value in values) / len(values) if values else 0
    if duration < 600:
        output["trend"] = {"status": "NOT_RUN", "reason": "30-second smoke has no independent 30-second head/tail windows"}
        return output, []
    head = [row for row in selected if 30 <= row["media_elapsed_seconds"] < 60]
    tail = [row for row in selected if duration - 30 <= row["media_elapsed_seconds"] < duration]
    trend = {"status": "FAIL", "head_samples": len(head), "tail_samples": len(tail),
             "head_window": [30, 60], "tail_window": [duration - 30, duration]}
    errors = []
    if min(len(head), len(tail)) < MINIMUM_WINDOW_SAMPLES:
        errors.append("Insufficient per-second samples for frozen resource windows")
    else:
        for field, limit in (("private_bytes", PRIVATE_GROWTH_LIMIT),
                             ("handles", HANDLE_GROWTH_LIMIT)):
            first, last = statistics.median(row[field] for row in head), statistics.median(row[field] for row in tail)
            difference = last - first
            trend[field] = {"head_median": first, "tail_median": last,
                            "tail_minus_head": difference, "limit": limit}
            violated = abs(difference) > limit if field == "handles" else difference > limit
            if violated:
                errors.append(field + " exceeded frozen growth limit")
        # Keep RSS/thread/GPU-memory trends as measurements; no threshold invented.
        for field in ("working_set_bytes", "threads", "gpu_dedicated_bytes", "gpu_shared_bytes"):
            def values(rows):
                return [(row.get("gpu", {}) if field.startswith("gpu_") else row).get(field) for row in rows]
            first = [v for v in values(head) if v is not None]
            last = [v for v in values(tail) if v is not None]
            trend[field] = {"head_median": statistics.median(first) if first else None,
                            "tail_median": statistics.median(last) if last else None,
                            "status": "MEASURED" if first and last else "UNKNOWN"}
        trend["status"] = "FAIL" if errors else "PASS"
    output["trend"] = trend
    return output, errors


def checkpoint(duration, evidence, samples, pids, observed_seconds,
               measurement_errors, process_observations, parent_formal_mode):
    inherited_errors = list(measurement_errors)
    errors, roles = list(inherited_errors), {}
    if len(set(pids.values())) != 2:
        errors.append("Independent sender/receiver process identities were not observed")
    for role in ROLES:
        observation = process_observations.get(role, {})
        if observation.get("pid") != pids.get(role):
            errors.append(role + ": checkpoint process identity differs")
        if observation.get("alive") is not True or observation.get("exit_code") is not None:
            errors.append(role + ": process was not active at checkpoint; exit=" +
                          str(observation.get("exit_code")))
        media = evidence[role]
        resources, resource_errors = resource_summary(samples[role], duration)
        errors.extend(role + ": " + message for message in resource_errors)
        last = media.rows[-1] if media.rows else None
        fps = None
        if last is None or last["elapsed_seconds"] < duration - 2:
            errors.append(role + ": Media telemetry did not reach checkpoint")
        else:
            field = "encoded_frames" if role == "sender" else "decoded_frames"
            fps = last[field] / last["elapsed_seconds"]
            if fps < FPS_GATE:
                errors.append(role + ": FPS below frozen 27fps gate")
            for field in ("encode_errors", "encoder_drops", "packets_lost"):
                if last.get(field, 0) != 0:
                    errors.append(role + ": " + field + " was nonzero")
            if any(last.get(field) != 1 for field in ("dtls_connected", "srtp", "udp_selected")):
                errors.append(role + ": connected UDP/DTLS/SRTP stats were not present")
        roles[role] = {"pid": pids[role], "process_observation": observation,
                       "actual_fps_until_latest_rtc_sample": fps,
                       "media": media.summary(), "resources": resources}
    return {"schema_version": 1, "status": "FAIL" if errors else "PASS",
            "provisional": True, "formal_acceptance": False,
            "parent_run_formal_mode": parent_formal_mode,
            "final_gates": {"final_runtime_result": "NOT_RUN",
                            "final_pending_and_release": "NOT_RUN",
                            "input_verification_after_run": "NOT_RUN"},
            "checkpoint_media_seconds": duration,
            "observed_media_elapsed_seconds": observed_seconds,
            "scope": "Provisional measurements of the same active process pair at an actual "
                     "wall-time checkpoint, including all prior required measurement errors. "
                     "Does not replace final runtime, pending/Release or input verification gates.",
            "recorded_at_utc": timestamp(), "roles": roles,
            "inherited_measurement_errors": inherited_errors, "errors": errors}


def host_metadata():
    environment = os.environ.copy()
    system = Path(environment.get("SystemRoot", "C:/Windows"))
    environment["PSModulePath"] = str(system / "System32/WindowsPowerShell/v1.0/Modules")
    command = r"""[Console]::OutputEncoding=New-Object System.Text.UTF8Encoding
[ordered]@{cpu=@(Get-CimInstance Win32_Processor|Select-Object Name,NumberOfCores,NumberOfLogicalProcessors);gpu=@(Get-CimInstance Win32_VideoController|Select-Object Name,DriverVersion,PNPDeviceID);power_scheme=@(powercfg /getactivescheme)}|ConvertTo-Json -Depth 5"""
    result = subprocess.run([str(system / "System32/WindowsPowerShell/v1.0/powershell.exe"),
        "-NoProfile", "-Command", command], capture_output=True, timeout=30,
        encoding="utf-8", errors="replace", env=environment,
        creationflags=subprocess.CREATE_NO_WINDOW)
    return {"logical_processors": psutil.cpu_count(), "psutil_version": psutil.__version__,
            "system_metadata": json.loads(result.stdout) if result.returncode == 0 else
                {"status": "UNKNOWN", "error": result.stderr},
            "scope": "Existing power scheme, priority and affinity retained"}


def make_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-evidence", required=True, type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--exe", type=Path, default=ROOT / "out/ReleasePrefixed/amf_peer_connection_smoke.exe")
    parser.add_argument("--codec", choices=("H264", "H265"), default="H264")
    parser.add_argument("--width", type=int, choices=(1280, 1920), default=1280)
    parser.add_argument("--height", type=int, choices=(720, 1080), default=720)
    parser.add_argument("--seconds", type=int, choices=(30, 65, 600, 1800, 3600), default=30)
    parser.add_argument("--drop-diagnostics", action="store_true",
                        help="Opt-in upstream verbose drop logs; diagnostic only, not formal performance/stability")
    parser.add_argument("--ffmpeg-dll-dir", type=Path)
    parser.add_argument("--source", choices=("synthetic", "desktop"), default="synthetic")
    parser.add_argument("--backend", choices=("AMF", "NVENC", "QSV"), default="AMF")
    parser.add_argument("--qsv-runtime-dll", type=Path,
                        help="Absolute dispatcher-selected Intel hardware runtime DLL; required for QSV")
    return parser


def observe_runtime(monitor, binding):
    target = Path(binding["runtime_path"])
    for item in monitor.memory_maps(grouped=False):
        if item.path and Path(item.path).resolve() == target:
            return {"status": "OBSERVED", "process_id": monitor.pid,
                    "observed_at_utc": timestamp(), "loaded_path": item.path,
                    "runtime": binding["runtime"],
                    "scope": "Sender mapped-module path matches the predeclared, pre/post hashed runtime; "
                             "QSV dispatcher-selected API/adapter diagnostics are separate evidence"}
    return None


def producer_contract_errors(result, role, args):
    errors = []
    for field in ("packets_lost", "disconnected_events", "software_identity_events",
                  "encode_errors", "decode_errors", "encoder_drops"):
        if result.get(field, 0) != 0:
            errors.append("nonzero " + field)
    if result.get("requested_backend") != args.backend or result.get("source") != args.source:
        errors.append("requested backend or source differs from runner configuration")
    prefix = "encoder" if role == "sender" else "decoder"
    if any(result.get(prefix + field) != 0 for field in
           ("_pending_final", "_pending_after_release", "_release_errors")):
        errors.append("pending frames or Release errors remain, or required evidence is absent")
    if result.get(prefix + "_release_calls", 0) < 1:
        errors.append("no actual codec Release call was recorded")
    if role == "sender":
        if not result.get("encoder_name", "").startswith(args.backend + " " + args.codec):
            errors.append("actual encoder identity is not the requested vendor/codec")
        if result.get("encoder_pending_peak") != 1:
            errors.append("encoder synchronous pending peak differs from one")
        if result.get("builtin_encoder_requested_format_verified") is not True:
            errors.append("builtin advertised encoder capability coverage was not verified")
        if any(result.get(field, {}).get("count", 0) < 1 for field in
               ("encode_latency", "encode_to_callback_latency", "encoded_callback_latency")):
            errors.append("required single-frame Encode/callback histograms are unavailable")
    elif not 1 <= result.get("decoder_pending_peak", 0) <= 256:
        errors.append("decoder pending peak is absent or outside the bounded contract")
    return errors


def main():
    parser = make_parser()
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This runner requires Windows process counters and PDH")
    if (args.width, args.height) not in ((1280, 720), (1920, 1080)):
        parser.error("Use 1280x720 or 1920x1080")
    if args.seconds == 65 and not args.drop_diagnostics:
        parser.error("65 seconds is reserved for --drop-diagnostics")
    args.build_evidence = (ROOT / args.build_evidence).resolve()
    gate, executable = (ROOT / args.evidence).resolve(), args.exe.resolve()
    if args.ffmpeg_dll_dir:
        if not args.ffmpeg_dll_dir.is_absolute():
            parser.error("--ffmpeg-dll-dir must be absolute")
        args.ffmpeg_dll_dir = args.ffmpeg_dll_dir.resolve()
    if args.qsv_runtime_dll:
        if not args.qsv_runtime_dll.is_absolute():
            parser.error("--qsv-runtime-dll must be absolute")
        args.qsv_runtime_dll = args.qsv_runtime_dll.resolve()
    gate.mkdir(parents=True, exist_ok=False)
    report = {"schema_version": 2, "status": "FAIL", "codec": args.codec,
        "width": args.width, "height": args.height, "requested_fps": 30,
        "requested_continuous_seconds": args.seconds,
        "input_source": args.source,
        "requested_backend": args.backend,
        "formal_acceptance": not args.drop_diagnostics,
        "diagnostic_overhead": "Upstream verbose message construction enabled" if args.drop_diagnostics else "Disabled",
        "scope": "Two independent local UDP ICE/DTLS/SRTP PeerConnections with actual decode; local hardware acceptance",
        "started_at_utc": timestamp(), "gates": FROZEN_GATES,
        "commands": {}, "process_ids": {}, "checkpoints": []}
    processes, monitors, logs, previous_cpu = {}, {}, [], {}
    samples = {role: [] for role in ROLES}
    media = {role: MediaEvidence(gate, role) for role in ROLES}
    errors, gpu, snapshot = [], None, None
    gpu_errors = []
    runtime_observation, runtime_sampling_errors = None, []
    start, media_start = time.monotonic(), None
    try:
        snapshot = input_snapshot(args, executable)
        report["vendor_binding"] = snapshot["vendor_binding"]
        write_json(gate / "inputs.json", snapshot)
        write_json(gate / "host.json", host_metadata())
        write_json(gate / "frozen-gates.json", FROZEN_GATES)
        try:
            gpu = GpuPdh()
            report["pdh_registration"] = gpu.registration
        except (OSError, AttributeError) as error:
            gpu_errors.append(type(error).__name__ + ": " + str(error))
        for role in ROLES:
            command = [str(executable), "--role", role, "--codec", args.codec,
                "--outputdir", str(gate), "--seconds", str(args.seconds),
                "--width", str(args.width), "--height", str(args.height), "--fps", "30",
                "--source", args.source, "--backend", args.backend]
            if args.drop_diagnostics:
                command.extend(("--drop-diagnostics", "1"))
            if args.codec == "H265":
                command.extend(("--ffmpeg-dll-dir", str(args.ffmpeg_dll_dir)))
            report["commands"][role] = command
            stdout = (gate / (role + "-stdout.txt")).open("xb")
            stderr = (gate / (role + "-stderr.txt")).open("xb")
            logs.extend((stdout, stderr))
            child = subprocess.Popen(command, cwd=ROOT, stdout=stdout, stderr=stderr,
                creationflags=subprocess.CREATE_NO_WINDOW)
            processes[role] = child
            report["process_ids"][role] = child.pid
            monitors[role] = psutil.Process(child.pid)
            cpu = monitors[role].cpu_times()
            previous_cpu[role] = (time.monotonic(), cpu.user + cpu.system)
        start = time.monotonic()
        next_sample = start + 1
        logical = psutil.cpu_count()
        if not logical:
            raise ValueError("Logical CPU count is unknown")
        pending_checkpoints = [value for value in (600, 1800) if value < args.seconds]
        with (gate / "resources.jsonl").open("x", encoding="utf-8", newline="\n") as stream:
            while any(child.poll() is None for child in processes.values()):
                now = time.monotonic()
                if now - start > args.seconds + 180:
                    errors.append("Child process exceeded bounded runtime watchdog")
                    break
                if now < next_sample:
                    time.sleep(min(next_sample - now, .2))
                    continue
                next_sample = now + 1  # No catch-up bursts if sampling is delayed.
                marker = gate / "media-start.signal"
                if media_start is None and marker.exists():
                    media_start = now - max(0, (time.time_ns() - marker.stat().st_mtime_ns) / 1e9)
                elapsed = now - media_start if media_start is not None else None
                try:
                    gpu_samples = gpu.sample(report["process_ids"].values()) if gpu else {}
                except (OSError, ValueError) as error:
                    gpu_samples = {}
                    gpu_errors.append(type(error).__name__ + ": " + str(error))
                for role, monitor in monitors.items():
                    row = {"recorded_at_utc": timestamp(), "role": role,
                           "pid": monitor.pid, "runner_elapsed_seconds": now - start,
                           "media_elapsed_seconds": elapsed,
                           "gpu": gpu_samples.get(monitor.pid, {"status": "UNKNOWN"})}
                    try:
                        with monitor.oneshot():
                            memory, cpu = monitor.memory_info(), monitor.cpu_times()
                            cpu_seconds = cpu.user + cpu.system
                            sample_time = time.monotonic()
                            old_time, old_cpu = previous_cpu[role]
                            row.update(process_status="SAMPLED",
                                cpu_machine_percent=100 * (cpu_seconds - old_cpu) /
                                    (sample_time - old_time) / logical,
                                process_cpu_seconds=cpu_seconds,
                                logical_processors=logical,
                                working_set_bytes=memory.rss, private_bytes=memory.private,
                                handles=monitor.num_handles(), threads=monitor.num_threads())
                            previous_cpu[role] = (sample_time, cpu_seconds)
                    except (psutil.NoSuchProcess, psutil.AccessDenied) as error:
                        row.update(process_status="UNKNOWN", error=type(error).__name__)
                        if processes[role].poll() is None:
                            errors.append(role + ": required process sampling failed")
                    if role == "sender" and elapsed is not None and runtime_observation is None:
                        try:
                            runtime_observation = observe_runtime(monitor, snapshot["vendor_binding"])
                        except (OSError, ValueError, psutil.Error) as error:
                            message = type(error).__name__ + ": " + str(error)
                            if message not in runtime_sampling_errors:
                                runtime_sampling_errors.append(message)
                    samples[role].append(row)
                    stream.write(json.dumps(row, allow_nan=False) + "\n")
                    media[role].read()
                stream.flush()
                process_observations = {}
                for role, child in processes.items():
                    code = child.poll()
                    process_observations[role] = {"pid": child.pid, "alive": code is None,
                                                  "exit_code": code,
                                                  "observed_at_utc": timestamp()}
                    if code not in (None, 0):
                        message = role + ": PeerConnection process failed; exit=" + str(code)
                        if message not in errors:
                            errors.append(message)
                if elapsed is not None:
                    for duration in list(pending_checkpoints):
                        if elapsed >= duration:
                            verify(snapshot["files"])
                            value = checkpoint(duration, media, samples,
                                               report["process_ids"], elapsed, errors,
                                               process_observations, not args.drop_diagnostics)
                            name = "checkpoint-" + str(duration) + ".json"
                            write_json(gate / name, value)
                            report["checkpoints"].append({"file": name, "status": value["status"],
                                "continuous_seconds": duration, "provisional": True,
                                "formal_acceptance": False})
                            errors.extend("checkpoint " + str(duration) + ": " + item
                                          for item in value["errors"] if item not in errors)
                            pending_checkpoints.remove(duration)
                            print("Checkpoint:", duration, value["status"], flush=True)
                if any(item["exit_code"] not in (None, 0)
                       for item in process_observations.values()):
                    break
        for role, child in processes.items():
            if child.poll() is None:
                child.kill()
            code = child.wait(timeout=15)
            report[role + "_exit_code"] = code
            media[role].read()
            if code:
                errors.append(role + ": process exit " + str(code))
            path = gate / (role + "-result.json")
            if not path.is_file():
                errors.append(role + ": result evidence missing")
                continue
            result = read_json(path)
            report[role] = result
            if result.get("overall_status") != "PASS":
                errors.append(role + ": " + result.get("failure", "functional gate did not PASS"))
            if (result.get("process_id") != child.pid or result.get("codec") != args.codec or
                result.get("width") != args.width or result.get("height") != args.height or
                result.get("requested_seconds") != args.seconds):
                errors.append(role + ": process result identity or configuration differs")
            errors.extend(role + ": " + item for item in producer_contract_errors(result, role, args))
            count_field = "encoded_frames" if role == "sender" else "decoded_frames"
            fps = result.get(count_field, 0) / args.seconds
            report[role + "_fps"] = fps
            if fps < FPS_GATE:
                errors.append(role + ": average FPS below frozen 27fps gate")
            if not result.get("duration_completed") or not result.get("peer_connection_close_completed"):
                errors.append(role + ": continuous duration or teardown not completed")
            resources, resource_errors = resource_summary(samples[role], args.seconds)
            report[role + "_resources"] = resources
            errors.extend(role + ": " + item for item in resource_errors)
            report[role + "_media_metrics"] = media[role].summary(result)
            if media[role].counters.remainder or media[role].stats.remainder:
                errors.append(role + ": truncated producer telemetry line")
        if len(set(report["process_ids"].values())) != 2:
            errors.append("Independent sender/receiver process identities were not observed")
        if pending_checkpoints:
            errors.append("Required same-process intermediate checkpoints were not reached")
        if not media["sender"].intervals:
            errors.append("RTCStats totalEncodeTime interval measurements were unavailable")
        if runtime_observation is None:
            errors.append("Requested vendor runtime was not observed in the actual sender process")
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError, psutil.Error) as error:
        errors.append(type(error).__name__ + ": " + str(error))
    except KeyboardInterrupt:
        errors.append("Interrupted by user; requested duration not completed")
    finally:
        for child in processes.values():
            if child.poll() is None:
                child.kill()
                child.wait(timeout=15)
        for log in logs:
            log.close()
        if gpu:
            gpu.close()
        report["input_verification_after_run"] = "NOT_RUN"
        if snapshot is not None:
            try:
                verify(snapshot["files"])
                report["input_verification_after_run"] = "PASS"
            except (OSError, ValueError) as error:
                report["input_verification_after_run"] = "FAIL"
                errors.append("Final fingerprint verification: " + str(error))
    for role in ROLES:
        write_json(gate / (role + "-encoding-intervals.json"), {
            "scope": "Interval means from actual RTCStats; not individual frame latency",
            "intervals": media[role].intervals,
            "actual_fps_intervals": media[role].fps_intervals})
    report["errors"], report["gpu_sampling_errors"] = errors, gpu_errors
    report["runtime_observation"] = runtime_observation or {
        "status": "UNKNOWN", "reason": "No matching sender runtime module was observed"}
    report["runtime_sampling_errors"] = runtime_sampling_errors
    report["status"] = "FAIL" if errors else "PASS"
    report["measurement_status"] = "MEASURED" if not errors else "INCONCLUSIVE"
    sender_gpu = report.get("sender_resources", {}).get("gpu_encode_engine_peak_percent", {})
    report["measurement_completeness"] = ("COMPLETE_FOR_DECLARED_METRICS" if
        not errors and sender_gpu.get("coverage_fraction", 0) >= .9 else "PARTIAL")
    report["longest_continuous_verified_seconds"] = args.seconds if not errors else None
    report["gpu_encode_counter_scope"] = ("AMD may expose Video Codec instead of "
        "VideoEncode; combined engine measurements are labeled separately and "
        "independent encoding utilization remains UNKNOWN when unavailable")
    report["qualification"] = ("Frozen local functional/FPS/resource/pending/Release gates; "
        "CPU/GPU values and individual synchronous Encode/callback histograms measured without "
        "performance thresholds. Input is explicitly synthetic or actual primary GDI desktop. "
        "Queue/end-to-end latency, cross-host and production qualification are not claimed.")
    report["finished_at_utc"] = timestamp()
    report["input_snapshot_sha256"] = record(gate / "inputs.json")["sha256"] if (gate / "inputs.json").is_file() else None
    write_json(gate / "result.json", report)
    write_json(gate / "artifact-hashes.json", {"recorded_at_utc": timestamp(),
        "files": [record(path) for path in sorted(gate.iterdir()) if path.is_file()]})
    print("PC validation:", report["status"], "Evidence:", gate, flush=True)
    print(json.dumps({"sender_fps": report.get("sender_fps"),
        "receiver_fps": report.get("receiver_fps"), "errors": errors}), flush=True)
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
