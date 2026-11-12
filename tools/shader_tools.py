#!/usr/bin/env python3
"""The shader compilers of the macOS build: glslang (HLSL -> SPIR-V) and SPIRV-Cross (SPIR-V -> MSL), built once from
the pinned sources in tools/shader_tools/CMakeLists.txt into build/build-tools/shader-tools (about a minute) - nothing
to install (DECISIONS.md 2026-11-12). tools/toolchain.py calls ensure() before configuring the game or the launcher on
macOS and passes the two programs to CMake (VCS_GLSLANG, VCS_SPIRV_CROSS).

    python3 tools/shader_tools.py [--rebuild]     build them (or say where they are)

VCS_GLSLANG / VCS_SPIRV_CROSS take other copies (e.g. Homebrew's glslang and spirv-cross).
"""
import argparse
import hashlib
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

SOURCE = os.path.join(tc.ROOT, "tools", "shader_tools")
OUT = os.path.join(tc.TOOLS_DIR, "shader-tools")
STAMP = os.path.join(OUT, "stamp.json")


def _pins():
    """A hash of the project file (its pins): a new pin builds the tools again."""
    with open(os.path.join(SOURCE, "CMakeLists.txt"), "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def _programs():
    bin_dir = os.path.join(OUT, "build", "bin")
    return {"glslang": os.path.join(bin_dir, tc.exe("glslang")),
            "spirv_cross": os.path.join(bin_dir, tc.exe("spirv-cross"))}


def current():
    """The two programs when they are built from the current pins, else None."""
    programs = _programs()
    try:
        with open(STAMP, encoding="utf-8") as f:
            stamp = json.load(f)
    except (OSError, ValueError):
        return None
    if stamp.get("pins") != _pins() or not all(os.path.isfile(p) for p in programs.values()):
        return None
    return programs


def ensure(env, rebuild=False):
    """{"glslang": path, "spirv_cross": path}: VCS_GLSLANG / VCS_SPIRV_CROSS when set, else the built ones (built
    now when missing or from other pins)."""
    given = {"glslang": env.get("VCS_GLSLANG"), "spirv_cross": env.get("VCS_SPIRV_CROSS")}
    if all(given.values()):
        return given
    programs = None if rebuild else current()
    if programs:
        return programs
    tc.stage("shader_tools", "Building the shader compilers (glslang, SPIRV-Cross)")
    print("[shader-tools] building glslang and SPIRV-Cross into " + tc.show(OUT))
    build = os.path.join(OUT, "build")
    if rebuild:
        shutil.rmtree(build, ignore_errors=True)
    os.makedirs(build, exist_ok=True)
    tools = tc.build_tools(env)
    cmd = ["cmake", "-S", SOURCE, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
           "-DCMAKE_MAKE_PROGRAM=" + tools["ninja"].replace(os.sep, "/")]
    if tc.IS_MACOS:
        cmd += tc.macos_compiler_defines()
    cmd += tc.deps_defines()
    log = os.path.join(OUT, "configure.log")
    if tc.run_with_progress(cmd, log, "[shader-tools] configure", env=env) != 0:
        print(tc.tail(log, 30))
        tc.fail("configuring the shader compilers failed (full log: %s)" % tc.show(log), tc.configure_failure(log), log)
    log = os.path.join(OUT, "build.log")
    if tc.run_with_progress(["cmake", "--build", build, "--target", "vcs_shader_tools"], log,
                            "[shader-tools] build", env=env) != 0:
        print(tc.tail(log, 30))
        tc.fail("building the shader compilers failed (full log: %s)" % tc.show(log), "build_failed", log)
    programs = _programs()
    missing = [p for p in programs.values() if not os.path.isfile(p)]
    if missing:
        tc.fail("the shader compilers' build made no %s" % ", ".join(missing), "build_failed")
    with open(STAMP, "w", encoding="utf-8") as f:
        json.dump({"pins": _pins()}, f)
    print("[shader-tools] glslang: %s\n[shader-tools] spirv-cross: %s" % (programs["glslang"], programs["spirv_cross"]))
    return programs


def cmake_defines(env):
    """-DVCS_GLSLANG / -DVCS_SPIRV_CROSS for a CMake configure."""
    programs = ensure(env)
    return ["-DVCS_GLSLANG=" + programs["glslang"].replace(os.sep, "/"),
            "-DVCS_SPIRV_CROSS=" + programs["spirv_cross"].replace(os.sep, "/")]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rebuild", action="store_true", help="build them again from scratch")
    a = ap.parse_args()
    programs = ensure(tc.build_env(), rebuild=a.rebuild)
    for name, path in programs.items():
        print("%s: %s" % (name, path))


if __name__ == "__main__":
    main()
