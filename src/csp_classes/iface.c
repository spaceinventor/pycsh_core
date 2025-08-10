
#include "iface.h"

#include "structmember.h"

#include <csp/csp_cmp.h>
#include <csp/csp_types.h>
#include <csp/interfaces/csp_if_zmqhub.h>

#include "../pycsh.h"
#include <pycsh/utils.h>
#include <apm/csh_api.h>


static PyObject * Interface_str(InterfaceObject *self) {
	csp_iface_t * i = self->iface;
	char tx_postfix, rx_postfix;
	unsigned long tx = csp_bytesize(i->txbytes, &tx_postfix);
	unsigned long rx = csp_bytesize(i->rxbytes, &rx_postfix);
	return PyUnicode_FromFormat("%-10s addr: %"PRIu16" netmask: %"PRIu16" dfl: %" PRIu32 "\r\n"
				  "           tx: %05" PRIu32 " rx: %05" PRIu32 " txe: %05" PRIu32 " rxe: %05" PRIu32 "\r\n"
				  "           drop: %05" PRIu32 " autherr: %05" PRIu32 " frame: %05" PRIu32 "\r\n"
				  "           txb: %" PRIu32 " (%" PRIu32 "%c) rxb: %" PRIu32 " (%" PRIu32 "%c) \r\n\r\n",
				  i->name, i->addr, i->netmask, i->is_default, i->tx, i->rx, i->tx_error, i->rx_error, i->drop,
				  i->autherr, i->frame, i->txbytes, tx, tx_postfix, i->rxbytes, rx, rx_postfix);
}

/* New reference tuple[Interface, ...] */
PyObject * csp_interfaces_to_tuple(void) {

    csp_iface_t * iface = csp_iflist_get();

    if (iface == NULL) {
        /* No interfaces added yet. */
		return PyTuple_New(0);
	}

    PyObject * iface_tuple AUTO_DECREF = PyTuple_New(0);
    if (!iface_tuple) {
        return PyErr_NoMemory();
    }

    csp_iface_t * csp_iflist_iterate(csp_iface_t * ifc);

	while ((iface = csp_iflist_iterate(iface)) != NULL) {

        const Py_ssize_t insert_index = PyTuple_GET_SIZE(iface_tuple);
        /* Resize tuple to fit reply, this could probably be done more efficiently. */
        if (_PyTuple_Resize(&iface_tuple, insert_index+1) < 0) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to resize tuple for ident replies");
            return NULL;
        }

        InterfaceObject * py_ifc = Interface_from_csp_iface_t(&InterfaceType, iface);
        if (py_ifc == NULL) {
            return NULL;
        }

        PyTuple_SET_ITEM(iface_tuple, insert_index, (PyObject*)py_ifc);
	}

    return Py_NewRef(iface_tuple);
}

InterfaceObject * Interface_from_csp_iface_t(PyTypeObject *type, csp_iface_t * ifc) {

	/* Shouldn't return NULL without setting an exception,
		and incorrect C argument doesn't really justify an exception, hence `assert()` */
	assert(ifc);

    if (type == NULL) {
        type = &InterfaceType;
    }

	InterfaceObject *self = (InterfaceObject *)type->tp_alloc(type, 0);
	if (self == NULL) {
		/* This is likely a memory allocation error, in which case we expect .tp_alloc() to have raised an exception. */
		return NULL;
	}

	self->iface = ifc;

	return self;
}

/**
 * @brief 
 * 
 * @returns New reference
 */
InterfaceObject * Interface_from_py_identifier(PyObject * identifier/*: int|str|Interface*/) {
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
		return (InterfaceObject*)Py_NewRef(identifier);
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

    /* If NULL is returned here, it is likely a memory allocation error, in which case we expect .tp_alloc() to have raised an exception. */
	return (PyObject*)Interface_from_py_identifier(identifier);
}

static PyObject * Interface_get_addr(InterfaceObject *self, void *closure) {
    assert(self->iface);
    return PyLong_FromUnsignedLong((unsigned long) self->iface->addr);
}

static PyObject * Interface_get_netmask(InterfaceObject *self, void *closure) {
    assert(self->iface);
    return PyLong_FromUnsignedLong((unsigned long) self->iface->netmask);
}

static PyObject * Interface_get_name(InterfaceObject *self, void *closure) {
    assert(self->iface);
    return PyUnicode_FromString(self->iface->name);
}

static PyObject * Interface_get_is_default(InterfaceObject *self, void *closure) {
    assert(self->iface);
    return PyBool_FromLong((long) self->iface->is_default);
}

static PyObject * Interface_get_tx(InterfaceObject *self, void *closure) {
    assert(self->iface);
    return PyLong_FromUnsignedLong(self->iface->tx);
}

static PyObject * Interface_get_rx(InterfaceObject *self, void *closure) {
    assert(self->iface);
    return PyLong_FromUnsignedLong(self->iface->rx);
}

static PyObject * Interface_get_tx_error(InterfaceObject *self, void *closure) {
    assert(self->iface);
    return PyLong_FromUnsignedLong(self->iface->tx_error);
}

static PyObject * Interface_get_rx_error(InterfaceObject *self, void *closure) {
    assert(self->iface);
    return PyLong_FromUnsignedLong(self->iface->rx_error);
}

static PyObject * Interface_get_drop(InterfaceObject *self, void *closure) {
    assert(self->iface);
    return PyLong_FromUnsignedLong(self->iface->drop);
}

static PyObject * Interface_get_autherr(InterfaceObject *self, void *closure) {
    assert(self->iface);
    return PyLong_FromUnsignedLong(self->iface->autherr);
}

static PyObject * Interface_get_frame(InterfaceObject *self, void *closure) {
    assert(self->iface);
    return PyLong_FromUnsignedLong(self->iface->frame);
}

static PyObject * Interface_get_txbytes(InterfaceObject *self, void *closure) {
    assert(self->iface);
    return PyLong_FromUnsignedLong(self->iface->txbytes);
}

static PyObject * Interface_get_rxbytes(InterfaceObject *self, void *closure) {
    assert(self->iface);
    return PyLong_FromUnsignedLong(self->iface->rxbytes);
}

static PyObject * Interface_get_irq(InterfaceObject *self, void *closure) {
    assert(self->iface);
    return PyLong_FromUnsignedLong(self->iface->irq);
}


static PyGetSetDef Interface_getset[] = {
    {"addr", (getter) Interface_get_addr, NULL, PyDoc_STR("Interface address"), NULL},
    {"netmask", (getter) Interface_get_netmask, NULL, PyDoc_STR("Subnet mask"), NULL},
    {"name", (getter) Interface_get_name, NULL, PyDoc_STR("Interface name"), NULL},
    {"is_default", (getter) Interface_get_is_default, NULL, PyDoc_STR("Default interface flag"), NULL},
    {"tx", (getter) Interface_get_tx, NULL, PyDoc_STR("Transmitted packets"), NULL},
    {"rx", (getter) Interface_get_rx, NULL, PyDoc_STR("Received packets"), NULL},
    {"tx_error", (getter) Interface_get_tx_error, NULL, PyDoc_STR("Transmit errors"), NULL},
    {"rx_error", (getter) Interface_get_rx_error, NULL, PyDoc_STR("Receive errors"), NULL},
    {"drop", (getter) Interface_get_drop, NULL, PyDoc_STR("Dropped packets"), NULL},
    {"autherr", (getter) Interface_get_autherr, NULL, PyDoc_STR("Authentication errors"), NULL},
    {"frame", (getter) Interface_get_frame, NULL, PyDoc_STR("Frame format errors"), NULL},
    {"txbytes", (getter) Interface_get_txbytes, NULL, PyDoc_STR("Transmitted bytes"), NULL},
    {"rxbytes", (getter) Interface_get_rxbytes, NULL, PyDoc_STR("Received bytes"), NULL},
    {"irq", (getter) Interface_get_irq, NULL, PyDoc_STR("Interrupts"), NULL},
    {NULL}  // Sentinel
};


PyObject * Interface_set_promisc(InterfaceObject * self, PyObject * args) {

    int csp_zmqhub_tx(csp_iface_t * iface, uint16_t via, csp_packet_t * packet, int from_me);
    if (self->iface->nexthop != csp_zmqhub_tx) {
        PyErr_SetString(PyExc_NotImplementedError, "`Interface.set_promisc()` can currently only be called on ZMQ interfaces");
        return NULL;
    }

	int new_promisc = false;

	if (!PyArg_ParseTuple(args, "p:set_promisc", &new_promisc)) {
		return NULL;
	}

	if (new_promisc) {
        csp_zmqhub_remove_filters(self->iface);
    } else {
        csp_zmqhub_add_filters(self->iface);
    }

	Py_RETURN_NONE;
}


static PyMethodDef Interface_methods[] = {
    {"set_promisc", (PyCFunction)Interface_set_promisc, METH_VARARGS, PyDoc_STR("Update whether CSP interface should be promisc")},
    {NULL, NULL, 0, NULL}
};

PyTypeObject InterfaceType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "pycsh.Interface",
    .tp_doc = "Wrapper class for local CSP interfaces.",
    .tp_basicsize = sizeof(InterfaceObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = Interface_new,
    .tp_getset = Interface_getset,
    .tp_methods = Interface_methods,
    .tp_str = (reprfunc)Interface_str,
};
