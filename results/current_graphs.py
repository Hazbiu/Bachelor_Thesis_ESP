#!/usr/bin/env python3
"""Discover repeated current tests, average aligned recordings, and save PNGs.

Default: arithmetic sample mean, equal weight per repetition, common elapsed
time range, no smoothing, no removal of startup samples. See README_GRAPHS.md.
"""
from __future__ import annotations

import argparse
import csv
import math
import os
from pathlib import Path
import re
import sys
import tempfile
import textwrap
from dataclasses import dataclass

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator, ScalarFormatter

FILE_PATTERN = re.compile(r"^(?P<group>.+?)[_-](?P<run>\d+)$")
TITLES = {
    "mode_active_fullpower": "Active Mode - Full Power",
    "mode_active_saving": "Active Mode - Optimized",
    "mode_deep": "Deep Sleep Mode",
    "mode_light": "Light Sleep Mode",
}
TIME_UNITS = {"s": 1.0, "ms": 1e-3, "us": 1e-6}
CURRENT_UNITS = {"A": 1.0, "mA": 1e-3, "uA": 1e-6}
# Keep each test number's color consistent across every measurement mode.
COLORS = ("#2E8B57", "#E5B700", "#78B9E7", "#A78AAA", "#7A9EAD")
MEAN_COLOR = "#153E62"
AVERAGE_COLOR = "#8B1E24"


@dataclass
class Run:
    path: Path
    number: int
    time: np.ndarray
    current: np.ndarray
    source_time_origin: float


@dataclass
class Test:
    folder: Path
    name: str
    title: str
    runs: list[Run]
    time: np.ndarray
    values: np.ndarray
    mean: np.ndarray
    average: float
    resampled: bool


def column_info(cell: str, kind: str, default_unit: str):
    key = re.sub(r"[^a-z0-9]", "", cell.strip().lstrip("#").lower().replace("µ", "u").replace("μ", "u"))
    if kind == "time":
        aliases = {"time": None, "times": "s", "timeseconds": "s", "timems": "ms", "timeus": "us", "elapsedtime": None, "elapsedtimes": "s"}
        units = TIME_UNITS
    else:
        aliases = {"current": None, "currenta": "A", "currentamps": "A", "currentma": "mA", "currentua": "uA"}
        units = CURRENT_UNITS
    if key not in aliases:
        return None
    return units[aliases[key] or default_unit]


def read_run(path: Path, number: int, args) -> Run:
    """Read the user's #time,current,... header, also allowing explicit units."""
    rows = []
    columns = None
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        for line_number, line in enumerate(handle, 1):
            if not line.strip():
                continue
            if columns is None:
                candidate = line.strip().lstrip("#").strip()
                for delimiter in (",", ";", "\t"):
                    header = next(csv.reader([candidate], delimiter=delimiter))
                    time_columns = [(j, column_info(s, "time", args.input_time_unit)) for j, s in enumerate(header)]
                    current_columns = [(j, column_info(s, "current", args.input_current_unit)) for j, s in enumerate(header)]
                    times = [(j, factor) for j, factor in time_columns if factor is not None]
                    currents = [(j, factor) for j, factor in current_columns if factor is not None]
                    if len(times) == len(currents) == 1:
                        columns = (times[0], currents[0], delimiter)
                        break
                if columns is None and not line.lstrip().startswith("#"):
                    raise ValueError(f"{path.name}:{line_number}: expected a header with time and current columns")
                continue
            if line.lstrip().startswith("#"):
                continue
            (ti, tf), (ci, cf), delimiter = columns
            parts = next(csv.reader([line], delimiter=delimiter))
            try:
                t, i = parts[ti].strip(), parts[ci].strip()
                if delimiter != ",":
                    t, i = t.replace(",", "."), i.replace(",", ".")
                values = (float(t) * tf, float(i) * cf)
                if not all(math.isfinite(v) for v in values):
                    raise ValueError("non-finite value")
                rows.append(values)
            except (ValueError, IndexError) as error:
                raise ValueError(f"{path.name}:{line_number}: invalid time/current data ({error})") from error
    if columns is None or len(rows) < 2:
        raise ValueError(f"{path.name}: a time/current header and at least two numeric samples are required")
    data = np.asarray(rows, dtype=np.float64)
    if np.any(np.diff(data[:, 0]) <= 0):
        raise ValueError(f"{path.name}: timestamps must increase strictly; duplicate or reversed times found")
    origin = float(data[0, 0])
    return Run(path, number, data[:, 0] - origin, data[:, 1], origin)


def discover(root: Path, output: Path):
    groups = {}
    def walk_error(error):
        raise error
    for directory, folders, files in os.walk(root, onerror=walk_error, followlinks=False):
        parent = Path(directory)
        folders[:] = sorted(name for name in folders if not name.startswith(".") and name not in {"venv", "env", "__pycache__"} and (parent / name).resolve() != output)
        for filename in sorted(files):
            path = parent / filename
            if path.suffix.lower() != ".csv":
                continue
            match = FILE_PATTERN.fullmatch(path.stem)
            if match:
                key = (parent.relative_to(root), match["group"])
                groups.setdefault(key, []).append((int(match["run"]), path))
    return groups


def title_for(name: str) -> str:
    if name.lower() in TITLES:
        return TITLES[name.lower()]
    text = re.sub(r"(?<=[a-z])(?=[A-Z])", " ", name)
    return re.sub(r"[_-]+", " ", text).strip()


def prepare_test(folder, name, files, args) -> Test:
    numbers = [number for number, _ in files]
    if len(numbers) != len(set(numbers)):
        raise ValueError(f"{folder}/{name}: duplicate repetition numbers")
    if args.expected_runs and len(files) != args.expected_runs:
        raise ValueError(f"{folder}/{name}: found {len(files)} runs; expected {args.expected_runs}. Add the missing files or explicitly use --expected-runs N (0 accepts any count).")
    runs = [read_run(path, number, args) for number, path in sorted(files)]
    end = min(float(run.time[-1]) for run in runs)
    if args.end is not None:
        end = min(end, args.end)
    if end <= args.start:
        raise ValueError(f"{folder}/{name}: no shared recording interval after {args.start:g} s")
    resampled = args.resample_step is not None
    if resampled:
        count = int(math.floor((end - args.start) / args.resample_step + 1e-9)) + 1
        if count > 5_000_000:
            raise ValueError(f"{folder}/{name}: resampling would exceed five million points; increase --resample-step")
        time = args.start + np.arange(count, dtype=np.float64) * args.resample_step
        time = time[time <= end + 1e-9]
        # Clamp only floating-point roundoff, never extend outside a recording.
        time = np.minimum(time, end)
        values = np.vstack([np.interp(time, run.time, run.current) for run in runs])
    else:
        selected = []
        for run in runs:
            mask = (run.time >= args.start - 1e-9) & (run.time <= end + 1e-9)
            selected.append((run.time[mask], run.current[mask]))
        time = selected[0][0]
        if len(time) < 2:
            raise ValueError(f"{folder}/{name}: the shared interval has fewer than two samples")
        for (times, _), run in zip(selected, runs):
            if times.shape != time.shape or not np.allclose(times, time, rtol=0, atol=1e-8):
                raise ValueError(f"{folder}/{name}: {run.path.name} has a different sample grid. Use --resample-step SECONDS to explicitly allow linear interpolation.")
        intervals = np.diff(time)
        if not np.allclose(intervals, intervals[0], rtol=1e-5, atol=1e-8):
            raise ValueError(f"{folder}/{name}: irregular sample intervals. Use --resample-step SECONDS to calculate on an evenly spaced grid.")
        values = np.vstack([currents for _, currents in selected])
    if len(time) < 2:
        raise ValueError(f"{folder}/{name}: the selected interval has fewer than two samples")
    mean = values.mean(axis=0, dtype=np.float64)
    return Test(folder, name, title_for(name), runs, time, values, mean, float(mean.mean(dtype=np.float64)), resampled)


def save_csv(path, header, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile("w", dir=path.parent, prefix=".graph-", suffix=".csv", encoding="utf-8", newline="", delete=False) as handle:
            temporary = Path(handle.name)
            writer = csv.writer(handle)
            writer.writerow(header)
            writer.writerows(rows)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def save_figure(fig, path, dpi):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".graph-", suffix=path.suffix, delete=False) as handle:
        temporary = Path(handle.name)
    try:
        fig.savefig(temporary, dpi=dpi, facecolor="white", format=path.suffix.lstrip("."))
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def plot_test(test, args):
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 12, "axes.labelsize": 14, "xtick.labelsize": 11, "ytick.labelsize": 11, "axes.edgecolor": "#9CA9B5", "axes.labelcolor": "#243441", "text.color": "#243441", "xtick.color": "#465661", "ytick.color": "#465661", "axes.linewidth": 0.8, "path.simplify": False})
    fig, ax = plt.subplots(figsize=(12, 7.5))
    fig.subplots_adjust(left=0.115, right=0.97, bottom=0.27, top=0.88)
    wrapped_title = textwrap.fill(test.title, width=62)
    fig.suptitle(wrapped_title, x=0.54, y=0.96, fontsize=20, fontweight="bold")
    if not args.mean_only:
        for index, run in enumerate(test.runs):
            ax.plot(test.time, test.values[index], color=COLORS[(run.number - 1) % len(COLORS)], alpha=0.74, linewidth=0.9, label=f"Test {run.number}", zorder=2)
    ax.plot(test.time, test.mean, color=MEAN_COLOR, linewidth=1.9, label="Mean", zorder=4)
    ax.axhline(test.average, color=AVERAGE_COLOR, linewidth=1.1, linestyle=(0, (5, 4)), label="Overall Average", zorder=3)
    ax.set_xlabel("Time (s)", labelpad=12)
    ax.set_ylabel("Current (A)", labelpad=14)
    ax.set_xlim(float(test.time[0]), float(test.time[-1]))
    ax.margins(y=0.12)
    if args.zero_baseline and np.all(test.values >= 0):
        ax.set_ylim(bottom=0)
    ax.xaxis.set_major_locator(MaxNLocator(nbins=7, min_n_ticks=4))
    ax.yaxis.set_major_locator(MaxNLocator(nbins=6, min_n_ticks=4))
    formatter = ScalarFormatter(useOffset=False)
    formatter.set_scientific(False)
    ax.yaxis.set_major_formatter(formatter)
    ax.grid(axis="both", color="#DDE4E9", linewidth=0.7, zorder=0)
    ax.set_axisbelow(True)
    ax.spines[["top", "right"]].set_visible(False)
    # A vertical legend beneath the bottom-right corner keeps the data visible.
    fig.legend(*ax.get_legend_handles_labels(), loc="lower right", bbox_to_anchor=(ax.get_position().x1, 0.025), bbox_transform=fig.transFigure, ncol=1, frameon=False, fontsize=10, handlelength=2.5, borderaxespad=0, labelspacing=0.45)
    fig.text(ax.get_position().x0, 0.080, f"Average Current Value = {test.average:.4f} A", ha="left", va="center", fontsize=16, fontweight="bold", color=MEAN_COLOR)
    try:
        for extension in (["png", "jpg"] if args.format == "both" else [args.format]):
            save_figure(fig, args.output / test.folder / f"{test.name}.{extension}", args.dpi)
    finally:
        plt.close(fig)


def write_outputs(tests, args):
    summary = []
    run_summary = []
    for test in tests:
        plot_test(test, args)
        average_csv = args.output / test.folder / f"{test.name}_average.csv"
        matrix = np.column_stack((test.time, test.values.T, test.mean))
        save_csv(average_csv, ["time_s", *[f"run_{r.number}_current_A" for r in test.runs], "mean_current_A"], ([format(float(x), ".15g") for x in row] for row in matrix))
        summary.append([test.title, test.folder.as_posix(), test.name, len(test.runs), f"{test.time[0]:.12g}", f"{test.time[-1]:.12g}", f"{test.time[1]-test.time[0]:.12g}", len(test.time), f"{test.average:.15g}", f"{test.average:.4f}", "linear interpolation" if test.resampled else "original samples", "; ".join(r.path.relative_to(args.root).as_posix() for r in test.runs)])
        for index, run in enumerate(test.runs):
            run_summary.append([test.title, run.path.relative_to(args.root).as_posix(), run.number, len(run.time), f"{run.source_time_origin:.15g}", f"{run.time[-1]:.12g}", len(test.time), f"{test.time[0]:.12g}", f"{test.time[-1]:.12g}", f"{test.values[index].mean():.15g}", f"{run.current.mean():.15g}"])
        print(f"  {test.title}: {test.average:.4f} A ({len(test.runs)} runs, {test.time[0]:g}-{test.time[-1]:g} s)")
    save_csv(args.output / "summary.csv", ["test", "source_folder", "test_group", "repetitions", "start_s", "end_s", "step_s", "samples_per_run", "average_current_A", "average_current_A_4dp", "alignment", "source_files"], summary)
    save_csv(args.output / "run_summary.csv", ["test", "source_file", "run", "original_samples", "original_time_origin_s", "original_duration_s", "selected_samples", "selected_start_s", "selected_end_s", "selected_mean_current_A", "full_recording_sample_mean_A"], run_summary)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent, help="Folder containing test folders (default: beside this script)")
    parser.add_argument("--output", type=Path, help="Output folder; a relative path is resolved under --root (default: graphs)")
    parser.add_argument("--expected-runs", type=int, default=3, metavar="N", help="Require N recordings per test; use 0 to accept any count (default: 3)")
    parser.add_argument("--start", type=float, default=0.0, metavar="SECONDS", help="First elapsed time to include (default: 0; preserves startup)")
    parser.add_argument("--end", type=float, metavar="SECONDS", help="Last elapsed time to include (default: shortest recording in each group)")
    parser.add_argument("--resample-step", type=float, metavar="SECONDS", help="Explicitly allow linear interpolation to this uniform time step")
    parser.add_argument("--input-time-unit", choices=TIME_UNITS, default="s", help="Unit for a bare time header (default: s)")
    parser.add_argument("--input-current-unit", choices=CURRENT_UNITS, default="A", help="Unit for a bare current header (default: A)")
    parser.add_argument("--format", choices=("png", "jpg", "both"), default="png")
    parser.add_argument("--dpi", type=int, default=300, help="Image resolution (default: 300)")
    parser.add_argument("--mean-only", action="store_true", help="Hide the individual run lines")
    parser.add_argument("--zero-baseline", action="store_true", help="Start the y-axis at zero when currents are nonnegative")
    args = parser.parse_args(argv)
    if args.expected_runs < 0 or args.dpi < 72:
        parser.error("--expected-runs must be nonnegative; --dpi must be at least 72")
    if not math.isfinite(args.start) or args.start < 0:
        parser.error("--start must be finite and nonnegative")
    if args.end is not None and (not math.isfinite(args.end) or args.end <= args.start):
        parser.error("--end must be finite and greater than --start")
    if args.resample_step is not None and (not math.isfinite(args.resample_step) or args.resample_step <= 0):
        parser.error("--resample-step must be finite and positive")
    args.root = args.root.expanduser().resolve()
    output = args.output.expanduser() if args.output else Path("graphs")
    args.output = (output if output.is_absolute() else args.root / output).resolve()
    if not args.root.is_dir():
        parser.error(f"input folder does not exist: {args.root}")
    if args.output == args.root or args.output in args.root.parents:
        parser.error("output must not equal or contain the input root")
    try:
        groups = discover(args.root, args.output)
        if not groups:
            raise ValueError("No repeated CSV tests found. Place files such as Mode_Deep_1.csv, Mode_Deep_2.csv, Mode_Deep_3.csv in folders beside this script.")
        # Validate every input before replacing any existing graph.
        tests = [prepare_test(folder, name, files, args) for (folder, name), files in sorted(groups.items())]
        print(f"Found {len(tests)} tests in: {args.root}")
        print(f"Bare time/current headers use {args.input_time_unit}/{args.input_current_unit}; output units are s/A.")
        print("Averaging original, equally spaced samples over each group's shared elapsed-time interval." if args.resample_step is None else "Averaging linearly interpolated samples over each group's shared elapsed-time interval.")
        write_outputs(tests, args)
        print(f"Graphs and numerical results replaced in: {args.output}")
        return 0
    except (ValueError, OSError, csv.Error) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

