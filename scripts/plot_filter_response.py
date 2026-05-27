#!/usr/bin/env python3
"""Plot the magnitude response of the rtsp_audio component's audio filter chain.

Generates docs/images/filter-response.png — three side-by-side panels, one
per shipping filter stage:

  - DC blocker:  1-pole HP at 5 Hz  (always-on, fixed, Q15)
  - Low-cut:     2nd-order Butterworth HP at 100 Hz  (user-tunable default)
  - High-cut:    2nd-order Butterworth LP at 10 kHz  (user-tunable example)

All coefficient formulas match the C++ side exactly so the plot reflects
shipping behaviour.

Run:
    pip install numpy scipy matplotlib
    python3 scripts/plot_filter_response.py
"""

import math
import pathlib

import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import numpy as np
from scipy import signal

SAMPLE_RATE_HZ = 32000.0
DC_BLOCKER_HZ = 5.0
LOW_CUT_HZ = 100.0
HIGH_CUT_HZ = 10000.0

Y_MIN_DB = -60.0
Y_MAX_DB = 3.0
N_FREQ_POINTS = 2048
FIGURE_SIZE_INCHES = (12.0, 5.0)
FIGURE_DPI = 150
OUTPUT_PATH = (
    pathlib.Path(__file__).resolve().parent.parent
    / "docs"
    / "images"
    / "filter-response.png"
)


def format_hz(value, _pos=None):
    """Plain-number tick label: '5 Hz', '1 kHz', '10 kHz'."""
    if value >= 1000:
        return f"{value / 1000.0:g} kHz"
    return f"{value:g} Hz"


def single_pole_hp_coeffs(fc_hz, fs_hz):
    """1-pole IIR high-pass (matches dc_blocker.h).

    H(z) = (1 - z^-1) / (1 - R*z^-1),  R = exp(-2*pi*fc/fs)
    """
    r = math.exp(-2.0 * math.pi * fc_hz / fs_hz)
    return [1.0, -1.0, 0.0], [1.0, -r, 0.0]


def butterworth_hp_coeffs(fc_hz, fs_hz):
    """2nd-order Butterworth high-pass (matches low_cut_biquad.h)."""
    w = math.tan(math.pi * fc_hz / fs_hz)
    w2 = w * w
    sqrt2 = math.sqrt(2.0)
    norm = 1.0 / (1.0 + sqrt2 * w + w2)
    b = [norm, -2.0 * norm, norm]
    a = [1.0, 2.0 * (w2 - 1.0) * norm, (1.0 - sqrt2 * w + w2) * norm]
    return b, a


def butterworth_lp_coeffs(fc_hz, fs_hz):
    """2nd-order Butterworth low-pass (matches high_cut_biquad.h)."""
    w = math.tan(math.pi * fc_hz / fs_hz)
    w2 = w * w
    sqrt2 = math.sqrt(2.0)
    norm = 1.0 / (1.0 + sqrt2 * w + w2)
    b = [w2 * norm, 2.0 * w2 * norm, w2 * norm]
    a = [1.0, 2.0 * (w2 - 1.0) * norm, (1.0 - sqrt2 * w + w2) * norm]
    return b, a


def magnitude_db(b, a, freqs_hz, fs_hz):
    """Return |H(e^jω)| in dB at the given frequencies."""
    w_rad = 2.0 * math.pi * np.asarray(freqs_hz) / fs_hz
    _, h = signal.freqz(b, a, worN=w_rad)
    floor_linear = 10.0 ** ((Y_MIN_DB - 20.0) / 20.0)
    return 20.0 * np.log10(np.maximum(np.abs(h), floor_linear))


def setup_ax(ax, title, f_min, f_max, x_ticks, cutoff_hz, color):
    """Apply common styling and a single cutoff marker to an axes."""
    freqs = np.logspace(math.log10(f_min), math.log10(f_max), N_FREQ_POINTS)
    ax.set_xscale("log")
    ax.set_xlim(f_min, f_max)
    ax.set_ylim(Y_MIN_DB, Y_MAX_DB)
    ax.set_xlabel("Frequency")
    ax.set_ylabel("Magnitude (dB)")
    ax.set_title(title, fontsize=10)
    ax.grid(True, which="major", linestyle="-", alpha=0.4)
    ax.grid(True, which="minor", linestyle=":", alpha=0.25)
    ax.xaxis.set_major_locator(mticker.FixedLocator(x_ticks))
    ax.xaxis.set_major_formatter(mticker.FuncFormatter(format_hz))
    ax.xaxis.set_minor_formatter(mticker.NullFormatter())
    ax.yaxis.set_major_locator(mticker.MultipleLocator(10))
    ax.axhline(-3.0, color="k", linestyle=":", linewidth=0.8, alpha=0.5, label="−3 dB")
    ax.axvline(
        cutoff_hz,
        color=color,
        linestyle="--",
        linewidth=0.8,
        alpha=0.6,
        label=f"Example low-cut cutoff = {format_hz(cutoff_hz)}",
    )
    return freqs


def main():
    fig, ax = plt.subplots(1, 1, figsize=FIGURE_SIZE_INCHES)

    freqs = setup_ax(
        ax,
        "",
        f_min=1.0,
        f_max=SAMPLE_RATE_HZ / 2.0,
        x_ticks=[1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000],
        cutoff_hz=LOW_CUT_HZ,
        color="#1f77b4",
    )
    ax.axvline(
        DC_BLOCKER_HZ,
        color="#888888",
        linestyle="--",
        linewidth=0.8,
        alpha=0.6,
        label=f"DC blocker cutoff = {format_hz(DC_BLOCKER_HZ)}",
    )
    ax.axvline(
        HIGH_CUT_HZ,
        color="#d62728",
        linestyle="--",
        linewidth=0.8,
        alpha=0.6,
        label=f"Example high-cut cutoff = {format_hz(HIGH_CUT_HZ)}",
    )

    db_dc = magnitude_db(
        *single_pole_hp_coeffs(DC_BLOCKER_HZ, SAMPLE_RATE_HZ), freqs, SAMPLE_RATE_HZ
    )
    db_lc = magnitude_db(
        *butterworth_hp_coeffs(LOW_CUT_HZ, SAMPLE_RATE_HZ), freqs, SAMPLE_RATE_HZ
    )
    db_hc = magnitude_db(
        *butterworth_lp_coeffs(HIGH_CUT_HZ, SAMPLE_RATE_HZ), freqs, SAMPLE_RATE_HZ
    )

    ax.plot(
        freqs,
        db_dc,
        color="#888888",
        linewidth=2.0,
        label=f"DC blocker — 1-pole HP @ {format_hz(DC_BLOCKER_HZ)}",
    )
    ax.plot(
        freqs,
        db_lc,
        color="#1f77b4",
        linewidth=2.0,
        label=f"Low-cut — Butterworth HP @ {format_hz(LOW_CUT_HZ)}",
    )
    ax.plot(
        freqs,
        db_hc,
        color="#d62728",
        linewidth=2.0,
        label=f"High-cut — Butterworth LP @ {format_hz(HIGH_CUT_HZ)}",
    )

    ax.annotate(
        "0 Hz: −∞ dB\n(DC fully blocked)",
        xy=(1.0, -14),
        xytext=(1.4, -46),
        fontsize=8,
        ha="left",
        arrowprops=dict(arrowstyle="->", color="k", lw=0.8),
    )
    ax.legend(loc="lower center", fontsize=8, ncol=2)

    fig.suptitle("Audio cleanup chain — filter frequency responses", fontsize=11)
    fig.tight_layout()
    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUTPUT_PATH, dpi=FIGURE_DPI, bbox_inches="tight")
    print(f"wrote {OUTPUT_PATH}")


if __name__ == "__main__":
    main()
