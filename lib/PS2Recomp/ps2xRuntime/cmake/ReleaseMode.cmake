include(CheckIPOSupported)

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)
option(PS2X_RELEASE_IPO "Link-time optimisation in release builds" ON)

function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # clang-cl: the same optimisation as the other platforms' release
        # builds. No fast math (the recompiled EE's float results must not
        # change with the compiler) and no instruction set beyond the one
        # the build asks for everywhere.
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:/O2 /Gy /Gw /GF /Zc:inline /DNDEBUG /GS->)
        target_link_options(${TargetName} PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
    elseif(MSVC)
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
                /arch:AVX2 # Advanced Vector Extensions 2
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

    if(IPO_SUPPORTED AND PS2X_RELEASE_IPO)
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    elseif(NOT IPO_SUPPORTED)
        message(WARNING "Interprocedural optimization not supported: ${ipo_error}")
    endif()
endfunction()