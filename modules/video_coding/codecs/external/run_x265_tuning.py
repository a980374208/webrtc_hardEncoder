#!/usr/bin/env python3
"""Compare opt-in x265 tuning, then replay one local primary-screen capture.

No builds, downloads, power settings, affinity changes, or uploads are performed.
Quality PASS is a separate fixed-sequence decoder/content gate; performance is
MEASURED without an invented real-time or release qualification threshold.
"""

import argparse
import csv
import math
import re
import statistics
import subprocess
import sys
from pathlib import Path

from run_x265_performance import (
    ROOT, host_metadata, inputs, read_json, record, timestamp, verify_inputs,
    write_json,
)


PROFILES = ("baseline", "wpp4", "wpp8", "wpp16", "wpp8-nostrict",
            "baseline-cu16", "wpp8-cu16", "wpp8-rdskip")
MINIMUM_PSNR_DB = 28.0
SOURCE_WIDTH, SOURCE_HEIGHT = 1920, 1080
FRAME_BYTES = SOURCE_WIDTH * SOURCE_HEIGHT * 3 // 2
SCOPE = ("Local x265 builtin-factory experimental tuning and immutable actual "
         "primary-desktop replay; not live capture-to-network, receiver, "
         "long stability, or release qualification")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def strict_cbr(profile):
    return profile != "wpp8-nostrict"


def discover_ffmpeg(explicit):
    if explicit:
        path = explicit.resolve()
        require(path.is_file(), "Explicit FFmpeg does not exist: " + str(path))
        return path, {"source": "explicit local --ffmpeg", "executable": record(path)}
    ledger = ROOT / "out/x265-local-validation-20261006-v1/x265-chain/result.json"
    reference = read_json(ledger)["referenceDecoder"]
    path = Path(reference["toolPath"]).resolve()
    expected = reference["version"]["sha256"]
    require(record(path)["sha256"] == expected,
            "Historical local reference FFmpeg hash changed")
    return path, {"source": str(ledger), "source_record": record(ledger),
                  "executable": record(path)}


def execute(command, directory, name, timeout):
    """Always retain process diagnostics, including spawn failure and timeout."""
    result = {"command": command, "started_at_utc": timestamp(),
              "timeout_seconds": timeout, "timed_out": False,
              "exit_code": None, "status": "FAIL"}
    stdout = stderr = b""
    try:
        process = subprocess.Popen(command, cwd=directory,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            result["timed_out"] = True
            process.kill()
            stdout, stderr = process.communicate(timeout=15)
        result["exit_code"] = process.returncode
        if process.returncode == 0 and not result["timed_out"]:
            result["status"] = "PASS"
    except (OSError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
    result["finished_at_utc"] = timestamp()
    for suffix, data in (("stdout.txt", stdout), ("stderr.txt", stderr)):
        path = directory / (name + "." + suffix)
        with path.open("xb") as stream:
            stream.write(data)
        result[suffix.split(".")[0] + "_file"] = path.name
    write_json(directory / (name + ".process.json"), result)
    return result


def nearest_rank(values, fraction):
    values = sorted(values)
    return values[math.ceil(len(values) * fraction) - 1] if values else 0.0


def check_probe(path, case):
    measurement = read_json(path)
    require(measurement.get("status") == "MEASURED", "Probe did not measure successfully")
    require(measurement.get("implementation") == "x265 H265" and
            measurement.get("hardware") is False, "Unexpected encoder identity")
    for key, expected in (("width", case["width"]), ("height", case["height"]),
                          ("target_bitrate_bps", case["bitrate"]), ("target_fps", 30),
                          ("tuning", case["tuning"]), ("mode", case["mode"]),
                          ("workload", case["workload"]),
                          ("quality_audit", case["mode"] == "sequence")):
        require(measurement.get(key) == expected, "Probe field mismatch: " + key)
    require(measurement.get("logical_processors") == case["logical_processors"],
            "Probe/host logical processor count differs")
    ring = int(measurement["ring_frames"])
    require(ring == (case["input_frames"] if case["workload"] == "desktop" else 60),
            "Input ring frame count differs")
    rows = list(csv.DictReader(path.with_suffix(".csv").open(encoding="utf-8")))
    require(len(rows) > 0 and len(rows) == measurement["completed_frames"] ==
            measurement["submitted_frames"], "CSV/callback frame counts differ")
    require(all(row["valid"] == "1" for row in rows), "Invalid CSV sample")
    slots = [int(row["slot"]) for row in rows]
    require(all(a < b for a, b in zip(slots, slots[1:])), "Slots are not strictly ordered")
    require(all(int(row["input_frame_index"]) == int(row["slot"]) % ring for row in rows),
            "Probe input frame indices differ from dataset replay")
    last_period = None
    for row in rows:
        period = int(row["slot"]) // case["key_interval"]
        if period != last_period:
            require(row["key"] == "1", "Requested periodic IDR was not reported")
        last_period = period
    if case["mode"] == "paced":
        require(measurement["scheduled_slots"] == case["seconds"] * 30 and
                len(rows) + measurement["skipped_slots"] == measurement["scheduled_slots"],
                "Paced completed/skipped slot accounting differs")
    if case["mode"] == "sequence":
        require(slots == list(range(ring)) and measurement["skipped_slots"] == 0,
                "Quality audit must submit every source frame exactly once")
    for field, selected in (("encode_call_ms", rows),
                            ("key_encode_call_ms", [r for r in rows if r["key"] == "1"]),
                            ("delta_encode_call_ms", [r for r in rows if r["key"] == "0"])):
        values = [float(row["encode_call_ms"]) for row in selected]
        require(all(math.isfinite(value) and value >= 0 for value in values),
                "Invalid raw encode latency")
        dist = measurement[field]
        expected = {"samples": len(values), "mean": statistics.mean(values) if values else 0,
                    "p50": nearest_rank(values, .50), "p95": nearest_rank(values, .95),
                    "p99": nearest_rank(values, .99), "max": max(values, default=0)}
        require(all(abs(dist[key] - value) < 1e-6 for key, value in expected.items()),
                "Raw latency distribution differs: " + field)
    require(sum(int(row["bytes"]) for row in rows) == measurement["encoded_bytes"],
            "Raw encoded bytes differ")
    require(sum(float(row["encode_call_ms"]) > 1000 / 30 for row in rows) ==
            measurement["encode_call_over_budget"], "Raw over-budget count differs")
    require(abs(measurement["process_cpu_percent"] - 100 * measurement["process_cpu_ms"] /
                measurement["wall_ms"] / measurement["logical_processors"]) < 1e-6,
            "Normalized process CPU formula differs")
    return measurement


def run_probe(executable, output, case, snapshot, dataset_snapshot):
    verify_inputs(snapshot)
    if dataset_snapshot:
        verify_inputs(dataset_snapshot)
    path = output / (case["name"] + ".json")
    command = [str(executable)]
    for key in ("width", "height", "bitrate", "fps", "seconds", "warmup_seconds",
                "mode", "workload", "tuning", "key_interval"):
        command.extend(["--" + key.replace("_", "-"), str(case[key])])
    if case["workload"] == "desktop":
        command.extend(["--input", case["input"], "--input-width", "1920",
                        "--input-height", "1080"])
    if case["mode"] == "sequence":
        command.extend(["--bitstream", str(output / (case["name"] + ".hevc"))])
    command.extend(["--output", str(path)])
    process = execute(command, output, case["name"] + "-probe",
                      (180 if case["mode"] == "sequence" else case["seconds"]) +
                      case["warmup_seconds"] + 90)
    result = {"case": case, "process": process, "status": "FAIL", "result_file": path.name}
    try:
        require(process["status"] == "PASS", "Probe process failed")
        result["measurement"] = check_probe(path, case)
        result["result_sha256"] = record(path)["sha256"]
        result["status"] = "MEASURED"
    except (ValueError, KeyError, OSError, TypeError) as error:
        result["error"] = str(error)
        if path.is_file():
            result["partial_result"] = record(path)
    write_json(output / (case["name"] + ".audit.json"), result)
    # Drift must abort the run, rather than merely disqualifying one candidate.
    verify_inputs(snapshot)
    if dataset_snapshot:
        verify_inputs(dataset_snapshot)
    return result


def decode_content(ffmpeg, directory, name, stream, reference, width, height, frames):
    result = {"name": name, "status": "FAIL", "expected_frames": frames,
              "decoded_frames": 0, "minimum_psnr_db": None, "minimum_gate_db": MINIMUM_PSNR_DB}
    decoded, framehash, stats = (directory / (name + suffix) for suffix in
                               (".decoded.i420", ".decoded.framehash", ".psnr.txt"))
    try:
        common = [str(ffmpeg), "-hide_banner", "-nostdin", "-n", "-xerror"]
        decode_command = common + ["-err_detect", "explode", "-hwaccel", "none", "-c:v", "hevc",
            "-f", "hevc", "-i", str(stream), "-map", "0:v:0", "-c:v", "rawvideo",
            "-pix_fmt", "yuv420p", "-fps_mode", "passthrough", "-f", "rawvideo", str(decoded),
            "-map", "0:v:0", "-c:v", "rawvideo", "-pix_fmt", "yuv420p", "-fps_mode",
            "passthrough", "-f", "framehash", "-hash", "SHA256", str(framehash)]
        result["decode"] = execute(decode_command, directory, name + "-decode", 180)
        require(result["decode"]["status"] == "PASS", "Independent software HEVC decode failed")
        expected_bytes = width * height * 3 // 2 * frames
        require(decoded.stat().st_size == expected_bytes and reference.stat().st_size == expected_bytes,
                "Decoded/reference payload does not contain exactly the expected frames")
        hash_lines = framehash.read_text(encoding="utf-8").splitlines()
        dimensions = [re.fullmatch(r"#dimensions\s+0:\s*(\d+)x(\d+)\s*", line)
                      for line in hash_lines if line.startswith("#dimensions")]
        require(len(dimensions) == 1 and dimensions[0] and
                tuple(map(int, dimensions[0].groups())) == (width, height),
                "Independent decoder dimensions differ")
        result["decoded_frames"] = sum(bool(re.match(r"^\s*0\s*,", line)) for line in hash_lines)
        require(result["decoded_frames"] == frames, "Independent decoder frame count differs")
        raw_args = ["-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size",
                    f"{width}x{height}", "-framerate", "30", "-i"]
        result["psnr"] = execute(common + raw_args + [str(reference)] + raw_args + [str(decoded)] +
            ["-filter_complex", f"[0:v][1:v]psnr=stats_file={stats.name}:shortest=1", "-f", "null", "-"],
            directory, name + "-psnr", 180)
        require(result["psnr"]["status"] == "PASS", "PSNR process failed")
        lines = stats.read_text(encoding="utf-8").splitlines()
        require(len(lines) == frames, "PSNR must compare every decoded frame")
        samples, values = [], []
        for index, line in enumerate(lines):
            number = re.search(r"^n:(\d+)\s", line)
            fields = dict(re.findall(r"\b(psnr_(?:avg|y|u|v)):(\S+)", line))
            require(number and int(number.group(1)) == index + 1 and "psnr_avg" in fields,
                    "Invalid or unordered PSNR sample")
            average = float(fields["psnr_avg"])
            require(not math.isnan(average) and average >= 0, "Invalid PSNR value")
            values.append(average)
            samples.append({"frame": index, **{key: ("inf" if math.isinf(float(value)) else float(value))
                                               for key, value in fields.items()},
                            "passes_minimum": average >= MINIMUM_PSNR_DB})
        result["frames"] = samples
        result["minimum_psnr_db"] = "inf" if math.isinf(min(values)) else min(values)
        result["below_minimum_frames"] = [s["frame"] for s in samples if not s["passes_minimum"]]
        require(not result["below_minimum_frames"], "Decoded content below fixed 28 dB minimum")
        result["status"] = "PASS"
    except (ValueError, KeyError, OSError, TypeError) as error:
        result["error"] = str(error)
    write_json(directory / (name + ".decode-quality.json"), result)
    return result


def quality_audit(ffmpeg, output, probe_result, expected_frames):
    case = probe_result["case"]
    name, width, height = case["name"], case["width"], case["height"]
    report = {"tuning": case["tuning"], "strict_cbr": strict_cbr(case["tuning"]),
              "status": "FAIL", "probe": probe_result, "independent_idrs": [],
              "gate": "Fixed entire desktop sequence and every IDR decoded independently; every frame PSNR >= 28 dB",
              "performance_exclusion": "Sequence audit retains AU copies; its timings are not performance samples"}
    try:
        require(probe_result["status"] == "MEASURED", "Quality sequence probe failed")
        stream = output / (name + ".hevc")
        reference = Path(str(stream) + ".reference.i420")
        keys = read_json(Path(str(stream) + ".keys.json"))
        expected_keys = list(range(0, expected_frames, case["key_interval"]))
        require([key["frame"] for key in keys] == expected_keys,
                "Independent IDR inventory differs from requested sequence")
        frame_bytes = width * height * 3 // 2
        with stream.open("rb") as stream_file, reference.open("rb") as reference_file:
            for index, key in enumerate(keys):
                key_name = f"{name}-idr{index:02d}-frame{key['frame']:03d}"
                require(key["offset"] >= 0 and key["bytes"] > 0 and
                        key["offset"] + key["bytes"] <= stream.stat().st_size,
                        "IDR byte interval outside stream")
                key_stream, key_reference = output / (key_name + ".hevc"), output / (key_name + ".reference.i420")
                stream_file.seek(key["offset"])
                with key_stream.open("xb") as target:
                    target.write(stream_file.read(key["bytes"]))
                reference_file.seek(key["frame"] * frame_bytes)
                with key_reference.open("xb") as target:
                    target.write(reference_file.read(frame_bytes))
                entry = decode_content(ffmpeg, output, key_name, key_stream, key_reference, width, height, 1)
                entry["source_frame"] = key["frame"]
                report["independent_idrs"].append(entry)
        # A below-threshold IDR must not suppress full-sequence or other IDR evidence.
        report["stream"] = decode_content(ffmpeg, output, name + "-stream", stream, reference,
                                          width, height, expected_frames)
        require(report["stream"]["status"] == "PASS" and
                all(entry["status"] == "PASS" for entry in report["independent_idrs"]),
                "At least one fixed-sequence content/decode gate failed")
        report["status"] = "PASS"
    except (ValueError, KeyError, OSError, TypeError) as error:
        report["error"] = str(error)
    write_json(output / (name + ".quality.json"), report)
    return report


def select_profiles(screening, quality):
    measurements = {(r["case"]["tuning"], r["case"]["workload"]): r["measurement"]
                    for r in screening if r["status"] == "MEASURED" and r["case"]["mode"] == "paced"}
    ranks = []
    for profile in PROFILES:
        if not strict_cbr(profile) or quality.get(profile, {}).get("status") != "PASS":
            continue
        try:
            ratios = [measurements[profile, workload]["key_encode_call_ms"]["mean"] /
                      measurements["baseline", workload]["key_encode_call_ms"]["mean"]
                      for workload in ("screen", "texture")]
            require(all(math.isfinite(ratio) and ratio > 0 for ratio in ratios), "Invalid ranking ratio")
            ranks.append({"tuning": profile, "score": math.sqrt(math.prod(ratios)),
                          "screen_key_mean_ratio": ratios[0], "texture_key_mean_ratio": ratios[1]})
        except (KeyError, ValueError, ZeroDivisionError):
            continue
    ranks.sort(key=lambda item: (item["score"], PROFILES.index(item["tuning"])))
    selected = ["baseline"]  # Baseline is always measured, even when its quality gate fails.
    pure = next((item["tuning"] for item in ranks if item["tuning"] in ("wpp4", "wpp8", "wpp16")), None)
    best = ranks[0]["tuning"] if ranks else None
    for profile in (pure, best):
        if profile and profile not in selected:
            selected.append(profile)
    return {"selected": selected, "ranking": ranks,
            "method": "Only strict-CBR quality-PASS candidates; geometric mean of 1080p paced key-frame mean latency ratios to baseline across screen and texture",
            "baseline_quality_status": quality.get("baseline", {}).get("status", "NOT_RUN"),
            "limitations": "One screening sample per configuration; measured ranking is not a universal best/default recommendation"}


def summarize(results, group_fields):
    groups = {}
    for result in results:
        if result["status"] != "MEASURED" or result["case"]["mode"] == "sequence":
            continue
        case = result["case"]
        groups.setdefault(tuple(case[field] for field in group_fields), []).append(result["measurement"])
    summary = []
    fields = (("cpu_percent", "process_cpu_percent"), ("core_equivalents", "process_core_equivalents"),
              ("fps", "completed_fps"), ("system_cpu_percent", "system_cpu_percent"),
              ("skipped_slots", "skipped_slots"), ("encoded_bytes", "encoded_bytes"))
    for key, values in groups.items():
        item = dict(zip(group_fields, key))
        item["repeats"] = len(values)
        item["strict_cbr"] = strict_cbr(item["tuning"])
        for name, source in fields:
            samples = [v[source] for v in values]
            item[name] = {"median": statistics.median(samples), "min": min(samples), "max": max(samples)}
        for name in ("encode_call_ms", "key_encode_call_ms", "delta_encode_call_ms"):
            item[name] = {metric: statistics.median(v[name][metric] for v in values)
                          for metric in ("mean", "p50", "p95", "p99", "max")}
        item["bitrate_bps"] = {"median": statistics.median(v["encoded_bytes"] * 8000 / v["wall_ms"] for v in values)}
        summary.append(item)
    return summary


def write_report(output, summary, quality, capture):
    lines = ["# x265 experimental tuning and actual desktop replay", "", SCOPE, "",
             f"Measurement: {summary['measurement_status']}. Quality gates remain separate.", "",
             "Process CPU percentages are normalized to the recorded whole-machine logical processor count. "
             "Encode latency includes adapter conversion, backend, AU validation and a light callback; "
             "capture, input scaling/loading, warmup, disk I/O and network are outside the performance window.", "",
             f"Captured once: {capture.get('frame_count', 'UNKNOWN')} frames; "
             f"{capture.get('skipped_slots', 'UNKNOWN')} capture slots skipped; "
             f"{capture.get('consecutive_changed_frames', 'UNKNOWN')} changed consecutive frames.", "",
             "Every desktop configuration replays the same SHA256-bound capture. Paced mode skips overdue "
             "slots and therefore samples a configuration-dependent subset; capacity mode cycles the ring "
             "at encoder speed. Neither preserves capture wall time. Fixed sequence quality compares "
             "every source frame once with IDR every 30 frames; performance uses every 60 nominal media frames.", "",
             "strict CBR keeps the original ABR/VBV/maxrate/buffer policy and may emit filler NALs. "
             "wpp8-nostrict is a separately labeled rate-policy experiment and is excluded from selection. "
             "28 dB is the existing minimum PSNR content gate, not a visual-quality or release guarantee.", "",
             "## Screening (one run per scenario)", "",
             "| Profile | Content | Mode | CPU % | FPS | Key mean/P95/P99 ms | Delta P95/P99 ms | Strict CBR |",
             "|---|---|---|---:|---:|---:|---:|---|"]
    for item in summary["screening"]:
        key, delta = item["key_encode_call_ms"], item["delta_encode_call_ms"]
        lines.append(f"| {item['tuning']} | {item['workload']} | {item['mode']} | "
                     f"{item['cpu_percent']['median']:.3f} | {item['fps']['median']:.2f} | "
                     f"{key['mean']:.2f}/{key['p95']:.2f}/{key['p99']:.2f} | "
                     f"{delta['p95']:.2f}/{delta['p99']:.2f} | {item['strict_cbr']} |")
    lines += ["", "## Fixed desktop quality", "",
              "| Profile | Quality | Stream minimum PSNR dB | Independent IDRs |", "|---|---|---:|---:|"]
    for profile in PROFILES:
        audit = quality.get(profile, {})
        minimum = audit.get("stream", {}).get("minimum_psnr_db", "UNKNOWN")
        lines.append(f"| {profile} | {audit.get('status', 'NOT_RUN')} | {minimum} | {len(audit.get('independent_idrs', []))} |")
    lines += ["", "Selected profiles: " + ", ".join(summary["selection"]["selected"]), "",
              summary["selection"]["method"], "", "## Actual desktop replay (three-round medians)", "",
              "| Resolution | Profile | Mode | CPU % | FPS | Key P95/P99 ms | Delta P95/P99 ms |", "|---|---|---|---:|---:|---:|---:|"]
    for item in summary["desktop"]:
        key, delta = item["key_encode_call_ms"], item["delta_encode_call_ms"]
        lines.append(f"| {item['width']}x{item['height']} | {item['tuning']} | {item['mode']} | "
                     f"{item['cpu_percent']['median']:.3f} | {item['fps']['median']:.2f} | "
                     f"{key['p95']:.2f}/{key['p99']:.2f} | {delta['p95']:.2f}/{delta['p99']:.2f} |")
    lines += ["", "P95/P99 above are medians of each run's nearest-rank percentiles, not pooled percentiles. "
              "Raw samples, subprocess failures, every IDR decode and per-frame PSNR are retained. "
              "A static captured desktop does not represent scrolling/video content. "
              "Rotating profile order exposes order drift but does not remove thermal/background bias.", ""]
    with (output / "report.md").open("x", encoding="utf-8", newline="\n") as stream:
        stream.write("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "out/x265-tuning-20261006-v1")
    parser.add_argument("--exe", type=Path, default=ROOT / "out/ReleasePrefixed/x265_performance_probe.exe")
    parser.add_argument("--capture-exe", type=Path, default=ROOT / "out/ReleasePrefixed/x265_screen_sequence_capture.exe")
    parser.add_argument("--build-evidence", type=Path, default=ROOT / "out/x265-tuning-build-20261006-v1")
    parser.add_argument("--ffmpeg", type=Path)
    args = parser.parse_args()
    output, executable, capture_exe = args.output.resolve(), args.exe.resolve(), args.capture_exe.resolve()
    output.mkdir(parents=True, exist_ok=False)
    results, screening, quality, desktop = [], [], {}, []
    captured, selection = None, {"selected": [], "ranking": [], "method": "NOT_RUN"}
    failure, snapshot, dataset_snapshot = None, None, None
    try:
        ffmpeg, decoder = discover_ffmpeg(args.ffmpeg)
        snapshot = inputs(executable, args.build_evidence.resolve())
        build = read_json(args.build_evidence.resolve() / "result.json")
        capture_record = record(capture_exe)
        require(capture_record in build.get("executables", []),
                "Capture executable does not match the successful current build")
        extras = [record(Path(__file__).resolve()), capture_record, record(ffmpeg)]
        if "source_record" in decoder:
            extras.append(decoder["source_record"])
        snapshot["files"].extend(item for item in extras if item not in snapshot["files"])
        snapshot["reference_decoder"] = decoder
        write_json(output / "inputs.json", snapshot)
        host = host_metadata()
        write_json(output / "host.json", host)
        require(host.get("status") == "RECORDED", "Host metadata unavailable; CPU normalization cannot be bound")
        logical = int(host["values"]["computer"]["NumberOfLogicalProcessors"])
        require(logical > 0, "Invalid host logical processor count")
        verify_inputs(snapshot)
        prefix = output / "desktop"
        capture_process = execute([str(capture_exe), "--output", str(prefix), "--seconds", "6", "--fps", "30"],
                                  output, "desktop-capture", 120)
        verify_inputs(snapshot)
        require(capture_process["status"] == "PASS", "One required desktop capture failed; it is not retried")
        captured = read_json(output / "desktop.metadata.json")
        require(captured.get("status") == "CAPTURED" and captured["width"] == 1920 and
                captured["height"] == 1080 and captured["pixel_format"] == "I420", "Unexpected desktop dataset")
        frames = int(captured["frame_count"])
        require(1 <= frames <= 180 and (output / "desktop.i420").stat().st_size == FRAME_BYTES * frames,
                "Capture packed I420 frame inventory differs")
        capture_rows = list(csv.DictReader((output / "desktop.frames.csv").open(encoding="utf-8")))
        require(len(capture_rows) == frames and all(row["valid"] == "1" for row in capture_rows),
                "Capture CSV has incomplete/invalid frames")
        require(frames + captured["skipped_slots"] == 180, "Capture slot accounting differs")
        dataset_snapshot = {"executable": capture_record, "files": [record(output / name) for name in
                            ("desktop.i420", "desktop.frames.csv", "desktop.metadata.json")]}
        write_json(output / "dataset-inputs.json", dataset_snapshot)
        print(f"Evidence: {output}; captured {frames} frames once", flush=True)

        def case(name, tuning, workload, mode, seconds, width=1920, height=1080, bitrate=4000000, round_index=1):
            return {"name": name, "tuning": tuning, "workload": workload, "mode": mode,
                    "width": width, "height": height, "bitrate": bitrate, "fps": 30,
                    "seconds": seconds, "warmup_seconds": 2, "key_interval": 30 if mode == "sequence" else 60,
                    "round": round_index, "logical_processors": logical,
                    "input": str(output / "desktop.i420") if workload == "desktop" else "",
                    "input_frames": frames if workload == "desktop" else 60}

        for profile in PROFILES:
            for workload in ("screen", "texture"):
                for mode, seconds in (("paced", 10), ("capacity", 5)):
                    result = run_probe(executable, output, case(f"screening-{profile}-{workload}-{mode}",
                                       profile, workload, mode, seconds), snapshot, dataset_snapshot)
                    results.append(result)
                    screening.append(result)
                    print(f"Screening [{len(screening)}/32] {profile} {workload} {mode}: {result['status']}", flush=True)
        for profile in PROFILES:
            result = run_probe(executable, output, case("quality-" + profile, profile, "desktop", "sequence", 6),
                               snapshot, dataset_snapshot)
            results.append(result)
            verify_inputs(snapshot)
            verify_inputs(dataset_snapshot)
            quality[profile] = quality_audit(ffmpeg, output, result, frames)
            verify_inputs(snapshot)
            verify_inputs(dataset_snapshot)
            print(f"Quality [{len(quality)}/8] {profile}: {quality[profile]['status']}", flush=True)
        write_json(output / "quality.json", {"status": "PASS" if all(q["status"] == "PASS" for q in quality.values()) else "FAIL",
                    "minimum_psnr_db": MINIMUM_PSNR_DB, "profiles": quality})
        selection = select_profiles(screening, quality)
        write_json(output / "selection.json", selection)
        for round_index in range(1, 4):
            chosen = selection["selected"]
            shift = (round_index - 1) % len(chosen)
            chosen = chosen[shift:] + chosen[:shift]
            for width, height, bitrate in ((1280, 720, 2000000), (1920, 1080, 4000000)):
                for profile in chosen:
                    for mode, seconds in (("paced", 20), ("capacity", 5)):
                        name = f"desktop-r{round_index}-{width}x{height}-{profile}-{mode}"
                        result = run_probe(executable, output, case(name, profile, "desktop", mode, seconds,
                                           width, height, bitrate, round_index), snapshot, dataset_snapshot)
                        results.append(result)
                        desktop.append(result)
                        print(f"Desktop [{len(desktop)}/{len(chosen)*12}] {name}: {result['status']}", flush=True)
        verify_inputs(snapshot)
        verify_inputs(dataset_snapshot)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        failure = str(error)
        print("ERROR: " + failure, file=sys.stderr, flush=True)
    measured = (failure is None and len(screening) == 32 and len(quality) == 8 and
                len(desktop) == len(selection["selected"]) * 12 and
                all(result["status"] == "MEASURED" for result in results))
    result = {"schema_version": 1, "status": "MEASURED" if measured else "FAIL", "scope": SCOPE,
              "recorded_at_utc": timestamp(), "error": failure, "cases": results,
              "quality_status": {profile: audit["status"] for profile, audit in quality.items()},
              "selection": selection, "capture": captured,
              "input_snapshot_sha256": record(output / "inputs.json")["sha256"]
              if (output / "inputs.json").is_file() else None}
    write_json(output / "result.json", result)
    summary = {"schema_version": 1, "measurement_status": result["status"], "scope": SCOPE,
               "screening": summarize(screening, ("tuning", "workload", "mode")),
               "desktop": summarize(desktop, ("width", "height", "tuning", "mode")),
               "selection": selection, "quality_status": result["quality_status"],
               "capture": captured, "error": failure}
    write_json(output / "summary.json", summary)
    if captured:
        write_report(output, summary, quality, captured)
    write_json(output / "artifact-hashes.json", {"recorded_at_utc": timestamp(),
               "files": [record(path, path.relative_to(output).as_posix()) for path in sorted(output.rglob("*")) if path.is_file()]})
    print("Measurement: " + result["status"] + " (quality status recorded separately)", flush=True)
    return 0 if measured else 1


if __name__ == "__main__":
    sys.exit(main())
