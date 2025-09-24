
#include "valueproxy.h"

#include "structmember.h"

#include <param/param.h>

#include <pycsh/pycsh.h>
#include <pycsh/utils.h>

/**
 * @brief 
 * 
 * @param self 
 * @param indexes 
 * @return * PyObject* borrowed reference to `self->value` or `NULL` on exception. Intended to be interpreted as a boolean.
 */
static PyObject * ValueProxy_eval_value(ValueProxyObject *self, PyObject* indexes) {
	assert(self->param);

	/* return cached value if we already have one. */
	if (self->value) {
		return self->value;
	}


	/* Explicit `None` returns whole array when `self->array`, otherwise index 0 */
	PyObject * _default_key AUTO_DECREF = NULL;
	if (!indexes || Py_IsNone(indexes)) {
		if (self->param->array_size > 1) {
			indexes = _default_key = PySlice_New(NULL, NULL, NULL);
		} else {
			indexes = _default_key = PyLong_FromLong(0);
		}
		if (!indexes) {
			return NULL;
		}
	}

	// Handle integer index
	if (PyLong_Check(indexes)) {
		long idx_raw = PyLong_AsLong(indexes);
		if (idx_raw == -1 && PyErr_Occurred()) {
			return NULL;
		}

		//int c_idx = idx_raw;
		//if (_pycsh_util_index(self->param->array_size, &c_idx) < 0) {
		//	return NULL;
		//}

		self->value = _pycsh_util_get_single(self->param, idx_raw, self->remote, self->host, self->timeout, self->retries, self->paramver, self->verbose);
		return self->value;
	}

	/* Check if `key` is iterable by simply getting an iterator for it. */
	assert(!PyErr_Occurred());
	PyObject *_iter AUTO_DECREF = PyObject_GetIter(indexes);
	PyErr_Clear();

	// Handle slicing
	if (PySlice_Check(indexes) || _iter) {
		self->value = _pycsh_util_get_array_indexes(self->param, indexes, self->remote, self->host, self->timeout, self->retries, self->paramver, self->verbose);
		return self->value;
	}

	PyErr_SetString(PyExc_TypeError, "indices must be integers, slices, Iterable[int] or None");
	return NULL;
}


static PyObject * ValueProxy_richcompare(ValueProxyObject *self, PyObject *other, int op) {

	if (!ValueProxy_eval_value(self, NULL)) {
		return NULL;
	}

	switch (op) {
		// case Py_LT:
		// 	break;
		// case Py_LE:
		// 	break;
		case Py_EQ:
		case Py_NE:
			return PyObject_RichCompare(self->value, other, op);
			break;
		// case Py_GT:
		// 	break;
		// case Py_GE:
		// 	break;
	}

    return Py_NewRef(Py_NotImplemented);
}

static PyObject * ValueProxy_str(ValueProxyObject *self) {

	assert(!PyErr_Occurred());
	if (!ValueProxy_eval_value(self, NULL)) {
		return NULL;
	}

	return PyObject_Str(self->value);
}


/* Create a ValueProxy object from a Parameter instance directly. */
PyObject * pycsh_ValueProxy_from_Parameter(PyTypeObject *type, ParameterObject * param) {
	assert(param);
	if (param == NULL) {
 		return NULL;
	}

	if (!PyObject_IsInstance((PyObject*)param, (PyObject*)&ParameterType)) {
		PyErr_Format(PyExc_RuntimeError, "Tried to create ValueProxy instance from %s class, expected `pycsh.Parameter`", param->ob_base.ob_type->tp_name);
		return NULL;
	}

	ValueProxyObject *self = (ValueProxyObject *)type->tp_alloc(type, 0);

	if (self == NULL) {
		return NULL;
	}

	self->host = (param->host != INT_MIN) ? param->host : *param->param->node;
	self->param = param->param;
	self->timeout = param->timeout;
	self->retries = param->retries;
	self->paramver = param->paramver;
	self->remote = true;

    return (PyObject *)self;
}

#if 0
static PyObject * ValueProxy_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {

	static char *kwlist[] = {"param_identifier", "node", "host", "paramver", "timeout", "retries", NULL};

	PyObject * param_identifier;  // Raw argument object/type passed. Identify its type when needed.
	int node = pycsh_dfl_node;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|i", kwlist, &param_identifier, &node)) {
		return NULL;  // TypeError is thrown
	}

	param_t * param = _pycsh_util_find_param_t(param_identifier, node);

	if (param == NULL)  // Did not find a match.
		return NULL;  // Raises TypeError or ValueError.

    return pycsh_Parameter_from_param(type, param, NULL, host, timeout, retries, paramver, PY_PARAM_FREE_NO);
}
#endif


static PyObject * ValueProxy_get_host(ValueProxyObject *self, void *closure) {
	if (self->host != INT_MIN)
		return Py_BuildValue("i", self->host);
	Py_RETURN_NONE;
}

static int ValueProxy_set_host(ValueProxyObject *self, PyObject *value, void *closure) {

	if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the host attribute");
        return -1;
    }

	if (value == Py_None) {  // Set the host to INT_MIN, which we use as no host.
		self->host = INT_MIN;
		return 0;
	}

	if(!PyLong_Check(value)) {
		PyErr_SetString(PyExc_TypeError,
                        "The host attribute must be set to an int or None");
        return -1;
	}

	int host = _PyLong_AsInt(value);

	if (PyErr_Occurred())
		return -1;  // 'Reraise' the current exception.

	self->host = host;

	return 0;
}


static PyObject * ValueProxy_get_timeout(ValueProxyObject *self, void *closure) {
	return Py_BuildValue("i", self->timeout);
}

static int ValueProxy_set_timeout(ValueProxyObject *self, PyObject *value, void *closure) {

	if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the timeout attribute");
        return -1;
    }

	if (value == Py_None) {
		self->timeout = pycsh_dfl_timeout;
		return 0;
	}

	if(!PyLong_Check(value)) {
		PyErr_SetString(PyExc_TypeError,
                        "The timeout attribute must be set to an int or None");
        return -1;
	}

	int timeout = _PyLong_AsInt(value);

	if (PyErr_Occurred())
		return -1;  // 'Reraise' the current exception.

	self->timeout = timeout;

	return 0;
}

static PyObject * ValueProxy_get_retries(ValueProxyObject *self, void *closure) {
	return Py_BuildValue("i", self->retries);
}

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 13
#define  _PyLong_AsInt PyLong_AsInt
#endif

static int ValueProxy_set_retries(ValueProxyObject *self, PyObject *value, void *closure) {

	if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the retries attribute");
        return -1;
    }

	if (value == Py_None) {
		self->retries = 1;
		return 0;
	}

	if(!PyLong_Check(value)) {
		PyErr_SetString(PyExc_TypeError,
                        "The retries attribute must be set to an int or None");
        return -1;
	}

	int retries = _PyLong_AsInt(value);

	if (PyErr_Occurred())
		return -1;  // 'Reraise' the current exception.

	self->retries = retries;

	return 0;
}

static long ValueProxy_hash(ValueProxyObject *self) {

	if (!ValueProxy_eval_value(self, NULL)) {
		return -1;
	}

	/* Use the ID of the parameter as the hash, as it is assumed unique. */
    return PyObject_Hash(self->value);
}

static void ValueProxy_dealloc(ValueProxyObject *self) {

	Py_XDECREF(self->value);

	PyTypeObject *baseclass = pycsh_get_base_dealloc_class(&ValueProxyType);
    baseclass->tp_dealloc((PyObject*)self);
}

static PyObject * ValueProxy_subscript(ValueProxyObject *self, PyObject *key) {

	assert(key);
	assert(self->param);

	if (!self->value && !ValueProxy_eval_value(self, key)) {
		return NULL;
	}

	/* TODO Kevin: What to do if we already have a value.
			- Error if the subscript length/index differs
			- Error regardless
			- Just return what we already have  (<-- currently)
			- Or query a new value? */
	return Py_NewRef(self->value);
}

/* Simply return the length of the parameter,
	as would be expected if we didn't have the `ValueProxy` */
static Py_ssize_t ValueProxy_length(ValueProxyObject *self) {
	return self->param->array_size;
}

int ValueProxy_ass_subscript(ValueProxyObject *self, PyObject *key, PyObject* value) {
	if (value == NULL) {
		PyErr_SetString(PyExc_TypeError, "Cannot delete Parameter.value");
		return -1;
	}

	/* TODO Kevin: Handle `key == NULL` */
	/* TODO Kevin: Probably check for `PARAM_TYPE_DATA` here as well? */
	if (self->param->type == PARAM_TYPE_STRING) {
		if (PyObject_IsTrue(key)) {  /* If `key` is not `None|0|[]` */
			PyErr_SetString(PyExc_NotImplementedError, "Cannot set string parameters by index.");
			return -1;
		}
		return (_pycsh_util_set_single(self->param, value, INT_MIN, self->host, self->timeout, self->retries, self->paramver, self->remote, self->verbose) < 0) ? -1 : 0;
	}
	
	if (PyLong_Check(key)) {
		return (_pycsh_util_set_single(self->param, value, PyLong_AS_LONG(key), self->host, self->timeout, self->retries, self->paramver, self->remote, self->verbose) < 0) ? -1 : 0;
	}

	/* Check if `key` is iterable by simply getting an iterator for it.
		We could also `_pycsh_util_set_array_indexes()` raise a `TypeError`.
		But we can make the message a bit clearer by doing it ourselves. */
	assert(!PyErr_Occurred());
	PyObject *_iter AUTO_DECREF = PyObject_GetIter(key);
	PyErr_Clear();

	if (_iter || Py_IsNone(key)) {
		PyObject * AUTO_DECREF success = _pycsh_util_set_array_indexes(self->param, value, key, self->remote, self->host, self->timeout, self->retries, self->paramver, self->verbose);
		return success ? 0 : -1;
	}

	PyErr_Format(PyExc_TypeError, "indices must be integers, slices, Iterable[int] or None, not `%s`", key->ob_type->tp_name);
	return -1;
}

static PyMappingMethods ValueProxy_as_mapping = {
    .mp_length = (lenfunc)ValueProxy_length,
    .mp_subscript = (binaryfunc)ValueProxy_subscript,
    .mp_ass_subscript = (objobjargproc)ValueProxy_ass_subscript,
};

/* 
The Python binding 'Parameter' class exposes most of its attributes through getters, 
as only its 'value', 'host' and 'node' are mutable, and even those are through setters.
*/
static PyGetSetDef ValueProxy_getsetters[] = {

	{"host", (getter)ValueProxy_get_host, (setter)ValueProxy_set_host,
     "host of the parameter", NULL},
	{"timeout", (getter)ValueProxy_get_timeout, (setter)ValueProxy_set_timeout,
     "timeout of the parameter", NULL},
	{"retries", (getter)ValueProxy_get_retries, (setter)ValueProxy_set_retries,
     "available retries of the parameter", NULL},
    {NULL, NULL, NULL, NULL}  /* Sentinel */
};

static PyObject *ValueProxy_iter(ValueProxyObject *self) {
	if (!ValueProxy_eval_value(self, NULL)) {
		return NULL;
	}
	return PyObject_GetIter(self->value);
}

static PyObject *ValueProxy_eval_and_return(ValueProxyObject *self) {
	if (!ValueProxy_eval_value(self, NULL)) {
		return NULL;
	}
	return Py_NewRef(self->value);
}

static PyNumberMethods ValueProxy_as_number = {
    .nb_int = (unaryfunc)ValueProxy_eval_and_return,
    .nb_index = (unaryfunc)ValueProxy_eval_and_return,
};

/**
 * @brief Set attributes on `self` and return `self`, builder pattern like.
 */
static PyObject * ValueProxy_call(ValueProxyObject *self, PyObject *args, PyObject *kwds) {
	static char *kwlist[] = {"host", "timeout", "retries", "paramver", "remote", "verbose", NULL};
 
	int remote = self->remote;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|iiiiii:ValueProxy.__call__", kwlist, &self->host, &self->timeout, &self->retries, &self->paramver, &remote, &self->verbose)) {
		return NULL;  // TypeError is thrown
	}
	self->remote = remote;  /* Bitfield */

	return (PyObject*)Py_NewRef(self);
}

PyTypeObject ValueProxyType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "pycsh.ValueProxy",
    .tp_doc = "Wrapper utility class for libparam parameters.",
	.tp_as_number = &ValueProxy_as_number,
    .tp_basicsize = sizeof(ValueProxyObject),
    .tp_itemsize = 0,
	.tp_iter = (getiterfunc)ValueProxy_iter,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_str = (reprfunc)ValueProxy_str,
	.tp_repr = (reprfunc)ValueProxy_str,
	.tp_as_mapping = &ValueProxy_as_mapping,
    //.tp_new = ValueProxy_new,
    .tp_dealloc = (destructor)ValueProxy_dealloc,
	.tp_getset = ValueProxy_getsetters,
	.tp_richcompare = (richcmpfunc)ValueProxy_richcompare,
	.tp_hash = (hashfunc)ValueProxy_hash,
	/* TODO Kevin: Consider implementing `.__call__()` and change `Parameter.get_value_array` to a property,
		that would allow the user to both `list(Parameter(<name>).get_value_array)` and list(Parameter(<name>).get_value_array(remote=True)) */
	.tp_call = (PyCFunctionWithKeywords)ValueProxy_call,
};
