"""Compare the two macOS readers and the effect of sampling, using one backend."""
import json
import os
from pathlib import Path
import random
import re
from statistics import median
import subprocess

HERE = Path(__file__).resolve().parent
MODES = {
    # Use the process reader? Sample every N rows? Zero measured rows means off.
    "off": (False, 0),
    "process_1": (True, 1),
    "thread_1": (False, 1),
    "thread_64": (False, 64),
}
QUERY = """EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, FORMAT JSON)
SELECT count(*) FROM pmc_probe JOIN pmc_build USING (id);"""


def run_queries():
    sql = [(HERE / "workload.sql").read_text()]
    rng = random.Random(1731)
    order = list(MODES)
    for round_no in range(-1, 25):
        # Warm each mode twice. Then balance every four positions over four rounds.
        if round_no <= 0 or (round_no - 1) % 4 == 0:
            rng.shuffle(order)
        shift = max(0, round_no - 1) % 4
        for mode in order[shift:] + order[:shift]:
            use_process, period = MODES[mode]
            sql.append(f"\\echo RUN {round_no} {mode}")
            if period:
                reader = "on" if use_process else "off"
                sql.extend([f"SET halo_pmc_use_process_reader = {reader};",
                            "SET halo_pmc_list = 'cycles,instructions';",
                            f"SET halo_pmc_sample_period = {period};"])
            else:
                sql.append("RESET halo_pmc_list;")
            sql.append(QUERY)
    proc = subprocess.run(["psql", "-XqAt", "-d", os.environ["PGDATABASE"],
                           "-v", "ON_ERROR_STOP=1", "-f", "-"],
                          input="\n".join(sql), stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True)
    Path("query-plans.log").write_text(proc.stdout)
    if proc.returncode:
        raise SystemExit(proc.stdout[-3000:])
    parts = re.split(r"^RUN (-?\d+) (\w+)$", proc.stdout, flags=re.M)
    if len(parts) != 1 + 26 * len(MODES) * 3:
        raise SystemExit("Incomplete database output. See query-plans.log.")
    return zip(parts[1::3], parts[2::3], parts[3::3])


def inspect(mode, output):
    if "WARNING:" in output or "ERROR:" in output:
        raise ValueError(output)
    plan = json.loads(output[output.index("["):])[0]
    join = plan["Plan"]["Plans"][0]
    build = join["Plans"][1]
    shape = (join["Node Type"], join["Actual Rows"], build["Node Type"],
             build["Actual Rows"], build["Actual Loops"])
    if shape != ("Hash Join", 10000, "Hash", 10000, 1):
        raise ValueError("Unexpected plan: " + output)
    use_process, period = MODES[mode]
    intervals = re.findall(r"HaloPMC target=ExecHashGetHashValue period=(\d+) samples=(\d+)", output)
    events = re.findall(r"PMC (cycles|instructions) raw=(\d+) boundary_reads=(\d+) source=(\w+)", output)
    if period == 0:
        if "HaloPMC target=" in output or events:
            raise ValueError("Unexpected measurement with PMC disabled: " + output)
        return 0, 0, plan["Execution Time"]
    samples = (10000 + period - 1) // period
    source = "proc_pid_rusage" if use_process else "thread_selfcounts"
    if (intervals != [(str(period), str(samples))] or len(events) != 2 or
            {event[0] for event in events} != {"cycles", "instructions"} or
            any(int(raw) <= 0 or int(calls) != 2 * samples or src != source
                for _, raw, calls, src in events)):
        raise ValueError("Counter or coverage mismatch: " + output)
    return int(intervals[0][1]), int(events[0][2]), plan["Execution Time"]


if __name__ == "__main__":
    times = {mode: [] for mode in MODES}
    print("round mode sampled_rows boundary_reads execution_ms")
    for number, mode, body in run_queries():
        samples, reads, ms = inspect(mode, body)
        if int(number) > 0:
            times[mode].append(ms)
            print(number, mode, samples, reads, f"{ms:.3f}")
    for mode, values in times.items():
        print(f"median_ms {mode} {median(values):.4f}")
    wins = sum(t < p for t, p in zip(times["thread_1"], times["process_1"]))
    print(f"paired_rounds_thread_1_faster {wins}/{len(times['thread_1'])}")
