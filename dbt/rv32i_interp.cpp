#include "dbt/execute.h"
#include "dbt/rv32i_decode.h"
#include "dbt/rv32i_runtime.h"
#include <cstdint>
#include <type_traits>

namespace dbt::rv32i::interp
{

#define GET_GIP() (gip)
#define SET_GIP(gip_) (gip = gip_)
#define RAISE_TRAP(trapno_)                                                                                  \
	do {                                                                                                 \
		s->ip = GET_GIP();                                                                           \
		s->trapno = trapno_;                                                                         \
		RaiseTrap();                                                                                 \
	} while (0)

#define HANDLER(name)                                                                                        \
	static ALWAYS_INLINE void HandleOp_##name(CPUState *s, u32 &gip, u8 *vmem, insn::Insn_##name i);     \
	static ALWAYS_INLINE void HandleFull_##name(CPUState *state, u32 &gip, u8 *vmem, u32 insn_raw)       \
	{                                                                                                    \
		insn::Insn_##name i{(u32)insn_raw};                                                          \
		static constexpr auto flags = decltype(i)::flags;                                            \
		if constexpr ((flags & insn::Flags::MayTrap)) {                                              \
			state->ip = GET_GIP();                                                               \
		}                                                                                            \
		HandleOp_##name(state, gip, vmem, i);                                                        \
		if constexpr (flags & insn::Flags::HasRd) {                                                  \
			state->gpr[0] = 0;                                                                   \
		}                                                                                            \
		if constexpr (flags & insn::Flags::Branch) {                                                 \
			state->ip = GET_GIP();                                                               \
		} else {                                                                                     \
			gip += 4;                                                                            \
		}                                                                                            \
	}                                                                                                    \
	static void HandleInterp_##name(CPUState *state, u32 gip, u8 *vmem, u32 insn_raw)                    \
	{                                                                                                    \
		static constexpr auto flags = insn::Insn_##name::flags;                                      \
		HandleFull_##name(state, gip, vmem, insn_raw);                                               \
		if constexpr (flags & insn::Flags::Branch)                                                   \
			return;                                                                              \
		MUSTTAIL return Dispatch(state, gip, vmem, 0);                                               \
	}                                                                                                    \
	extern "C" void __attribute__((used)) HelperOp_##name(CPUState *state, u32 insn_raw)                 \
	{                                                                                                    \
		u32 gip = state->ip;                                                                         \
		HandleFull_##name(state, gip, mmu::base, insn_raw);                                          \
		state->ip = gip;                                                                             \
	}                                                                                                    \
	static ALWAYS_INLINE void HandleOp_##name(CPUState *s, u32 &gip, u8 *vmem, insn::Insn_##name i)

#define HANDLER_BCC(name, type, cond)                                                                        \
	HANDLER(name)                                                                                        \
	{                                                                                                    \
		bool pred = (type)s->gpr[i.rs1()] cond(type) s->gpr[i.rs2()];                                \
		if (!pred) {                                                                                 \
			SET_GIP(GET_GIP() + 4);                                                              \
			return;                                                                              \
		}                                                                                            \
		if (unlikely(i.imm() % 4)) {                                                                 \
			RAISE_TRAP(TrapCode::UNALIGNED_IP);                                                  \
		}                                                                                            \
		SET_GIP(GET_GIP() + i.imm());                                                                \
	}
#define HANDLER_Load(name, type)                                                                             \
	HANDLER(name)                                                                                        \
	{                                                                                                    \
		s->gpr[i.rd()] = unaligned_load<type>(vmem + s->gpr[i.rs1()] + i.imm());                     \
	}
#define HANDLER_Store(name, type)                                                                            \
	HANDLER(name)                                                                                        \
	{                                                                                                    \
		unaligned_store<type>(vmem + s->gpr[i.rs1()] + i.imm(), s->gpr[i.rs2()]);                    \
	}
#define HANDLER_ArithmRR(name, type, op)                                                                     \
	HANDLER(name)                                                                                        \
	{                                                                                                    \
		s->gpr[i.rd()] = (type)s->gpr[i.rs1()] op(type) s->gpr[i.rs2()];                             \
	}
#define HANDLER_ArithmRI(name, type, op)                                                                     \
	HANDLER(name)                                                                                        \
	{                                                                                                    \
		s->gpr[i.rd()] = (type)s->gpr[i.rs1()] op(type) i.imm();                                     \
	}

HANDLER(ill)
{
	RAISE_TRAP(TrapCode::ILLEGAL_INSN);
}
HANDLER(lui)
{
	s->gpr[i.rd()] = i.imm();
}
HANDLER(auipc)
{
	s->gpr[i.rd()] = i.imm() + GET_GIP();
}
HANDLER(jal)
{
	if (unlikely(i.imm() % 4)) {
		RAISE_TRAP(TrapCode::UNALIGNED_IP);
	}
	s->gpr[i.rd()] = GET_GIP() + 4;
	SET_GIP(GET_GIP() + i.imm());
}
HANDLER(jalr)
{
	u32 target = (s->gpr[i.rs1()] + i.imm()) & ~(u32)1;
	if (unlikely(target % 4)) {
		RAISE_TRAP(TrapCode::UNALIGNED_IP);
	}
	s->gpr[i.rd()] = GET_GIP() + 4;
	SET_GIP(target);
}
HANDLER_BCC(beq, u32, ==);
HANDLER_BCC(bne, u32, !=);
HANDLER_BCC(blt, i32, <);
HANDLER_BCC(bge, i32, >=);
HANDLER_BCC(bltu, u32, <);
HANDLER_BCC(bgeu, u32, >=);
HANDLER_Load(lb, i8);
HANDLER_Load(lh, i16);
HANDLER_Load(lw, i32);
HANDLER_Load(lbu, u8);
HANDLER_Load(lhu, u16);
HANDLER_Store(sw, u32);
HANDLER_Store(sb, u8);
HANDLER_Store(sh, u16);
HANDLER_ArithmRI(addi, u32, +);
HANDLER_ArithmRI(slti, i32, <);
HANDLER_ArithmRI(sltiu, u32, <);
HANDLER_ArithmRI(xori, u32, ^);
HANDLER_ArithmRI(ori, u32, |);
HANDLER_ArithmRI(andi, u32, &);
HANDLER(slli)
{
	s->gpr[i.rd()] = (u32)s->gpr[i.rs1()] << i.imm();
}
HANDLER(srai)
{
	s->gpr[i.rd()] = (i32)s->gpr[i.rs1()] >> i.imm();
}
HANDLER(srli)
{
	s->gpr[i.rd()] = (u32)s->gpr[i.rs1()] >> i.imm();
}
HANDLER_ArithmRR(sub, u32, -);
HANDLER_ArithmRR(add, u32, +);
HANDLER(sll)
{
	s->gpr[i.rd()] = (u32)s->gpr[i.rs1()] << (s->gpr[i.rs2()] & 31);
}
HANDLER_ArithmRR(slt, i32, <);
HANDLER_ArithmRR(sltu, u32, <);
HANDLER_ArithmRR(xor, u32, ^);
HANDLER(sra)
{
	s->gpr[i.rd()] = (i32)s->gpr[i.rs1()] >> (s->gpr[i.rs2()] & 31);
}
HANDLER(srl)
{
	s->gpr[i.rd()] = (u32)s->gpr[i.rs1()] >> (s->gpr[i.rs2()] & 31);
}
HANDLER_ArithmRR(or, u32, |);
HANDLER_ArithmRR(and, u32, &);
HANDLER(ecall)
{
	RAISE_TRAP(TrapCode::ECALL);
	RaiseTrap();
}
HANDLER(ebreak)
{
	RAISE_TRAP(TrapCode::EBREAK);
	RaiseTrap();
}

void Dispatch(CPUState *state, u32 gip, u8 *vmem, [[maybe_unused]] u32 unused)
{
	u32 insn_raw = *(u32 *)(gip + vmem);
	insn::DecodeParams insn{insn_raw};
#define OP(name) MUSTTAIL return HandleInterp_##name(state, gip, vmem, insn_raw);
#define OP_ILL OP(ill)
	RV32I_DECODE_SWITCH(insn)
#undef OP_ILL
#undef OP
	unreachable("");
}

} // namespace dbt::rv32i::interp