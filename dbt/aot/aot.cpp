#include "dbt/aot/aot.h"
#include "dbt/qmc/compile.h"
#include "dbt/tcache/objprof.h"

#include "elfio/elfio.hpp"
#include <sstream>
namespace elfio = ELFIO;

extern "C" {
#include <fcntl.h>
}

namespace dbt
{
LOG_STREAM(aot)

static inline std::string MakeAotSymbol(u32 ip)
{
	std::stringstream ss;
	ss << AOT_SYM_PREFIX << std::hex << ip;
	return ss.str();
}

struct AOTCompilerRuntime final : CompilerRuntime {
	AOTCompilerRuntime(elfio::section *elf_text_, const elfio::string_section_accessor &elf_stra_,
			   const elfio::symbol_section_accessor &elf_syma_, std::vector<AOTSymbol> &aotsyms_)
	    : code_arena(256_MB), elf_text(elf_text_), elf_stra(elf_stra_), elf_syma(elf_syma_),
	      aotsyms(aotsyms_)
	{
	}

	void *AllocateCode(size_t sz, uint align) override
	{
		return code_arena.Allocate(sz, align);
	}

	bool AllowsRelocation() const override
	{
		return true;
	}

	uptr GetVMemBase() const override
	{
		return (uptr)mmu::base;
	}

	void UpdateIPBoundary(std::pair<u32, u32> &iprange) const override
	{
		u32 upper = iprange.second;
		upper = std::min(upper, roundup(iprange.first, mmu::PAGE_SIZE));
		if (auto *tb_upper = tcache::LookupUpperBound(iprange.first)) {
			upper = std::min(upper, tb_upper->ip);
		}
		iprange.second = upper;
	}

	void *AnnounceRegion(u32 ip, std::span<u8> const &code) override
	{
		auto str_idx = elf_stra.add_string(MakeAotSymbol(ip));
		uptr code_offs = (uptr)code.data() - (uptr)code_arena.BaseAddr();

		elf_syma.add_symbol(str_idx, code_offs, code.size(), elfio::STB_GLOBAL, elfio::STT_FUNC, 0,
				    elf_text->get_index());
		aotsyms.push_back({ip, code_offs});

		return nullptr;
	}

	MemArena code_arena;
	elfio::section *elf_text;
	elfio::string_section_accessor elf_stra;
	elfio::symbol_section_accessor elf_syma;
	std::vector<AOTSymbol> &aotsyms;
};

using FilePageData = objprof::PageData;

static void AOTCompilePage(AOTCompilerRuntime *aotrt, FilePageData const &page)
{
	u32 page_vaddr = page.pageno << mmu::PAGE_BITS;

	for (u32 idx = 0; idx < page.executed.size(); ++idx) {
		if (!page.executed[idx]) {
			continue;
		}
		u32 ip = page_vaddr + FilePageData::idx2po(idx);
		qir::CompileAt(aotrt, {ip, -1});
	}
}

static void FixupAOTTabSection();

void AOTCompileElf()
{
	if (!objprof::HasProfile()) {
		log_aot("No profile data found");
		return;
	}
	log_aot("Start aot compilation");

	elfio::elfio writer;
	writer.create(elfio::ELFCLASS64, elfio::ELFDATA2LSB);
	writer.set_os_abi(elfio::ELFOSABI_LINUX);
	writer.set_type(elfio::ET_REL);
	writer.set_machine(elfio::EM_X86_64);

	elfio::section *aot_sec = writer.sections.add(".aot");
	aot_sec->set_type(elfio::SHT_PROGBITS);
	aot_sec->set_flags(elfio::SHF_ALLOC | elfio::SHF_EXECINSTR | elfio::SHF_WRITE);
	aot_sec->set_addr_align(0x10);

	std::vector<AOTSymbol> aot_symbols;
	aot_symbols.reserve(64_KB);

	elfio::section *aottab_sec = writer.sections.add(".aottab");
	aottab_sec->set_type(elfio::SHT_PROGBITS);
	aottab_sec->set_flags(elfio::SHF_ALLOC | elfio::SHF_WRITE);
	aottab_sec->set_addr_align(0x1000);

	elfio::section *str_sec = writer.sections.add(".strtab");
	str_sec->set_type(elfio::SHT_STRTAB);
	elfio::section *sym_sec = writer.sections.add(".symtab");
	sym_sec->set_type(elfio::SHT_SYMTAB);
	sym_sec->set_info(1);
	sym_sec->set_addr_align(0x4);
	sym_sec->set_entry_size(writer.get_default_entry_size(elfio::SHT_SYMTAB));
	sym_sec->set_link(str_sec->get_index());

	elfio::string_section_accessor stra(str_sec);
	elfio::symbol_section_accessor syma(writer, sym_sec);
	AOTCompilerRuntime aotrt(aot_sec, stra, syma, aot_symbols);

	for (auto const &page : objprof::GetProfile()) {
		AOTCompilePage(&aotrt, page);
	}

	aot_sec->set_data((char const *)aotrt.code_arena.BaseAddr(), aotrt.code_arena.GetUsedSize());

	auto header = AOTTabHeader{.n_sym = aot_symbols.size()};
	aottab_sec->append_data((char const *)&header, sizeof(header));
	aottab_sec->append_data((char const *)aot_symbols.data(), sizeof(AOTSymbol) * aot_symbols.size());
	syma.add_symbol(stra.add_string(AOT_SYM_AOTTAB), 0, aottab_sec->get_size(), elfio::STB_GLOBAL,
			elfio::STT_OBJECT, 0, aottab_sec->get_index());

	assert(writer.validate().empty());

	auto obj_path = objprof::GetCachePath(AOT_O_EXTENSION);
	auto so_path = objprof::GetCachePath(AOT_SO_EXTENSION);
	writer.save(obj_path);

	if (system(
		("/usr/bin/ld -z relro --hash-style=gnu -m elf_x86_64 -shared -o" + so_path + " " + obj_path)
		    .c_str()) < 0) {
		Panic();
	}
	FixupAOTTabSection();
}

static void FixupAOTTabSection()
{
	auto aot_path = objprof::GetCachePath(AOT_SO_EXTENSION);

	elfio::elfio elf;
	if (!elf.load(aot_path)) {
		log_aot("no such file: %s", aot_path.c_str());
	}

	elfio::section *sym_sec = nullptr;

	for (const auto &section : elf.sections) {
		auto test_sec = [&section](elfio::section **res, elfio::Elf_Word type, char const *name) {
			if (section->get_type() == type &&
			    std::string(section->get_name()) == std::string(name)) {
				*res = section.get();
			}
		};
		test_sec(&sym_sec, elfio::SHT_SYMTAB, ".symtab");
	}

	elfio::symbol_section_accessor syma(elf, sym_sec);

	auto resolve_sym = [&](std::string const &name) {
		elfio::Elf64_Addr addr;
		elfio::Elf_Xword size;
		u8 bind, type, other;
		elfio::Elf_Half shidx;
		if (!syma.get_symbol(name, addr, size, bind, type, shidx, other)) {
			Panic();
		}
		return std::make_pair(addr, shidx);
	};

	AOTTabHeader const *aottab{};
	size_t aottab_offs;
	{
		auto [vaddr, shidx] = resolve_sym("_aot_tab");
		auto section = elf.sections[shidx];
		// stupid action to retrive loaded data
		auto soffs = vaddr - section->get_address();
		aottab_offs = section->get_offset() + soffs;
		aottab = (AOTTabHeader const *)(section->get_data() + soffs);
	}
	auto const aottab_sz = aottab->n_sym;
	auto const aottab_mem_sz = sizeof(AOTTabHeader) + sizeof(AOTSymbol) * aottab_sz;

	auto aottab_res = (AOTTabHeader *)malloc(aottab_mem_sz);
	aottab_res->n_sym = aottab_sz;

	for (u64 idx = 0; idx < aottab_sz; ++idx) {
		auto gip = aottab->sym[idx].gip;
		aottab_res->sym[idx] = {gip, resolve_sym(MakeAotSymbol(gip)).first};
	}

	int fd = open(aot_path.c_str(), O_WRONLY);
	if (!fd) {
		Panic();
	}
	if (lseek(fd, aottab_offs, SEEK_SET) == (off_t)-1) {
		Panic();
	}
	if (write(fd, aottab_res, aottab_mem_sz) != aottab_mem_sz) {
		Panic();
	}
	close(fd);
}

} // namespace dbt
