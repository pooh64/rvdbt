#pragma once

#include "dbt/guest/rv32_insn.h"
#include "dbt/qjit/qir_builder.h"
#include <array>

namespace dbt::qir::rv32
{
using namespace dbt::rv32;

struct RV32Translator {

#define OP(name, format_, flags_)                                                                            \
	void H_##name(void *insn);                                                                           \
	void V_##name(rv32::insn::Insn_##name);                                                              \
	static constexpr auto _##name = &RV32Translator::H_##name;
	RV32_OPCODE_LIST()
#undef OP

	static void Translate(qir::Region *region, u32 ip, u32 boundary_ip, uptr vmem);

	static StateInfo const *const state_info;

private:
	static StateInfo const *GetStateInfo();

	explicit RV32Translator(qir::Region *region, u32 ip, uptr vmem);
	void PreSideeff();
	void TranslateInsn();

	void TranslateLoad(insn::I i, VType type, VSign sgn);
	void TranslateStore(insn::S i, VType type, VSign sgn);
	void TranslateBrcc(insn::B i, CondCode cc);
	inline void TranslateSetcc(insn::R i, CondCode cc);
	inline void TranslateSetcc(insn::I i, CondCode cc);
	inline void TranslateHelper(insn::Base i, RuntimeStubId stub);

	qir::Builder qb;
	enum class Control { NEXT, BRANCH, TB_OVF } control{Control::NEXT};
	uptr vmem_base{}; // jit/aot
	u32 insn_ip{0};
	u32 bb_ip{}; // for cflow_dump
};

} // namespace dbt::qir::rv32

namespace dbt::qir
{
using IRTranslator = qir::rv32::RV32Translator;
} // namespace dbt::qir
