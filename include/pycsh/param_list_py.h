
#pragma once

#include <inttypes.h>

/**
 * @brief Version of `libparam`s `param_list_remove()` that will not destroy `param_t`s referenced by a `ParameterObject` wrapper,
 * instead only removing them from the parameter list.
 * 
 * Currently the caller is required to have initialized Python, but not PyCSH.
 * The caller is also not required to hold the GIL.
 */
int param_list_remove_py(int node, uint8_t verbose);
