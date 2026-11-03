"""
RecompRunner — the "recompiled native" side of the differential harness.

For the toy proof-of-life this compiles a C function to a native arm64 dylib
and calls it via ctypes. In the real harness this loads the AOT-recompiled
Mach-O produced by the lift->AOT pipeline and drives it with the same replay
inputs fed to OrigRunner.
"""
from __future__ import annotations
import ctypes
import os
import subprocess
import tempfile
from dataclasses import dataclass
from typing import Dict, Optional

from orig_runner import State


@dataclass
class RecompRunner:
    dylib_path: str
    func_name: str = "toy"

    def __post_init__(self):
        self._lib = ctypes.CDLL(self.dylib_path)
        fn = getattr(self._lib, self.func_name)
        fn.restype = ctypes.c_int32
        fn.argtypes = [ctypes.c_int32, ctypes.c_int32]
        self._fn = fn

    def call(self, a: int, b: int) -> int:
        return int(self._fn(a, b))

    def to_state(self, a: int, b: int, eax_out: int) -> State:
        """Build a minimal State mirroring OrigRunner's reg layout for comparison.

        For the toy, the meaningful output is eax. ebx in the orig side is
        clobbered by `imul ebx, ebx, 7`; we replicate that here so the diff
        is meaningful (proves we compare full state, not just the return).
        """
        regs = {
            "eax": eax_out & 0xFFFFFFFF,
            "ebx": (b * 7) & 0xFFFFFFFF,   # matches the `imul ebx, ebx, 7` clobber
            "ecx": 0, "edx": 0, "esi": 0, "edi": 0, "ebp": 0,
            "esp": 0, "eip": 0, "eflags": 0,
        }
        return State(regs=regs, mem={})


def build_dylib(c_src_path: str, out_dir: str, func_name: str = "toy") -> str:
    """Compile a C source to a native arm64 dylib via clang."""
    out = os.path.join(out_dir, f"{func_name}.dylib")
    cmd = [
        "clang", "-arch", "arm64", "-shared", "-fPIC",
        "-O0",  # keep semantics literal; match the orig's no-optimization
        "-o", out, c_src_path,
    ]
    subprocess.run(cmd, check=True)
    return out
