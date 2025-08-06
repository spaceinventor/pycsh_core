#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <csp/csp_cmp.h>
#include <csp/csp_types.h>


typedef struct {
    PyObject_HEAD

    PyObject * interfaces;
    PyObject * routes;

} InfoObject;

//InfoObject * Info_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
InfoObject * _Info_new_internal(void);

/**
 * @brief 
 * 
 * @returns New reference
 */
InfoObject * Info_from_py_identifier(PyObject * identifier/*: int|str|Interface*/);

extern PyTypeObject InfoType;
