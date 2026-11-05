find_program(GLSLC glslc REQUIRED)
find_program(GLSLANG_VALIDATOR glslangValidator)
# Metal takes MSL translated from the Vulkan SPIR-V (docs/shaders.md, Metal)
if (APPLE)
    find_program(SPIRV_CROSS spirv-cross REQUIRED)
endif ()
# The dxc of the Windows SDK lacks the SPIR-V back end the HLSL check uses; the Vulkan SDK's has it
function(puppetxr_dxc_has_spirv result candidate)
    set(probe ${CMAKE_BINARY_DIR}/CMakeFiles/dxc_probe.hlsl)
    file(WRITE ${probe} "float4 main () : SV_Target { return 0; }\n")
    execute_process(COMMAND ${candidate} -nologo -spirv -T ps_6_0 -E main -Fo ${probe}.spv ${probe} RESULT_VARIABLE exit_code OUTPUT_QUIET ERROR_QUIET)
    if (NOT exit_code EQUAL 0)
        set(${result} FALSE PARENT_SCOPE)
    endif ()
endfunction()
find_program(DXC dxc HINTS $ENV{VULKAN_SDK}/Bin $ENV{VULKAN_SDK}/bin VALIDATOR puppetxr_dxc_has_spirv)
set(PUPPETXR_HLSL_DEFINES "" CACHE STRING "Extra defines of the DXBC compile, NAME=VALUE;... (the mingw-debug preset works around Wine's compiler)")

# puppetxr_add_shaders(<target> <shader files...>)
# Shaders are written in the dialect of shaders/shader.h (docs/shaders.md). Each file is compiled
# with glslc for Vulkan and expanded to one GL source; <build>/shaders/<file>.h embeds both as the
# R_ShaderCode `<name>_shader`, e.g. `composite_vert_shader`. When glslangValidator and dxc are found, the GL 330, GLSL ES 300
# and HLSL variants are compiled too, so that a shader that breaks a backend fails the build. HLSL
# goes through the SPIR-V back end of dxc: the DXIL one needs the validator library that Linux
# builds of dxc often lack, and the front end is what checks the dialect.
# <target> is an interface library carrying the include dir.
function(puppetxr_add_shaders target)
    set(out_dir ${CMAKE_BINARY_DIR}/shaders)
    set(src_dir ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
    file(MAKE_DIRECTORY ${out_dir}/variants)
    set(headers "")
    set(stamps "")
    set(includes ${src_dir}/shader.h ${src_dir}/blocks.h ${src_dir}/composite.h)
    foreach (source ${ARGN})
        get_filename_component(name ${source} NAME)
        get_filename_component(extension ${source} LAST_EXT)
        string(MAKE_C_IDENTIFIER ${name} symbol)
        if (extension STREQUAL ".vert")
            set(stage VERTEX)
            set(stage_short vert)
            set(samplers DEFAULT)
            set(hlsl_profile vs_6_0)
        else ()
            set(stage FRAGMENT)
            set(stage_short frag)
            set(samplers DEFAULT R_SAMPLER_2D R_SAMPLER_CUBE)
            set(hlsl_profile ps_6_0)
        endif ()
        set(input ${CMAKE_CURRENT_SOURCE_DIR}/${source})
        set(spv ${out_dir}/${name}.spv)
        set(glsl ${out_dir}/${name}.glsl)
        set(header ${out_dir}/${name}.h)
        add_custom_command(
            OUTPUT ${spv}
            COMMAND ${GLSLC} --target-env=vulkan1.0 -std=450core -DR_SHADER_VULKAN=1 -DR_STAGE_${stage}=1 -fshader-stage=${stage_short} -O ${input} -o ${spv}
            DEPENDS ${input} ${includes}
            COMMENT "Compiling ${source}"
            VERBATIM)
        add_custom_command(
            OUTPUT ${glsl}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${input} -DOUTPUT=${glsl} -DPRELUDE= -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/shader_text.cmake
            DEPENDS ${input} ${includes} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/shader_text.cmake
            COMMENT "Expanding ${source}"
            VERBATIM)
        # DXBC (shader model 5.0, which D3D12 runs too) from the single-source text, includes inlined
        set(dxbc "")
        if (TARGET puppetxr_hlsl)
            set(dxbc ${out_dir}/${name}.dxbc)
            string(REPLACE "_6_0" "_5_0" dxbc_profile ${hlsl_profile})
            set(dxbc_defines -DR_SHADER_HLSL=1 -DR_STAGE_${stage}=1)
            foreach (define ${PUPPETXR_HLSL_DEFINES})
                list(APPEND dxbc_defines -D${define})
            endforeach ()
            # The preprocessor of D3DCompile rejects a function-like macro passed by name, which
            # the X-macro blocks of blocks.h do, so the C compiler preprocesses first
            set(hlsl ${out_dir}/${name}.hlsl)
            if (CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
                set(preprocess ${CMAKE_C_COMPILER} /nologo /P /EP /Zc:preprocessor ${dxbc_defines} /Fi${hlsl} /Tc${glsl})
            else ()
                set(preprocess ${CMAKE_C_COMPILER} -E -P -x c ${dxbc_defines} ${glsl} -o ${hlsl})
            endif ()
            add_custom_command(
                OUTPUT ${dxbc}
                COMMAND ${preprocess}
                COMMAND puppetxr_hlsl ${hlsl} ${dxbc} ${dxbc_profile}
                DEPENDS ${glsl} puppetxr_hlsl
                COMMENT "Compiling ${source} to DXBC"
                VERBATIM)
        endif ()
        # Metal puts NDC y = +1 at the top row where Vulkan puts y = -1, so vertex shaders flip y
        set(msl "")
        if (SPIRV_CROSS)
            set(msl ${out_dir}/${name}.metal)
            set(flip "")
            if (stage STREQUAL "VERTEX")
                set(flip --flip-vert-y)
            endif ()
            add_custom_command(
                OUTPUT ${msl}
                COMMAND ${SPIRV_CROSS} --msl --msl-version 20300 --msl-decoration-binding ${flip} --output ${msl} ${spv}
                DEPENDS ${spv}
                COMMENT "Translating ${source} to MSL"
                VERBATIM)
        endif ()
        add_custom_command(
            OUTPUT ${header}
            COMMAND ${CMAKE_COMMAND} -DSPIRV=${spv} -DTEXT=${glsl} -DDXBC=${dxbc} -DMSL=${msl} -DSOURCE=${input} -DNAME=${symbol} -DOUTPUT=${header} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
            DEPENDS ${spv} ${glsl} ${dxbc} ${msl} ${input} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
            COMMENT "Embedding ${name}"
            VERBATIM)
        list(APPEND headers ${header})

        if (GLSLANG_VALIDATOR)
            foreach (api GL GLES)
                if (api STREQUAL "GL")
                    set(version "330 core")
                else ()
                    set(version "300 es")
                endif ()
                foreach (sampler ${samplers})
                    set(prelude "#version ${version}\\n#define R_SHADER_${api} 1\\n#define R_STAGE_${stage} 1\\n")
                    if (NOT sampler STREQUAL "DEFAULT")
                        string(APPEND prelude "#define ${sampler} 1\\n")
                    endif ()
                    string(TOLOWER "${api}_${sampler}" variant)
                    set(text ${out_dir}/variants/${name}.${variant}.${stage_short})
                    set(stamp ${text}.ok)
                    add_custom_command(
                        OUTPUT ${stamp}
                        COMMAND ${CMAKE_COMMAND} -DINPUT=${input} -DOUTPUT=${text} -DPRELUDE=${prelude} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/shader_text.cmake
                        COMMAND ${GLSLANG_VALIDATOR} -S ${stage_short} ${text}
                        COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
                        DEPENDS ${input} ${includes} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/shader_text.cmake
                        COMMENT "Validating ${source} for ${api} (${sampler})"
                        VERBATIM)
                    list(APPEND stamps ${stamp})
                endforeach ()
            endforeach ()
        endif ()

        if (DXC)
            set(stamp ${out_dir}/variants/${name}.hlsl.ok)
            add_custom_command(
                OUTPUT ${stamp}
                COMMAND ${DXC} -nologo -WX -spirv -T ${hlsl_profile} -E main -DR_SHADER_HLSL=1 -DR_STAGE_${stage}=1 -I ${src_dir} -Fo ${out_dir}/variants/${name}.hlsl.spv ${input}
                COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
                DEPENDS ${input} ${includes}
                COMMENT "Validating ${source} for HLSL"
                VERBATIM)
            list(APPEND stamps ${stamp})
        endif ()
    endforeach ()
    if (NOT GLSLANG_VALIDATOR OR NOT DXC)
        message(STATUS "Shaders: glslangValidator or dxc not found; GL, GLES or HLSL variants are not validated")
    endif ()
    add_custom_target(${target}_generate DEPENDS ${headers} ${stamps})
    add_library(${target} INTERFACE)
    add_dependencies(${target} ${target}_generate)
    target_include_directories(${target} INTERFACE ${out_dir} ${src_dir})
endfunction()
