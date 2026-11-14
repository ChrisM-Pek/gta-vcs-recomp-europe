include(CheckIPOSupported)

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

# [gta-recomp] The CPU baseline (PS2X_CPU, set by tools/build.py from the PC that builds): AVX2 (/arch:AVX2,
# x86-64-v3: AVX2, FMA, BMI1/2), AVX for CPUs before Haswell (/arch:AVX: clang-cl targets Sandy Bridge - SSE4.2,
# POPCNT, AVX, no FMA) or SSE4 for CPUs without AVX (x86-64-v2). A binary built for AVX2 stops at its first AVX2 / BMI instruction on an older CPU (illegal
# instruction, before it prints anything). The code checks __AVX2__ / __SSE4_1__ and has a path for each.
if(NOT PS2X_CPU)
    set(PS2X_CPU "AVX2")
endif()
string(TOUPPER "${PS2X_CPU}" PS2X_CPU)
if(PS2X_CPU STREQUAL "AVX2")
    set(_ps2x_arch_msvc "/arch:AVX2")
    set(_ps2x_arch_clang "-march=x86-64-v3")
elseif(PS2X_CPU STREQUAL "AVX")
    set(_ps2x_arch_msvc "/arch:AVX")
    set(_ps2x_arch_clang "-march=sandybridge")  # the same CPU as clang-cl's /arch:AVX
elseif(PS2X_CPU STREQUAL "SSE4")
    set(_ps2x_arch_msvc "/clang:-march=x86-64-v2")  # SSE4.2, SSSE3, POPCNT (clang-cl's /arch has no SSE4 level)
    set(_ps2x_arch_clang "-march=x86-64-v2")
elseif(PS2X_CPU STREQUAL "ARM64")
    # [gta-recomp] macOS on Apple Silicon (docs/MACOS.md): the M1 is the baseline of every M-series chip. The SSE code
    # runs through sse2neon (../CMakeLists.txt, USE_SSE2NEON).
    set(_ps2x_arch_msvc "")
    set(_ps2x_arch_clang "-mcpu=apple-m1")
else()
    message(FATAL_ERROR "PS2X_CPU=${PS2X_CPU}: AVX2, AVX, SSE4 or ARM64")
endif()

function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    # [gta-recomp] clang-cl rejects /Qspectre- and /GL//LTCG need MSVC's own
    # linker; use plain optimisation flags there and leave LTO to IPO below.
    # LTO is opt-in (PS2X_LTO=THIN|FULL): with full LTO every relink of the
    # 150 MB game re-codegens everything, which kills iteration time.
    # -ffp-contract=off: /arch:AVX2 enables FMA and clang would fuse `a + b * c` into one rounding; the PS2's
    # FMACs (VU and EE FPU) round the product and the sum separately.
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:/O2 /Oi /Gy /Gw /GF /DNDEBUG ${_ps2x_arch_msvc} /GS- /clang:-fno-math-errno /clang:-ffp-contract=off>
        )
        if(PS2X_LTO STREQUAL "THIN")
            target_compile_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-flto=thin>)
            # [gta-recomp] the cache is pruned: at most 1 GB, nothing older than 3 days
            target_link_options(${TargetName} PRIVATE $<$<CONFIG:Release>:/lldltocache:${CMAKE_BINARY_DIR}/lto-cache
                                /lldltocachepolicy:cache_size_bytes=1g:prune_after=72h>)
        elseif(PS2X_LTO STREQUAL "FULL" AND IPO_SUPPORTED)
            set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        endif()
        # [gta-recomp] Profile-guided optimisation (PS2X_PGO, tools/build.py --opt pgo): GEN = instrumented build that
        # writes a .profraw when the game exits; a .profdata path = optimise with that profile. Functions the profile
        # doesn't match (code changed since) are optimised as without it.
        if(PS2X_PGO STREQUAL "GEN")
            target_compile_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-fprofile-instr-generate>)
            target_compile_definitions(${TargetName} PRIVATE $<$<CONFIG:Release>:PS2X_PGO_GEN=1>)
            get_target_property(_type ${TargetName} TYPE)
            if(_type STREQUAL "EXECUTABLE")
                execute_process(COMMAND ${CMAKE_CXX_COMPILER} /clang:-print-resource-dir OUTPUT_VARIABLE _resource
                                OUTPUT_STRIP_TRAILING_WHITESPACE)
                target_link_options(${TargetName} PRIVATE "${_resource}/lib/windows/clang_rt.profile-x86_64.lib")
            endif()
        elseif(PS2X_PGO)
            target_compile_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-fprofile-instr-use=${PS2X_PGO}
                                   -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled -Wno-profile-instr-missing
                                   -Wno-backend-plugin>)
        endif()
        return()
    endif()
    # [gta-recomp] Linux (clang, the only supported compiler there): the same code and numerics as clang-cl above.
    # x86-64-v3 is /arch:AVX2 (AVX2, FMA, BMI); -ffp-contract=off for the same reason. clang-cl defaults to
    # -fno-strict-aliasing (MSVC compatibility) and the runtime and generated code read guest memory through casts, so
    # say it here. No stack protector / CET (Ubuntu turns them on; clang-cl gets /GS-, no CET).
    # [gta-recomp] macOS: Apple's clang takes the same flags (it fell into the generic branch below before and lost
    # -ffp-contract=off). arm64 always has FMA, so FP contraction is off in every configuration there, not only Release.
    if((CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang") AND NOT MSVC)
        if(APPLE)
            target_compile_options(${TargetName} PRIVATE -ffp-contract=off -fno-strict-aliasing
                $<$<CONFIG:Release>:-O2 -DNDEBUG ${_ps2x_arch_clang} -fno-math-errno -fno-stack-protector>)
        else()
            target_compile_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:-O2 -DNDEBUG ${_ps2x_arch_clang} -ffp-contract=off -fno-math-errno -fno-strict-aliasing
                                    -fno-stack-protector -fcf-protection=none>
            )
        endif()
        get_target_property(_type ${TargetName} TYPE)
        if(PS2X_LTO STREQUAL "THIN" AND APPLE)
            # [gta-recomp] Apple's linker (ld64) runs ThinLTO through libLTO: its cache and job flags differ from lld's
            target_compile_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-flto=thin>)
            if(_type STREQUAL "EXECUTABLE")
                target_link_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-flto=thin
                                    -Wl,-cache_path_lto,${CMAKE_BINARY_DIR}/lto-cache
                                    -Wl,-prune_after_lto,259200 -Wl,-max_relative_cache_size_lto,10
                                    -Wl,-object_path_lto,${CMAKE_BINARY_DIR}/lto-objects>)
                # (ld64 has no ThinLTO job count: libLTO takes one backend per core)
            endif()
        elseif(PS2X_LTO STREQUAL "THIN")
            target_compile_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-flto=thin>)
            if(_type STREQUAL "EXECUTABLE")
                target_link_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-flto=thin
                                    -Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/lto-cache
                                    -Wl,--thinlto-cache-policy=cache_size_bytes=1g:prune_after=72h>)
                # ThinLTO's backends run in parallel; each needs memory for a share of the 150 MB game
                if(PS2X_LTO_JOBS)
                    target_link_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-Wl,--thinlto-jobs=${PS2X_LTO_JOBS}>)
                endif()
            endif()
        elseif(PS2X_LTO STREQUAL "FULL" AND IPO_SUPPORTED)
            set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        endif()
        if(PS2X_PGO STREQUAL "GEN")
            target_compile_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-fprofile-instr-generate>)
            target_compile_definitions(${TargetName} PRIVATE $<$<CONFIG:Release>:PS2X_PGO_GEN=1>)
            if(_type STREQUAL "EXECUTABLE")
                target_link_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-fprofile-instr-generate>)
            endif()
        elseif(PS2X_PGO)
            target_compile_options(${TargetName} PRIVATE $<$<CONFIG:Release>:-fprofile-instr-use=${PS2X_PGO}
                                   -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled -Wno-profile-instr-missing
                                   -Wno-backend-plugin>)
        endif()
        return()
    endif()
    if(MSVC)
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:
                /O2 # speed
                /Ob2 # inline aggressively
                /Oi # intrinsics
                /GL # whole program opt
                /Gy # function-level linking
                /Gw # global data in COMDAT
                /GF # string pooling
                /Zc:inline # remove unreferenced inline
                /fp:fast # fast math (graphics friendly)
                /DNDEBUG
                ${_ps2x_arch_msvc} # [gta-recomp] PS2X_CPU (was /arch:AVX2)
                /GS- # Disable Buffer Security Check (faster)
                /Qspectre- # Disable Spectre mitigations (faster)
            >
        )

        if(TARGET ${TargetName})
            target_link_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:
                    /LTCG # link-time code generation
                    /OPT:REF # remove unreferenced
                    /OPT:ICF # fold identical COMDATs
                >
            )
        endif()
    endif()

    if(IPO_SUPPORTED)
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    else()
        message(WARNING "Interprocedural optimization not supported: ${ipo_error}")
    endif()
endfunction()