//! `bo2-dead-flags`: x86 flag stores the calling convention makes dead.
//!
//! The Skyrim port's lifter keeps x86 flags in C locals: the C compiler deletes every flag nobody reads, and flags
//! are never preserved across a call. Remill stores each arithmetic instruction's CF/PF/AF/ZF/SF/OF into State, and
//! LLVM keeps those stores whenever a call follows (the callee could read State). This pass deletes the stores that
//! are dead under the x86 calling convention: no compiler-generated caller reads flags a callee left, and no callee
//! reads flags its caller set, so at a return and at a call into lifted code the flags are dead.
//!
//! Backward liveness per lifted function (`ptr noalias %state`) over the six flag bytes:
//! - a load of a flag byte (or of a range covering one) makes it live;
//! - a return, `unreachable`, and a call into lifted code (sub_*, __remill_function_call, __remill_function_return*)
//!   make all flags dead before them;
//! - any other call that receives a State pointer (__remill_jump, __remill_missing_block, hyper calls, error...),
//!   any call not known to leave State alone, and a State load at an unknown offset keep all flags live;
//! - a store of a whole flag byte defines it: if the flag is dead right after, the store is deleted.
//! Calls carrying the bo2.state noalias scope (stream_scope_state.py: memory slow paths, lock stripes, preemption
//! tick), LLVM intrinsics without State arguments and the x87 libm calls leave the flags alone. DF (0x81b) is not
//! touched: string instructions in a callee read it. A function whose State pointer escapes is left alone.
//! The computations feeding a deleted store become dead and go with the next DCE (clang -O2, or instcombine).
//!
//! Hand-written asm breaks the convention in two ways, both found by scanning the exe (tools/llvm-pass/flagscan.py)
//! for a direct call followed by a flag reader before any flag writer, and for a call target that reads a flag before
//! writing one. Both are the CRT's x87 transcendental helpers: two loaders return "operand is NaN/Inf" in ZF
//! (FLAGS_OUT: their returns keep every flag live) and the bodies they fall into or are called next read that ZF
//! (FLAGS_IN: a call to them keeps every flag live). Two more call targets read flags on entry only to pushfd them
//! (CPUID-style probes); listed in FLAGS_IN too, it costs nothing. BO2_DEAD_FLAGS_OUT / BO2_DEAD_FLAGS_IN add names
//! (comma-separated) for another exe.
//!
//! BO2_DEAD_FLAGS_STATS=1 prints the offsets of every 1-byte State store seen near ArithFlags (layout check).

use std::collections::{BTreeMap, HashMap, HashSet};
use std::os::raw::c_char;

use llvm_plugin::inkwell::module::Module;
use llvm_plugin::{LlvmModulePass, ModuleAnalysisManager, PreservedAnalyses};
use llvm_sys::core::*;
use llvm_sys::prelude::*;
use llvm_sys::target::*;
use llvm_sys::{LLVMAtomicOrdering, LLVMOpcode, LLVMTypeKind};

use super::{is_state_scope_list, value_name};

pub struct DeadFlags;

impl LlvmModulePass for DeadFlags {
    fn run_pass(&self, module: &mut Module<'_>, _manager: &ModuleAnalysisManager) -> PreservedAnalyses {
        unsafe { dead_flags_module(module.as_mut_ptr()) }
    }
}

/// X86State's ArithFlags at 0x810: _0 cf _1 pf _2 af _3 zf _4 sf _5 df _6 of _7 _8. The six arithmetic flags:
const FLAG_OFFS: [i64; 6] = [0x811, 0x813, 0x815, 0x817, 0x819, 0x81d];
const ALL_FLAGS: u8 = 0x3f;

fn flags_in(lo: i64, hi: i64) -> u8 {
    let mut m = 0u8;
    for (i, f) in FLAG_OFFS.iter().enumerate() {
        if *f >= lo && *f < hi {
            m |= 1 << i;
        }
    }
    m
}

#[derive(Clone, Copy, PartialEq)]
enum Loc {
    Exact(i64),
    Range(i64, i64), // somewhere in [lo, hi): a variable index into an array of State
    Unknown,
}

#[derive(Clone, Copy)]
enum Effect {
    Use(u8),
    UseAll,
    Def(u8),
    Store1(u8), // a 1-byte store of exactly one flag: deleted when that flag is dead after it
    Kill,
}

/// Where a GEP off a State pointer at `base` points: exact when every index is constant, else the byte range of the
/// array the first variable index selects into.
unsafe fn gep_loc(dl: LLVMTargetDataRef, gep: LLVMValueRef, base: Loc) -> Loc {
    let base_off = match base {
        Loc::Exact(o) => o,
        // Already somewhere inside an array: stay conservative and keep that range.
        Loc::Range(lo, hi) => return Loc::Range(lo, hi),
        Loc::Unknown => return Loc::Unknown,
    };
    let n = LLVMGetNumOperands(gep) as u32;
    let mut ty = LLVMGetGEPSourceElementType(gep);
    let mut off: i64 = base_off;
    for i in 1..n {
        let idx = LLVMGetOperand(gep, i);
        let konst = !LLVMIsAConstantInt(idx).is_null();
        if i == 1 {
            if !konst {
                return Loc::Unknown;
            }
            off += LLVMConstIntGetSExtValue(idx) * LLVMABISizeOfType(dl, ty) as i64;
            continue;
        }
        match LLVMGetTypeKind(ty) {
            LLVMTypeKind::LLVMStructTypeKind => {
                if !konst {
                    return Loc::Unknown;
                }
                let c = LLVMConstIntGetSExtValue(idx) as u32;
                off += LLVMOffsetOfElement(dl, ty, c) as i64;
                ty = LLVMStructGetTypeAtIndex(ty, c);
            }
            LLVMTypeKind::LLVMArrayTypeKind | LLVMTypeKind::LLVMVectorTypeKind => {
                if !konst {
                    return Loc::Range(off, off + LLVMABISizeOfType(dl, ty) as i64);
                }
                ty = LLVMGetElementType(ty);
                off += LLVMConstIntGetSExtValue(idx) * LLVMABISizeOfType(dl, ty) as i64;
            }
            _ => return Loc::Unknown,
        }
    }
    Loc::Exact(off)
}

struct FlagWalk {
    dl: LLVMTargetDataRef,
    eff: HashMap<LLVMValueRef, Effect>,
    state_calls: HashSet<LLVMValueRef>,
    hist: BTreeMap<i64, u32>,
}

impl FlagWalk {
    /// Every use of a State pointer at `loc`. False: the pointer escapes (the function is left alone).
    unsafe fn walk(&mut self, p: LLVMValueRef, loc: Loc) -> bool {
        let mut u = LLVMGetFirstUse(p);
        while !u.is_null() {
            let user = LLVMGetUser(u);
            u = LLVMGetNextUse(u);
            if LLVMIsAInstruction(user).is_null() {
                return false;
            }
            match LLVMGetInstructionOpcode(user) {
                LLVMOpcode::LLVMGetElementPtr => {
                    if LLVMGetOperand(user, 0) != p {
                        return false;
                    }
                    let l = gep_loc(self.dl, user, loc);
                    if !self.walk(user, l) {
                        return false;
                    }
                }
                LLVMOpcode::LLVMLoad => {
                    let size = LLVMStoreSizeOfType(self.dl, LLVMTypeOf(user)) as i64;
                    let e = match loc {
                        Loc::Exact(o) => Effect::Use(flags_in(o, o + size)),
                        Loc::Range(lo, hi) => Effect::Use(flags_in(lo, hi)),
                        Loc::Unknown => Effect::UseAll,
                    };
                    self.merge(user, e);
                }
                LLVMOpcode::LLVMStore => {
                    if LLVMGetOperand(user, 0) == p {
                        return false; // the State pointer itself is stored: escapes
                    }
                    let size = LLVMStoreSizeOfType(self.dl, LLVMTypeOf(LLVMGetOperand(user, 0))) as i64;
                    if let Loc::Exact(o) = loc {
                        if size == 1 {
                            *self.hist.entry(o).or_insert(0) += 1;
                        }
                        let m = flags_in(o, o + size);
                        if m != 0 {
                            let simple = LLVMGetVolatile(user) == 0
                                && LLVMGetOrdering(user) == LLVMAtomicOrdering::LLVMAtomicOrderingNotAtomic;
                            let e = if size == 1 && simple { Effect::Store1(m) } else { Effect::Def(m) };
                            self.merge(user, e);
                        }
                    }
                    // A store at a range or an unknown offset may or may not write a flag: it neither defines nor uses.
                }
                LLVMOpcode::LLVMCall => {
                    self.state_calls.insert(user);
                }
                LLVMOpcode::LLVMICmp => {}
                _ => return false, // phi, select, ptrtoint, invoke...
            }
        }
        true
    }

    /// An instruction reached through two State pointers keeps the more conservative effect.
    fn merge(&mut self, i: LLVMValueRef, e: Effect) {
        let merged = match (self.eff.get(&i).copied(), e) {
            (None, e) => e,
            (Some(Effect::UseAll), _) | (_, Effect::UseAll) => Effect::UseAll,
            (Some(Effect::Use(a)), Effect::Use(b)) => Effect::Use(a | b),
            (Some(_), _) => Effect::UseAll,
        };
        self.eff.insert(i, merged);
    }
}

/// Lifted functions that return a status in the flags: their returns keep every flag live.
const FLAGS_OUT: [&str; 2] = ["sub_a32b85", "sub_a32bc8"];
/// Lifted functions that read a flag their caller set: a call to them keeps every flag live.
const FLAGS_IN: [&str; 10] = [
    "sub_a21418", "sub_a22448", "sub_a2bda8", "sub_a2be7d", "sub_a2c048", "sub_a2c468", "sub_a2c598", "sub_a32e98",
    "sub_9ddf40", "sub_af0fc9",
];

struct Exempt {
    out: HashSet<String>,
    inn: HashSet<String>,
}

fn exempt_set(fixed: &[&str], env: &str) -> HashSet<String> {
    let mut s: HashSet<String> = fixed.iter().map(|n| n.to_string()).collect();
    if let Ok(v) = std::env::var(env) {
        s.extend(v.split(',').map(|n| n.trim()).filter(|n| !n.is_empty()).map(|n| n.to_string()));
    }
    s
}

fn is_lifted_callee(name: &str) -> bool {
    name.starts_with("sub_") || name == "__remill_function_call" || name.starts_with("__remill_function_return")
}

thread_local! {
    static BARRIERS: std::cell::RefCell<BTreeMap<String, u32>> = std::cell::RefCell::new(BTreeMap::new());
}

fn note_barrier(name: &str) {
    BARRIERS.with(|b| *b.borrow_mut().entry(if name.is_empty() { "(indirect)".into() } else { name.into() }).or_insert(0) += 1);
}

unsafe fn call_effect(call: LLVMValueRef, receives_state: bool, kind_noalias: u32, ex: &Exempt) -> Effect {
    let callee = LLVMGetCalledValue(call);
    let named = !callee.is_null() && !LLVMIsAFunction(callee).is_null();
    let name = if named { value_name(callee) } else { String::new() };
    if named && ex.inn.contains(&name) {
        note_barrier(&name);
        return Effect::UseAll;
    }
    if receives_state {
        if named && is_lifted_callee(&name) {
            return Effect::Kill;
        }
        // memset into State writes, it never reads: no flag becomes live.
        if named && name.starts_with("llvm.memset.") {
            return Effect::Use(0);
        }
        note_barrier(&name);
        return Effect::UseAll;
    }
    // Remill's value helpers (no State argument; folded or inlined later by stream_inline_triv.py and the read/write
    // folds): __remill_undefined_* (an undefined value), __remill_compare_* (identity), __remill_fpu_exception_* and
    // the rounding stubs, the flag-computation identities, and the inline guest-memory accessors.
    const HELPERS: [&str; 7] = [
        "__remill_undefined_", "__remill_compare_", "__remill_fpu_", "__remill_flag_computation_",
        "__remill_read_memory_", "__remill_write_memory_", "__remill_atomic_",
    ];
    if named && HELPERS.iter().any(|h| name.starts_with(h)) {
        return Effect::Use(0);
    }
    if is_state_scope_list(LLVMGetMetadata(call, kind_noalias)) {
        return Effect::Use(0);
    }
    if named {
        if LLVMGetIntrinsicID(callee) != 0 {
            return Effect::Use(0);
        }
        const PURE: [&str; 28] = [
            "sqrtl", "atanl", "tanl", "log2l", "exp2l", "fmodl", "remainderl", "sqrt", "sqrtf", "atan2", "floor",
            "ceil", "trunc", "nearbyint", "rint", "fabs", "sin", "cos", "tan", "atan", "sinf", "cosf", "sinl", "cosl",
            "exp", "log", "log10", "pow",
        ];
        if PURE.iter().any(|p| *p == name) || name.starts_with("bo2_x87_") {
            return Effect::Use(0);
        }
        if is_lifted_callee(&name) {
            return Effect::Kill;
        }
    }
    note_barrier(&name);
    Effect::UseAll
}

fn apply(live: u8, e: Effect) -> u8 {
    match e {
        Effect::Use(m) => live | m,
        Effect::UseAll => ALL_FLAGS,
        Effect::Kill => 0,
        Effect::Def(m) | Effect::Store1(m) => live & !m,
    }
}

/// (flag stores seen, flag stores deleted), or None when the State pointer escapes.
unsafe fn dead_flags_function(f: LLVMValueRef, state: LLVMValueRef, dl: LLVMTargetDataRef, kind_noalias: u32,
                              ex: &Exempt, hist: &mut BTreeMap<i64, u32>) -> Option<(u32, u32)> {
    let at_ret = if ex.out.contains(&value_name(f)) { Effect::UseAll } else { Effect::Kill };
    let mut w = FlagWalk { dl, eff: HashMap::new(), state_calls: HashSet::new(), hist: BTreeMap::new() };
    if !w.walk(state, Loc::Exact(0)) {
        return None;
    }
    for (k, v) in &w.hist {
        *hist.entry(*k).or_insert(0) += v;
    }
    let mut blocks: Vec<LLVMBasicBlockRef> = Vec::new();
    let mut bb = LLVMGetFirstBasicBlock(f);
    while !bb.is_null() {
        blocks.push(bb);
        bb = LLVMGetNextBasicBlock(bb);
    }
    let bidx: HashMap<LLVMBasicBlockRef, usize> = blocks.iter().enumerate().map(|(i, b)| (*b, i)).collect();
    let mut succs: Vec<Vec<usize>> = vec![Vec::new(); blocks.len()];
    let mut code: Vec<Vec<(LLVMValueRef, Effect)>> = vec![Vec::new(); blocks.len()];
    let mut n_stores = 0u32;
    for (bi, blk) in blocks.iter().enumerate() {
        let t = LLVMGetBasicBlockTerminator(*blk);
        if !t.is_null() {
            for s in 0..LLVMGetNumSuccessors(t) {
                if let Some(j) = bidx.get(&LLVMGetSuccessor(t, s)) {
                    succs[bi].push(*j);
                }
            }
        }
        let mut i = LLVMGetFirstInstruction(*blk);
        while !i.is_null() {
            let e = match LLVMGetInstructionOpcode(i) {
                LLVMOpcode::LLVMRet => Some(at_ret),
                LLVMOpcode::LLVMUnreachable => Some(Effect::Kill),
                LLVMOpcode::LLVMCall => Some(call_effect(i, w.state_calls.contains(&i), kind_noalias, ex)),
                _ => w.eff.get(&i).copied(),
            };
            if let Some(e) = e {
                if let Effect::Store1(_) = e {
                    n_stores += 1;
                }
                code[bi].push((i, e));
            }
            i = LLVMGetNextInstruction(i);
        }
    }
    // Backward liveness to a fixed point.
    let mut live_in = vec![0u8; blocks.len()];
    loop {
        let mut changed = false;
        for bi in (0..blocks.len()).rev() {
            let mut live = 0u8;
            for s in &succs[bi] {
                live |= live_in[*s];
            }
            for (_, e) in code[bi].iter().rev() {
                live = apply(live, *e);
            }
            if live != live_in[bi] {
                live_in[bi] = live;
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    // The stores whose flag is dead right after them.
    let mut dead: Vec<LLVMValueRef> = Vec::new();
    for bi in 0..blocks.len() {
        let mut live = 0u8;
        for s in &succs[bi] {
            live |= live_in[*s];
        }
        for (i, e) in code[bi].iter().rev() {
            if let Effect::Store1(m) = *e {
                if live & m == 0 {
                    dead.push(*i);
                }
            }
            live = apply(live, *e);
        }
    }
    let n_dead = dead.len() as u32;
    for i in dead {
        LLVMInstructionEraseFromParent(i);
    }
    Some((n_stores, n_dead))
}

unsafe fn dead_flags_module(m: LLVMModuleRef) -> PreservedAnalyses {
    let llctx = LLVMGetModuleContext(m);
    let dl = LLVMGetModuleDataLayout(m);
    let kind_noalias = LLVMGetMDKindIDInContext(llctx, "noalias".as_ptr() as *const c_char, 7);
    let noalias_attr = LLVMGetEnumAttributeKindForName("noalias".as_ptr() as *const c_char, 7);
    let (mut funcs, mut skipped, mut stores, mut deleted) = (0u32, 0u32, 0u32, 0u32);
    let mut hist: BTreeMap<i64, u32> = BTreeMap::new();
    let ex = Exempt { out: exempt_set(&FLAGS_OUT, "BO2_DEAD_FLAGS_OUT"), inn: exempt_set(&FLAGS_IN, "BO2_DEAD_FLAGS_IN") };
    let mut f = LLVMGetFirstFunction(m);
    while !f.is_null() {
        let next = LLVMGetNextFunction(f);
        if LLVMIsDeclaration(f) == 0 && LLVMCountParams(f) > 0 {
            let p = LLVMGetParam(f, 0);
            let is_ptr = LLVMGetTypeKind(LLVMTypeOf(p)) == LLVMTypeKind::LLVMPointerTypeKind;
            let noalias = !LLVMGetEnumAttributeAtIndex(f, 1, noalias_attr).is_null();
            if is_ptr && noalias && value_name(p) == "state" {
                funcs += 1;
                match dead_flags_function(f, p, dl, kind_noalias, &ex, &mut hist) {
                    Some((s, d)) => {
                        stores += s;
                        deleted += d;
                    }
                    None => skipped += 1,
                }
            }
        }
        f = next;
    }
    eprintln!(
        "[bo2-dead-flags] {} functions ({} left alone: State escapes), {} of {} flag stores deleted",
        funcs, skipped, deleted, stores
    );
    if std::env::var("BO2_DEAD_FLAGS_STATS").map(|v| v == "1").unwrap_or(false) {
        let near: Vec<String> = hist
            .iter()
            .filter(|(o, _)| (0x800..0x830).contains(*o))
            .map(|(o, n)| format!("{:#x}:{}", o, n))
            .collect();
        eprintln!("[bo2-dead-flags] 1-byte State stores near ArithFlags: {}", near.join(" "));
        BARRIERS.with(|b| {
            let mut v: Vec<(String, u32)> = b.borrow().iter().map(|(k, n)| (k.clone(), *n)).collect();
            v.sort_by(|a, b| b.1.cmp(&a.1));
            let top: Vec<String> = v.iter().take(12).map(|(k, n)| format!("{}:{}", k, n)).collect();
            eprintln!("[bo2-dead-flags] calls that keep every flag live: {}", top.join(" "));
        });
    }
    if deleted > 0 {
        PreservedAnalyses::None
    } else {
        PreservedAnalyses::All
    }
}
