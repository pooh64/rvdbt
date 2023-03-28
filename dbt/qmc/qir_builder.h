#pragma once

#include "dbt/qmc/qir.h"
#include "dbt/qmc/qir_opt.h"

namespace dbt::qir
{

struct Builder {
	explicit Builder() : bb(nullptr), it(nullptr) {}
	explicit Builder(Block *bb_) : bb(bb_), it(bb->ilist.end()) {}
	explicit Builder(Block *bb_, IListIterator<Inst> it_) : bb(bb_), it(it_) {}
	NO_COPY(Builder)
	DEFAULT_MOVE(Builder)

	auto GetIterator() const
	{
		return it;
	}

	Block *GetBlock() const
	{
		return bb;
	}

	Block *CreateBlock() const
	{
		return bb->GetRegion()->CreateBlock();
	}

	RegN CreateVGPR(VType type) const
	{
		return bb->GetRegion()->GetVRegsInfo()->AddLocal(type);
	}

private:
	using Flags = Inst::Flags;

public:
#define OP(name, cls, flags)                                                                                 \
	template <typename... Args>                                                                          \
	Inst *Create_##name(Args &&...args)                                                                  \
	{                                                                                                    \
		auto ins = Create<cls>(Op::_##name, std::forward<Args>(args)...);                            \
		ins->SetFlags(Flags(flags));                                                                 \
		return ins;                                                                                  \
	}
	QIR_LEAF_OPS_LIST(OP)
#undef OP

#define OP(name, cls, flags)                                                                                 \
	template <typename... Args>                                                                          \
	Inst *Create_##name(Args &&...args)                                                                  \
	{                                                                                                    \
		auto ins = Create<cls>(std::forward<Args>(args)...);                                         \
		ins->SetFlags(Flags(flags));                                                                 \
		return ins;                                                                                  \
	}
	QIR_BASE_OPS_LIST(OP)
#undef OP

#define CLASS(cls, beg, end)                                                                                 \
	template <typename... Args>                                                                          \
	Inst *Create##cls(Op op, Args &&...args)                                                             \
	{                                                                                                    \
		auto ins = Create<cls>(std::forward<Args>(args)...);                                         \
		ins->SetFlags(GetOpFlags(op));                                                               \
		return ins;                                                                                  \
	}
	QIR_CLASS_LIST(CLASS)
#undef CLASS

private:
	template <typename T, typename... Args>
	requires std::is_base_of_v<Inst, T> Inst *Create(Args &&...args)
	{
		auto *ins = bb->GetRegion()->_Create<T>(std::forward<Args>(args)...);
		bb->ilist.insert(it, *ins);
		return ApplyFolder(bb, ins);
	}

	Block *bb;
	IListIterator<Inst> it;
};

} // namespace dbt::qir
