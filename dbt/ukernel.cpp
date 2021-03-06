#include "dbt/ukernel.h"
#include "dbt/core.h"
#include "dbt/execute.h"
#include "dbt/rv32i_runtime.h"
#include <alloca.h>
#include <cstring>

extern "C" {
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
};

namespace dbt
{
void ukernel::Execute(rv32i::CPUState *state)
{
	while (true) {
		dbt::Execute(state);
		switch (state->trapno) {
		case rv32i::TrapCode::EBREAK:
			std::cout << "[APP]: ebreak terminate\n";
			return;
		case rv32i::TrapCode::ECALL:
			state->ip += 4;
			ukernel::Syscall(state);
			break;
		default:
			unreachable("no handle for trap");
		}
	}
}

void ukernel::InitThread(rv32i::CPUState *state, ElfImage *elf)
{
	state->gpr[2] = elf->stack_start;
}

void ukernel::Syscall(rv32i::CPUState *state)
{
	state->trapno = rv32i::TrapCode::NONE;
	u32 id = state->gpr[10];
	switch (id) {
	case 1: {
		std::cout << "[APP]: input request\n";
		u32 res;
		std::cin >> res;
		state->gpr[10] = res;
		return;
	}
	case 2: {
		std::cout << "[APP]: " << state->gpr[11] << "\n";
		return;
	}
	default:
		Panic("unknown syscallno");
	}
}

// -ffreestanding -march=rv32i -O0 -fpic -fpie -nostartfiles -nolibc -static
void ukernel::LoadElf(const char *path, ElfImage *elf)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		Panic("no such elf file");
	}

	Elf32_Ehdr ehdr;

	if (pread(fd, &ehdr, sizeof(ehdr), 0) != sizeof(ehdr)) {
		Panic("can't read elf header");
	}
	if (memcmp(ehdr.e_ident,
		   "\x7f"
		   "ELF",
		   4)) {
		Panic("that's not elf");
	}
	if (ehdr.e_machine != EM_RISCV) {
		Panic("elf's machine doesn't match");
	}
	if (ehdr.e_type != ET_EXEC) {
		Panic("unuspported elf type");
	}

	elf->entry = ehdr.e_entry;

	ssize_t phtab_sz = sizeof(Elf32_Phdr) * ehdr.e_phnum;
	auto *phtab = (Elf32_Phdr *)alloca(phtab_sz);
	if (pread(fd, phtab, phtab_sz, ehdr.e_phoff) != phtab_sz) {
		Panic("can't read phtab");
	}

	for (size_t i = 0; i < ehdr.e_phnum; ++i) {
		auto *phdr = &phtab[i];
		if (phdr->p_type != PT_LOAD)
			continue;

		int prot = 0;
		if (phdr->p_flags & PF_R) {
			prot |= PROT_READ;
		}
		if (phdr->p_flags & PF_W) {
			prot |= PROT_WRITE;
		}
		if (phdr->p_flags & PF_X) {
			prot |= PROT_EXEC;
		}

		auto vaddr = phdr->p_vaddr;
		auto vaddr_ps = rounddown(phdr->p_vaddr, (u32)mmu::PAGE_SIZE);
		auto vaddr_po = vaddr - vaddr_ps;

		if (phdr->p_filesz != 0) {
			u32 len = roundup(phdr->p_filesz + vaddr_po, (u32)mmu::PAGE_SIZE);
			// shared flags
			mmu::MMap(vaddr_ps, len, prot, MAP_FIXED | MAP_PRIVATE, fd,
				  phdr->p_offset - vaddr_po);
			if (phdr->p_memsz > phdr->p_filesz) {
				auto bss_start = vaddr + phdr->p_filesz;
				auto bss_end = vaddr_ps + phdr->p_memsz;
				auto bss_start_nextp = roundup(bss_start, (u32)mmu::PAGE_SIZE);
				auto bss_len = roundup(bss_end - bss_start, (u32)mmu::PAGE_SIZE);
				mmu::MMap(bss_start_nextp, bss_len, prot, MAP_FIXED | MAP_PRIVATE | MAP_ANON);
				u32 prev_sz = bss_start_nextp - bss_start;
				if (prev_sz != 0) {
					memset(mmu::g2h(bss_start), 0, prev_sz);
				}
			}
		} else if (phdr->p_memsz != 0) {
			u32 len = roundup(phdr->p_memsz + vaddr_po, (u32)mmu::PAGE_SIZE);
			mmu::MMap(vaddr_ps, len, prot, MAP_FIXED | MAP_PRIVATE | MAP_ANON);
		}
	}

	static constexpr u32 stk_size = 32 * mmu::PAGE_SIZE;
#if 0
	void *stk_ptr = mmu::MMap(0, stk_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE);
#else
	// ASAN somehow breaks MMap lookup if it's not MAP_FIXED
	void *stk_ptr = mmu::MMap(mmu::ASPACE_SIZE - stk_size, stk_size, PROT_READ | PROT_WRITE,
				  MAP_ANON | MAP_PRIVATE | MAP_FIXED);
#endif
	elf->stack_start = mmu::h2g(stk_ptr) + stk_size - 2 * 4; // argc, argv
}

} // namespace dbt
