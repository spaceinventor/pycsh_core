#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <vmem/vmem_server.h>

extern PyDictObject * vmem_dict;

csp_packet_t * pycsh_vmem_client_list_get(int node, int timeout, int version);

typedef struct {
    PyObject_HEAD

    /* TODO Kevin: There are some API's to get the ID,
        when should we use them over just storing what we get from the remote host? */
    uint8_t vmem_id;

    /* Use largest vmem type, which we hope will remain "backwards compatible". */
    //vmem_list3_t vmem;

    /* TODO Kevin: Storing the `name` like this,
        prevents us from freeing the wrapper while the `vmem` is in the linked list.
        Nevertheless it is probably the simplest solution. */
    char name[16+1];

    vmem_t * vmem;

    PyObject * py_read;
    PyObject * py_write;

} VmemObject;

extern PyMethodDef Vmem_class_methods[2];

extern PyTypeObject VmemType;
