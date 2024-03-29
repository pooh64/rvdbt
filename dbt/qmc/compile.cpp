#include "dbt/qmc/compile.h"
#include "dbt/guest/rv32_qir.h"
#include "dbt/qmc/qcg/qcg.h"
#include "dbt/qmc/qir_printer.h"

namespace dbt::qir
{

void *CompilerDoJob(CompilerJob &job)
{
	MemArena arena(1_MB);

	auto entry_ip = job.iprange[0].first;
	auto region = CompilerGenRegionIR(&arena, job);

	auto tcode = qcg::GenerateCode(job.cruntime, &job.segment, region, entry_ip);
	return job.cruntime->AnnounceRegion(entry_ip, tcode);
}

qir::Region *CompilerGenRegionIR(MemArena *arena, CompilerJob &job)
{
	auto *region = arena->New<Region>(arena, IRTranslator::state_info);

	IRTranslator::Translate(region, &job.iprange, job.vmem);
	PrinterPass::run(log_qir, "Initial IR after IRTranslator", region);

	return region;
}

} // namespace dbt::qir
