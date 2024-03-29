import csv
import optparse
import os
import subprocess
import filecmp
import sys
import time
from typing import Type


class BaseExec:
    def __init__(self):
        self.name = None
        self.root = None
        self.args = None
        self.rc = None
        self.out = None
        self.err = None
        self.time = None

    def setup(self, root, cmd):
        self.root = root
        self.args = cmd

    def run(self):
        pass


class QEMUExec(BaseExec):
    bin_path = "qemu-riscv32"

    def __init__(self):
        self.name = "qemu"

    def run(self):
        pargs = [QEMUExec.bin_path] + self.args
        timer = time.time()
        p = subprocess.Popen(pargs, cwd=self.root,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.out, self.err = p.communicate()
        timer = time.time() - timer
        self.rc = p.returncode
        self.time = timer


class RVDBTExec(BaseExec):
    build_dir = None

    def __init__(self, aot, llvm=False):
        super().__init__()
        self.name = "rvdbt-" + ("jit", ("qcgaot", "llvmaot")[llvm])[aot]
        self.aot = aot
        self.llvm = llvm

    def setup(self, root, cmd):
        super().setup(root, cmd)
        if not self.aot:
            self.setup_ok = True
            return
        pargs = [RVDBTExec.build_dir + "/bin/elfaot",
                 "--cache=dbtcache",
                 "--llvm=" + ("off", "on")[self.llvm],
                 "--elf=" + self.root + "/" + self.args[0]]
        p = subprocess.Popen(pargs, cwd=RVDBTExec.build_dir,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.out, self.err = p.communicate()
        self.rc = p.returncode
        self.setup_ok = self.rc == 0

    def run(self):
        if not self.setup_ok:
            return
        pargs = [RVDBTExec.build_dir + "/bin/elfrun",
                 "--cache=dbtcache",
                 "--fsroot=" + self.root]
        pargs += ["--aot=" + ("off", "on")[self.aot]]
        pargs += ["--"] + self.args
        timer = time.time()
        p = subprocess.Popen(pargs, cwd=RVDBTExec.build_dir,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.out, self.err = p.communicate()
        timer = time.time() - timer
        self.rc = p.returncode
        self.time = timer


class Benchmark:
    def __init__(self, root, args, cmp_out=False, ofile=None):
        self.root = root
        self.args = args
        self.cmp_out = cmp_out
        self.ofile = self.root + "/" + ofile if ofile is not None else None

    def get_ofile(self, exec):
        assert (self.ofile is not None)
        return self.ofile + "." + exec.name

    def launch_with(self, exec: BaseExec):
        exec.setup(self.root, self.args)
        exec.run()
        if self.ofile is not None and os.path.isfile(self.ofile):
            os.rename(self.ofile, self.get_ofile(exec))
        return exec

    def verify(self, exec: BaseExec, exec_ref: BaseExec):
        if exec_ref.rc != exec.rc:
            return "retcode,out=" + str(exec.err)

        if self.cmp_out and exec_ref.out != exec.out:
            return "stdout"

        if self.ofile is not None:
            if not os.path.isfile(self.get_ofile(exec)):
                return "outfile-missing"
            if not filecmp.cmp(self.get_ofile(exec_ref), self.get_ofile(exec)):
                return "outfile-diff"

        return None

    class Result:
        def __init__(self, res, time, score):
            self.res = res
            self.time = time
            self.score = score

    def result(self, exec: BaseExec, exec_ref: BaseExec = None) -> Result:
        is_ref = ((exec == exec_ref) or (exec_ref is None))
        if is_ref:
            res = "ok"
        else:
            res = self.verify(exec, exec_ref)
            res = (res, "ok")[res is None]
        res += ":" + str(exec.rc)
        score = None
        if exec.time is not None:
            score = f"{exec_ref.time / exec.time:.3f}"
        return Benchmark.Result(res, exec.time, score)


# mibench.automotive
def GetBenchmarks_Automotive(prebuilts_dir):
    b: list[Benchmark] = []
    root = os.path.join(prebuilts_dir + "/automotive")
    # b.append(Benchmark(root + "/basicmath", ["basicmath_small"], True))
    b.append(Benchmark(root + "/basicmath", ["basicmath_large"]))
    b.append(Benchmark(root + "/bitcnts", ["bitcnts", "10000000"]))
    # b.append(Benchmark(root + "/qsort",
    #         ["qsort_small", "input_small.dat"], True))
    b.append(Benchmark(root + "/qsort",
             ["qsort_large", "input_large.dat"], True))
    b.append(Benchmark(root + "/susan",
             ["susan", "input_large.pgm", "_bout", "-s"], False, "_bout"))
    return b


# mibench.network
def GetBenchmarks_Network(prebuilts_dir):
    b: list[Benchmark] = []
    root = os.path.join(prebuilts_dir + "/network")
    # b.append(Benchmark(root + "/dijkstra",
    #        ["dijkstra_small", "input.dat"], True))
    b.append(Benchmark(root + "/dijkstra",
             ["dijkstra_large", "input.dat"], True))
    return b


# mibench.security
def GetBenchmarks_Security(prebuilts_dir):
    b: list[Benchmark] = []
    root = os.path.join(prebuilts_dir + "/security")
    b.append(Benchmark(root + "/blowfish",
             ["bf", "e", "myinput.asc", "_bout", "1234567890abcdeffedcba0987654321"], False, "_bout"))
    return b


# mibench.telecomm
def GetBenchmarks_Telecomm(prebuilts_dir):
    b: list[Benchmark] = []
    root = os.path.join(prebuilts_dir + "/telecomm")
    b.append(Benchmark(root + "/FFT", ["fft", "8", "32768"], True))
    b.append(Benchmark(root + "/gsm",
             ["toast", "-fps", "-c", "large.au"], True))
    return b


# coremark
def GetBenchmarks_Coremark(prebuilts_dir):
    b: list[Benchmark] = []
    root = os.path.join(prebuilts_dir + "/coremark")
    b.append(Benchmark(
        root + "/", ["coremark.exe", "0x0", "0x0", "0x66", "15000", "7", "1", "2000"]))
    return b


def GetBenchmarks(opts):
    benchmarks: list[Benchmark] = []
    benchmarks += GetBenchmarks_Automotive(opts.prebuilts_dir)
    # benchmarks += GetBenchmarks_Network(opts.prebuilts_dir)
    benchmarks += GetBenchmarks_Security(opts.prebuilts_dir)
    benchmarks += GetBenchmarks_Telecomm(opts.prebuilts_dir)
    benchmarks += GetBenchmarks_Coremark(opts.prebuilts_dir)
    return benchmarks


def RunTests(opts):
    benchmarks = GetBenchmarks(opts)

    def get_execs(): return [QEMUExec(), RVDBTExec(
        False), RVDBTExec(True, False), RVDBTExec(True, True)]

    csvtab = [["tab"] + list(map(lambda e: e.name, get_execs()))]

    for b in benchmarks:
        print(b.root + " " + " ".join(b.args))
        scores = [b.root.removeprefix(opts.prebuilts_dir)]

        execs = get_execs()
        ref_exec = execs[0]
        for e in execs:
            b.launch_with(e)
            res = b.result(e, ref_exec)
            print([e.name, res.res, res.time, res.score])
            scores += [res.score]

        csvtab += [scores]

    csv.writer(sys.stdout).writerows(csvtab)


def main():
    op = optparse.OptionParser()
    op.add_option("--build-dir", dest="build_dir")
    op.add_option("--prebuilts", dest="prebuilts_dir")

    (opts, args) = op.parse_args()
    if not opts.build_dir or not opts.prebuilts_dir:
        op.error("usage")

    RVDBTExec.build_dir = opts.build_dir

    RunTests(opts)


main()
