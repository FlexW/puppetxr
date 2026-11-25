#include "shader.h"
#include "blocks.h"

R_PUSH_BLOCK(COMP_PUSH_FIELDS)
R_TEXTURE_LAYERED(source, 0)

#include "composite.h"
