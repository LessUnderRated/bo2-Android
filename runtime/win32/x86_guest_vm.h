#pragma once

#include <unicorn/unicorn.h>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Original-byte x86 executor. One instance belongs to ONE guest thread, including
// its debug registers; callers must not share an instance between host threads.
// Not connected to production dispatch until its adapter is validated.
namespace bo2 {
class X86GuestVM {
public:
    // Blocks, not instructions: no hook runs on every instruction (Z-NATIF-15h).
    struct Limits { uint64_t blocks = 200000000; uint32_t milliseconds = 15000; };
    struct Result {
        bool returned = false;
        uint32_t pc = 0;
        uint64_t blocks = 0, exceptions = 0;
        // Of this run: translation-cache flushes (guest code changed outside the VM), memory-hook calls,
        // external calls; and the code pages the kept translations were made from.
        uint64_t flushes = 0, memoryCalls = 0, externals = 0, codePages = 0;
        std::string error;
    };
    using ExternalPredicate = std::function<bool(uint32_t)>;
    using ExternalCall = std::function<void(X86GuestVM&, uint32_t)>;

    X86GuestVM();
    ~X86GuestVM();
    X86GuestVM(const X86GuestVM&) = delete;
    X86GuestVM& operator=(const X86GuestVM&) = delete;
    void map(uint32_t address, uint64_t size, void* backing = nullptr);
    void read(uint32_t address, void* out, size_t size) const;
    void write(uint32_t address, const void* data, size_t size);
    uint32_t read32(uint32_t address) const;
    void write32(uint32_t address, uint32_t value);
    uint32_t reg(int id) const;
    void reg(int id, uint32_t value);
    // For vector/FPU registers, including future Remill adapters.
    void readReg(int id, void* out) const;
    void writeReg(int id, const void* data);
    // GDT page must already be mapped and reserved by the caller.
    void configureThread(uint32_t gdt, uint32_t teb, uint32_t stackLow, uint32_t stackHigh);
    // The predicate is consulted only at addresses inside `ranges` (first and last address of each).
    void externalCalls(ExternalPredicate predicate, ExternalCall call, const std::vector<std::pair<uint32_t,uint32_t>>& ranges);
    // Consulted at the start of every translated block, before it runs (interp.cpp): go on, hand pc to the external
    // adapter of externalCalls (the block is not run), or end the run before the block. prevEnd = end of the block
    // run just before (a `call` pushed it when [ESP] equals it). Off when not set.
    enum class BlockAction { Continue, External, Stop };
    using BlockPredicate = std::function<BlockAction(uint32_t pc, uint32_t prevEnd)>;
    void blockCalls(BlockPredicate predicate);
    // An external at `address` that never writes code: its call skips the copy and compare of the code pages.
    void codeSafeExternal(uint32_t address);
    uint32_t addVeh(bool first, uint32_t handler);
    bool removeVeh(uint32_t handle);
    void debugRegisters(const std::array<uint32_t, 6>& dr); // DR0..3,6,7
    const std::array<uint32_t, 6>& debugRegisters() const;
    // Called only by the external import adapter, outside uc_emu_start.
    void raiseException(uint32_t code, uint32_t flags, uint32_t count, uint32_t arguments);
    // ESP and other registers are supplied by caller. Stops BEFORE stopAddress.
    // A handler call uses a private sentinel, never replaces the guest's return.
    Result run(uint32_t entry, uint32_t stopAddress, Limits limits);
private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
} // namespace bo2
