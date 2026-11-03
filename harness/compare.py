"""
compare — snapshot diff engine for the differential harness.

Compares two State snapshots. Integer regs compared exactly. Float regs
(when added) compared with a ULP tolerance per docs/HARNESS.md. Reports the
first divergent field and an aggregate pass/fail.
"""
from __future__ import annotations
import struct
from dataclasses import dataclass
from typing import Dict, Tuple

from orig_runner import State


@dataclass
class CompareResult:
    passed: bool
    n_compared: int
    n_diff: int
    first_diff: Dict[str, object]

    def report(self) -> str:
        if self.passed:
            return f"PASS — {self.n_compared} fields matched, 0 diffs."
        return (f"FAIL — {self.n_diff}/{self.n_compared} fields differ. "
                f"First diff: {self.first_diff}")


def _float_ulp(a: float, b: float) -> int:
    """Distance in ULP between two float32-representable values."""
    ai, _ = struct.unpack(">i", struct.pack(">f", a))
    bi, _ = struct.unpack(">i", struct.pack(">f", b))
    return abs(ai - bi)


def compare(orig: State, recomp: State, ulp_tol: int = 4,
            fields: "set[str] | None" = None) -> CompareResult:
    """Diff two states.

    `fields`: if given, compare only those register names (plus all memory).
    If None, compare every register present. Per-tranche the harness declares
    which state is in-scope: result + clobbered regs for a ctypes stand-in,
    full GPRs + eip/esp/eflags when both sides are full emulations (the real
    harness recomp side, not a ctypes call).
    """
    diffs: Dict[str, object] = {}
    n = 0
    first_diff: Dict[str, object] = {}
    all_keys = set(orig.regs) & set(recomp.regs) if fields is None else set(fields)
    for k in all_keys:
        n += 1
        ov = orig.regs.get(k)
        rv = recomp.regs.get(k)
        if ov != rv:
            diffs[k] = {"orig": ov, "recomp": rv}
            if not first_diff:
                first_diff = {k: {"orig": ov, "recomp": rv}}
    for addr, b in orig.mem.items():
        n += 1
        ob = recomp.mem.get(addr)
        if b != ob:
            diffs[f"mem@{addr:#x}"] = {"orig": bytes(b), "recomp": bytes(ob) if ob else None}
            if not first_diff:
                first_diff = {f"mem@{addr:#x}": {"orig": bytes(b), "recomp": ob}}
    return CompareResult(passed=not diffs, n_compared=n, n_diff=len(diffs),
                         first_diff=first_diff)
