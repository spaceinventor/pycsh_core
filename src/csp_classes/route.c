
#include "route.h"

#include "structmember.h"

#include <csp/csp_cmp.h>
#include <csp/csp_types.h>
#include <csp/csp_rtable.h>

#include "iface.h"
#include "../pycsh.h"
#include <pycsh/utils.h>
#include <apm/csh_api.h>


static PyObject * Route_str(RouteObject *self) {
    csp_route_t * route = self->route;
    if (route->via == CSP_NO_VIA_ADDRESS) {
		return PyUnicode_FromFormat("%u/%u %s\r\n", route->address, route->netmask, route->iface->name);
	} else {
		return PyUnicode_FromFormat("%u/%u %s %u\r\n", route->address, route->netmask, route->iface->name, route->via);
	}
}

RouteObject * Route_from_csp_route_t(PyTypeObject *type, csp_route_t * route) {

	/* Shouldn't return NULL without setting an exception,
		and incorrect C argument doesn't really justify an exception, hence `assert()` */
	assert(route);

    if (type == NULL) {
        type = &RouteType;
    }

	RouteObject *self = (RouteObject *)type->tp_alloc(type, 0);
	if (self == NULL) {
		/* This is likely a memory allocation error, in which case we expect .tp_alloc() to have raised an exception. */
		return NULL;
	}

	self->route = route;

	return self;
}

/* Populate `ctx` (as a tuple[Route, ...]) from CSP routes. */
static bool route_init_iter(void * ctx, csp_route_t * route) {

    /* Skip operations if a previous iteration caused an exception.
        Not too nice to use a global for this. But I guess we hold the GIL anyway. */
    if (PyErr_Occurred()) {
        return false;/*?  Is false bad?*/
    }
    
    PyObject **routes_tuple = (PyObject **)ctx;
    assert(routes_tuple && PyTuple_Check(*routes_tuple));


    const Py_ssize_t insert_index = PyTuple_GET_SIZE(*routes_tuple);
    
    /* No way to adapt return to fit exception here,
        since we can't break out of the iterator. */
    if (_PyTuple_Resize(routes_tuple, insert_index+1) < 0) {
        return false;  // Resize failed, exception already set
    }

    PyObject *route_obj = (PyObject *)Route_from_csp_route_t(&RouteType, route);
    if (!route_obj) {
        return false;
    }

    // Steals reference
    PyTuple_SET_ITEM(*routes_tuple, insert_index, route_obj);

    return true;
}

/* New reference tuple[Route, ...] */
PyObject * csp_routes_to_tuple(void) {

    PyObject * routes_tuple AUTO_DECREF = PyTuple_New(0);
    if (!routes_tuple) {
        return NULL;
    }

    csp_rtable_iterate(route_init_iter, &routes_tuple);

    if (PyErr_Occurred()) {
        /* Error during tuple initialization/population */
        return NULL;
    }

    return Py_NewRef(routes_tuple);
}

#if 0
/**
 * @brief 
 * 
 * @returns New reference
 */
RouteObject * Route_from_py_identifier(PyObject * identifier/*: int|str|Route*/) {
	if (PyLong_Check(identifier)) {
		
		/* All 3 of these are quite cool, but `_addr()` and `_subnet()` don't account for dual-CAN. */
		csp_iface_t * ifc = csp_iflist_get_by_index(PyLong_AsLong(identifier));
		//csp_iface_t * ifc = csp_iflist_get_by_subnet();
		//csp_iface_t * ifc = csp_iflist_get_by_addr();

		if (ifc == NULL) {
			PyErr_Format(PyExc_ValueError, "Failed to find local CSP interface by index %ld", PyLong_AsLong(identifier));
			return NULL;
		}

		return Interface_from_csp_iface_t(&InterfaceType, ifc);
	}

	if (PyUnicode_Check(identifier)) {
		csp_iface_t * ifc = csp_iflist_get_by_name(PyUnicode_AsUTF8(identifier));

		if (ifc == NULL) {
			PyErr_Format(PyExc_ValueError, "Failed to find local CSP interface by name %s", PyUnicode_AsUTF8(identifier));
			return NULL;
		}

		return Interface_from_csp_iface_t(&InterfaceType, ifc);
	}

	if (PyObject_IsInstance(identifier, (PyObject*)&InterfaceType)) {
		return (PyObject*)Py_NewRef(identifier);
	}

	PyErr_Format(PyExc_TypeError, "Unsupported Interface identifier type (%s), options are int|str|pycsh.Interface", identifier->ob_type->tp_name);
	return NULL;
}

PyObject * Interface_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {

    PyObject * identifier = NULL;

    static char *kwlist[] = {"identifier", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kwlist, &identifier)) {
        return NULL;  // TypeError is thrown
    }

	InterfaceObject *self = Route_from_py_identifier(identifier);
	if (self == NULL) {
		/* This is likely a memory allocation error, in which case we expect .tp_alloc() to have raised an exception. */
		return NULL;
	}
}
#endif

static PyObject * Route_get_address(RouteObject *self, void *closure) {
    return PyLong_FromUnsignedLong((unsigned long)self->route->address);
}

static PyObject * Route_get_netmask(RouteObject *self, void *closure) {
    return PyLong_FromUnsignedLong((unsigned long)self->route->netmask);
}

static PyObject * Route_get_via(RouteObject *self, void *closure) {
    return PyLong_FromUnsignedLong((unsigned long)self->route->via);
}

static PyObject * Route_get_iface(RouteObject *self, void *closure) {
    assert(self->route && self->route->iface);
    return (PyObject *) Interface_from_csp_iface_t(&InterfaceType, self->route->iface);
}

static PyGetSetDef Route_getsetters[] = {
    {"addr", (getter)Route_get_address, NULL, "Route address", NULL},
    {"mask", (getter)Route_get_netmask, NULL, "Route netmask", NULL},
    {"via",     (getter)Route_get_via,     NULL, "Route via", NULL},
    {"iface",   (getter)Route_get_iface,   NULL, "Route interface", NULL},
    {NULL}  // Sentinel
};

PyTypeObject RouteType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "pycsh.Route",
    .tp_doc = "Wrapper class for a CSP route",
    .tp_basicsize = sizeof(RouteObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    //.tp_new = Interface_new,
	.tp_getset = Route_getsetters,
	.tp_str = (reprfunc)Route_str,
};
