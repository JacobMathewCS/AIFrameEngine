"""Summarize ORT host events; these are NOT direct GPU execution timestamps."""
import argparse
import collections
import json
from pathlib import Path

def summarize(events):
    runs = [e for e in events if e.get("name") == "model_run" and e.get("ph") == "X"]
    nodes = [e for e in events if e.get("cat") == "Node" and e.get("ph") == "X"
             and e.get("name", "").endswith("_kernel_time")]
    providers = collections.Counter(e.get("args", {}).get("provider", "unknown") for e in nodes)
    rows = []
    for run in sorted(runs, key=lambda e: e["dur"], reverse=True)[:10]:
        start, end = run["ts"], run["ts"] + run["dur"]
        contained = [n for n in nodes if start <= n["ts"] and n["ts"] + n["dur"] <= end]
        rows.append({"start_us": start, "run_ms": run["dur"] / 1000,
                     "node_host_sum_ms": sum(n["dur"] for n in contained) / 1000,
                     "largest_node_events": [{"name": n["name"], "ms": n["dur"] / 1000,
                         "provider": n.get("args", {}).get("provider", "unknown")}
                         for n in sorted(contained, key=lambda n: n["dur"], reverse=True)[:4]]})
    return {"run_count_including_warmup": len(runs), "node_provider_counts": dict(providers),
            "slowest_runs": rows,
            "caution": "Host event durations can include waiting/transfers; not GPU kernel time. Warmup is included."}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("profile", type=Path)
    args = p.parse_args()
    print(json.dumps(summarize(json.loads(args.profile.read_text(encoding="utf-8-sig"))), indent=2))

if __name__ == "__main__":
    main()