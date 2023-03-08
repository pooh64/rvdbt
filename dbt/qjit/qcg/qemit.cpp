#include "dbt/qjit/qcg/qemit.h"

namespace dbt::qcg
{

QEmit::QEmit(qir::Region *region, bool jit_mode_) : jit_mode(jit_mode_)
{
	if (jcode.init(jrt.environment())) {
		Panic();
	}
	jcode.attach(j._emitter());
	j.setErrorHandler(&jerr);
	jcode.setErrorHandler(&jerr);

	u32 n_labels = region->GetNumBlocks();
	labels.reserve(n_labels);
	for (u32 i = 0; i < n_labels; ++i) {
		labels.push_back(j.newLabel());
	}
}

TBlock::TCode QEmit::EmitTCode()
{
	// assert(jit_mode);
	jcode.flatten();
	jcode.resolveUnresolvedLinks();

	size_t jit_sz = jcode.codeSize();
	TBlock::TCode tc{nullptr, jit_sz};
	tc.ptr = tcache::AllocateCode(jit_sz, 8);
	if (tc.ptr == nullptr) {
		Panic();
	}

	jcode.relocateToBase((uptr)tc.ptr);
	if (jit_sz < jcode.codeSize()) {
		Panic();
	}
	jcode.copyFlattenedData(tc.ptr, tc.size);
	tc.size = jcode.codeSize();
	return tc;
}

void QEmit::DumpTCode(TBlock::TCode const &tcode)
{
	if (!log_qcg.enabled()) {
		return;
	}

	auto str = MakeHexStr((u8 *)tcode.ptr, tcode.size);
	log_qcg.write(str.c_str());
}

static inline asmjit::x86::Gp make_gpr(qir::RegN pr, qir::VType type)
{
	switch (type) {
	case qir::VType::I8:
		return asmjit::x86::gpb(pr);
	case qir::VType::I16:
		return asmjit::x86::gpw(pr);
	case qir::VType::I32:
		return asmjit::x86::gpd(pr);
	default:
		unreachable("");
	}
}

static inline asmjit::x86::Gp make_gpr(qir::VOperand opr)
{
	return make_gpr(opr.GetPGPR(), opr.GetType());
}

static inline asmjit::Imm make_imm(qir::VOperand opr)
{
	return asmjit::imm(opr.GetConst());
}

static inline asmjit::x86::Mem make_slot(qir::VOperand opr)
{
	auto offs = opr.GetSlotOffs();
	auto size = VTypeToSize(opr.GetType());
	auto base = opr.IsLSlot() ? QEmit::R_SP : QEmit::R_STATE;
	return asmjit::x86::Mem(base, offs, size);
}

static inline asmjit::Operand make_operand(qir::VOperand opr)
{
	if (likely(opr.IsGPR())) {
		return make_gpr(opr);
	}
	if (opr.IsConst()) {
		return make_imm(opr);
	}
	return make_slot(opr);
}

static inline asmjit::x86::CondCode make_cc(qir::CondCode cc)
{
	switch (cc) {
	case qir::CondCode::EQ:
		return asmjit::x86::CondCode::kEqual;
	case qir::CondCode::NE:
		return asmjit::x86::CondCode::kNotEqual;
	case qir::CondCode::LE:
		return asmjit::x86::CondCode::kSignedLE;
	case qir::CondCode::LT:
		return asmjit::x86::CondCode::kSignedLT;
	case qir::CondCode::GE:
		return asmjit::x86::CondCode::kSignedGE;
	case qir::CondCode::GT:
		return asmjit::x86::CondCode::kSignedGT;
	case qir::CondCode::LEU:
		return asmjit::x86::CondCode::kUnsignedLE;
	case qir::CondCode::LTU:
		return asmjit::x86::CondCode::kUnsignedLT;
	case qir::CondCode::GEU:
		return asmjit::x86::CondCode::kUnsignedGE;
	case qir::CondCode::GTU:
		return asmjit::x86::CondCode::kUnsignedGT;
	default:
		unreachable("");
	}
}

void QEmit::Prologue(u32 ip)
{
	// j.int3();
#ifdef CONFIG_DUMP_TRACE
	// j.mov(ctx->vreg_ip->GetSpill(), ctx->tb->ip);
	j.mov(asmjit::x86::Mem(R_STATE, offsetof(CPUState, ip), 4), ip);
	j.call(jitabi::stub_trace);
#endif
}

void QEmit::StateFill(qir::RegN p, qir::VType type, u16 offs)
{
	auto slot = asmjit::x86::ptr(R_STATE, offs);
	slot.setSize(VTypeToSize(type));
	j.mov(make_gpr(p, type), slot);
}

void QEmit::StateSpill(qir::RegN p, qir::VType type, u16 offs)
{
	auto slot = asmjit::x86::ptr(R_STATE, offs);
	slot.setSize(VTypeToSize(type));
	j.mov(slot, make_gpr(p, type));
}

void QEmit::LocFill(qir::RegN p, qir::VType type, u16 offs)
{
	auto slot = asmjit::x86::ptr(R_SP, offs);
	slot.setSize(VTypeToSize(type));
	j.mov(make_gpr(p, type), slot);
}

void QEmit::LocSpill(qir::RegN p, qir::VType type, u16 offs)
{
	auto slot = asmjit::x86::ptr(R_SP, offs);
	slot.setSize(VTypeToSize(type));
	j.mov(slot, make_gpr(p, type));
}

void QEmit::Emit_hcall(qir::InstHcall *ins)
{
	j.mov(asmjit::x86::rdi, R_STATE);
	j.emit(asmjit::x86::Inst::kIdMov, asmjit::x86::rsi, make_operand(ins->i(0)));
	if (jit_mode) {
		j.call(stub_tab[ins->stub]);
	} else {
		j.call(asmjit::x86::Mem(R_STATE,
					offsetof(CPUState, stub_tab) + RuntimeStubTab::offs(ins->stub)));
	}
}

void QEmit::Emit_br(qir::InstBr *ins)
{
	auto &bb_s = **bb->GetSuccs().begin();
	auto &bb_ff = *++bb->getIter();
	if (&bb_s != &bb_ff) {
		j.jmp(labels[bb_s.GetId()]);
	}
}

void QEmit::Emit_brcc(qir::InstBrcc *ins)
{
	auto sit = bb->GetSuccs().begin();
	auto &bb_t = **sit;
	auto &bb_f = **++sit;
	auto &bb_ff = *++bb->getIter();

	auto &vs0 = ins->i(0);
	auto &vs1 = ins->i(1);

	// constfolded
	if (vs0.IsConst()) {
		std::swap(vs0, vs1);
		ins->cc = qir::SwapCC(ins->cc);
	}
	auto cc = ins->cc;

	j.emit(asmjit::x86::Inst::kIdCmp, make_operand(vs0), make_operand(vs1));
	auto jcc = asmjit::x86::Inst::jccFromCond(make_cc(cc));
	j.emit(jcc, labels[bb_t.GetId()]);

	if (&bb_f != &bb_ff) {
		j.jmp(labels[bb_f.GetId()]);
	}
}

void QEmit::Emit_gbr(qir::InstGBr *ins)
{
	static constexpr size_t patch_size = sizeof(jitabi::ppoint::BranchSlot);
	j.embedUInt8(0, patch_size);
	auto *slot = (jitabi::ppoint::BranchSlot *)(j.bufferPtr() - patch_size);
	slot->gip = ins->tpc.GetConst();
	if (jit_mode) {
		slot->LinkLazyJIT();
	} else {
		slot->LinkLazyAOT();
	}
}

void QEmit::Emit_gbrind(qir::InstGBrind *ins)
{
	auto ptgt = make_gpr(ins->i(0));
	{ // TODO: force si alloc
		auto tmp_ptgt = asmjit::x86::gpd(asmjit::x86::Gp::kIdSi);
		j.mov(tmp_ptgt, ptgt);
		ptgt = tmp_ptgt;
	}

	auto slowpath = j.newLabel();
	{
		// Inlined jmp_cache lookup
		auto tmp0 = asmjit::x86::rdi;
		auto tmp1 = asmjit::x86::rdx;
		if (jit_mode) {
			j.mov(tmp1.r64(), (uptr)tcache::jmp_cache_brind.data());
		} else {
			j.mov(tmp1.r64(), asmjit::x86::Mem(R_STATE, offsetof(CPUState, jmp_cache_brind)));
		}

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

	j.mov(asmjit::x86::gpq(asmjit::x86::Gp::kIdDi), R_STATE);
	assert(ptgt.id() == asmjit::x86::Gp::kIdSi);
	j.call(stub_tab[RuntimeStubId::id_brind]);
	j.jmp(asmjit::x86::rdx);
}

// set size manually
static inline asmjit::x86::Mem make_vmem(qir::VOperand vbase)
{
	if constexpr (config::zero_membase) {
		if (likely(vbase.IsPGPR())) {
			return asmjit::x86::ptr(make_gpr(vbase));
		} else {
			return asmjit::x86::ptr(vbase.GetConst());
		}
	} else {
		if (likely(vbase.IsPGPR())) {
			return asmjit::x86::ptr(QEmit::R_MEMBASE, make_gpr(vbase));
		} else {
			return asmjit::x86::ptr(QEmit::R_MEMBASE, vbase.GetConst());
		}
	}
}

void QEmit::Emit_vmload(qir::InstVMLoad *ins)
{
	auto &vrd = ins->o(0);
	auto &vbase = ins->i(0);
	auto sgn = ins->sgn;

	auto prd = make_gpr(vrd);
	auto mem = make_vmem(vbase);

	assert(vrd.GetType() == qir::VType::I32);
	switch (ins->sz) {
	case qir::VType::I8:
		mem.setSize(1);
		if (sgn == qir::VSign::U) {
			j.movzx(prd, mem);
		} else {
			j.movsx(prd, mem);
		}
		break;
	case qir::VType::I16:
		mem.setSize(2);
		if (sgn == qir::VSign::U) {
			j.movzx(prd, mem);
		} else {
			j.movsx(prd, mem);
		}
		break;
	case qir::VType::I32:
		mem.setSize(4);
		j.mov(prd, mem);
		break;
	default:
		unreachable("");
	};
}

void QEmit::Emit_vmstore(qir::InstVMStore *ins)
{
	auto &vbase = ins->i(0);
	auto &vdata = ins->i(1);

	auto pdata = make_operand(vdata);
	auto mem = make_vmem(vbase);

	assert(ins->sgn == qir::VSign::U);
	mem.setSize(VTypeToSize(ins->sz));
	j.emit(asmjit::x86::Inst::kIdMov, mem, pdata);
}

void QEmit::Emit_setcc(qir::InstSetcc *ins)
{
	auto prd = make_gpr(ins->o(0));
	auto vs0 = ins->i(0);
	auto vs1 = ins->i(1);
	auto cc = ins->cc;

	bool dst_aliased = vs0.GetPGPR() == prd.id() || (vs1.IsPGPR() && vs1.GetPGPR() == prd.id());

	if (!dst_aliased) {
		j.xor_(prd, prd);
	}

	j.emit(asmjit::x86::Inst::kIdCmp, make_operand(vs0), make_operand(vs1));
	auto setcc = asmjit::x86::Inst::setccFromCond(make_cc(cc));
	j.emit(setcc, prd.r8());

	if (dst_aliased) {
		j.movzx(prd, prd.r8());
	}
}

void QEmit::Emit_mov(qir::InstUnop *ins)
{
	auto vrd = ins->o(0);
	auto vs0 = ins->i(0);
	// TODO: slowed code by ~3%, try again after bb merging
	if (unlikely(false && vs0.IsConst() && vs0.GetConst() == 0 && vrd.IsPGPR())) {
		auto prd = make_gpr(vrd);
		j.emit(asmjit::x86::Inst::kIdXor, prd, prd);
		return;
	}
	j.emit(asmjit::x86::Inst::kIdMov, make_operand(vrd), make_operand(vs0));
}

template <asmjit::x86::Inst::Id Op>
ALWAYS_INLINE void QEmit::EmitInstBinop(qir::InstBinop *ins)
{
	auto &vrd = ins->o(0);
	[[maybe_unused]] auto vs0 = ins->i(0);
	auto vs1 = ins->i(1);

	assert(vrd.GetPGPR() == vs0.GetPGPR());
	j.emit(Op, make_gpr(vrd), make_operand(vs1));
}

void QEmit::Emit_add(qir::InstBinop *ins)
{
	EmitInstBinop<asmjit::x86::Inst::kIdAdd>(ins);
}

void QEmit::Emit_sub(qir::InstBinop *ins)
{
	EmitInstBinop<asmjit::x86::Inst::kIdSub>(ins);
}

void QEmit::Emit_and(qir::InstBinop *ins)
{
	EmitInstBinop<asmjit::x86::Inst::kIdAnd>(ins);
}

void QEmit::Emit_or(qir::InstBinop *ins)
{
	EmitInstBinop<asmjit::x86::Inst::kIdOr>(ins);
}

void QEmit::Emit_xor(qir::InstBinop *ins)
{
	EmitInstBinop<asmjit::x86::Inst::kIdXor>(ins);
}

void QEmit::Emit_sra(qir::InstBinop *ins)
{
	[[maybe_unused]] auto vs1 = ins->i(1);
	assert(vs1.IsConst() || vs1.GetPGPR() == asmjit::x86::Gp::kIdCx);
	EmitInstBinop<asmjit::x86::Inst::kIdSar>(ins);
}

void QEmit::Emit_srl(qir::InstBinop *ins)
{
	[[maybe_unused]] auto vs1 = ins->i(1);
	assert(vs1.IsConst() || vs1.GetPGPR() == asmjit::x86::Gp::kIdCx);
	EmitInstBinop<asmjit::x86::Inst::kIdShr>(ins);
}

void QEmit::Emit_sll(qir::InstBinop *ins)
{
	[[maybe_unused]] auto vs1 = ins->i(1);
	assert(vs1.IsConst() || vs1.GetPGPR() == asmjit::x86::Gp::kIdCx);
	EmitInstBinop<asmjit::x86::Inst::kIdShl>(ins);
}

} // namespace dbt::qcg
