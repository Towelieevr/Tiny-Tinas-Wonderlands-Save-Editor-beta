#!/usr/bin/env python3

import glob, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..", "tools")
OO = os.path.abspath(os.path.join(TOOLS, "OpenOrbis", "OpenOrbis", "PS4Toolchain"))
SDK = os.path.abspath(os.path.join(TOOLS, "GoldHEN_Plugins_SDK"))
REPO = os.path.abspath(os.path.join(TOOLS, "GoldHEN_Plugins_Repository"))
LLVM = r"C:\Program Files\LLVM\bin"
CC, AR, LD = (os.path.join(LLVM, x) for x in ("clang.exe", "llvm-ar.exe", "ld.lld.exe"))
FSELF = os.path.join(OO, "bin", "windows", "create-fself.exe")
os.environ["OO_PS4_TOOLCHAIN"] = OO

def run(cmd, cwd=None):
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    if r.returncode or r.stderr.strip():
        print(" ".join(f'"{c}"' if " " in c else c for c in cmd[:3]), "...")
        print(r.stdout + r.stderr)
    if r.returncode: sys.exit(f"FAILED ({r.returncode})")

def sdk():
    flags = ["--target=x86_64-pc-freebsd12-elf", "-DORBIS=1", "-D__ORBIS__=1", "-DPS4=1", "-DOO=1", "-D__PS4__=1",
             "-DOOPS4=1", "-D__OOPS4__=1", "-D__BSD_VISIBLE=1", "-D_BSD_SOURCE=1", "-march=btver2", "-mtune=btver2",
             "-fPIC", "-std=c11", "-isysroot", OO, "-isystem", os.path.join(OO, "include"), "-I", os.path.join(SDK, "include")]
    out = os.path.join(SDK, "build"); os.makedirs(out, exist_ok=True); objs = []
    for src in glob.glob(os.path.join(SDK, "source", "*.c")):
        o = os.path.join(out, os.path.basename(src)[:-2] + ".o"); run([CC, *flags, "-c", src, "-o", o]); objs.append(o)
    crt = os.path.join(out, "crtprx.o")
    run([CC, "-target", "x86_64-pc-linux-gnu", "-ffreestanding", "-nostdlib", "-fno-builtin", "-fPIC", "-isysroot", OO,
         "-isystem", os.path.join(OO, "include"), "-I", os.path.join(SDK, "include"), "-c", os.path.join(SDK, "crt", "crtprx.c"), "-o", crt])
    lib = os.path.join(SDK, "libGoldHEN_Hook.a")
    if os.path.exists(lib): os.remove(lib)
    run([AR, "--format=bsd", "rcs", lib, *objs, crt])
    print("built", lib, os.path.getsize(lib), "bytes")

def plugin(d):
    d = os.path.abspath(d); name = os.path.basename(d.rstrip("\\/"))
    out = os.path.join(d, "build"); os.makedirs(out, exist_ok=True)
    defs = ["-D_BSD_SOURCE=1", "-D__BSD_VISIBLE=1", "-D__PS4__=1", "-DOO=1", "-D__OPENORBIS__=1", "-D__OOPS4__=1", "-D__FINAL__=1"]
    flags = ["--target=x86_64-pc-freebsd12-elf", "-fPIC", "-funwind-tables", "-O2", "-mstackrealign", "-isysroot", OO, "-isystem", os.path.join(OO, "include"),
             "-I", os.path.join(SDK, "include"), "-I", os.path.join(REPO, "common"), "-I", os.path.join(d, "include"),
             "-Wno-c99-designator", "-fvisibility=hidden", *defs]
    gv = os.path.join(REPO, "common", "git_ver.h")
    if not os.path.exists(gv):
        open(gv, "w").write('#pragma once\n#define GIT_COMMIT "local"\n#define GIT_VER "local"\n#define GIT_NUM 0\n#define BUILD_DATE __DATE__\n')
    objs = []
    for src in glob.glob(os.path.join(REPO, "common", "*.c")) + glob.glob(os.path.join(d, "source", "*.c")):
        o = os.path.join(out, os.path.basename(src) + ".o"); run([CC, *flags, f"-fmacro-prefix-map={os.path.dirname(src)}{os.sep}=", "-c", src, "-o", o]); objs.append(o)
    elf = os.path.join(out, name + ".elf")
    libs = ["-lGoldHEN_Hook", "-lkernel", "-lc", "-lSceSysmodule", "-lSceSystemService", "-lScePad", "-lSceUserService"]
    run([LD, "-m", "elf_x86_64", "-pie", "--export-dynamic", "-e", "_init", "--script", os.path.join(OO, "link.x"), "--eh-frame-hdr",
         "-L", os.path.join(OO, "lib"), "-L", SDK, *objs, *libs, "-o", elf])
    prx = os.path.join(out, name + ".prx")
    run([FSELF, f"-in={name}.elf", f"-out={name}.oelf", f"-lib={name}.prx", "--paid", "0x3800000000000011"], cwd=out)
    print("built", prx, os.path.getsize(prx), "bytes")

if __name__ == "__main__":
    {"sdk": lambda: sdk(), "plugin": lambda: plugin(sys.argv[2])}[sys.argv[1]]()
