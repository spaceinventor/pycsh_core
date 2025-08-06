#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <csp/csp_cmp.h>
#include <csp/csp_types.h>


typedef struct {
    PyObject_HEAD

    /* Keeping this is a pointer is nice should we wan't to modify the actual csp_route_t.
        But it again begs the question of when it gets removed from the linked list.
        We really need some more hooks. */
    csp_route_t * route;

} RouteObject;

RouteObject * Route_from_csp_route_t(PyTypeObject *type, csp_route_t * route);
//PyObject * Route_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

/**
 * @brief 
 * 
 * @returns New reference
 */
RouteObject * Route_from_py_identifier(PyObject * identifier/*: int|str|Interface*/);

/* New reference tuple[Route, ...] */
PyObject * csp_routes_to_tuple(void);

extern PyTypeObject RouteType;
