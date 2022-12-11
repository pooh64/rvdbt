#include "dbt/qjit/qjit.h"
#include "dbt/execute.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <sys/types.h>
#include <type_traits>

namespace dbt::qjit
{

HELPER_ASM BranchSlot *trampoline_host_to_qjit(CPUState *state, void *vmem, void *tc_ptr)
{
	__asm("pushq	%rbp\n\t"
	      "movq	%rsp, %rbp\n\t"
	      "pushq	%rbx\n\t"
	      "pushq	%r12\n\t"
	      "pushq	%r13\n\t"
	      "pushq	%r14\n\t"
	      "pushq	%r15\n\t"
	      "movq 	%rdi, %r13\n\t" // STATE
	      "movq	%rsi, %r12\n\t" // MEMBASE
	      "subq	$248, %rsp\n\t" // RegAlloc::frame_size
	      "jmpq	*%rdx\n\t");	// tc_ptr
}

HELPER_ASM void trampoline_qjit_to_host()
{
	__asm("addq	$248, %rsp\n\t" // RegAlloc::frame_size
	      "popq	%r15\n\t"
	      "popq	%r14\n\t"
	      "popq	%r13\n\t"
	      "popq	%r12\n\t"
	      "popq	%rbx\n\t"
	      "popq	%rbp\n\t"
	      "retq	\n\t");
}
static_assert(RegAlloc::frame_size == 248);

HELPER_ASM void stub_link_branch()
{
	__asm("popq	%rdi\n\t"
	      "callq	helper_link_branch@plt\n\t"
	      "jmpq	*%rdx\n\t");
}

HELPER _RetPair helper_link_branch(void *p_slot)
{
	auto *slot = (BranchSlot *)((uptr)p_slot - sizeof(BranchSlotPatch::Call64Abs));
	auto found = tcache::Lookup(slot->gip);
	if (likely(found)) {
		slot->Link(found->tcode.ptr);
		return {slot, found->tcode.ptr};
	}
	return {slot, (void *)trampoline_qjit_to_host};
}

HELPER _RetPair helper_brind(CPUState *state, u32 gip)
{
	state->ip = gip;
	auto *found = tcache::Lookup(gip);
	if (likely(found)) {
		tcache::OnBrind(found);
		return {nullptr, (void *)found->tcode.ptr};
	}
	return {nullptr, (void *)trampoline_qjit_to_host};
}

HELPER void helper_raise()
{
	RaiseTrap();
}

HELPER_ASM void stub_trace()
{
	__asm("pushq	%rax\n\t"
	      "pushq	%rcx\n\t"
	      "pushq	%rdx\n\t"
	      "pushq	%rbx\n\t"
	      "pushq	%rsp\n\t"
	      "pushq	%rbp\n\t"
	      "pushq	%rsi\n\t"
	      "pushq	%rdi\n\t"
	      "pushq	%r8\n\t"
	      "pushq	%r9\n\t"
	      "pushq	%r10\n\t"
	      "pushq	%r11\n\t"
	      "pushq	%r12\n\t"
	      "pushq	%r13\n\t"
	      "pushq	%r14\n\t"
	      "pushq	%r15\n\t"
	      "movq	%r13, %rdi\n\t" // STATE
	      "subq	$8, %rsp\n\t"
	      "callq	helper_dump_trace@plt\n\t"
	      "addq	$8, %rsp\n\t"
	      "popq	%r15\n\t"
	      "popq	%r14\n\t"
	      "popq	%r13\n\t"
	      "popq	%r12\n\t"
	      "popq	%r11\n\t"
	      "popq	%r10\n\t"
	      "popq	%r9\n\t"
	      "popq	%r8\n\t"
	      "popq	%rdi\n\t"
	      "popq	%rsi\n\t"
	      "popq	%rbp\n\t"
	      "popq	%rsp\n\t"
	      "popq	%rbx\n\t"
	      "popq	%rdx\n\t"
	      "popq	%rcx\n\t"
	      "popq	%rax\n\t"
	      "retq	\n\t");
}

HELPER void helper_dump_trace(CPUState *state)
{
	state->DumpTrace("entry");
}

Codegen::Codegen()
{
	if (jcode.init(jrt.environment())) {
		Panic();
	}
	jcode.attach(j._emitter());
	j.setErrorHandler(&jerr);
	jcode.setErrorHandler(&jerr);
}

void Codegen::SetupCtx(QuickJIT *ctx_)
{
	ctx = ctx_;
	auto ra = ctx->ra;
	// ra->ascope->fixed.Set(RegAlloc::PReg(asmjit::x86::Gp::kIdBp));
	ra->fixed.Set(RegAlloc::PReg(TMP1C));

	ra->state_base = ra->AllocVRegFixed(RegAlloc::VReg::Type::I64, RegAlloc::PReg(STATE));
	ra->frame_base = ra->AllocVRegFixed(RegAlloc::VReg::Type::I64, RegAlloc::PReg(SP));
	if (mmu::base) {
		ra->mem_base = ra->AllocVRegFixed(RegAlloc::VReg::Type::I64, RegAlloc::PReg(MEMBASE));
	}
#ifdef CONFIG_USE_STATEMAPS
	ra->state_map = ra->AllocVRegFixed("statemap", RegAlloc::VReg::Type::I64,
					   RegAlloc::PReg(asmjit::x86::Gp::kIdR13));
#endif
}

void Codegen::Prologue()
{
	tcache::OnTranslate(ctx->tb);

#ifdef CONFIG_DUMP_TRACE
	j.mov(ctx->vreg_ip->GetSpill(), ctx->tb->ip);
	j.call(stub_trace);
#endif
}

void Codegen::Epilogue()
{
	// j.int3();
}

void Codegen::EmitCode()
{
	auto *tb = ctx->tb;
	jcode.flatten();
	jcode.resolveUnresolvedLinks();

	size_t jit_sz = jcode.codeSize();
	tb->tcode.size = jit_sz;
	tb->tcode.ptr = tcache::AllocateCode(jit_sz, 8);
	if (tb->tcode.ptr == nullptr) {
		Panic();
	}

	jcode.relocateToBase((uintptr_t)tb->tcode.ptr);
	if (jit_sz < jcode.codeSize()) {
		Panic();
	}
	jcode.copyFlattenedData(tb->tcode.ptr, tb->tcode.size);
	tb->tcode.size = jcode.codeSize();
}

void Codegen::DumpCode()
{
	if (!log_qjit.enabled()) {
		return;
	}
	auto &tcode = ctx->tb->tcode;
	size_t sz = tcode.size;
	auto p = (u8 *)tcode.ptr;

	std::array<char, 4096> buf;

	if (sz * 2 + 1 > buf.size()) {
		log_qjit("jitcode is too long for dump");
		return;
	}

	for (size_t i = 0; i < sz; ++i) {
		sprintf(&buf[2 * i], "%02x", p[i]);
	}
	log_qjit.write(buf.data());
}

#ifdef CONFIG_USE_STATEMAPS
void QuickJIT::CreateStateMap()
{
	StateMap sm{};
	u8 map_idx = 0;
	for (RegAlloc::PReg p = 0; p < RegAlloc::N_PREGS; ++p) {
		if (ra->fixed.Test(p)) {
			continue;
		}
		auto *v = ra->p2v[p];
		if (!v || !v->has_statemap) {
			continue;
		}
		uintptr_t v_idx = ((uintptr_t)v - (uintptr_t)vreg_gpr.data()) / sizeof(*v);
		if (v_idx < vreg_gpr.size()) {
			sm.Set(map_idx, v_idx);
		}
	}

	if (active_sm.first && active_sm.second.data == sm.data) {
		return;
	}
	active_sm = {true, sm};
	cg.j.mov(ra->state_map->GetPReg(), asmjit::imm(sm.data));
}
#endif

void Codegen::Call(asmjit::Operand const *args, u8 nargs)
{
	ctx->ra->CallOp(); // may spill, but not modify pregs in args

	switch (nargs) {
	case 4:
		j.emit(asmjit::x86::Inst::kIdMov, asmjit::x86::rdx, args[3]);
	case 3:
		j.emit(asmjit::x86::Inst::kIdMov, asmjit::x86::rsi, args[2]);
	case 2:
		j.emit(asmjit::x86::Inst::kIdMov, asmjit::x86::rdi, args[1]);
	case 1:
		break;
	default:
		assert(0);
	}

	auto &callee = args[0];
	if (!callee.isPhysReg()) {
		assert(callee.as<asmjit::x86::Gp>().id() == asmjit::x86::Gp::kIdAx);
	}
	j.emit(asmjit::x86::Inst::kIdCall, callee);
}

void Codegen::BranchTBDir(u32 ip)
{
	tcache::OnTranslateBr(ctx->tb, ip);
	ctx->ra->BlockBoundary();

	j.embedUInt8(0, sizeof(BranchSlot));
	auto *slot = (BranchSlot *)(j.bufferPtr() - sizeof(BranchSlot));
	slot->gip = ip;
	slot->Reset();
}

void Codegen::x86Cmp(asmjit::x86::CondCode *cc, asmjit::Operand lhs, asmjit::Operand rhs)
{
	assert(lhs.isRegOrMem() || rhs.isRegOrMem());
	if (!lhs.isRegOrMem()) {
		*cc = asmjit::x86::reverseCond(*cc);
		std::swap(lhs, rhs);
	}
	j.emit(asmjit::x86::Inst::kIdCmp, lhs, rhs);
}

void Codegen::SetCC(asmjit::x86::CondCode cc, asmjit::Operand rd, asmjit::Operand lhs, asmjit::Operand rhs)
{
	x86Cmp(&cc, lhs, rhs);
	j.emit(asmjit::x86::Inst::setccFromCond(cc), rd);
	assert(rd.isPhysReg());
	auto prd = rd.as<asmjit::x86::Gp>();
	j.emit(asmjit::x86::Inst::kIdMovzx, prd, prd.r8());
}

void Codegen::BranchCC(asmjit::Label taken, asmjit::x86::CondCode cc, asmjit::Operand lhs,
		       asmjit::Operand rhs)
{
	ctx->ra->BlockBoundary();
	x86Cmp(&cc, lhs, rhs);
	j.emit(asmjit::x86::Inst::jccFromCond(cc), taken);
}

void Codegen::Bind(asmjit::Label l)
{
	ctx->ra->BlockBoundary();
	j.bind(l);
}

void Codegen::BranchTBInd(asmjit::Operand target)
{
	tcache::OnTranslateBrind(ctx->tb);
	assert(target.isPhysReg() && target.id() == TMP1C);
	auto ptgt = target.as<asmjit::x86::Gp>().r32();

	ctx->ra->BlockBoundary();
	auto slowpath = j.newLabel();
	{
		// Inlined jmp_cache lookup
		auto tmp0 = asmjit::x86::gpq(asmjit::x86::Gp::kIdDi);
		auto tmp1 = asmjit::x86::gpq(asmjit::x86::Gp::kIdSi);
		j.mov(tmp1.r64(), (uintptr_t)tcache::jmp_cache_brind.data());

		j.mov(tmp0.r32(), ptgt.r32());
		j.shr(tmp0.r32(), 2);
		j.and_(tmp0.r32(), (1ull << tcache::JMP_CACHE_BITS) - 1);

		j.mov(tmp0.r64(), asmjit::x86::ptr(tmp1.r64(), tmp0.r64(), 3));
		j.test(tmp0.r64(), tmp0.r64());
		j.je(slowpath);

		j.cmp(ptgt.r32(), asmjit::x86::ptr(tmp0.r64(), offsetof(dbt::TBlock, ip)));
		j.jne(slowpath);

		j.mov(tmp0.r64(), asmjit::x86::ptr(tmp0.r64(), offsetof(dbt::TBlock, tcode) +
								   offsetof(dbt::TBlock::TCode, ptr)));
		j.jmp(tmp0.r64());
	}

	j.bind(slowpath);
	std::array<asmjit::Operand, 3> call_ops = {asmjit::imm((uintptr_t)helper_brind),
						   ctx->ra->state_base->GetPReg(), ptgt.r64()};
	Call(call_ops.data(), call_ops.size());
	j.jmp(asmjit::x86::rdx);
}

void Codegen::Spill(RegAlloc::VReg *v)
{
	ctx->cg->j.mov(
	    asmjit::x86::ptr(v->spill_base->GetPReg(), v->spill_offs, RegAlloc::TypeToSize(v->type)),
	    v->GetPReg());
}

void Codegen::Fill(RegAlloc::VReg *v)
{
	ctx->cg->j.mov(v->GetPReg(), asmjit::x86::ptr(v->spill_base->GetPReg(), v->spill_offs,
						      RegAlloc::TypeToSize(v->type)));
}

QuickJIT::QuickJIT()
{
	ra = new RegAlloc();
	cg = new Codegen();
	cg->SetupCtx(this);
	ra->SetupCtx(this);
}

QuickJIT::~QuickJIT()
{
	delete ra;
	delete cg;
}

} // namespace dbt::qjit
