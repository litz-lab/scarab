#!/usr/bin/env python3
# Copyright 2026 University of California Santa Cruz
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""Generate the Ramulator 2 configs Scarab ships (src/ramulator2/*.yaml).

Ramulator 2's C++ side only reads fully expanded configs: the DRAM presets and
the timing-constraint table are computed by its Python DSL. This script builds
each config with that DSL (from the src/deps/ramulator2 submodule) and writes
it as YAML, with a header naming the Ramulator 2 commit and the command. Only
this script needs Python; building and running Scarab do not.

Each config also gets <name>.params: the Scarab params a run with it needs
(dram_backend, the ramulator_* organization and tCK the backend checks against
the YAML, bus width, scheduler). Append it to a PARAMS file or pass it on the
command line, together with --ramulator2_config <path to the .yaml>.

Usage (from the repository root):
  python3 src/ramulator2/gen_configs.py           # (re)write every config
  python3 src/ramulator2/gen_configs.py --check   # exit 1 if any file is stale

Scarab's frontend replaces the "frontend" section and sets the read/write
buffer sizes from ramulator_readq_entries / ramulator_writeq_entries when it
loads a config, so those values here are placeholders.
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
R2_DIR = os.path.normpath(os.path.join(HERE, "..", "deps", "ramulator2"))
sys.path.insert(0, os.path.join(R2_DIR, "python"))

import ramulator  # noqa: E402
from ramulator.export import dict_to_yaml  # noqa: E402


def generic_ddr(standard, org_preset, timing_preset, channels, **overrides):
    """GenericDDR controllers with FRFCFS, all-bank refresh, open rows and RoBaRaCoCh."""
    controllers = []
    for _ in range(channels):
        dram = getattr(ramulator.dram, standard)(org_preset=org_preset, timing_preset=timing_preset, **overrides)
        controllers.append(
            ramulator.controller.GenericDDR(
                dram=dram,
                scheduler=ramulator.scheduler.FRFCFS(),
                refresh_manager=ramulator.refresh_manager.AllBank(),
                row_policy=ramulator.row_policy.Open(),
                addr_mapper=ramulator.addr_mapper.RoBaRaCoCh(),
            )
        )
    return ramulator.memory_system.GenericDRAM(
        clock_ratio=1,
        controllers=controllers,
        channel_mapper=ramulator.channel_mapper.CacheLineInterleave(),
    )


def ddr5_4800an_16gb_x8(channels):
    """DDR5-4800AN, 16 Gb x8 chips, 1 rank. A Ramulator 2 DDR5 channel is one 32-bit subchannel,
    so 2 channels make one DIMM (64 data bits, 38.4 GB/s)."""
    return generic_ddr("DDR5", "DDR5_16Gb_x8", "DDR5_4800AN", channels, rank=1)


def ddr4_2400r_8gb_x8(channels):
    """DDR4-2400R, 8 Gb x8 chips, 1 rank: the configuration of Scarab's PARAMS files.

    nRFC and nREFI are pinned to Ramulator 1's values (420, 9360). Ramulator 2 derives
    421 and 9363 from its integer tCK_ps = 833, and the difference would show up as a
    model change that has nothing to do with the controller.
    """
    controllers = []
    for _ in range(channels):
        dram = ramulator.dram.DDR4(
            org_preset="DDR4_8Gb_x8",
            timing_preset="DDR4_2400R",
            rank=1,
            nRFC=420,
            nREFI=9360,
        )
        controllers.append(
            ramulator.controller.GenericDDR(
                dram=dram,
                scheduler=ramulator.scheduler.FRFCFS(),
                refresh_manager=ramulator.refresh_manager.AllBank(),
                row_policy=ramulator.row_policy.Open(),
                addr_mapper=ramulator.addr_mapper.RoBaRaCoCh(),
            )
        )
    return ramulator.memory_system.GenericDRAM(
        clock_ratio=1,
        controllers=controllers,
        channel_mapper=ramulator.channel_mapper.CacheLineInterleave(),
    )


CONFIGS = {
    "DDR4_2400R_8Gb_x8_1ch.yaml": lambda: ddr4_2400r_8gb_x8(1),
    "DDR4_2400R_8Gb_x8_2ch.yaml": lambda: ddr4_2400r_8gb_x8(2),
    "DDR5_4800AN_16Gb_x8_1ch.yaml": lambda: ddr5_4800an_16gb_x8(1),
    "DDR5_4800AN_16Gb_x8_2ch.yaml": lambda: ddr5_4800an_16gb_x8(2),
}


def scarab_params(memory_system):
    """The Scarab params matching a memory system: what Ramulator2Backend checks, plus the bus
    width the power model and perf_pred use."""
    dram = memory_system.controllers[0].dram
    org, timing = dram.resolve()
    levels = {name.lower() for name in type(dram).levels}
    # A DDR clock runs at half the transfer rate. The YAML keeps tCK in whole picoseconds,
    # so derive femtoseconds from the rate when the two agree (they do for DDR4 and DDR5).
    tck_fs = round(2e9 / timing["rate"]) if abs(2e6 / timing["rate"] - timing["tCK_ps"]) < 1 else timing["tCK_ps"] * 1000
    params = [
        ("dram_backend", "ramulator2"),
        ("ramulator_scheduling_policy", "FRFCFS"),
        ("ramulator_tCK", tck_fs),
        ("ramulator_channels", len(memory_system.controllers)),
    ]
    for level, param in (("rank", "ranks"), ("bankgroup", "bankgroups"), ("bank", "banks"),
                         ("row", "rows"), ("column", "cols")):
        if level in levels:
            params.append((f"ramulator_{param}", org[level]))
    params += [("ramulator_chip_width", org["dq"]), ("bus_width_in_bytes", org["channel_width"] // 8)]
    return "".join(f"--{name:<32} {value}\n" for name, value in params)


def r2_commit():
    def git(*args):
        return subprocess.run(["git", "-C", R2_DIR] + list(args), check=True, capture_output=True,
                              text=True).stdout.strip()
    if git("status", "--porcelain"):
        sys.exit(f"{R2_DIR} has local changes; configs must come from a pinned, clean commit")
    return git("rev-parse", "HEAD"), git("describe", "--tags", "--always")


def render(name, make, commit):
    """Returns {file name: text} for the config and its .params file."""
    sha, desc = commit
    memory_system = make()
    config = {
        # Replaced by Scarab's frontend at load time.
        "frontend": {"impl": "External", "clock_ratio": 1},
        "memory_system": memory_system.to_config(),
    }
    header = (
        f"# {{}}: generated by src/ramulator2/gen_configs.py. Do not edit; edit the script and rerun it.\n"
        f"# Ramulator 2: {sha} ({desc}), src/deps/ramulator2\n"
        f"# Command: python3 src/ramulator2/gen_configs.py\n"
    )
    params_name = name[: -len(".yaml")] + ".params"
    return {
        name: header.format(name) + dict_to_yaml(config),
        params_name: header.format(params_name)
        + f"# Scarab params for --ramulator2_config {name}\n"
        + scarab_params(memory_system),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="only check that the files are up to date")
    a = ap.parse_args()
    commit = r2_commit()
    stale = []
    for name, make in CONFIGS.items():
        for out_name, text in render(name, make, commit).items():
            path = os.path.join(HERE, out_name)
            old = open(path).read() if os.path.exists(path) else None
            if old == text:
                continue
            stale.append(out_name)
            if not a.check:
                with open(path, "w") as f:
                    f.write(text)
                print(f"wrote {path}")
    if a.check and stale:
        sys.exit("stale: " + ", ".join(stale))


if __name__ == "__main__":
    main()
