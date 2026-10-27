/*
 * py_csp.h
 *
 * Wrappers for src/slash_csp.c
 *
 */

#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

PyObject * pycsh_slash_stdbuf(PyObject * self, PyObject * args, PyObject * kwds);
