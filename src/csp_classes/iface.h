#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <csp/csp_cmp.h>
#include <csp/csp_types.h>


typedef struct {
    PyObject_HEAD

    /* Keeping this is a pointer is nice should we wan't to modify the actual interface.
        But it again begs the question of when it gets removed from the linked list.
        We really need some more hooks. */
    csp_iface_t * iface;

    /* Practically all CSP interface types have a concept of `promisc`,
        but they don't store the explicit state on the struct.
        Perhaps we should? But we need to know how the interface was initially initialized. */

} InterfaceObject;

InterfaceObject * Interface_from_csp_iface_t(PyTypeObject *type, csp_iface_t * ifc);
//PyObject * Interface_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

/**
 * @brief 
 * 
 * @returns New reference
 */
InterfaceObject * Interface_from_py_identifier(PyObject * identifier/*: int|str|Interface*/);

/* New reference tuple[Interface, ...] */
PyObject * csp_interfaces_to_tuple(void);

extern PyTypeObject InterfaceType;
