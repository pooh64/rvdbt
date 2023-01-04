#pragma once

#include "dbt/arena.h"
#include "dbt/bitfield.h"
#include "dbt/common.h"
#include "dbt/logger.h"
#include "dbt/qjit/ilist.h"
#include "dbt/qjit/qir_ops.h"
#include "dbt/qjit/regalloc.h"

#include <algorithm>
#include <bit>
#include <type_traits>
#include <variant>
#include <vector>

namespace dbt::qir
{
LOG_STREAM(qir);

template <typename D, typename B>
requires std::is_base_of_v<B, D> ALWAYS_INLINE D *as(B *b)
{
	if (!D::classof(b)) {
		return nullptr;
	}
	return static_cast<D *>(b);
}

template <typename D, typename B>
ALWAYS_INLINE D *cast(B *b)
{
	auto res = as<D>(b);
	assert(res);
	return res;
}

enum class Op : u8 {
#define OP(name, base) _##name,
#define GROUP(cls, beg, end) cls##_begin = _##beg, cls##_end = _##end,
	QIR_OPS_LIST(OP) QIR_GROUPS_LIST(GROUP)
#undef OP
#undef GROUP
	    Count,
};

enum class VType : u8 {
	UNDEF,
	I8,
	I16,
	I32,
	Count,
};

inline u8 VTypeToSize(VType type)
{
	switch (type) {
	case VType::I8:
		return 1;
	case VType::I16:
		return 2;
	case VType::I32:
		return 4;
	default:
		unreachable("");
	}
}

enum class VSign : u8 {
	U = 0,
	S = 1,
};

using RegN = u16;

struct VOperand {
private:
	enum class Kind : u8 {
		CONST = 0,
		GPR,
		SLOT,
		// FPR
		BAD,
		Count,
	};

	inline VOperand(uintptr_t value_) : value(value_) {}

public:
	// TODO: delete operators

	inline VOperand() : value(f_kind::encode(uintptr_t(0), Kind::BAD)) {}

	static inline VOperand MakeVGPR(VType type, RegN reg)
	{
		uintptr_t value = 0;
		value = f_kind::encode(value, Kind::GPR);
		value = f_type::encode(value, type);
		value = f_is_virtual::encode(value, true);
		value = f_reg::encode(value, reg);
		return VOperand(value);
	}

	static inline VOperand MakePGPR(VType type, RegN reg)
	{
		uintptr_t value = 0;
		value = f_kind::encode(value, Kind::GPR);
		value = f_type::encode(value, type);
		value = f_reg::encode(value, reg);
		return VOperand(value);
	}

	static inline VOperand MakeConst(VType type, u32 cval)
	{
		uintptr_t value = 0;
		value = f_kind::encode(value, Kind::CONST);
		value = f_type::encode(value, type);
		value = f_const::encode(value, cval);
		return VOperand(value);
	}

	static inline VOperand MakeSlot(bool is_glob, VType type, u16 offs)
	{
		uintptr_t value = 0;
		value = f_kind::encode(value, Kind::SLOT);
		value = f_type::encode(value, type);
		value = f_slot_offs::encode(value, offs);
		value = f_slot_is_global::encode(value, is_glob);
		return VOperand(value);
	}

	inline VType GetType() const
	{
		return static_cast<VType>(f_type::decode(value));
	}

	inline bool IsConst() const
	{
		return GetKind() == Kind::CONST;
	}

	// preg or vreg
	inline bool IsGPR() const
	{
		return GetKind() == Kind::GPR;
	}

	inline bool IsSlot() const
	{
		return GetKind() == Kind::SLOT;
	}

	inline bool IsV() const
	{
		return f_is_virtual::decode(value);
	}

	inline bool IsPGPR() const
	{
		return IsGPR() && !IsV(); // TODO: split checks and add asserts
	}

	inline bool IsVGPR() const
	{
		return IsGPR() && IsV();
	}

	inline bool IsGSlot() const
	{
		return IsSlot() && f_slot_is_global::decode(value);
	}

	inline bool IsLSlot() const
	{
		return IsSlot() && !f_slot_is_global::decode(value);
	}

	inline u32 GetConst() const
	{
		assert(IsConst());
		return f_const::decode(value);
	}

	inline RegN GetPGPR() const
	{
		assert(IsPGPR());
		return f_reg::decode(value);
	}

	inline RegN GetVGPR() const
	{
		assert(IsVGPR());
		return f_reg::decode(value);
	}

	inline u16 GetSlotOffs() const
	{
		assert(IsSlot());
		return f_slot_offs::decode(value);
	}

private:
	inline Kind GetKind() const
	{
		return static_cast<Kind>(f_kind::decode(value));
	}

	uintptr_t value{0};

	using f_kind = bf_first<std::underlying_type_t<Kind>, enum_bits(Kind::Count)>;
	using f_type = f_kind::next<std::underlying_type_t<VType>, enum_bits(VType::Count)>;
	using f_is_virtual = f_type::next<bool, 1>;
	using last_ = f_is_virtual;

	static constexpr auto data_bits = bit_size<uintptr_t> - last_::container_size;
	using f_reg = last_::next<RegN, bit_size<RegN>>;
	using f_const = last_::next<u32, 32>; // TODO: cpool
	using f_slot_offs = last_::next<u16, 16>;
	using f_slot_is_global = f_slot_offs::next<bool, 1>;
};

struct Inst : IListNode<Inst> {
	enum Flags { // TODO: rethink, refactor, gen by ir defs
		SIDEEFF = 1 << 0,
		REXIT = 1 << 1,
	};

	inline Op GetOpcode() const
	{
		return opcode;
	}

	inline u32 GetId() const
	{
		return id;
	}

	inline void SetId(u32 id_)
	{
		id = id_;
	}

	inline auto GetFlags() const
	{
		return flags;
	}

	inline void SetFlags(u8 flags_)
	{
		flags = flags_;
	}

protected:
	inline Inst(Op opcode_) : opcode(opcode_) {}

private:
	Inst(Inst const &) = delete;
	Inst(Inst &&) = delete;
	Inst &operator=(Inst const &) = delete;
	Inst &operator=(Inst &&) = delete;

	friend struct Region;

	u32 id{(u32)-1};
	Op opcode;
	u8 flags{0};
};

template <size_t N_OUT, size_t N_IN>
struct InstWithOperands : Inst {
protected:
	InstWithOperands(Op opcode_, std::array<VOperand, N_OUT> &&o_, std::array<VOperand, N_IN> &&i_)
	    : Inst(opcode_), o{o_}, i{i_}
	{
	}

public:
	std::array<VOperand, N_OUT> o{};
	std::array<VOperand, N_IN> i{};
};

/* Common classes */

struct InstUnop : InstWithOperands<1, 1> {
	InstUnop(Op opcode_, VOperand d, VOperand s) : InstWithOperands(opcode_, {d}, {s})
	{
		assert(HasOpcode(opcode_));
	}

	static bool classof(Inst *op)
	{
		return HasOpcode(op->GetOpcode());
	}

	static bool HasOpcode(Op opcode)
	{
		return opcode >= Op::InstUnop_begin && opcode <= Op::InstUnop_end;
	}
};

struct InstBinop : InstWithOperands<1, 2> {
	InstBinop(Op opcode_, VOperand d, VOperand sl, VOperand sr) : InstWithOperands(opcode_, {d}, {sl, sr})
	{
		assert(HasOpcode(opcode_));
	}

	static bool classof(Inst *op)
	{
		return HasOpcode(op->GetOpcode());
	}

	static bool HasOpcode(Op opcode)
	{
		return opcode >= Op::InstBinop_begin && opcode <= Op::InstBinop_end;
	}
};

/* Custom classes */

struct Block;

struct InstBr : Inst {
	InstBr(Block *target_) : Inst(Op::_br), target(target_) {}

	Block *target;
};

enum class CondCode : u8 {
	EQ,
	NE,
	LT,
	GE,
	LTU,
	GEU,
	Count,
};

inline CondCode InverseCC(CondCode cc)
{
	switch (cc) {
	case CondCode::EQ:
		return CondCode::NE;
	case CondCode::NE:
		return CondCode::EQ;
	case CondCode::LT:
		return CondCode::GE;
	case CondCode::GE:
		return CondCode::LT;
	case CondCode::LTU:
		return CondCode::GEU;
	case CondCode::GEU:
		return CondCode::LTU;
	default:
		unreachable("");
	}
}

struct InstBrcc : InstWithOperands<0, 2> {
	InstBrcc(CondCode cc_, VOperand s1, VOperand s2) : InstWithOperands(Op::_brcc, {}, {s1, s2}), cc(cc_)
	{
	}

	CondCode cc;
};

// TODO: group with gbrind?
struct InstGBr : Inst {
	InstGBr(VOperand tpc_) : Inst(Op::_gbr), tpc(tpc_)
	{
		SetFlags(Flags::REXIT);
		assert(tpc_.IsConst());
	}

	VOperand tpc;
};

struct InstGBrind : InstWithOperands<0, 1> {
	InstGBrind(VOperand tpc_) : InstWithOperands(Op::_gbrind, {}, {tpc_})
	{
		SetFlags(Flags::REXIT);
	}
};

struct InstHcall : InstWithOperands<0, 1> {
	// TODO: variable number of operands
	InstHcall(void *stub_, VOperand arg_) : InstWithOperands(Op::_hcall, {}, {arg_}), stub(stub_)
	{
		SetFlags(Flags::SIDEEFF);
	}

	void *stub;
};

struct InstVMLoad : InstWithOperands<1, 1> {
	InstVMLoad(VType sz_, VSign sgn_, VOperand d, VOperand ptr)
	    : InstWithOperands(Op::_vmload, {d}, {ptr}), sz(sz_), sgn(sgn_)
	{
		SetFlags(Flags::SIDEEFF);
	}

	VType sz;
	VSign sgn;
};

struct InstVMStore : InstWithOperands<0, 2> {
	InstVMStore(VType sz_, VSign sgn_, VOperand ptr, VOperand val)
	    : InstWithOperands(Op::_vmstore, {}, {ptr, val}), sz(sz_), sgn(sgn_)
	{
		SetFlags(Flags::SIDEEFF);
	}

	VType sz;
	VSign sgn;
};

struct InstSetcc : InstWithOperands<1, 2> {
	InstSetcc(CondCode cc_, VOperand d, VOperand sl, VOperand sr)
	    : InstWithOperands(Op::_setcc, {d}, {sl, sr}), cc(cc_)
	{
	}

	CondCode cc;
};

struct Block;
struct Region;

struct Block : IListNode<Block> {
	Block(Region *rn_) : rn(rn_) {}

	IList<Inst> ilist;

	inline Region *GetRegion() const
	{
		return rn;
	}

	inline u32 GetId() const
	{
		return id;
	}

	inline void SetId(u32 id_)
	{
		id = id_;
	}

	struct Link : IListNode<Link> {
		static Link *Create(Region *rn_, Block *to);

		Block &operator*() const
		{
			return *ptr;
		}

	private:
		Link(Block *bb) : ptr(bb) {}

		Block *ptr;
	};

	void AddSucc(Block *succ)
	{
		succs.push_back(Link::Create(rn, succ));
		succ->preds.push_back(Link::Create(rn, this));
	}

	auto &GetSuccs()
	{
		return succs;
	}
	auto &GetPreds()
	{
		return preds;
	}

private:
	Region *rn;
	IList<Link> succs, preds;
	u32 id{(u32)-1};
};

struct StateReg {
	u16 state_offs;
	VType type;
	char const *name;
};

struct StateInfo {
	StateReg const *GetStateReg(RegN idx) const
	{
		if (idx < n_regs) {
			return &regs[idx];
		}
		return nullptr;
	}

	StateReg *regs{};
	RegN n_regs{};
};

struct VRegsInfo {
	VRegsInfo(StateInfo const *glob_info_) : glob_info(glob_info_) {}

	inline auto NumGlobals() const
	{
		return glob_info->n_regs;
	}

	inline auto NumAll() const
	{
		return glob_info->n_regs + loc_info.size();
	}

	inline bool IsGlobal(RegN idx) const
	{
		return idx < glob_info->n_regs;
	}

	inline bool IsLocal(RegN idx) const
	{
		return !IsGlobal(idx);
	}

	inline StateReg const *GetGlobalInfo(RegN idx) const
	{
		assert(IsGlobal(idx));
		return &glob_info->regs[idx];
	}

	inline VType GetLocalType(RegN idx) const
	{
		assert(IsLocal(idx));
		return loc_info[idx - glob_info->n_regs];
	}

	inline RegN AddLocal(VType type)
	{
		auto idx = loc_info.size() + glob_info->n_regs;
		loc_info.push_back(type);
		return idx;
	}

private:
	StateInfo const *glob_info{};
	std::vector<VType> loc_info{};
};

struct Region {
	Region(MemArena *arena_, StateInfo const *state_info_) : arena(arena_), vregs_info(state_info_) {}

	IList<Block> blist;

	Block *CreateBlock()
	{
		auto res = CreateInArena<Block>(this);
		res->SetId(bb_id_counter++);
		blist.insert(blist.end(), *res);
		return res;
	}

	template <typename T, typename... Args>
	requires std::is_base_of_v<Inst, T> T *Create(Args &&...args)
	{
		auto res = CreateInArena<T>(std::forward<Args>(args)...);
		res->SetId(inst_id_counter++);
		return res;
	}

	u32 GetNumBlocks() const
	{
		return bb_id_counter;
	}

	MemArena *GetArena()
	{
		return arena;
	}

	VRegsInfo *GetVRegsInfo()
	{
		return &vregs_info;
	}

private:
	template <typename T, typename... Args>
	T *CreateInArena(Args &&...args)
	{
		auto *mem = arena->Allocate<T>();
		assert(mem);
		return new (mem) T(std::forward<Args>(args)...);
	}

	MemArena *arena;

	VRegsInfo vregs_info;

	u32 inst_id_counter{0};
	u32 bb_id_counter{0};
};

inline Block::Link *Block::Link::Create(Region *rn_, Block *to)
{
	auto *mem = rn_->GetArena()->Allocate<Block::Link>();
	assert(mem);
	return new (mem) Block::Link(to);
}

template <typename Derived, typename RT>
struct InstVisitor {
#define VIS_CLASS(cls) return static_cast<Derived *>(this)->visit##cls(static_cast<cls *>(ins))

#define OP(name, cls)                                                                                        \
	RT visit_##name(cls *ins)                                                                            \
	{                                                                                                    \
		VIS_CLASS(cls);                                                                              \
	}
	QIR_OPS_LIST(OP)
#undef OP

#define OP(name, cls)                                                                                        \
	RT visit##cls(cls *ins)                                                                              \
	{                                                                                                    \
		VIS_CLASS(Inst);                                                                             \
	}
	QIR_CLSOPS_LIST(OP)
#undef OP

#define GROUP(cls, beg, end)                                                                                 \
	RT visit##cls(cls *ins)                                                                              \
	{                                                                                                    \
		VIS_CLASS(Inst);                                                                             \
	}
	QIR_GROUPS_LIST(GROUP)
#undef GROUP

	void visitInst(Inst *ins) {}

	RT visit(Inst *ins)
	{
		switch (ins->GetOpcode()) {
#define OP(name, cls)                                                                                        \
	case Op::_##name:                                                                                    \
		return static_cast<Derived *>(this)->visit_##name(static_cast<cls *>(ins));
			QIR_OPS_LIST(OP)
#undef OP
		default:
			unreachable("");
		};
	}
};

} // namespace dbt::qir
