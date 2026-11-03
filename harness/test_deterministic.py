"""
test_deterministic — prove two DeterminismControllers seeded identically
produce byte-identical clock + RNG + input streams across a simulated run.

This is what makes differential testing meaningful: the Win32 shim's
GetTickCount/QPC/rand and the input feed are injected, so divergence can
only come from the recomp itself, not from real time or scheduling.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from deterministic import DeterminismController, InputEvent


def run_trace(seed: int, n_frames: int) -> list:
    c = DeterminismController(seed=seed)
    # seed a deterministic input stream
    c.input.add(InputEvent(t_ms=16, kind="mouse_move", payload=(100, 100)))
    c.input.add(InputEvent(t_ms=48, kind="key_down", payload=(0x1B,)))  # ESC
    trace = []
    for _ in range(n_frames):
        evt = c.input.next_at(c.clock.get_tick_count())
        trace.append({
            "tick": c.clock.get_tick_count(),
            "qpc": c.clock.query_performance_counter(),
            "rng_i": c.rng.next_int(0, 1000),
            "rng_f": round(c.rng.next_float(), 6),
            "evt": (evt.kind, evt.payload) if evt else None,
        })
        c.advance_frame()
    return trace


def main():
    a = run_trace(seed=0xDEADBEEF, n_frames=6)
    b = run_trace(seed=0xDEADBEEF, n_frames=6)
    c = run_trace(seed=0xCAFEBABE, n_frames=6)  # different seed -> should differ

    same = (a == b)
    diff = (a != c)

    print("trace A (seed=0xDEADBEEF):")
    for t in a:
        print(f"  {t}")
    print("\ntrace B (seed=0xDEADBEEF):")
    for t in b:
        print(f"  {t}")

    failures = []
    if not same:
        failures.append("identical seeds produced different traces (expected equal)")
    if not diff:
        failures.append("different seeds produced identical traces (expected different)")

    print("\n" + ("PASS — identical seeds => identical traces; different seed => different."
                  if not failures else "FAIL — " + " | ".join(failures)))
    sys.exit(0 if not failures else 1)


if __name__ == "__main__":
    main()
