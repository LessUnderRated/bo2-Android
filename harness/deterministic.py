"""
deterministic — clock/RNG/input injection for the differential harness.

The harness must force determinism so divergence is meaningful (see
docs/HARNESS.md). Both the original (Unicorn) and recomp sides draw time,
randomness, and input from the SAME injected streams, seeded identically.

  - DeterministicClock:    GetTickCount / QueryPerformanceCounter semantics,
                           advanced in fixed increments per "tick".
  - DeterministicRNG:      xorshift32, reproducible from a seed.
  - InputReplay:           a captured input stream replayed deterministically.
  - DeterminismController: unified facade the runners query.

In the real harness the Win32 shim's GetTickCount/QueryPerformanceCounter/rand
implementations call into a DeterminismController instead of the real OS, so
both sides see byte-identical "environment" state.
"""
from __future__ import annotations
from dataclasses import dataclass, field
from typing import Iterator, List, Optional


class DeterministicClock:
    """Fake monotonic clock. Same seed -> same sequence on both sides."""

    def __init__(self, start_ms: int = 0, tick_ms: int = 16):
        self._ms = start_ms
        self._tick = tick_ms          # 16ms ~ 60fps frame

    def get_tick_count(self) -> int:           # Win32 GetTickCount
        return self._ms & 0xFFFFFFFF

    def query_performance_counter(self) -> int:  # Win32 QPC
        return self._ms * 10_000  # pretend 10kHz counter

    def advance(self, ticks: int = 1) -> None:
        self._ms += ticks * self._tick


class DeterministicRNG:
    """xorshift32 — reproducible from a 32-bit seed."""

    def __init__(self, seed: int):
        self._state = seed & 0xFFFFFFFF
        if self._state == 0:
            self._state = 0x9E3779B9  # avoid the 0 fixed-point

    def next(self) -> int:
        x = self._state
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= (x >> 17)
        x ^= (x << 5) & 0xFFFFFFFF
        self._state = x & 0xFFFFFFFF
        return self._state

    def next_float(self) -> float:
        return self.next() / 0x100000000

    def next_int(self, lo: int, hi: int) -> int:  # inclusive lo, exclusive hi
        return lo + (self.next() % (hi - lo))


@dataclass
class InputEvent:
    t_ms: int            # when it occurs
    kind: str            # "mouse_move", "key_down", "key_up", "mouse_down"...
    payload: tuple = ()


@dataclass
class InputReplay:
    events: List[InputEvent] = field(default_factory=list)
    _idx: int = 0

    def add(self, evt: InputEvent) -> None:
        self.events.append(evt)
        self.events.sort(key=lambda e: e.t_ms)

    def next_at(self, t_ms: int) -> Optional[InputEvent]:
        """Return the next event whose time has arrived by t_ms, or None."""
        if self._idx < len(self.events) and self.events[self._idx].t_ms <= t_ms:
            e = self.events[self._idx]
            self._idx += 1
            return e
        return None

    def reset(self) -> None:
        self._idx = 0


class DeterminismController:
    """Unified facade. Both sides hold an instance seeded identically."""

    def __init__(self, seed: int, start_ms: int = 0, tick_ms: int = 16):
        self.clock = DeterministicClock(start_ms=start_ms, tick_ms=tick_ms)
        self.rng = DeterministicRNG(seed=seed)
        self.input = InputReplay()

    def advance_frame(self) -> None:
        self.clock.advance(1)

    def snapshot(self) -> dict:
        return {
            "tick_count": self.clock.get_tick_count(),
            "qpc": self.clock.query_performance_counter(),
            "rng_state": self.rng._state,
            "input_idx": self.input._idx,
        }
