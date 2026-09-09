"""Summarize diagnostic PSO samples without accepting the plan's performance gate."""
import argparse
import csv
import hashlib
import json
import math
from collections import defaultdict
from pathlib import Path
from statistics import median


def request_metrics(row):
    e, q, b, n, x, d, r = (int(row[field]) for field in (
        "entry_ns", "scheduled_ns", "body_start_ns", "native_start_ns",
        "native_end_ns", "body_end_ns", "returned_ns"))
    if row["success"] != "1" or not 0 < e <= q <= b <= d <= r:
        raise ValueError("Failed request or invalid boundary ordering")
    if bool(n) != bool(x) or (n and not b <= n <= x <= d):
        raise ValueError("Invalid native boundary ordering")
    native = int(row.get("native_ns", x-n))
    dependency = int(row.get("dependency_ns", 0))
    if native < 0 or dependency < 0 or native + dependency > d-b:
        raise ValueError("Overlapping or invalid creation phases")
    result = {"request_us": r-e, "pre_schedule_us": q-e,
              "schedule_to_body_us": b-q, "body_us": d-b,
              "body_cpu_us": d-b-native-dependency,
              "dependency_us": dependency, "body_to_return_us": r-d}
    if n:
        result["native_us"] = native
    for field in ("normalization", "preparation"):
        if field + "_ns" in row:
            result[field + "_us"] = int(row[field + "_ns"])
    if "admitted_ns" in row:
        admitted, wait_begin, wait_end = (int(row[field]) for field in (
            "admitted_ns", "wait_begin_ns", "wait_end_ns"))
        if admitted:
            if not q <= admitted <= b or not admitted <= wait_begin <= wait_end <= r:
                raise ValueError("Invalid admission/wait ordering")
            result.update(queue_us=b-admitted, admission_delay_us=admitted-q,
                          consumer_wait_us=wait_end-wait_begin)
        elif wait_begin or wait_end:
            raise ValueError("Inline call unexpectedly waited")
        else:
            result.update(queue_us=0, admission_delay_us=0, consumer_wait_us=0)
    return {key: value / 1000 for key, value in result.items()}


def stats(values):
    values = sorted(values)
    return {"count": len(values), "median": median(values),
            "p95": values[math.ceil(len(values)*0.95)-1], "maximum": values[-1]}


def csv_rows(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def group_requests(rows, key_for):
    groups = defaultdict(lambda: defaultdict(list))
    rounds = defaultdict(set)
    ids = set()
    for row in rows:
        request = int(row["request"])
        if request in ids:
            raise ValueError("Duplicate request ID")
        ids.add(request)
        key = key_for(row)
        rounds[key].add(int(row["round"]))
        for name, value in request_metrics(row).items():
            groups[key][name].append(value)
    return groups, rounds


def summarize(root):
    groups, rounds = group_requests(csv_rows(root / "requests.csv"), lambda row:
        row["scenario"] + ("/compute" if row["compute"] == "1" else "/graphics"))
    expected = {"cold": 30, "hot": 3000, "duplicate16": 480, "distinct16": 480, "batch64": 1920}
    if len(groups) != 10:
        raise ValueError("Expected five scenarios for both pipeline kinds")
    for key, metrics in groups.items():
        if rounds[key] != set(range(30)) or len(metrics["request_us"]) != expected[key.split("/")[0]]:
            raise ValueError("Incomplete fixed sample set: " + key)
    return {"status": "diagnostic; performance acceptance requires all plan evidence",
            "seed_sha256": digests(root, "*-seed.bin"), "shader_sha256": digests(root, "Shader*.spv"),
            "scenarios": {key: {name: stats(values) for name, values in metrics.items()}
                          for key, metrics in sorted(groups.items())}}


def digests(root, pattern):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(root.glob(pattern))}


def add_memory(result, rows, key_for):
    groups = defaultdict(lambda: defaultdict(list))
    for row in rows:
        if row["memory_available"] != "1" or int(row["memory_samples"]) < 2:
            raise ValueError("Memory provider or samples unavailable")
        for metric in ("private_peak_bytes", "local_video_peak_bytes", "nonlocal_video_peak_bytes",
                       "max_sample_gap_ns", "process_lifetime_commit_peak_bytes"):
            groups[key_for(row)][metric].append(int(row[metric]))
    for key, metrics in groups.items():
        result["scenarios"].setdefault(key, {}).update({k: stats(v) for k, v in metrics.items()})


def add_resources(result, root):
    groups, rounds = group_requests(csv_rows(root / "requests.csv"), lambda row: "resource/" + row["kind"])
    if set(groups) != {"resource/" + str(kind) for kind in range(2, 9)}:
        raise ValueError("Missing resource kind")
    for key, metrics in groups.items():
        if rounds[key] != set(range(30)) or len(metrics["request_us"]) != 30*64:
            raise ValueError("Incomplete resource batch")
        result["scenarios"][key] = {k: stats(v) for k, v in metrics.items()}
    rows = csv_rows(root / "rounds.csv")
    if {int(row["round"]) for row in rows} != set(range(30)) or len(rows) != 30:
        raise ValueError("Incomplete resource rounds")
    result["scenarios"]["resource/batch"] = {
        field + "_us": stats([int(row[field + "_ns"])/1000 for row in rows])
        for field in ("creation", "upload_record", "upload_wait")}
    add_memory(result, rows, lambda row: "resource/batch")
    result["resource_input_sha256"] = digests(root, "initial-data.bin") | digests(root, "Shader*.spv")


def add_material(result, root):
    rows = csv_rows(root / "frames.csv")
    if {(int(r["round"]), int(r["frame"])) for r in rows} != {
            (r, f) for r in range(30) for f in range(121)} or len(rows) != 30*121:
        raise ValueError("Incomplete material frames")
    groups = defaultdict(lambda: defaultdict(list))
    for row in rows:
        p, b, e, g, c, trigger = (int(row[f]) for f in ("producer_begin_ns", "render_begin_ns",
            "render_end_ns", "gpu_complete_ns", "producer_complete_ns", "prewarm_trigger_ns"))
        if not trigger <= p <= b <= e <= g <= c:
            raise ValueError("Invalid complete-frame boundaries")
        key = "material/first" if row["frame"] == "0" else "material/following120"
        values = {"frame_us": c-p, "render_cpu_us": e-b, "gpu_tail_wait_us": g-e}
        if "replay_complete_ns" in row:
            replay = int(row["replay_complete_ns"])
            if not e <= replay <= g:
                raise ValueError("Invalid replay/GPU-idle boundary")
            values["replay_flush_us"] = replay-e
            values["gpu_idle_host_us"] = g-replay
        if int(row.get("gpu_work_ns", "0")) > 0:
            # Optional device interval; zero means disabled, not free GPU work.
            values["gpu_work_us"] = int(row["gpu_work_ns"])
        if row["frame"] == "0":
            values["trigger_to_visible_us"] = g-trigger
        for metric, value in values.items():
            groups[key][metric].append(value / 1000)
    for key, metrics in groups.items():
        result["scenarios"][key] = {k: stats(v) for k, v in metrics.items()}
    request_groups, _ = group_requests(csv_rows(root / "requests.csv"), lambda row: "material/requests")
    for key, metrics in request_groups.items():
        result["scenarios"][key] = {k: stats(v) for k, v in metrics.items()}
    add_memory(result, rows, lambda row: "material/first" if row["frame"] == "0" else "material/following120")
    result["material_input_sha256"] = digests(root, "scene.txt") | digests(root, "material-shader*.spv")


def add_overhead(result, root):
    groups = defaultdict(list)
    rows = csv_rows(root / "overhead.csv")
    if len(rows) != 60000:
        raise ValueError("Incomplete overhead observations")
    for row in rows:
        groups[row["enabled"]].append(int(row["elapsed_ns"])/1000)
    if {k: len(v) for k, v in groups.items()} != {"0": 30000, "1": 30000}:
        raise ValueError("Incomplete overhead groups")
    for key, values in groups.items():
        result["scenarios"]["instrumentation/" + key] = {"elapsed_us": stats(values)}


def add_async(result, root):
    rounds = csv_rows(root / "rounds.csv")
    observers = csv_rows(root / "observers.csv")
    native = csv_rows(root / "native.csv")
    scenarios = {"cold": 1, "hot": 100, "duplicate16": 16, "distinct16": 16, "batch64": 64}
    expected = {(r, s, c) for r in range(30) for s in scenarios for c in ("0", "1")}
    identity = lambda row: (int(row["round"]), row["scenario"], row["compute"])
    if len(rounds) != 300 or {identity(r) for r in rounds} != expected:
        raise ValueError("Incomplete asynchronous rounds")
    group = lambda row: "async/" + row["scenario"] + ("/compute" if row["compute"] == "1" else "/graphics")
    metrics = defaultdict(lambda: defaultdict(list))
    observer_counts, native_counts = defaultdict(int), defaultdict(int)
    observer_keys, native_keys = defaultdict(set), defaultdict(set)
    for row in observers:
        e, r, ready, wait = (int(row[k]) for k in ("entry_ns", "request_returned_ns", "ready_observed_ns", "consumer_wait_ns"))
        if not 0 < e <= r <= ready or wait < 0 or wait > ready-r:
            raise ValueError("Invalid asynchronous observation boundaries")
        observer_counts[identity(row)] += 1
        observer_keys[identity(row)].add(row["key"])
        metrics[group(row)]["observer_request_us"].append((r-e)/1000)
    for row in native:
        if row["background"] != "1":
            raise ValueError("Async native record is not from the creator")
        for name, value in request_metrics(row).items():
            metrics[group(row)]["creator_" + name].append(value)
        native_counts[identity(row)] += 1
        native_keys[identity(row)].add(row["key"])
    for row in rounds:
        key = identity(row)
        count = scenarios[row["scenario"]]
        creations = 0 if row["scenario"] == "hot" else count if row["scenario"] in ("distinct16", "batch64") else 1
        if (observer_counts[key] != count or int(row["observers"]) != count
                or int(row["native_creations"]) != creations or native_counts[key] != creations
                or len(native_keys[key]) != creations or not native_keys[key] <= observer_keys[key]
                or row["synchronous_operations"] != "0" or row["replay_serials"] != "1"):
            raise ValueError("Async count, identity, or replay contract mismatch: " + str(key))
        for field in ("admission", "consumer_wait", "total", "replay_marker"):
            metrics[group(row)][field + "_us"].append(int(row[field + "_ns"])/1000)
        metrics[group(row)]["metadata_bytes"].append(int(row["metadata_bytes"]))
    for key, values in metrics.items():
        result["scenarios"][key] = {name: stats(samples) for name, samples in values.items()}
    add_memory(result, rounds, group)
    result["async_observation_contract"] = (
        "One Core worker and one native creator; readiness is observed by sequential caller waits, "
        "not the publication timestamp. Native records correlate by round, scenario, kind and key. "
        "One independent replay marker per round is excluded from creation round trips.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--compare", type=Path)
    parser.add_argument("--resource-root", type=Path)
    parser.add_argument("--material-root", type=Path)
    parser.add_argument("--overhead-root", type=Path)
    parser.add_argument("--async-root", type=Path)
    args = parser.parse_args()
    result = summarize(args.root)
    round_rows = csv_rows(args.root / "rounds.csv")
    if round_rows and "memory_available" in round_rows[0]:
        add_memory(result, round_rows, lambda row: row["scenario"] + (
            "/compute" if row["compute"] == "1" else "/graphics"))
    if args.resource_root:
        add_resources(result, args.resource_root)
    if args.material_root:
        add_material(result, args.material_root)
    if args.overhead_root:
        add_overhead(result, args.overhead_root)
    if args.compare:
        before = json.loads(args.compare.read_text()) if args.compare.is_file() else summarize(args.compare)
        changes = {}
        p95_changes = {}
        for key, metrics in result["scenarios"].items():
            for metric, stats in metrics.items():
                if key not in before["scenarios"] or metric not in before["scenarios"][key]:
                    continue
                old = before["scenarios"][key][metric]["median"]
                new = stats["median"]
                changes[key + "/" + metric] = (new-old)/old*100 if old else None
                old_p95 = before["scenarios"][key][metric]["p95"]
                p95_changes[key + "/" + metric] = (stats["p95"]-old_p95)/old_p95*100 if old_p95 else None
        result["median_change_percent"] = changes
        result["p95_change_percent"] = p95_changes
        result["unstable_request_or_native_medians"] = [
            key for key, delta in changes.items()
            if key.endswith(("/request_us", "/native_us", "/frame_us", "/trigger_to_visible_us")) and (delta is None or abs(delta) > 5)]
        result["same_seed_bytes"] = result["seed_sha256"] == before["seed_sha256"]
        result["same_shader_bytes"] = (len(result["shader_sha256"]) == 3
                                       and result["shader_sha256"] == before["shader_sha256"])
        result["same_scenario_metrics"] = {
            key: sorted(metrics) for key, metrics in result["scenarios"].items()
        } == {key: sorted(metrics) for key, metrics in before["scenarios"].items()}
        for field in ("resource_input_sha256", "material_input_sha256"):
            if field in result or field in before:
                result["same_" + field] = (bool(result.get(field))
                                           and result.get(field) == before.get(field))
    # New asynchronous contracts have no pre-refactor async service baseline.
    # Keep them outside the legacy same-metric-set comparison.
    if args.async_root:
        add_async(result, args.async_root)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
