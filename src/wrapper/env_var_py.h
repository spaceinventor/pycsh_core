/*
 * param_py.h
 *
 * Wrappers for lib/param/src/param/param_slash.c
 *
 */

#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

PyObject * pycsh_var_show(PyObject * self, PyObject * args);
PyObject * pycsh_var_get(PyObject * self, PyObject * args, PyObject * kwds);
PyObject * pycsh_var_set(PyObject * self, PyObject * args, PyObject * kwds);
PyObject * pycsh_var_unset(PyObject * self, PyObject * args, PyObject * kwds);
