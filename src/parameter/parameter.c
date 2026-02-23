/*
 * parameter.c
 *
 * Contains the Parameter base class.
 *
 *  Created on: Apr 28, 2022
 *      Author: Kevin Wallentin Carlsen
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <pycsh/parameter.h>

#include "structmember.h"

#include <param/param.h>

#include <pycsh/pycsh.h>
#include <pycsh/utils.h>
#include <pycsh/attr_malloc.h>

#include "valueproxy.h"

// Instantiated in our PyMODINIT_FUNC
PyObject * PyExc_ParamCallbackError;
PyObject * PyExc_InvalidParameterTypeError;

/* Maps param_t to its corresponding PythonParameter for use by C callbacks. */
PyDictObject * param_callback_dict = NULL;

/* 1 for success. Compares the wrapped param_t for parameters, otherwise 0. Assumes self to be a ParameterObject. */
static int Parameter_equal(PyObject *self, PyObject *other) {
	if (PyObject_TypeCheck(other, &ParameterType) && (memcmp(&(((ParameterObject *)other)->param), &(((ParameterObject *)self)->param), sizeof(param_t)) == 0))
		return 1;
	return 0;
}

static PyObject * Parameter_richcompare(PyObject *self, PyObject *other, int op) {

	PyObject *result = Py_NotImplemented;

	switch (op) {
		// case Py_LT:
		// 	break;
		// case Py_LE:
		// 	break;
		case Py_EQ:
			result = (Parameter_equal(self, other)) ? Py_True : Py_False;
			break;
		case Py_NE:
			result = (Parameter_equal(self, other)) ? Py_False : Py_True;
			break;
		// case Py_GT:
		// 	break;
		// case Py_GE:
		// 	break;
	}

    Py_XINCREF(result);
    return result;
}

static PyObject * Parameter_str(ParameterObject *self) {
	char buf[100];
	sprintf(buf, "[id:%i|node:%i] %s | %s", self->param->id, *self->param->node, self->param->name, self->type->tp_name);
	return Py_BuildValue("s", buf);
}

/* Create a Python Parameter object from a param_t pointer directly. */
PyObject * pycsh_Parameter_from_param(PyTypeObject *type, param_t * param, const PyObject * callback, int host, int timeout, int retries, int paramver, py_param_free_e free_in_dealloc) {
	if (param == NULL) {
 		return NULL;
	}
	// This parameter is already wrapped by a ParameterObject, which we may return instead.
	ParameterObject * existing_parameter;
	if ((existing_parameter = Parameter_wraps_param(param)) != NULL) {
		/* TODO Kevin: How should we handle when: host, timeout, retries and paramver are different for the existing parameter? */
		return (PyObject*)Py_NewRef(existing_parameter);
	}

	ParameterObject *self = (ParameterObject *)type->tp_alloc(type, 0);

	if (self == NULL) {
		return NULL;
	}

	{   /* Add ourselves to the callback/lookup dictionary */
		PyObject *key AUTO_DECREF = PyLong_FromVoidPtr(param);
		assert(key != NULL);
		assert(!PyErr_Occurred());
		assert(PyDict_GetItem((PyObject*)param_callback_dict, key) == NULL);
		int set_res = PyDict_SetItem((PyObject*)param_callback_dict, key, (PyObject*)self);
		assert(set_res == 0);  // Allows the param_t callback to find the corresponding ParameterObject.
		(void)set_res;
		/* Just sanity check that we can get the item, so it doesn't later fail in `Parameter_dealloc()` */
		assert(PyDict_GetItem((PyObject*)param_callback_dict, key) != NULL);
		assert(!PyErr_Occurred());

		assert(self);
		Py_DECREF(self);  // param_callback_dict should hold a weak reference to self

		#if 0
		assert(self->ob_base.ob_type);
		/* The parameter linked list should maintain an eternal reference to Parameter() instances, and subclasses thereof (with the exception of PythonParameter() and its subclasses).
			This check should ensure that: Parameter("name") is Parameter("name") == True.
			This check doesn't apply to PythonParameter()'s, because its reference is maintained by .keep_alive */
		int is_pythonparameter = PyObject_IsSubclass((PyObject*)(type), (PyObject*)&PythonParameterType);
        if (is_pythonparameter < 0) {
			assert(false);
			return NULL;
		}

		if (is_pythonparameter) {
			Py_DECREF(self);  // param_callback_dict should hold a weak reference to self
		}
		#endif
	}

	self->host = (host != INT_MIN) ? host : *param->node;
	self->param = param;
	self->timeout = timeout;
	self->retries = retries;
	self->paramver = paramver;
	self->free_in_dealloc = free_in_dealloc;

	self->type = (PyTypeObject *)pycsh_util_get_type((PyObject *)self, NULL);

    return (PyObject *) self;
}

static PyObject * Parameter_find(PyTypeObject *type, PyObject *args, PyObject *kwds) {

	static char *kwlist[] = {"param_identifier", "node", "host", "paramver", "timeout", "retries", NULL};

	PyObject * param_identifier;  // Raw argument object/type passed. Identify its type when needed.
	PyObject * node = NULL;
	int host = INT_MIN;
	int paramver = 2;
	int timeout = pycsh_dfl_timeout;
	int retries = 1;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|Oiiii", kwlist, &param_identifier, &node, &host, &paramver, &timeout, &retries)) {
		return NULL;  // TypeError is thrown
	}

	param_t * param = _pycsh_util_find_param_t_hostname(param_identifier, node);

	if (param == NULL)  // Did not find a match.
		return NULL;  // Raises TypeError or ValueError.

    return pycsh_Parameter_from_param(type, param, NULL, host, timeout, retries, paramver, PY_PARAM_FREE_NO);
}

static PyObject * Parameter_get_name(ParameterObject *self, void *closure) {
	return Py_BuildValue("s", self->param->name);
}

static PyObject * Parameter_get_unit(ParameterObject *self, void *closure) {
	return Py_BuildValue("s", self->param->unit);
}

static PyObject * Parameter_get_docstr(ParameterObject *self, void *closure) {
	return Py_BuildValue("s", self->param->docstr);
}

static PyObject * Parameter_get_id(ParameterObject *self, void *closure) {
	return Py_BuildValue("H", self->param->id);
}

static PyObject * Parameter_get_node(ParameterObject *self, void *closure) {
	return Py_BuildValue("H", *self->param->node);
}

static PyObject * Parameter_get_storage_type(ParameterObject *self, void *closure) {
    assert(self->param);
    if (self->param->vmem == NULL) {
        return Py_BuildValue("i", (int)VMEM_TYPE_UNKNOWN);
    }
	return Py_BuildValue("i", self->param->vmem->type);
}

/* This will change self->param to be one by the same name at the specified node. */
static int Parameter_set_node(ParameterObject *self, PyObject *value, void *closure) {

	if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the node attribute");
        return -1;
    }

	if(!PyLong_Check(value)) {
		PyErr_SetString(PyExc_TypeError,
                        "The node attribute must be set to an int");
        return -1;
	}

	uint16_t node;

	// This is pretty stupid, but seems to be the easiest way to convert a long to short using Python.
	PyObject * value_tuple AUTO_DECREF = PyTuple_Pack(1, value);
	if (!PyArg_ParseTuple(value_tuple, "H", &node)) {
		return -1;
	}

	param_t * param = param_list_find_id(node, self->param->id);

	if (param == NULL)  // Did not find a match.
		return -1;  // Raises either TypeError or ValueError.

	self->param = param;

	return 0;
}

static PyObject * Parameter_get_host(ParameterObject *self, void *closure) {
	if (self->host != INT_MIN)
		return Py_BuildValue("i", self->host);
	Py_RETURN_NONE;
}

static int Parameter_set_host(ParameterObject *self, PyObject *value, void *closure) {

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



static PyObject * Parameter_get_py_type(ParameterObject *self, void *closure) {

	return (PyObject *)Py_NewRef(self->type);
}

static PyObject * Parameter_gettype_deprecated(ParameterObject *self, void *closure) {

	if (PyErr_WarnEx(PyExc_DeprecationWarning, "Use `.py_type` instead", 2) < 0) {
		return NULL;
	}

	return Parameter_get_py_type(self, closure);
}

static PyObject * Parameter_get_c_type(ParameterObject *self, void *closure) {

	return Py_BuildValue("i", self->param->type);
}

#ifdef OLD_PARAM_API_ERROR

static PyObject * Parameter_get_oldvalue(ParameterObject *self, void *closure) {
	PyErr_SetString(PyExc_AttributeError, "`Parameter.remote_value` and `Parameter.cached_value` have been changed to: `.get_value()`, `.set_value()`, `.get_value_array()` and `.set_value_array()`.");
	return NULL;
}

static int Parameter_set_oldvalue(ParameterObject *self, PyObject *value, void *closure) {
	PyErr_SetString(PyExc_AttributeError, "`Parameter.remote_value` and `Parameter.cached_value` have been changed to: `.get_value()`, `.set_value()`, `.get_value_array()` and `.set_value_array()`.");
	return -1;
}

#endif  /* OLD_PARAM_API_ERROR */


static PyObject * Parameter_get_valueproxy(ParameterObject *self, void *closure) {
	/* Default to remote, user can override by calling the ValueProxy, i.e: `.value_index(remote=False)[0]` */
	return pycsh_ValueProxy_from_Parameter(&ValueProxyType, self);
}

static PyObject * Parameter_get_valueproxy_cached(ParameterObject *self, void *closure) {

	if (PyErr_WarnEx(PyExc_DeprecationWarning, "`Parameter.remote_value` and `Parameter.cached_value` have been changed to `Parameter.value` `ValueProxy` property", 2) < 0) {
		return NULL;
	}

	/* Default to remote, user can override by calling the ValueProxy, i.e: `.value_index(remote=False)[0]` */
	ValueProxyObject * const value_proxy = (ValueProxyObject*)pycsh_ValueProxy_from_Parameter(&ValueProxyType, self);
	if (!value_proxy) {
		return NULL;
	}
	value_proxy->remote = false;
	return (PyObject*)value_proxy;  /* Already new reference */
}

static int Parameter_set_valueproxy(ParameterObject *self, PyObject *value, void *closure) {

	ValueProxyObject * const value_proxy = (ValueProxyObject*)pycsh_ValueProxy_from_Parameter(&ValueProxyType, self);
	if (!value_proxy) {
		return -1;
	}
	return ValueProxy_ass_subscript(value_proxy, Py_None, value);
}

static int Parameter_set_valueproxy_cached(ParameterObject *self, PyObject *value, void *closure) {

	if (PyErr_WarnEx(PyExc_DeprecationWarning, "`Parameter.remote_value` and `Parameter.cached_value` have been changed to `Parameter.value` `ValueProxy` property", 2) < 0) {
		return -1;
	}

	ValueProxyObject * const value_proxy = (ValueProxyObject*)pycsh_ValueProxy_from_Parameter(&ValueProxyType, self);
	if (!value_proxy) {
		return -1;
	}
	value_proxy->remote = false;
	return ValueProxy_ass_subscript(value_proxy, Py_None, value);
}


static PyObject * Parameter_is_vmem(ParameterObject *self, void *closure) {
	// I believe this is the most appropriate way to check for vmem parameters.
	PyObject * result = self->param->vmem == NULL ? Py_False : Py_True;
	return Py_NewRef(result);
}

static PyObject * Parameter_get_timeout(ParameterObject *self, void *closure) {
	return Py_BuildValue("i", self->timeout);
}

static int Parameter_set_timeout(ParameterObject *self, PyObject *value, void *closure) {

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

static PyObject * Parameter_getmask(ParameterObject *self, void *closure) {
	return Py_BuildValue("I", self->param->mask);
}

static PyObject * Parameter_gettimestamp(ParameterObject *self, void *closure) {
	/* TODO Kevin: Convert to float with `->tv_nsec` as decimals */
	return Py_BuildValue("I", self->param->timestamp->tv_sec);
}

static PyObject * Parameter_get_retries(ParameterObject *self, void *closure) {
	return Py_BuildValue("i", self->retries);
}

static int Parameter_set_retries(ParameterObject *self, PyObject *value, void *closure) {

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


// Source: https://chat.openai.com
/**
 * @brief Check that the callback accepts exactly one Parameter and one integer,
 *  as specified by "void (*callback)(struct param_s * param, int offset)"
 * 
 * Currently also checks type-hints (if specified).
 * Additional optional arguments are also allowed,
 *  as these can be disregarded by the caller.
 * 
 * @param callback function to check
 * @param raise_exc Whether to set exception message when returning false.
 * @return true for success
 */
bool is_valid_callback(const PyObject *callback, bool raise_exc) {

    /*We currently allow both NULL and Py_None,
            as those are valid to have on PythonParameterObject */
    if (callback == NULL || callback == Py_None) {
        return true;
    }

    // Suppress the incompatible pointer type warning when AUTO_DECREF is used on subclasses of PyObject*
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wincompatible-pointer-types"

    // Get the __code__ attribute of the function, and check that it is a PyCodeObject
    // TODO Kevin: Hopefully it's safe to assume that PyObject_GetAttrString() won't mutate callback
    PyCodeObject *func_code AUTO_DECREF = (PyCodeObject*)PyObject_GetAttrString((PyObject*)callback, "__code__");
    if (!func_code || !PyCode_Check(func_code)) {
        if (raise_exc)
            PyErr_SetString(PyExc_TypeError, "Provided callback must be callable");
        return false;
    }

    int accepted_pos_args = pycsh_get_num_accepted_pos_args(callback, raise_exc);
    if (accepted_pos_args < 2) {
        if (raise_exc)
            PyErr_SetString(PyExc_TypeError, "Provided callback must accept at least 2 positional arguments");
        return false;
    }

    // Check for too many required arguments
    int num_non_default_pos_args = pycsh_get_num_required_args(callback, raise_exc);
    if (num_non_default_pos_args > 2) {
        if (raise_exc)
            PyErr_SetString(PyExc_TypeError, "Provided callback must not require more than 2 positional arguments");
        return false;
    }

    // Get the __annotations__ attribute of the function
    // TODO Kevin: Hopefully it's safe to assume that PyObject_GetAttrString() won't mutate callback
    PyDictObject *func_annotations AUTO_DECREF = (PyDictObject *)PyObject_GetAttrString((PyObject*)callback, "__annotations__");

    // Re-enable the warning
    #pragma GCC diagnostic pop

    assert(PyDict_Check(func_annotations));
    if (!func_annotations) {
        return true;  // It's okay to not specify type-hints for the callback.
    }

    // Get the parameters annotation
    // PyCode_GetVarnames() exists and should be exposed, but it doesn't appear to be in any visible header.
    PyObject *param_names AUTO_DECREF = PyObject_GetAttrString((PyObject*)func_code, "co_varnames");// PyCode_GetVarnames(func_code);
    if (!param_names) {
        return true;  // Function parameters have not been annotated, this is probably okay.
    }

    // Check if it's a tuple
    if (!PyTuple_Check(param_names)) {
        // TODO Kevin: Not sure what exception to set here.
        if (raise_exc)
            PyErr_Format(PyExc_TypeError, "param_names type \"%s\" %p", param_names->ob_type->tp_name, param_names);
        return false;  // Not sure when this fails, but it's probably bad if it does.
    }

    PyObject *typing_module_name AUTO_DECREF = PyUnicode_FromString("typing");
    if (!typing_module_name) {
        return false;
    }

    PyObject *typing_module AUTO_DECREF = PyImport_Import(typing_module_name);
    if (!typing_module) {
        if (raise_exc)
            PyErr_SetString(PyExc_ImportError, "Failed to import typing module");
        return false;
    }

#if 1
    PyObject *get_type_hints AUTO_DECREF = PyObject_GetAttrString(typing_module, "get_type_hints");
    if (!get_type_hints) {
        if (raise_exc)
            PyErr_SetString(PyExc_ImportError, "Failed to get 'get_type_hints()' function");
        return false;
    }
    assert(PyCallable_Check(get_type_hints));


    PyObject *type_hint_dict AUTO_DECREF = PyObject_CallFunctionObjArgs(get_type_hints, callback, NULL);

#else

    PyObject *get_type_hints_name AUTO_DECREF = PyUnicode_FromString("get_type_hints");
    if (!get_type_hints_name) {
        return false;
    }

    PyObject *type_hint_dict AUTO_DECREF = PyObject_CallMethodObjArgs(typing_module, get_type_hints_name, callback, NULL);
    if (!type_hint_dict) {
        if (raise_exc)
            PyErr_SetString(PyExc_ImportError, "Failed to get type hints of callback");
        return false;
    }
#endif

    // TODO Kevin: Perhaps issue warnings for type-hint errors, instead of errors.
    {   // Checking first parameter type-hint

        // co_varnames may be too short for our index, if the signature has *args, but that's okay.
        if (PyTuple_Size(param_names)-1 <= 0) {
            return true;
        }

        PyObject *param_name = PyTuple_GetItem(param_names, 0);
        if (!param_name) {
            if (raise_exc)
                PyErr_SetString(PyExc_IndexError, "Could not get first parameter name");
            return false;
        }

        PyObject *param_annotation = PyDict_GetItem(type_hint_dict, param_name);
        if (param_annotation != NULL && param_annotation != Py_None) {
            if (!PyType_Check(param_annotation)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "First parameter annotation is %s, which is not a type", param_annotation->ob_type->tp_name);
                return false;
            }
            if (!PyObject_IsSubclass(param_annotation, (PyObject *)&ParameterType)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "First callback parameter should be type-hinted as Parameter (or subclass). (not %s)", param_annotation->ob_type->tp_name);
                return false;
            }
        }
    }

    {   // Checking second parameter type-hint

        // co_varnames may be too short for our index, if the signature has *args, but that's okay.
        if (PyTuple_Size(param_names)-1 <= 1) {
            return true;
        }

        PyObject *param_name = PyTuple_GetItem(param_names, 1);
        if (!param_name) {
            if (raise_exc)
                PyErr_SetString(PyExc_IndexError, "Could not get second parameter name");
            return false;
        }

        PyObject *param_annotation = PyDict_GetItem(type_hint_dict, param_name);
        if (param_annotation != NULL && param_annotation != Py_None) {
            if (!PyType_Check(param_annotation)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "Second parameter annotation is %s, which is not a type", param_annotation->ob_type->tp_name);
                return false;
            }
            if (!PyObject_IsSubclass(param_annotation, (PyObject *)&PyLong_Type)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "Second callback parameter should be type-hinted as int offset. (not %s)", param_annotation->ob_type->tp_name);
                return false;
            }
        }
    }

    return true;
}


static int Parameter_set_callback(ParameterObject *self, PyObject *value, void *closure) {

    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the callback attribute");
        return -1;
    }

    assert(self->param && self->param->name);
    //if (param_is_static(self->param)) {
    if (self->param->callback && self->param->callback != Parameter_callback) {
        PyErr_Format(PyExc_TypeError, "Cannot set callback of parameter ('%s') created in C", self->param->name);
        return -1;
    }

    if (!is_valid_callback(value, true)) {
        return -1;
    }

    if (value == self->callback) {
        return 0;  // No work to do
    }

    /* We now know that 'value' is a new callable. */

    /* When replacing a previous callable. */
    if (self->callback != value) {
        Py_XDECREF(self->callback);
    }

    self->callback = Py_NewRef(value);
    if (self->param->callback == NULL && value != Py_None) {
        self->param->callback = Parameter_callback;
    } else if (self->param->callback == Parameter_callback && value == Py_None) {
        self->param->callback = NULL;
    }

    return 0;
}


static long Parameter_hash(ParameterObject *self) {
	/* Use the ID of the parameter as the hash, as it is assumed unique. */
    return self->param->id;
}

static void Parameter_dealloc(ParameterObject *self) {

	{   /* Remove ourselves from the callback/lookup dictionary */
        PyObject *key AUTO_DECREF = PyLong_FromVoidPtr(self->param);
        assert(key != NULL);
		assert(!PyErr_Occurred());
        if (PyDict_GetItem((PyObject*)param_callback_dict, key) != NULL) {
			/* `param_callback_dict` currently holds a weak reference to `self`.
				`PyDict_DelItem()` will `Py_DECREF()`,
				which should not cause `self->ob_base.ob_refcnt` to go negative. */
			Py_INCREF(key);
            if (PyDict_DelItem((PyObject*)param_callback_dict, key) != 0) {
				/* Restore reference count if removal failed. */
				Py_DECREF(key);
			}
        }
    }

    /* TODO Kevin: How, and to what extent, should we reset the Python parameter callback?
        I suppose it's possible that someone creates a bunch of parameters using Python,
        letting the wrappers themselves get freed afterwards. But still wanting the callbacks to be called.
        On the other hand, if we don't "free()" the callback now, `libparam` won't know to do it later. */
    //if (self->callback != NULL && self->callback != Py_None) {
    //   Py_XDECREF(self->callback);
    //   self->callback = NULL;
    //}

    //assert(!PyErr_Occurred());
    //Parameter_set_callback(self, Py_None, NULL);
    //PyErr_Clear();

	assert(self->param);
	const param_t * const list_param = param_list_find_id(*self->param->node, self->param->id);
	if (list_param == NULL || list_param != self->param) {
		/* Our parameter is not in the list, we should free it. */
		param_list_destroy(self->param);
	}

	

	#if 0
	if (self->free_in_dealloc == PY_PARAM_FREE_PARAM_T) {
		free(self->param);
	} else if (self->free_in_dealloc == PY_PARAM_FREE_LIST_DESTROY) {
		/* With `param_list_destroy(self->param);` users of `self->free_in_dealloc` must take we to not resuse param_t buffer fields with param in list. */
		param_list_destroy(self->param);
	} else if (param_list_find_id(*self->param->node, self->param->id) != self->param && self->param != NULL) {
		/* Somehow we hold a reference to a parameter that is not in the list,
			this should only be possible if it was "list forget"en, after we wrapped it.
			It should therefore follow that we are now responsible for its memory (<-- TODO Kevin: Is this true? It has probably already been freed).
			We must therefore free() it, now that we are being deallocated.
			We check that (self->param != NULL), just in case we allow that to raise exceptions in the future. */
		param_list_destroy(self->param);
	}
	#endif

	PyTypeObject *baseclass = pycsh_get_base_dealloc_class(&ParameterType);
    baseclass->tp_dealloc((PyObject*)self);
}

/**
 * @brief Shared callback for all param_t's wrapped by a Parameter instance,
 * 	that must call a PyObject* callback function.
 */
void Parameter_callback(param_t * param, int offset) {
    PyGILState_STATE CLEANUP_GIL gstate = PyGILState_Ensure();
    assert(Parameter_wraps_param(param));
    
    /* `Parameter_callback` may be called many times before we call the `on_python_slash_execute_post_hook()`.
        Imagine that we're throwing up a ball, and blindly waiting for it to possibly come back down. */
    if (PyErr_Occurred()) {
        PyErr_Print();
    }

    PyObject *key AUTO_DECREF = PyLong_FromVoidPtr(param);
    ParameterObject *python_param = (ParameterObject*)PyDict_GetItem((PyObject*)param_callback_dict, key);

    /* This param_t uses the Python Parameter callback, but doesn't actually point to a Parameter.
        Perhaps it was deleted? Or perhaps it was never set correctly. */
    if (python_param == NULL) {
        assert(false);  // TODO Kevin: Is this situation worthy of an assert(), or should we just ignore it?
        return;
    }

    // PythonParameterObject *python_param = (PythonParameterObject *)((char *)param - offsetof(PythonParameterObject, parameter_object.param));
    PyObject *python_callback = python_param->callback;

    /* This Parameter has no callback */
    /* Python_callback should not be NULL here when Parameter_wraps_param(), but we will allow it for now... */
    if (python_callback == NULL || python_callback == Py_None) {
        return;
    }

    assert(is_valid_callback(python_callback, false));
    /* Create the arguments. */
    PyObject *pyoffset AUTO_DECREF = Py_BuildValue("i", offset);
    PyObject * args AUTO_DECREF = PyTuple_Pack(2, python_param, pyoffset);
    /* Call the user Python callback */
    PyObject *value AUTO_DECREF = PyObject_CallObject(python_callback, args);

    if (PyErr_Occurred()) {
        /* It may not be clear to the user, that the exception came from the callback,
            we therefore chain unto the existing exception, for better clarity. */
        /* _PyErr_FormatFromCause() seems simpler than PyException_SetCause() and PyException_SetContext() */
        // TODO Kevin: It seems exceptions raised in the CSP thread are ignored.
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 13
    PyErr_Format(PyExc_ParamCallbackError, "Error calling Python callback");
#else
    _PyErr_FormatFromCause(PyExc_ParamCallbackError, "Error calling Python callback");
#endif
        #if PYCSH_HAVE_APM  // TODO Kevin: This is pretty ugly, but we can't let the error propagate when building for APM, as there is no one but us to catch it.
            /* It may not be clear to the user, that the exception came from the callback,
                we therefore chain unto the existing exception, for better clarity. */
            /* _PyErr_FormatFromCause() seems simpler than PyException_SetCause() and PyException_SetContext() */
            // TODO Kevin: It seems exceptions raised in the CSP thread are ignored.
            //_PyErr_FormatFromCause(PyExc_ParamCallbackError, "Error calling Python callback");
            PyErr_Print();
        #endif
    }
}

/* Internal API for creating an entirely new ParameterObject. */
ATTR_MALLOC(Parameter_dealloc, 1)
ParameterObject * Parameter_create_new(PyTypeObject *type, uint16_t id, param_type_e param_type, uint32_t mask, char * name, char * unit, char * docstr, int array_size, const PyObject * callback, int host, int timeout, int retries, int paramver) {

    /* Check for valid parameter type. param_list_create_remote() should always return NULL for errors,
        but this allows us to raise a specific exception. */
    /* I'm not sure whether we can use (param_type > PARAM_TYPE_INVALID) to check for invalid parameters,
        so for now we will use a switch. This should also make GCC warn us when new types are added. */
    switch (param_type) {

        case PARAM_TYPE_UINT8:
        case PARAM_TYPE_UINT16:
        case PARAM_TYPE_UINT32:
        case PARAM_TYPE_UINT64:
        case PARAM_TYPE_INT8:
        case PARAM_TYPE_INT16:
        case PARAM_TYPE_INT32:
        case PARAM_TYPE_INT64:
        case PARAM_TYPE_XINT8:
        case PARAM_TYPE_XINT16:
        case PARAM_TYPE_XINT32:
        case PARAM_TYPE_XINT64:
        case PARAM_TYPE_FLOAT:
        case PARAM_TYPE_DOUBLE:
        case PARAM_TYPE_STRING:
        case PARAM_TYPE_DATA:
        case PARAM_TYPE_INVALID:
            break;
        
        default:
            PyErr_SetString(PyExc_InvalidParameterTypeError, "An invalid parameter type was specified during creation of a new parameter");
            return NULL;
    }

#if 0   /* It is permissable for duplicate wrapper instances to exist, as long as only 1 of them is added to the parameter list. */
    if (param_list_find_id(host, id) != NULL) {
        /* Run away as quickly as possible if this ID is already in use, we would otherwise get a segfault, which is driving me insane. */
        PyErr_Format(PyExc_ValueError, "Parameter with id %d already exists", id);
        return NULL;
    }

    if (param_list_find_name(host, name)) {
        /* While it is perhaps technically acceptable, it's probably best if we don't allow duplicate names either. */
        PyErr_Format(PyExc_ValueError, "Parameter with name \"%s\" already exists", name);
        return NULL;
    }
#endif

    if (!is_valid_callback(callback, true)) {
        return NULL;  // Exception message set by is_valid_callback();
    }

    param_t * new_param = param_list_create_remote(id, 0, param_type, mask, array_size, name, unit, docstr, -1);
    if (new_param == NULL) {
        return (ParameterObject*)PyErr_NoMemory();
    }

    ParameterObject * self = (ParameterObject *)pycsh_Parameter_from_param(type, new_param, callback, host, timeout, retries, paramver, PY_PARAM_FREE_LIST_DESTROY);
    if (self == NULL) {
        /* This is likely a memory allocation error, in which case we expect .tp_alloc() to have raised an exception. */
        return NULL;
    }

    *(self->param->node) = self->host;  /* TODO Kevin: Possibly breaking some functionality for requesting remote parameters here. Confirm with unit-test. */

    /* Found existing `Parameter` instance, which isn't using our `param_t` instance. */
    if (self->param != new_param) {
        param_list_destroy(new_param);
    }

    /* NULL callback becomes None on a ParameterObject instance */
    if (callback == NULL)
        callback = Py_None;

    if (Parameter_set_callback(self, (PyObject *)callback, NULL) == -1) {
        Py_DECREF(self);
        return NULL;
    }

    ((ParameterObject*)self)->param->callback = Parameter_callback;

    return self;
}

static PyObject * Parameter_new(PyTypeObject *cls, PyObject * args, PyObject * kwds) {

    uint16_t id; static_assert(sizeof(unsigned short int) == sizeof(uint16_t));  /* For `PyArg_ParseTupleAndKeywords()` */
    char * name;
    param_type_e param_type; static_assert(sizeof(param_type_e) == sizeof(int));  /* For `PyArg_ParseTupleAndKeywords()` */
    PyObject * mask_obj;
	int array_size = 0;
    PyObject * callback = NULL;
    char * unit = "";
    char * docstr = "";
    int host = INT_MIN;
    // TODO Kevin: What are these 2 doing here?
    int timeout = pycsh_dfl_timeout;
    int retries = 0;
    int paramver = 2;

    /* TODO Kevin: Should `host` and `node` be separate arguments. */
    static char *kwlist[] = {"id", "name", "type", "mask", "array_size", "callback", "unit", "docstr", "host", "timeout", "retries", "paramver", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "HsiO|iOzziiii", kwlist, &id, &name, &param_type, &mask_obj, &array_size, &callback, &unit, &docstr, &host, &timeout, &retries, &paramver))
        return NULL;  // TypeError is thrown

    uint32_t mask;
    if (pycsh_parse_param_mask(mask_obj, &mask) != 0) {
        return NULL;  // Exception message set by pycsh_parse_param_mask()
    }

    if (array_size < 1)
        array_size = 1;

    ParameterObject * python_param = Parameter_create_new(cls, id, param_type, mask, name, unit, docstr, array_size, callback, host, timeout, retries, paramver);
    if (python_param == NULL) {
        // Assume exception message to be set by Parameter_create_new()
        /* physaddr should be freed in dealloc() */
        return NULL;
    }

    /* return should steal the reference created by Parameter_create_new() */
    return (PyObject *)python_param;
}
PyMethodDef Parameter_class_methods[2] = {
	{"new", (PyCFunction)Parameter_new, METH_VARARGS | METH_KEYWORDS | METH_CLASS, "Create an entirely new parameter, instead of just wrapping an existing one"},
	{"find", (PyCFunction)Parameter_find, METH_VARARGS | METH_KEYWORDS | METH_CLASS, "Find an existing parameter in the global parameter list, using a parameter identifier."},
};

static PyObject * Parameter_get_callback(ParameterObject *self, void *closure) {

    if (self->param->callback != Parameter_callback) {
        Py_RETURN_NONE;  /* TODO Kevin: What to do when the user tries to get the callback for a C parameter? */
    }

    assert(self->callback);  /* We could handle NULL as Py_None here, but so far we don't. */
    return Py_NewRef(self->callback);
}

static PyObject * Parameter_GetItem(ParameterObject *self, PyObject *item) {

	#pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
	ValueProxyObject * const value_proxy AUTO_DECREF = pycsh_ValueProxy_from_Parameter(&ValueProxyType, self);
	#pragma GCC diagnostic pop
	if (!value_proxy) {
		return NULL;
	}

	return ValueProxy_subscript(value_proxy, item);
}

static int Parameter_SetItem(ParameterObject *self, PyObject* key, PyObject* value) {

	if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete parameter array indexes.");
        return -1;
    }

	#pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
	ValueProxyObject * const value_proxy AUTO_DECREF = pycsh_ValueProxy_from_Parameter(&ValueProxyType, self);
	#pragma GCC diagnostic pop
	if (!value_proxy) {
		return -1;
	}

	return ValueProxy_ass_subscript(value_proxy, key, value);
}

static Py_ssize_t ParameterArray_length(ParameterObject *self) {
	// We currently raise an exception when getting len() of non-array type parameters.
	// This stops PyCharm (Perhaps other IDE's) from showing their length as 0. ¯\_(ツ)_/¯
	return self->param->array_size;
}

static PyMappingMethods ParameterArray_as_mapping = {
    (lenfunc)ParameterArray_length,
    (binaryfunc)Parameter_GetItem,
    (objobjargproc)Parameter_SetItem
};

/* 
The Python binding 'Parameter' class exposes most of its attributes through getters, 
as only its 'value', 'host' and 'node' are mutable, and even those are through setters.
*/
static PyGetSetDef Parameter_getsetters[] = {

#if 1  // param_t getsetters
	{"name", (getter)Parameter_get_name, NULL,
     PyDoc_STR("Returns the name of the wrapped param_t C struct."), NULL},
    {"unit", (getter)Parameter_get_unit, NULL,
     PyDoc_STR("The unit of the wrapped param_t c struct as a string or None."), NULL},
	{"docstr", (getter)Parameter_get_docstr, NULL,
     PyDoc_STR("The help-text of the wrapped param_t c struct as a string or None."), NULL},
	{"id", (getter)Parameter_get_id, NULL,
     PyDoc_STR("id of the parameter"), NULL},
	{"type", (getter)Parameter_gettype_deprecated, NULL,
     PyDoc_STR("type of the parameter"), NULL},
	{"py_type", (getter)Parameter_get_py_type, NULL,
     PyDoc_STR("type of the parameter"), NULL},
	{"c_type", (getter)Parameter_get_c_type, NULL,
     PyDoc_STR("type of the parameter"), NULL},
	{"mask", (getter)Parameter_getmask, NULL,
     PyDoc_STR("mask of the parameter"), NULL},
	{"timestamp", (getter)Parameter_gettimestamp, NULL,
     PyDoc_STR("timestamp of the parameter"), NULL},
	{"node", (getter)Parameter_get_node, (setter)Parameter_set_node,
     PyDoc_STR("node of the parameter"), NULL},
#endif

#if 1  // Parameter getsetters
	{"host", (getter)Parameter_get_host, (setter)Parameter_set_host,
     PyDoc_STR("host of the parameter"), NULL},
#ifdef OLD_PARAM_API_ERROR
	{"remote_value", (getter)Parameter_get_oldvalue, (setter)Parameter_set_oldvalue,
     PyDoc_STR("get/set the remote (and cached) value of the parameter"), NULL},
	{"cached_value", (getter)Parameter_get_oldvalue, (setter)Parameter_set_oldvalue,
     PyDoc_STR("get/set the cached value of the parameter"), NULL},
#else  /* OLD_PARAM_API_ERROR */
	{"remote_value", (getter)Parameter_get_valueproxy, (setter)Parameter_set_valueproxy,
     PyDoc_STR("get/set the remote (and cached) value of the parameter"), NULL},
	{"cached_value", (getter)Parameter_get_valueproxy_cached, (setter)Parameter_set_valueproxy_cached,
     PyDoc_STR("get/set the cached value of the parameter"), NULL},
#endif  /* OLD_PARAM_API_ERROR */

	{"callback", (getter)Parameter_get_callback, (setter)Parameter_set_callback,
     "callback of the parameter", NULL},

	{"value", (getter)Parameter_get_valueproxy, (setter)Parameter_set_valueproxy,
     PyDoc_STR("get/set the remote/cached value of the parameter"), NULL},

	{"is_vmem", (getter)Parameter_is_vmem, NULL,
     PyDoc_STR("whether the parameter is a vmem parameter"), NULL},
	{"storage_type", (getter)Parameter_get_storage_type, NULL,
     PyDoc_STR("storage type of the parameter"), NULL},
	{"timeout", (getter)Parameter_get_timeout, (setter)Parameter_set_timeout,
     PyDoc_STR("timeout of the parameter"), NULL},
	{"retries", (getter)Parameter_get_retries, (setter)Parameter_set_retries,
     PyDoc_STR("available retries of the parameter"), NULL},
#endif
    {NULL, NULL, NULL, NULL}  /* Sentinel */
};

static PyObject * Parameter_list_add(ParameterObject *self, PyObject *args, PyObject *kwds) {

	int return_self = true;

	static char * kwlist[] = {"return_self", NULL};

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|p", kwlist, &return_self)) {
		return NULL;  // TypeError is thrown
	}

	param_t * const list_param = param_list_find_id(*self->param->node, self->param->id);
	
	if (list_param == self->param) {
		/* Existing parameter is ourself.
			So there is no reason to assign the list parameter to what it already is. */
		/* 5 is a unique return value (albeit obtuse) (unlikely to be used by `param_list_add()` in the future),
			which indicates that we did not update `self` in the list. */
		return return_self ? Py_NewRef(self) : Py_BuildValue("i", 5);
	}


	/* res==1=="existing parameter updated" */
	const int res = param_list_add(self->param);

    /* `self` is now added to the list.
        Although if we updated an existing parameter,
        we should point to that instead. */
    /* In any case, the list now holds a reference to `self`. */
    Py_INCREF(self);
	
	if (res!=1) {
		/* Did not update existing parameter. We either added a new parameter, or error. */
		return return_self ? Py_NewRef(self) : Py_BuildValue("i", res);
	}

	/* NOTE Kevin: Be very careful about `destroy=true` here.
		it `assert()`s that we never create multiple `ParameterObject`s pointing/reusing the same `self->param`.
		We can ensure this by keeping a weakref dict of existing `ParameterObject` instances.
		(weakref dict should use `&param_t` as key, which should it to return both new parameters we've created, and wrappers for existing parameters). */
	/* TODO Kevin: Test correct handling of `param_heap_t` here.  */
	param_list_remove_specific(self->param, 0, true);

	/* NOTE: This assignment has big implications of state. */
	self->param = list_param;

	return return_self ? Py_NewRef(self) : Py_BuildValue("i", res);
}
static PyObject * Parameter_list_forget(ParameterObject *self, PyObject *args, PyObject *kwds) {
	int verbose = pycsh_dfl_verbose;

	static char * kwlist[] = {"verbose", NULL};

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|i", kwlist, &verbose)) {
		return NULL;  // TypeError is thrown
	}

    const param_t * const list_param_before = param_list_find_id(*self->param->node, self->param->id);

	/* `param_list_destroy()` will be called by `Parameter_dealloc()` */
	param_list_remove_specific(self->param, verbose, false);

    const param_t * const list_param_after = param_list_find_id(*self->param->node, self->param->id);

    /* Successfully removed `self` from the parameter list, so it no longer holds a reference. */
    if (list_param_before && !list_param_after) {
        Py_DECREF(self);
    }

	Py_RETURN_NONE;
}
static PyMethodDef Parameter_methods[] = {
    {"list_add", (PyCFunction)Parameter_list_add, METH_VARARGS | METH_KEYWORDS, PyDoc_STR("Add `self` the global parameter list. Exposes it to other CSP nodes on the network, "\
		"And allows it to be found in `pycsh.list()`")},
    {"list_forget", (PyCFunction)Parameter_list_forget, METH_VARARGS | METH_KEYWORDS, PyDoc_STR("Remove this parameter from the global parameter list. Hiding it from other CSP nodes on the network. "\
		"Also removes it from `pycsh.list()`")},
    {NULL, NULL, 0, NULL}
};

PyTypeObject ParameterType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "pycsh.Parameter",
    .tp_doc = "Wrapper utility class for libparam parameters.",
    .tp_basicsize = sizeof(ParameterObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = Parameter_find,
    .tp_dealloc = (destructor)Parameter_dealloc,
	.tp_getset = Parameter_getsetters,
	// .tp_members = Parameter_members,
	.tp_as_mapping = &ParameterArray_as_mapping,
	.tp_methods = Parameter_methods,
	.tp_str = (reprfunc)Parameter_str,
	.tp_richcompare = (richcmpfunc)Parameter_richcompare,
	.tp_hash = (hashfunc)Parameter_hash,
};
