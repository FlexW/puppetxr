# Usage: cmake -DSPIRV=file.spv -DTEXT=file.glsl [-DDXBC=file.dxbc] [-DMSL=file.metal] -DSOURCE=file -DNAME=symbol -DOUTPUT=file.h -P embed.cmake
# Emits the SPIR-V as `NAME_spv` (32-bit little-endian words, so the array is aligned for Vulkan),
# the GL text as `NAME_glsl`, the D3D bytecode as `NAME_dxbc` and the Metal source as `NAME_msl`
# when there are, and
# `NAME_shader`, the R_ShaderCode of them. Texture names come from
# the R_TEXTURE_* declarations of SOURCE, by binding.
file(READ ${SPIRV} hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR size "${hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])" "0x\\4\\3\\2\\1," words "${hex}")

file(READ ${TEXT} text_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," chars "${text_hex}")

file(READ ${SOURCE} source)
set(texture0 "nullptr")
set(texture1 "nullptr")
string(REGEX MATCHALL "R_TEXTURE_[A-Z0-9_]+\\([a-z_0-9]+, [0-9]+\\)" textures "${source}")
foreach (texture IN LISTS textures)
    string(REGEX REPLACE "R_TEXTURE_[A-Z0-9_]+\\(([a-z_0-9]+), ([0-9]+)\\)" "\\1;\\2" parts "${texture}")
    list(GET parts 0 texture_name)
    list(GET parts 1 texture_binding)
    if (texture_binding GREATER 1)
        message(FATAL_ERROR "${SOURCE}: binding ${texture_binding} is beyond R_MAX_TEXTURES")
    endif ()
    set(texture${texture_binding} "\"${texture_name}\"")
endforeach ()

set(dxbc_array "")
set(dxbc_field "nullptr")
set(dxbc_size 0)
if (DXBC)
    file(READ ${DXBC} dxbc_hex HEX)
    string(LENGTH "${dxbc_hex}" dxbc_hex_length)
    math(EXPR dxbc_size "${dxbc_hex_length} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," dxbc_bytes "${dxbc_hex}")
    set(dxbc_array "static const unsigned char ${NAME}_dxbc[] = {${dxbc_bytes}};\n")
    set(dxbc_field "${NAME}_dxbc")
endif ()

set(msl_array "")
set(msl_field "nullptr")
if (MSL)
    file(READ ${MSL} msl_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," msl_chars "${msl_hex}")
    set(msl_array "static const char ${NAME}_msl[] = {${msl_chars}0x00};\n")
    set(msl_field "${NAME}_msl")
endif ()

file(WRITE ${OUTPUT} "static const unsigned int ${NAME}_spv[] = {${words}};\n"
    "static const char ${NAME}_glsl[] = {${chars}0x00};\n"
    "${dxbc_array}"
    "${msl_array}"
    "static const R_ShaderCode ${NAME}_shader = { .spirv = ${NAME}_spv, .spirv_size = ${size}, .glsl = ${NAME}_glsl, .dxbc = ${dxbc_field}, .dxbc_size = ${dxbc_size}, .textures = { ${texture0}, ${texture1} }, .msl = ${msl_field} };\n")
