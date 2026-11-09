//! bo2-pass — LLVM 17 new-PM pass plugin for the Android fold.
//!
//!   opt -load-pass-plugin=target/release/libbo2_pass.so -passes='always-inline,bo2-state-promote,sroa' in.bc -o out.bc
//!   opt -load-pass-plugin=target/release/libbo2_pass.so -passes=bo2-dead-flags in.bc -o out.bc
//!
//! `bo2-dead-flags` (dead_flags.rs, in the fold with BO2_FOLD_PASS=1): deletes the State flag stores the x86 calling
//! convention makes dead. `bo2-state-promote` below is a prototype, not in the build.
//!
//! `bo2-state-promote`: the "registers as locals" model of the Skyrim Android port (ttom795/Skyrim_Android), for
//! Remill's lifted code, where the x86 registers, flags and PC live in the `State` struct (`ptr noalias %state`).
//!
//! In each lifted function:
//! - a SLOT is a (constant offset, type) of State that the function loads or stores. A slot is promoted only if no
//!   access of another shape touches its bytes (eax and al at one offset: neither is promoted), so a promoted slot is
//!   never seen half-written;
//! - promoted slots live in allocas (SROA turns them into SSA values): loaded once at entry, written back ("spilled")
//!   before anything that may read State and reloaded after anything that may write it;
//! - spill points: calls that receive %state or a pointer derived from it (other lifted functions, the runtime's
//!   dispatch/return/error entries...), calls to anything not known to leave State alone, State accesses at a
//!   dynamic offset (the x87 stack indexed by TOP), and returns. Calls carrying the `bo2.state` noalias scope that
//!   stream_scope_state.py puts on the memory slow paths, the lock stripes and the preemption tick are not spill
//!   points (they never touch State), and neither are LLVM intrinsics nor the libm calls of the x87 semantics;
//! - a spill-point call directly followed by `ret` (tail position, musttail included): spilled before, no reload,
//!   no spill at the `ret` (whatever the callee left in State must survive, and nothing may sit between a musttail
//!   call and its `ret`);
//! - if a State pointer escapes some other way (stored to memory, through a phi or select, ptrtoint), the function
//!   is left alone.
//! The new State loads and stores carry the `bo2.state` alias scope, so the noalias reasoning keeps working.
//!
//! BO2_PROMOTE_FLAGS_AT_RET_ONLY=1 (experiment): 1-byte slots in [0x810, 0x820) (the arithmetic flags) are spilled
//! only at returns and never reloaded after calls: x86 code does not read flags a callee left behind, but
//! hand-written asm could, so it is off by default.
//!
//! Bindings: llvm-plugin 0.6 (llvm17-0) for the pass-manager glue; the LLVM C API (llvm-sys 170) for the IR walk and
//! rewrite. The one C++-only helper needed, GEPOperator::accumulateConstantOffset, is rebuilt from the C API's
//! data-layout queries (LLVMOffsetOfElement, LLVMABISizeOfType, LLVMGetGEPSourceElementType). Dominance is not
//! needed: allocas plus SROA build the SSA form.

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::ffi::CString;
use std::os::raw::c_char;

use llvm_plugin::inkwell::module::Module;
use llvm_plugin::{LlvmModulePass, ModuleAnalysisManager, PassBuilder, PipelineParsing, PreservedAnalyses};
use llvm_sys::core::*;
use llvm_sys::prelude::*;
use llvm_sys::target::*;
use llvm_sys::{LLVMAtomicOrdering, LLVMOpcode, LLVMTypeKind};

mod dead_flags;
use dead_flags::DeadFlags;

#[llvm_plugin::plugin(name = "bo2-pass", version = "0.1")]
fn plugin_registrar(builder: &mut PassBuilder) {
    builder.add_module_pipeline_parsing_callback(|name, manager| {
        if name == "bo2-state-promote" {
            manager.add_pass(StatePromote);
            PipelineParsing::Parsed
        } else if name == "bo2-dead-flags" {
            manager.add_pass(DeadFlags);
            PipelineParsing::Parsed
        } else {
            PipelineParsing::NotParsed
        }
    });
}

struct StatePromote;

#[derive(Default)]
struct Stats {
    funcs: u32,
    promoted: u32,
    escaped: u32,
    slots: u32,
    accesses: u32,
    spill_points: u32,
}

#[derive(Clone, Copy)]
struct Access {
    inst: LLVMValueRef,
    off: i64,
    ty: LLVMTypeRef,
    size: u64,
}

// (offset, type) as the slot key; types are uniqued in a context, so their pointers compare.
type SlotKey = (i64, usize);

fn cstr(s: &str) -> CString {
    CString::new(s).unwrap()
}

unsafe fn value_name(v: LLVMValueRef) -> String {
    let mut len = 0usize;
    let p = LLVMGetValueName2(v, &mut len);
    if p.is_null() {
        return String::new();
    }
    String::from_utf8_lossy(std::slice::from_raw_parts(p as *const u8, len)).into_owned()
}

/// The `bo2.state` scope list as stream_scope_state.py writes it: !{!scope}, !scope = distinct !{!scope, !domain,
/// !"bo2.state"}.
unsafe fn is_state_scope_list(list: LLVMValueRef) -> bool {
    if list.is_null() || LLVMIsAMDNode(list).is_null() {
        return false;
    }
    let n = LLVMGetMDNodeNumOperands(list) as usize;
    let mut ops: Vec<LLVMValueRef> = vec![std::ptr::null_mut(); n];
    LLVMGetMDNodeOperands(list, ops.as_mut_ptr());
    for scope in ops {
        if scope.is_null() || LLVMIsAMDNode(scope).is_null() {
            continue;
        }
        let m = LLVMGetMDNodeNumOperands(scope) as usize;
        let mut sops: Vec<LLVMValueRef> = vec![std::ptr::null_mut(); m];
        LLVMGetMDNodeOperands(scope, sops.as_mut_ptr());
        for so in sops {
            if so.is_null() || LLVMIsAMDString(so).is_null() {
                continue;
            }
            let mut len = 0u32;
            let p = LLVMGetMDString(so, &mut len);
            if !p.is_null() && std::slice::from_raw_parts(p as *const u8, len as usize) == b"bo2.state" {
                return true;
            }
        }
    }
    false
}

struct Ctx {
    dl: LLVMTargetDataRef,
    llctx: LLVMContextRef,
    kind_scope: u32,
    kind_noalias: u32,
    scope: LLVMValueRef, // the bo2.state scope list, or null
    flags_ret_only: bool,
    gpr_only: bool,
}

/// Constant byte offset of a GEP whose indices are all constant integers (GEPOperator::accumulateConstantOffset).
unsafe fn gep_const_offset(dl: LLVMTargetDataRef, gep: LLVMValueRef) -> Option<i64> {
    let n = LLVMGetNumOperands(gep) as u32;
    let mut ty = LLVMGetGEPSourceElementType(gep);
    let mut off: i64 = 0;
    for i in 1..n {
        let idx = LLVMGetOperand(gep, i);
        if LLVMIsAConstantInt(idx).is_null() {
            return None;
        }
        let c = LLVMConstIntGetSExtValue(idx);
        if i == 1 {
            off += c * LLVMABISizeOfType(dl, ty) as i64;
            continue;
        }
        match LLVMGetTypeKind(ty) {
            LLVMTypeKind::LLVMStructTypeKind => {
                off += LLVMOffsetOfElement(dl, ty, c as u32) as i64;
                ty = LLVMStructGetTypeAtIndex(ty, c as u32);
            }
            LLVMTypeKind::LLVMArrayTypeKind | LLVMTypeKind::LLVMVectorTypeKind => {
                ty = LLVMGetElementType(ty);
                off += c * LLVMABISizeOfType(dl, ty) as i64;
            }
            _ => return None,
        }
    }
    Some(off)
}

unsafe fn is_simple(inst: LLVMValueRef) -> bool {
    LLVMGetVolatile(inst) == 0 && LLVMGetOrdering(inst) == LLVMAtomicOrdering::LLVMAtomicOrderingNotAtomic
}

struct Func<'a> {
    cx: &'a Ctx,
    state: LLVMValueRef,
    accesses: Vec<Access>,
    dynamic: Vec<LLVMValueRef>,
    state_calls: HashSet<LLVMValueRef>,
}

impl<'a> Func<'a> {
    /// Walk the users of a State pointer; `off` is its constant offset when `known`. False: State escapes.
    unsafe fn collect(&mut self, p: LLVMValueRef, off: i64, known: bool) -> bool {
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
                    let c = if known { gep_const_offset(self.cx.dl, user) } else { None };
                    if !self.collect(user, off + c.unwrap_or(0), c.is_some()) {
                        return false;
                    }
                }
                LLVMOpcode::LLVMLoad => {
                    if !known || !is_simple(user) {
                        self.dynamic.push(user);
                        continue;
                    }
                    let ty = LLVMTypeOf(user);
                    self.accesses.push(Access { inst: user, off, ty, size: LLVMStoreSizeOfType(self.cx.dl, ty) });
                }
                LLVMOpcode::LLVMStore => {
                    if LLVMGetOperand(user, 0) == p {
                        return false; // the pointer itself is stored: escapes
                    }
                    if !known || !is_simple(user) {
                        self.dynamic.push(user);
                        continue;
                    }
                    let ty = LLVMTypeOf(LLVMGetOperand(user, 0));
                    self.accesses.push(Access { inst: user, off, ty, size: LLVMStoreSizeOfType(self.cx.dl, ty) });
                }
                LLVMOpcode::LLVMCall => {
                    self.state_calls.insert(user);
                }
                LLVMOpcode::LLVMICmp => {}
                _ => return false, // phi, select, ptrtoint, invoke, ...
            }
        }
        true
    }

    unsafe fn is_safe_call(&self, call: LLVMValueRef) -> bool {
        if is_state_scope_list(LLVMGetMetadata(call, self.cx.kind_noalias)) {
            return true;
        }
        let callee = LLVMGetCalledValue(call);
        if callee.is_null() || LLVMIsAFunction(callee).is_null() {
            return false;
        }
        if LLVMGetIntrinsicID(callee) != 0 {
            return true; // State-pointer arguments make it a state call anyway
        }
        const PURE: [&str; 16] = [
            "sqrtl", "atanl", "tanl", "log2l", "exp2l", "fmodl", "remainderl", "sqrt", "sqrtf", "atan2", "floor",
            "ceil", "trunc", "nearbyint", "rint", "fabs",
        ];
        let name = value_name(callee);
        PURE.iter().any(|p| *p == name)
    }

    unsafe fn run(&mut self, f: LLVMValueRef, st: &mut Stats) -> bool {
        if !self.collect(self.state, 0, true) {
            st.escaped += 1;
            return false;
        }
        // Shapes and their byte ranges; any overlap between two different shapes disqualifies both.
        let mut shapes: BTreeMap<SlotKey, (i64, i64, LLVMTypeRef)> = BTreeMap::new();
        for a in &self.accesses {
            shapes.insert((a.off, a.ty as usize), (a.off, a.off + a.size as i64, a.ty));
        }
        let keys: Vec<SlotKey> = shapes.keys().copied().collect();
        let mut bad: BTreeSet<SlotKey> = BTreeSet::new();
        for i in 0..keys.len() {
            let (b0, e0, _) = shapes[&keys[i]];
            for j in i + 1..keys.len() {
                let (b1, e1, _) = shapes[&keys[j]];
                if b1 >= e0 {
                    break; // sorted by offset
                }
                if b1 < e0 && b0 < e1 {
                    bad.insert(keys[i]);
                    bad.insert(keys[j]);
                }
            }
        }
        let mut slots: BTreeMap<SlotKey, (LLVMTypeRef, LLVMValueRef, LLVMValueRef)> = BTreeMap::new(); // ty, alloca, ptr
        for (k, (_, _, ty)) in &shapes {
            if bad.contains(k) {
                continue;
            }
            // BO2_PROMOTE_GPR_ONLY=1 (experiment): only 4-byte integer slots of the GPR/PC block of X86State.
            if self.cx.gpr_only
                && !((0x840..0x9b0).contains(&k.0) && LLVMGetTypeKind(*ty) == LLVMTypeKind::LLVMIntegerTypeKind
                    && LLVMStoreSizeOfType(self.cx.dl, *ty) == 4)
            {
                continue;
            }
            match LLVMGetTypeKind(*ty) {
                LLVMTypeKind::LLVMIntegerTypeKind
                | LLVMTypeKind::LLVMFloatTypeKind
                | LLVMTypeKind::LLVMDoubleTypeKind
                | LLVMTypeKind::LLVMPointerTypeKind
                | LLVMTypeKind::LLVMVectorTypeKind => {
                    slots.insert(*k, (*ty, std::ptr::null_mut(), std::ptr::null_mut()));
                }
                _ => {}
            }
        }
        if slots.is_empty() {
            return false;
        }
        let b = LLVMCreateBuilderInContext(self.cx.llctx);
        let entry = LLVMGetEntryBasicBlock(f);
        LLVMPositionBuilderBefore(b, LLVMGetFirstInstruction(entry));
        let i8t = LLVMInt8TypeInContext(self.cx.llctx);
        let i64t = LLVMInt64TypeInContext(self.cx.llctx);
        let n_slot = cstr("bo2.slot");
        let n_sp = cstr("bo2.sp");
        let n_v = cstr("");
        for ((off, _), (ty, al, ptr)) in slots.iter_mut() {
            *al = LLVMBuildAlloca(b, *ty, n_slot.as_ptr());
            let mut idx = [LLVMConstInt(i64t, *off as u64, 1)];
            *ptr = LLVMBuildInBoundsGEP2(b, i8t, self.state, idx.as_mut_ptr(), 1, n_sp.as_ptr());
        }
        let tag = |i: LLVMValueRef| {
            if !self.cx.scope.is_null() {
                LLVMSetMetadata(i, self.cx.kind_scope, self.cx.scope);
            }
        };
        // Entry: every slot loaded from State once.
        for (_, (ty, al, ptr)) in &slots {
            let v = LLVMBuildLoad2(b, *ty, *ptr, n_v.as_ptr());
            tag(v);
            LLVMBuildStore(b, v, *al);
        }
        // Accesses to promoted slots go to the allocas.
        for a in &self.accesses {
            if let Some((_, al, _)) = slots.get(&(a.off, a.ty as usize)) {
                let idx = if LLVMGetInstructionOpcode(a.inst) == LLVMOpcode::LLVMLoad { 0 } else { 1 };
                LLVMSetOperand(a.inst, idx, *al);
                LLVMSetMetadata(a.inst, self.cx.kind_scope, std::ptr::null_mut());
                LLVMSetMetadata(a.inst, self.cx.kind_noalias, std::ptr::null_mut());
                st.accesses += 1;
            }
        }
        // Sync points: spill-point calls, State accesses at a dynamic offset, returns.
        let mut calls: Vec<LLVMValueRef> = Vec::new();
        let mut rets: Vec<LLVMValueRef> = Vec::new();
        let mut blocks: Vec<LLVMBasicBlockRef> = Vec::new();
        let mut bb = LLVMGetFirstBasicBlock(f);
        while !bb.is_null() {
            blocks.push(bb);
            let mut i = LLVMGetFirstInstruction(bb);
            while !i.is_null() {
                match LLVMGetInstructionOpcode(i) {
                    LLVMOpcode::LLVMCall => {
                        if self.state_calls.contains(&i) || !self.is_safe_call(i) {
                            calls.push(i);
                        }
                    }
                    LLVMOpcode::LLVMRet => rets.push(i),
                    _ => {}
                }
                i = LLVMGetNextInstruction(i);
            }
            bb = LLVMGetNextBasicBlock(bb);
        }
        let tail_calls: HashSet<LLVMValueRef> = calls
            .iter()
            .copied()
            .filter(|c| {
                let n = LLVMGetNextInstruction(*c);
                !n.is_null() && LLVMGetInstructionOpcode(n) == LLVMOpcode::LLVMRet
            })
            .collect();
        let call_set: HashSet<LLVMValueRef> = calls.iter().copied().collect();
        let dyn_set: HashSet<LLVMValueRef> = self.dynamic.iter().copied().collect();
        // A return right after a tail call: whatever the callee left in State survives, nothing is spilled there.
        let ret_set: HashSet<LLVMValueRef> = rets
            .iter()
            .copied()
            .filter(|r| {
                let prev = LLVMGetPreviousInstruction(*r);
                prev.is_null() || !tail_calls.contains(&prev)
            })
            .collect();
        // Slot indices, the promoted stores, and the flag mask (BO2_PROMOTE_FLAGS_AT_RET_ONLY).
        let keys: Vec<SlotKey> = slots.keys().copied().collect();
        let nw = (keys.len() + 63) / 64;
        let slot_index: BTreeMap<SlotKey, usize> = keys.iter().enumerate().map(|(i, k)| (*k, i)).collect();
        let mut store_slot: HashMap<LLVMValueRef, usize> = HashMap::new();
        for a in &self.accesses {
            if LLVMGetInstructionOpcode(a.inst) == LLVMOpcode::LLVMStore {
                if let Some(i) = slot_index.get(&(a.off, a.ty as usize)) {
                    store_slot.insert(a.inst, *i);
                }
            }
        }
        let mut flag_mask = vec![0u64; nw];
        if self.cx.flags_ret_only {
            for (i, k) in keys.iter().enumerate() {
                let ty = slots[k].0;
                if (0x810..0x820).contains(&k.0) && LLVMStoreSizeOfType(self.cx.dl, ty) == 1 {
                    flag_mask[i / 64] |= 1u64 << (i % 64);
                }
            }
        }
        // May-dirty dataflow: a slot is dirty where its alloca may hold a value newer than State. A sync point
        // stores the dirty slots and leaves everything clean (calls and dynamic stores are followed by a reload; with
        // BO2_PROMOTE_FLAGS_AT_RET_ONLY the flags stay dirty until a return). Spilling a clean slot would only
        // rewrite the value State already holds, so only dirty slots are stored, and clean values need not stay live
        // up to the next call.
        let bidx: HashMap<LLVMBasicBlockRef, usize> = blocks.iter().enumerate().map(|(i, b)| (*b, i)).collect();
        let mut preds: Vec<Vec<usize>> = vec![Vec::new(); blocks.len()];
        for (i, blk) in blocks.iter().enumerate() {
            let t = LLVMGetBasicBlockTerminator(*blk);
            if t.is_null() {
                continue;
            }
            for s in 0..LLVMGetNumSuccessors(t) {
                if let Some(j) = bidx.get(&LLVMGetSuccessor(t, s)) {
                    preds[*j].push(i);
                }
            }
        }
        let mut dirty_at: HashMap<LLVMValueRef, Vec<u64>> = HashMap::new();
        let mut out: Vec<Vec<u64>> = vec![vec![0u64; nw]; blocks.len()];
        loop {
            let mut changed = false;
            for (bi, blk) in blocks.iter().enumerate() {
                let mut d = vec![0u64; nw];
                for p in &preds[bi] {
                    for w in 0..nw {
                        d[w] |= out[*p][w];
                    }
                }
                let mut i = LLVMGetFirstInstruction(*blk);
                while !i.is_null() {
                    if let Some(si) = store_slot.get(&i) {
                        d[si / 64] |= 1u64 << (si % 64);
                    } else if call_set.contains(&i) || dyn_set.contains(&i) || ret_set.contains(&i) {
                        dirty_at.insert(i, d.clone());
                        let at_ret = ret_set.contains(&i);
                        for w in 0..nw {
                            d[w] = if at_ret { 0 } else { d[w] & flag_mask[w] };
                        }
                    }
                    i = LLVMGetNextInstruction(i);
                }
                if d != out[bi] {
                    out[bi] = d;
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
        let spill = |at: LLVMValueRef, at_ret: bool| {
            let d = match dirty_at.get(&at) {
                Some(d) => d.clone(),
                None => return,
            };
            LLVMPositionBuilderBefore(b, at);
            for (i, k) in keys.iter().enumerate() {
                let bit = 1u64 << (i % 64);
                if d[i / 64] & bit == 0 || (!at_ret && flag_mask[i / 64] & bit != 0) {
                    continue;
                }
                let (ty, al, ptr) = slots[k];
                let v = LLVMBuildLoad2(b, ty, al, n_v.as_ptr());
                tag(LLVMBuildStore(b, v, ptr));
            }
        };
        let reload = |after: LLVMValueRef| {
            let next = LLVMGetNextInstruction(after);
            if next.is_null() {
                return;
            }
            LLVMPositionBuilderBefore(b, next);
            for (i, k) in keys.iter().enumerate() {
                if flag_mask[i / 64] & (1u64 << (i % 64)) != 0 {
                    continue;
                }
                let (ty, al, ptr) = slots[k];
                let v = LLVMBuildLoad2(b, ty, ptr, n_v.as_ptr());
                tag(v);
                LLVMBuildStore(b, v, al);
            }
        };
        for c in &calls {
            spill(*c, false);
            st.spill_points += 1;
            let n = LLVMGetNextInstruction(*c);
            let ends = n.is_null() || LLVMGetInstructionOpcode(n) == LLVMOpcode::LLVMUnreachable;
            if !tail_calls.contains(c) && !ends {
                reload(*c);
            }
        }
        for d in &self.dynamic {
            spill(*d, false);
            st.spill_points += 1;
            if LLVMGetInstructionOpcode(*d) == LLVMOpcode::LLVMStore {
                reload(*d);
            }
        }
        for r in &rets {
            if ret_set.contains(r) {
                spill(*r, true);
                st.spill_points += 1;
            }
        }
        LLVMDisposeBuilder(b);
        st.promoted += 1;
        st.slots += slots.len() as u32;
        true
    }
}

impl LlvmModulePass for StatePromote {
    fn run_pass(&self, module: &mut Module<'_>, _manager: &ModuleAnalysisManager) -> PreservedAnalyses {
        unsafe { promote_module(module.as_mut_ptr()) }
    }
}

unsafe fn promote_module(m: LLVMModuleRef) -> PreservedAnalyses {
    let llctx = LLVMGetModuleContext(m);
    let dl = LLVMGetModuleDataLayout(m);
    let k = |s: &str| LLVMGetMDKindIDInContext(llctx, s.as_ptr() as *const c_char, s.len() as u32);
    let kind_scope = k("alias.scope");
    let kind_noalias = k("noalias");
    let noalias_attr = LLVMGetEnumAttributeKindForName("noalias".as_ptr() as *const c_char, 7);
    let flags_ret_only = std::env::var("BO2_PROMOTE_FLAGS_AT_RET_ONLY").map(|v| v == "1").unwrap_or(false);
    let gpr_only = std::env::var("BO2_PROMOTE_GPR_ONLY").map(|v| v == "1").unwrap_or(false);
    // The bo2.state scope list, from any instruction that carries it.
    let mut scope: LLVMValueRef = std::ptr::null_mut();
    let mut f = LLVMGetFirstFunction(m);
    'find: while !f.is_null() {
        let mut bb = LLVMGetFirstBasicBlock(f);
        while !bb.is_null() {
            let mut i = LLVMGetFirstInstruction(bb);
            while !i.is_null() {
                let md = LLVMGetMetadata(i, kind_scope);
                if is_state_scope_list(md) {
                    scope = md;
                    break 'find;
                }
                i = LLVMGetNextInstruction(i);
            }
            bb = LLVMGetNextBasicBlock(bb);
        }
        f = LLVMGetNextFunction(f);
    }
    let cx = Ctx { dl, llctx, kind_scope, kind_noalias, scope, flags_ret_only, gpr_only };
    let mut st = Stats::default();
    let mut changed = false;
    let mut f = LLVMGetFirstFunction(m);
    while !f.is_null() {
        let next = LLVMGetNextFunction(f);
        if LLVMIsDeclaration(f) == 0 && LLVMCountParams(f) > 0 {
            let p = LLVMGetParam(f, 0);
            let is_ptr = LLVMGetTypeKind(LLVMTypeOf(p)) == LLVMTypeKind::LLVMPointerTypeKind;
            let noalias = !LLVMGetEnumAttributeAtIndex(f, 1, noalias_attr).is_null();
            if is_ptr && noalias && value_name(p) == "state" {
                st.funcs += 1;
                let mut fx = Func { cx: &cx, state: p, accesses: Vec::new(), dynamic: Vec::new(), state_calls: HashSet::new() };
                changed |= fx.run(f, &mut st);
            }
        }
        f = next;
    }
    let msg = format!(
        "[bo2-state-promote] {}/{} functions, {} slots, {} accesses moved to locals, {} spill points, {} left alone \
         (State escapes){}{}\n",
        st.promoted,
        st.funcs,
        st.slots,
        st.accesses,
        st.spill_points,
        st.escaped,
        if scope.is_null() { ", NO bo2.state scope found" } else { "" },
        if flags_ret_only { ", flags spilled at returns only" } else { "" }
    );
    eprint!("{}", msg);
    if changed {
        PreservedAnalyses::None
    } else {
        PreservedAnalyses::All
    }
}
