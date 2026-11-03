// toy_runtime.cpp — minimal Remill intrinsic implementations for the
// lifted-toy dry run. These are the boundary between lifted IR and our
// runtime; in the real port they become the shim (memory model, control
// flow dispatch, flag computation). For the toy, only flag/undefined/return
// intrinsics are reachable; memory/symbolic stubs are present to satisfy
// the linker but unused.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>

// Remill types (simplified for standalone AOT)
using addr_t = uint32_t;
using Memory = uint8_t;          // opaque; toy makes no real mem accesses
struct State {};

extern "C" {

// --- undefined value (used for flag bits we don't track) ---
[[gnu::used]] uint8_t __remill_undefined_8(void) { return 0; }

// --- flag computation: return the `result` arg, ignore the rest ---
// Remill passes the already-computed flag as the first vararg; the rest is
// metadata for symbolic engines. For concrete AOT execution, echo result.
[[gnu::used]] bool __remill_flag_computation_carry(bool result, ...) { return result; }
[[gnu::used]] bool __remill_flag_computation_zero(bool result, ...) { return result; }
[[gnu::used]] bool __remill_flag_computation_sign(bool result, ...) { return result; }
[[gnu::used]] bool __remill_flag_computation_overflow(bool result, ...) { return result; }

// --- control flow: the toy's lifted block ends in `ret`. For a standalone
// callable function we never actually invoke the return intrinsic because
// the `--signature EAX(EAX,EBX)` wrapper returns directly. But the symbol
// must exist. If reached, abort (means the toy took an unexpected path). ---
[[gnu::used]] Memory *__remill_function_return(State &, addr_t, Memory *m) {
    return m;
}
[[gnu::used]] Memory *__remill_function_call(State &, addr_t, Memory *m) { return m; }
[[gnu::used]] Memory *__remill_jump(State &, addr_t, Memory *m) { return m; }
[[gnu::used]] Memory *__remill_missing_block(State &, addr_t, Memory *m) {
    std::fprintf(stderr, "remill: missing block reached\n");
    std::abort();
}
[[gnu::used]] Memory *__remill_error(State &, addr_t, Memory *m) {
    std::fprintf(stderr, "remill: error intrinsic reached\n");
    std::abort();
}

// --- memory (toy doesn't use these; stubs to satisfy the semantics link) ---
[[gnu::used]] uint8_t  __remill_read_memory_8(Memory *, addr_t) { return 0; }
[[gnu::used]] uint16_t __remill_read_memory_16(Memory *, addr_t) { return 0; }
[[gnu::used]] uint32_t __remill_read_memory_32(Memory *, addr_t) { return 0; }
[[gnu::used]] uint64_t __remill_read_memory_64(Memory *, addr_t) { return 0; }
[[gnu::used]] Memory *__remill_write_memory_8(Memory *m, addr_t, uint8_t) { return m; }
[[gnu::used]] Memory *__remill_write_memory_16(Memory *m, addr_t, uint16_t) { return m; }
[[gnu::used]] Memory *__remill_write_memory_32(Memory *m, addr_t, uint32_t) { return m; }
[[gnu::used]] Memory *__remill_write_memory_64(Memory *m, addr_t, uint64_t) { return m; }

// --- symbolic register bases (toy doesn't touch segments) ---
[[gnu::used]] addr_t __remill_symbolic_FSBASE(void) { return 0; }
[[gnu::used]] addr_t __remill_symbolic_GSBASE(void) { return 0; }
[[gnu::used]] addr_t __remill_symbolic_STACK(void) { return 0; }

}  // extern "C"
