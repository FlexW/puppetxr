#include "shader.h"
#include "blocks.h"

#define COMP_DEPTH 1

R_PUSH_BLOCK(COMP_PUSH_FIELDS)
R_TEXTURE_LAYERED(source, 0)
R_TEXTURE_ARRAY(resolved_depth, 1)

#include "composite.h"
