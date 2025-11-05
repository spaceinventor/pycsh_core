
#include "info.h"

#include "structmember.h"

#include <csp/csp_cmp.h>
#include <csp/csp_types.h>

#include "iface.h"
#include "route.h"
#include <pycsh/pycsh.h>
#include <pycsh/utils.h>
#include <pycsh/attr_malloc.h>
#include <apm/csh_api.h>


static PyObject * Info_str(InfoObject *self) {
	PyObject * info_string = PyUnicode_New(0, 0);

	assert(self->routes && PyTuple_Check(self->routes));
	Py_ssize_t route_cnt = PyTuple_Size(self->routes);
	for (size_t i = 0; i < route_cnt; i++) {
		PyUnicode_AppendAndDel(&info_string, PyObject_Str(PyTuple_GET_ITEM(self->routes, i)));
		assert(info_string);
	}
	
	assert(self->interfaces && PyTuple_Check(self->interfaces));
	Py_ssize_t interfaces_cnt = PyTuple_Size(self->interfaces);
	for (size_t i = 0; i < interfaces_cnt; i++) {
		PyUnicode_AppendAndDel(&info_string, PyObject_Str(PyTuple_GET_ITEM(self->interfaces, i)));
		assert(info_string);
	}
	
	return info_string;
}

static void Info_dealloc(InfoObject *self) {

    Py_XDECREF(self->routes);
    Py_XDECREF(self->interfaces);

    // Get the type of 'self' in case the user has subclassed 'Info' (No they haven't :P).
    Py_TYPE(self)->tp_free((PyObject *) self);
}

ATTR_MALLOC(Info_dealloc, 1)
InfoObject * _Info_new_internal(void) {

	#pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
	InfoObject *self AUTO_DECREF = (InfoObject *)InfoType.tp_alloc(&InfoType, 0);
    #pragma GCC diagnostic pop

	if (!self) {
		return NULL;
	}

	self->interfaces = csp_interfaces_to_tuple();
	if (!self->interfaces) {
		return NULL;
	}
	self->routes = csp_routes_to_tuple();
	if (!self->routes) {
		return NULL;
	}

	return (InfoObject*)Py_NewRef((PyObject*)self);
}

ATTR_MALLOC(Info_dealloc, 1)
PyObject * Info_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {

    static char *kwlist[] = {NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "", kwlist)) {
        return NULL;  // TypeError is thrown
    }

	return (PyObject*)_Info_new_internal();
}

static PyMemberDef Info_members[] = {
    {"interfaces", T_OBJECT, offsetof(InfoObject, interfaces), READONLY, "Tuple of local CSP interfaces"},
    {"routes", T_OBJECT, offsetof(InfoObject, routes), READONLY, "Tuple of local CSP routes"},

    {NULL}  /* Sentinel */
};

PyTypeObject InfoType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "pycsh.Info",
    .tp_doc = "Class to access CSP routes and interfaces similar to CSH",
    .tp_basicsize = sizeof(InfoObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = Info_new,
	.tp_dealloc = (destructor)Info_dealloc,
	.tp_members = Info_members,
	.tp_str = (reprfunc)Info_str,
};
