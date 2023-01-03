#pragma once

#include "dbt/guest/rv32_cpu.h"
#include "dbt/guest/rv32_insn.h"
#include "dbt/qjit/qir.h"
#include "dbt/tcache/tcache.h"
#include <array>

namespace dbt::qir::rv32
{
using namespace dbt::rv32;

struct RV32Translator {
	static constexpr u16 TB_MAX_INSNS = dbt::rv32::TB_MAX_INSNS;

	enum class Control { NEXT, BRANCH, TB_OVF } control{Control::NEXT};
	u32 insn_ip{0};

#define OP(name, format_, flags_)                                                                            \
	void H_##name(void *insn);                                                                           \
	void V_##name(rv32::insn::Insn_##name);                                                              \
	static constexpr auto _##name = &RV32Translator::H_##name;
	RV32_OPCODE_LIST()
#undef OP

	static TBlock *Translate(CPUState *state, u32 ip);

private:
	static StateInfo const *GetStateInfo(); 
	static StateInfo const *const state_info; 

	explicit RV32Translator(qir::Region *region);
	void TranslateInsn();

	qir::Builder qb;

	void TranslateLoad(insn::I i, VType type, VSign sgn);
	void TranslateStore(insn::S i, VType type, VSign sgn);
	void TranslateBrcc(insn::B i, CondCode cc);
	inline void TranslateSetcc(insn::R i, CondCode cc);
	inline void TranslateSetcc(insn::I i, CondCode cc);
	inline void TranslateHelper(insn::Base i, void *stub);
};

} // namespace dbt::qir::rv32
