#pragma once

#include "dbt/guest/rv32_cpu.h"

namespace dbt
{

LOG_STREAM(dbt);

extern sigjmp_buf trap_unwind_env;

ALWAYS_INLINE void RaiseTrap()
{
	siglongjmp(trap_unwind_env, 1);
}

void Execute(CPUState *state);

} // namespace dbt
