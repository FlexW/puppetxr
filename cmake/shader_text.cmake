# Usage: cmake -DINPUT=file -DOUTPUT=file -DPRELUDE=text -P shader_text.cmake
# Writes PRELUDE (\n separates lines), then INPUT with the `#include "x.h"` of headers next to it
# inlined: GL and GLES drivers take one source without includes. Other includes stay as they are.
# Line continuations are joined, GLSL has them only from 4.20 and ES 3.00.
get_filename_component(dir ${INPUT} DIRECTORY)
file(READ ${INPUT} text)
string(REGEX MATCHALL "#include \"[a-z_]+\\.h\"" includes "${text}")
foreach (include IN LISTS includes)
    string(REGEX REPLACE "#include \"([a-z_]+\\.h)\"" "\\1" name "${include}")
    if (EXISTS ${dir}/${name})
        file(READ ${dir}/${name} header)
        string(REPLACE "${include}" "${header}" text "${text}")
    endif ()
endforeach ()
string(REPLACE "\\\n" " " text "${text}")
string(REPLACE "\\n" "\n" prelude "${PRELUDE}")
file(WRITE ${OUTPUT} "${prelude}${text}")
