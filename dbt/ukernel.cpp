#include "dbt/ukernel.h"
#include "dbt/mmu.h"
#include "dbt/execute.h"
#include <alloca.h>
#include <cstring>

extern "C" {
#include <fcntl.h>
#include <libgen.h>
#include <linux/limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
};

namespace dbt
{
LOG_STREAM(ukernel);

void ukernel::Execute(CPUState *state)
{
	while (true) {
		dbt::Execute(state);
		switch (state->trapno) {
		case rv32::TrapCode::EBREAK:
			log_ukernel("ebreak termiante");
			return;
		case rv32::TrapCode::ECALL:
			state->ip += 4;
#ifdef CONFIG_LINUX_GUEST
			ukernel::SyscallLinux(state);
#else
			ukernel::SyscallDemo(state);
#endif
			if (state->trapno != rv32::TrapCode::NONE) {
				log_ukernel("exiting...");
				return;
			}
			break;
		case rv32::TrapCode::ILLEGAL_INSN:
			log_ukernel("illegal instruction at %08x", state->ip);
			return;
		default:
			unreachable("no handle for trap");
		}
	}
}

void ukernel::InitThread(CPUState *state, ElfImage *elf)
{
	state->gpr[2] = elf->stack_start;
}

void ukernel::SyscallDemo(CPUState *state)
{
	state->trapno = rv32::TrapCode::NONE;
	u32 id = state->gpr[10];
	switch (id) {
	case 1: {
		log_ukernel("syscall readnum");
		u32 res;
		fscanf(stdin, "%d", &res);
		state->gpr[10] = res;
		return;
	}
	case 2: {
		log_ukernel("syscall writenum");
		fprintf(stdout, "%d\n", state->gpr[11]);
		return;
	}
	default:
		Panic("unknown syscall");
	}
}

void ukernel::SyscallLinux(CPUState *state)
{
	state->trapno = rv32::TrapCode::NONE;
	u32 args[7] = {state->gpr[10], state->gpr[11], state->gpr[12], state->gpr[13],
		       state->gpr[14], state->gpr[15], state->gpr[16]};
	u32 syscallno = state->gpr[17];
	u32 rc;

#define RCERRNO(rc)                                                                                          \
	do {                                                                                                 \
		if ((rc) < 0)                                                                                \
			(rc) = -errno;                                                                       \
	} while (0)

#define DEF_SYSCALL(no, name)                                                                                \
	break;                                                                                               \
	case no:                                                                                             \
		log_ukernel("sys_%s (no=%d)", #name, syscallno);

	switch (syscallno) {
		DEF_SYSCALL(64, write)
		{
			rc = write((i32)args[0], mmu::g2h(args[1]), args[2]);
			RCERRNO(rc);
		}
		DEF_SYSCALL(78, readlinkat)
		{
			char pathbuf[PATH_MAX];
			char const *path = (char *)mmu::g2h(args[1]);
			if (*path) {
				rc = PathResolution((i32)args[0], path, pathbuf);
				if (rc < 0) {
					rc = -errno;
					break;
				}
			} else {
				pathbuf[0] = 0;
			}
			rc = readlinkat((i32)args[0], pathbuf, (char *)mmu::g2h(args[2]), args[3]);
			RCERRNO(rc);
		}
		DEF_SYSCALL(94, exit_group)
		{
			rc = 0;
			state->trapno = rv32::TrapCode::TERMINATED;
		}
		DEF_SYSCALL(160, uname)
		{
			auto *un = (struct utsname *)mmu::g2h(args[0]);
			rc = uname(un);
			strcpy(un->machine, "riscv32");
			RCERRNO(rc);
		}
		DEF_SYSCALL(174, getuid)
		{
			rc = getuid();
			RCERRNO(rc);
		}
		DEF_SYSCALL(175, geteuid)
		{
			rc = geteuid();
			RCERRNO(rc);
		}
		DEF_SYSCALL(176, getgid)
		{
			rc = getgid();
			RCERRNO(rc);
		}
		DEF_SYSCALL(177, getegid)
		{
			rc = getegid();
			RCERRNO(rc);
		}
		DEF_SYSCALL(214, brk)
		{
			rc = do_sys_brk(args[0]);
		}
		DEF_SYSCALL(226, mprotect)
		{
			// TODO: pass to mmu
			rc = mprotect(mmu::base + args[0], args[1], args[2]);
			RCERRNO(rc);
		}
		DEF_SYSCALL(291, statx)
		{
			char pathbuf[PATH_MAX];
			char const *path = (char *)mmu::g2h(args[1]);
			if (*path) {
				rc = PathResolution((i32)args[0], path, pathbuf);
				if (rc < 0) {
					rc = -errno;
				}
			} else {
				pathbuf[0] = 0;
			}
			rc =
			    statx((i32)args[0], pathbuf, args[2], args[3], (struct statx *)mmu::g2h(args[4]));
			RCERRNO(rc);
		}
		break;
	default:
		log_ukernel("sys_ (no=%4d) is unknown", syscallno);
		Panic("unknown syscall");
	}
	log_ukernel("sys ret: %d", rc);
	state->gpr[10] = rc;
}

int ukernel::PathResolution(int dirfd, char const *path, char *resolved)
{
	char const *fake_root = "."; // TODO: set it proper somewhere
	char rp_fake_root[PATH_MAX];
	char rp_buf[PATH_MAX];

	log_ukernel("start path resolution: %s", path);

	if (!realpath(fake_root, rp_fake_root)) {
		Panic("can't resolve dbtroot");
	}
	size_t rp_fake_root_len = strlen(rp_fake_root);

	if (path[0] == '/') {
		snprintf(rp_buf, sizeof(rp_buf), "%s/%s", rp_fake_root, path);
	} else {
		if (dirfd == AT_FDCWD) {
			getcwd(rp_buf, sizeof(rp_buf));
		} else {
			char fdpath[64];
			sprintf(fdpath, "/proc/self/fd/%d", dirfd);
			if (readlink(fdpath, rp_buf, sizeof(rp_buf)) < 0) {
				log_ukernel("bad dirfd");
				return -1;
			}
		}
		size_t pref_sz = strlen(rp_buf);
		strncpy(rp_buf + pref_sz, path, sizeof(rp_buf) - pref_sz);
	}

	// TODO: make it preceise, resolve "/.." and symlinks
	if (!realpath(rp_buf, resolved)) {
		log_ukernel("unresolved path %s", rp_buf);
		return -1;
	}
	if (strncmp(resolved, rp_fake_root, rp_fake_root_len)) {
		Panic("escaped dbtroot");
	}

	if (path[0] != '/') {
		strcpy(resolved, path);
	}
	return 0;
}

u32 ukernel::do_sys_brk(u32 newbrk)
{
	if (newbrk <= brk) {
		log_ukernel("do_sys_brk: newbrk too small: %08x %08x", newbrk, brk);
		return brk;
	}
	u32 brk_p = roundup(brk, mmu::PAGE_SIZE);
	if (newbrk < brk_p) {
		memset(mmu::g2h(newbrk), 0, newbrk - brk);
		return newbrk;
	}
	void *mem =
	    mmu::MMap(brk_p, newbrk - brk_p, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED);
	if (mmu::h2g(mem) != brk_p) {
		log_ukernel("do_sys_brk: not enough mem");
		munmap(mem, newbrk - brk_p);
		return brk;
	}
	memset(mmu::g2h(brk), 0, brk_p - brk);
	return newbrk;
}

// -march=rv32i -O2 -fpic -fpie -static
// -march=rv32i -O2 -fpic -fpie -static -ffreestanding -nostartfiles -nolibc
void ukernel::LoadElf(const char *path, ElfImage *elf)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		Panic("no such elf file");
	}

	{ // TODO: for AT_FDCWD resolution, remove it
		char buf[PATH_MAX];
		strncpy(buf, path, sizeof(buf));
		chdir(dirname(buf));
	}

	auto &ehdr = elf->ehdr;

	if (pread(fd, &elf->ehdr, sizeof(ehdr), 0) != sizeof(ehdr)) {
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

	ssize_t phtab_sz = sizeof(Elf32_Phdr) * ehdr.e_phnum;
	auto *phtab = (Elf32_Phdr *)alloca(phtab_sz);
	if (pread(fd, phtab, phtab_sz, ehdr.e_phoff) != phtab_sz) {
		Panic("can't read phtab");
	}
	elf->load_addr = -1;
	elf->brk = 0;
	elf->entry = elf->ehdr.e_entry;

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
		auto vaddr_ps = rounddown(phdr->p_vaddr, mmu::PAGE_SIZE);
		auto vaddr_po = vaddr - vaddr_ps;

		if (phdr->p_filesz != 0) {
			u32 len = roundup(phdr->p_filesz + vaddr_po, mmu::PAGE_SIZE);
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

		elf->load_addr = std::min(elf->load_addr, vaddr - phdr->p_offset);
		elf->brk = std::max(elf->brk, vaddr + phdr->p_memsz);
	}
	close(fd);

	brk = elf->brk; // TODO: move it out

	static constexpr u32 stk_size = 32 * mmu::PAGE_SIZE;
#if 0
	void *stk_ptr = mmu::MMap(0, stk_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE);
#else
	[[maybe_unused]] void *stk_guard = mmu::MMap(mmu::ASPACE_SIZE - mmu::PAGE_SIZE, mmu::PAGE_SIZE, 0,
						     MAP_ANON | MAP_PRIVATE | MAP_FIXED);
	// ASAN somehow breaks MMap lookup if it's not MAP_FIXED
	void *stk_ptr = mmu::MMap(mmu::ASPACE_SIZE - mmu::PAGE_SIZE - stk_size, stk_size,
				  PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED);
#endif
	elf->stack_start = mmu::h2g(stk_ptr) + stk_size;
}

static u32 AllocAVectorStr(u32 stk, void const *str, u16 sz)
{
	stk -= sz;
	memcpy(mmu::g2h(stk), str, sz);
	return stk;
}

static inline u32 AllocAVectorStr(u32 stk, char const *str)
{
	return AllocAVectorStr(stk, str, strlen(str) + 1);
}

void ukernel::InitAVectors(ElfImage *elf, int argv_n, char **argv)
{
	u32 stk = elf->stack_start;

	u32 foo_str_g = stk = AllocAVectorStr(stk, "__foo_str__");
	u32 lc_all_str_g = stk = AllocAVectorStr(stk, "LC_ALL=C");
	char auxv_salt[16] = {0, 1, 2, 3, 4, 5, 6};
	u32 auxv_salt_g = stk = AllocAVectorStr(stk, auxv_salt, sizeof(auxv_salt));

	u32 *argv_strings_g = (u32 *)alloca(sizeof(char *) * argv_n);
	for (int i = 0; i < argv_n; ++i) {
		argv_strings_g[i] = stk = AllocAVectorStr(stk, argv[i]);
	}

	stk &= -4;

	int envp_n = 1;
	int auxv_n = 64;

	int stk_vsz = argv_n + envp_n + auxv_n + 3;
	stk -= stk_vsz * sizeof(u32);
	u32 argc_p = stk;
	u32 argv_p = argc_p + sizeof(u32);
	u32 envp_p = argv_p + sizeof(u32) * (argv_n + 1);
	u32 auxv_p = envp_p + sizeof(u32) * (envp_n + 1);

#define PUSH_AVVAL(vec, val)                                                                                 \
	do {                                                                                                 \
		*(u32 *)mmu::g2h(vec) = (val);                                                               \
		vec += sizeof(u32);                                                                          \
	} while (0)

#define PUSH_AUXV(idx, val)                                                                                  \
	do {                                                                                                 \
		*(u32 *)mmu::g2h(auxv_p) = (idx);                                                            \
		*(u32 *)mmu::g2h(auxv_p + sizeof(u32)) = (val);                                              \
		auxv_p += 2 * sizeof(u32);                                                                   \
		/* log_ukernel("put auxv %08x=%08x", (idx), (val)); */                                       \
	} while (0)

	PUSH_AVVAL(argc_p, argv_n);

	for (int i = 0; i < argv_n; ++i) {
		PUSH_AVVAL(argv_p, argv_strings_g[i]);
	}
	PUSH_AVVAL(argv_p, 0);

	PUSH_AVVAL(envp_p, lc_all_str_g);
	PUSH_AVVAL(envp_p, 0);

	PUSH_AUXV(AT_PHDR, elf->ehdr.e_phoff + elf->load_addr);
	PUSH_AUXV(AT_PHENT, sizeof(Elf32_Phdr));
	PUSH_AUXV(AT_PHNUM, elf->ehdr.e_phnum);
	PUSH_AUXV(AT_PAGESZ, mmu::PAGE_SIZE);
	PUSH_AUXV(AT_BASE, 0);
	PUSH_AUXV(AT_FLAGS, 0);
	PUSH_AUXV(AT_ENTRY, elf->entry);

	PUSH_AUXV(AT_UID, getuid());
	PUSH_AUXV(AT_GID, getgid());
	PUSH_AUXV(AT_EUID, geteuid());
	PUSH_AUXV(AT_EGID, getegid());

	PUSH_AUXV(AT_EXECFN, foo_str_g);
	PUSH_AUXV(AT_SECURE, false);
	PUSH_AUXV(AT_HWCAP, 0);
	PUSH_AUXV(AT_CLKTCK, sysconf(_SC_CLK_TCK));
	PUSH_AUXV(AT_RANDOM, auxv_salt_g);

	PUSH_AUXV(AT_NULL, 0);

	elf->stack_start = stk;
}

} // namespace dbt
