/*
 * parameter.c
 *
 * Contains miscellaneous utilities used by PyCSH.
 *
 *  Created on: Apr 28, 2022
 *      Author: Kevin Wallentin Carlsen
 */

#include <pycsh/utils.h>

#include <dirent.h>
#include <csp/csp_hooks.h>
#include <csp/csp_buffer.h>
#include <param/param_server.h>
#include <param/param_client.h>
#include <param/param_string.h>
#include <param/param_serializer.h>

#include <pycsh/pycsh.h>
#include <pycsh/parameter.h>
#include "parameter/parameterlist.h"

#undef NDEBUG
#include <assert.h>


__attribute__((weak))  void cleanup_free(void *const* obj) {
    if (*obj == NULL) {
        return;
	}
    free(*obj);
	/* Setting *obj=NULL should never have a visible effect when using __attribute__((cleanup()).
		But accepting a *const* allows for more use cases. */
    //*obj = NULL;  // 
}
/* __attribute__(()) doesn't like to treat char** and void** interchangeably. */
void cleanup_str(char *const* obj) {
    cleanup_free((void *const*)obj);
}
void _close_dir(DIR *const* dir) {
	if (dir == NULL || *dir == NULL) {
		return;
	}
	closedir(*dir);
	//*dir = NULL;
}
void _close_file(FILE *const* file) {
	if (file == NULL || *file == NULL) {
		return;
	}
	fclose(*file);
}
void cleanup_GIL(PyGILState_STATE * gstate) {
    //if (*gstate == PyGILState_UNLOCKED)
    //    return
    PyGILState_Release(*gstate);
    //*gstate = NULL;
}
void cleanup_pyobject(PyObject * const * obj) {
    Py_XDECREF(*obj);
}

void state_release_GIL(PyThreadState ** state) {
	if (*state == NULL) {
		return;  // We didn't have the GIL, so there's nothing to release.
	}
    *state = PyEval_SaveThread();
}

__attribute__((malloc(free, 1)))
__attribute__((weak)) 
char *safe_strdup(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    return strdup(s);
}

/* Source: https://pythonextensionpatterns.readthedocs.io/en/latest/super_call.html */
PyObject * call_super_pyname_lookup(PyObject *self, PyObject *func_name, PyObject *args, PyObject *kwargs) {
    PyObject *result        = NULL;
    PyObject *builtins      = NULL;
    PyObject *super_type    = NULL;
    PyObject *super         = NULL;
    PyObject *super_args    = NULL;
    PyObject *func          = NULL;

    builtins = PyImport_AddModule("builtins");
    if (! builtins) {
        assert(PyErr_Occurred());
        goto except;
    }
    // Borrowed reference
    Py_INCREF(builtins);
    super_type = PyObject_GetAttrString(builtins, "super");
    if (! super_type) {
        assert(PyErr_Occurred());
        goto except;
    }
    super_args = PyTuple_New(2);
    Py_INCREF(self->ob_type);
    if (PyTuple_SetItem(super_args, 0, (PyObject*)self->ob_type)) {
        assert(PyErr_Occurred());
        goto except;
    }
    Py_INCREF(self);
    if (PyTuple_SetItem(super_args, 1, self)) {
        assert(PyErr_Occurred());
        goto except;
    }
    super = PyObject_Call(super_type, super_args, NULL);
    if (! super) {
        assert(PyErr_Occurred());
        goto except;
    }
    func = PyObject_GetAttr(super, func_name);
    if (! func) {
        assert(PyErr_Occurred());
        goto except;
    }
    if (! PyCallable_Check(func)) {
        PyErr_Format(PyExc_AttributeError,
                     "super() attribute \"%S\" is not callable.", func_name);
        goto except;
    }
    result = PyObject_Call(func, args, kwargs);
    assert(! PyErr_Occurred());
    goto finally;
except:
    assert(PyErr_Occurred());
    Py_XDECREF(result);
    result = NULL;
finally:
    Py_XDECREF(builtins);
    Py_XDECREF(super_args);
    Py_XDECREF(super_type);
    Py_XDECREF(super);
    Py_XDECREF(func);
    return result;
}

/**
 * @brief Get the first base-class with a .tp_dealloc() different from the specified class.
 * 
 * If the specified class defines its own .tp_dealloc(),
 * if should be safe to assume the returned class to be no more abstract than object(),
 * which features its .tp_dealloc() that ust be called anyway.
 * 
 * This function is intended to be called in a subclassed __del__ (.tp_dealloc()),
 * where it will mimic a call to super().
 * 
 * @param cls Class to find a super() .tp_dealloc() for.
 * @return PyTypeObject* super() class.
 */
PyTypeObject * pycsh_get_base_dealloc_class(PyTypeObject *cls) {
	
	/* Keep iterating baseclasses until we find one that doesn't use this deallocator. */
	PyTypeObject *baseclass = cls->tp_base;
	for (; baseclass->tp_dealloc == cls->tp_dealloc; (baseclass = baseclass->tp_base));

    assert(baseclass->tp_dealloc != NULL);  // Assert that Python installs some deallocator to classes that don't specifically implement one (Whether pycsh.Parameter or object()).
	return baseclass;
}

/**
 * @brief Goes well with (__DATE__, __TIME__) and (csp_cmp_message.ident.date, csp_cmp_message.ident.time)
 * 
 * 'date' and 'time' are separate arguments, because it's most convenient when working with csp_cmp_message.
 * 
 * @param date __DATE__ or csp_cmp_message.ident.date
 * @param time __TIME__ or csp_cmp_message.ident.time
 * @return New reference to a PyObject* datetime.datetime() from the specified time and dated
 */
PyObject *pycsh_ident_time_to_datetime(const char * const date, const char * const time) {

	PyObject *datetime_module AUTO_DECREF = PyImport_ImportModule("datetime");
	if (!datetime_module) {
		return NULL;
	}

	PyObject *datetime_class AUTO_DECREF = PyObject_GetAttrString(datetime_module, "datetime");
	if (!datetime_class) {
		return NULL;
	}

	PyObject *datetime_strptime AUTO_DECREF = PyObject_GetAttrString(datetime_class, "strptime");
	if (!datetime_strptime) {
		return NULL;
	}

	//PyObject *datetime_str AUTO_DECREF = PyUnicode_FromFormat("%U %U", self->date, self->time);
	PyObject *datetime_str AUTO_DECREF = PyUnicode_FromFormat("%s %s", date, time);
	if (!datetime_str) {
		return NULL;
	}

	PyObject *format_str AUTO_DECREF = PyUnicode_FromString("%b %d %Y %H:%M:%S");
	if (!format_str) {
		return NULL;
	}

	PyObject *datetime_args AUTO_DECREF = PyTuple_Pack(2, datetime_str, format_str);
	if (!datetime_args) {
		return NULL;
	}

	/* No DECREF, we just pass the new reference (returned by PyObject_CallObject()) to the caller.
		No NULL check either, because the caller has to do that anyway.
		And the only cleanup we need (for exceptions) is already done by AUTO_DECREF */
	PyObject *datetime_obj = PyObject_CallObject(datetime_strptime, datetime_args);

	return datetime_obj;
}

int pycsh_get_num_accepted_pos_args(const PyObject *function, bool raise_exc) {

	// Suppress the incompatible pointer type warning when AUTO_DECREF is used on subclasses of PyObject*
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    PyCodeObject *func_code AUTO_DECREF = (PyCodeObject*)PyObject_GetAttrString((PyObject*)function, "__code__");
	// Re-enable the warning
    #pragma GCC diagnostic pop

    if (!func_code || !PyCode_Check(func_code)) {
        if (raise_exc)
            PyErr_SetString(PyExc_TypeError, "Provided function must be callable");
        return -1;
    }

    // Check if the function accepts *args
    int accepts_varargs = (func_code->co_flags & CO_VARARGS) ? 1 : 0;

    // Return INT_MAX if *args is present
    if (accepts_varargs) {
        return INT_MAX;
    }

    // Number of positional arguments excluding *args
    int num_pos_args = func_code->co_argcount;
    return num_pos_args;
}


// Source: https://chatgpt.com
int pycsh_get_num_required_args(const PyObject *function, bool raise_exc) {

	// Suppress the incompatible pointer type warning when AUTO_DECREF is used on subclasses of PyObject*
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    PyCodeObject *func_code AUTO_DECREF = (PyCodeObject*)PyObject_GetAttrString((PyObject*)function, "__code__");
	// Re-enable the warning
    #pragma GCC diagnostic pop

    if (!func_code || !PyCode_Check(func_code)) {
        if (raise_exc)
            PyErr_SetString(PyExc_TypeError, "Provided callback must be callable");
        return -1;
    }

    int num_required_pos_args = func_code->co_argcount - func_code->co_kwonlyargcount;

    PyObject *defaults AUTO_DECREF = PyObject_GetAttrString((PyObject*)function, "__defaults__");
    Py_ssize_t num_defaults = (defaults && PyTuple_Check(defaults)) ? PyTuple_Size(defaults) : 0;

    int num_non_default_pos_args = num_required_pos_args - (int)num_defaults;
    return num_non_default_pos_args;
}


/* Retrieves a param_t from either its name, id or wrapper object.
   May raise TypeError or ValueError, returned value will be NULL in either case. */
param_t * _pycsh_util_find_param_t(PyObject * param_identifier, int node) {

	param_t * param = NULL;

	if (PyUnicode_Check(param_identifier))  // is_string
		param = param_list_find_name(node, (char*)PyUnicode_AsUTF8(param_identifier));
	else if (PyLong_Check(param_identifier))  // is_int
		param = param_list_find_id(node, (int)PyLong_AsLong(param_identifier));
	else if (PyObject_TypeCheck(param_identifier, &ParameterType))
		param = ((ParameterObject *)param_identifier)->param;
	else {  // Invalid type passed.
		PyErr_SetString(PyExc_TypeError,
			"Parameter identifier must be either an integer or string of the parameter ID or name respectively.");
		return NULL;
	}

	if (param == NULL)  // Check if a parameter was found.
		PyErr_SetString(PyExc_ValueError, "Could not find a matching parameter.");

	return param;  // or NULL for ValueError.
}


/* Gets the best Python representation of the param_t's type, i.e 'int' for 'uint32'.
   Does not increment the reference count of the found type before returning.
   May raise TypeError for unsupported parameter types (none exist at time of writing). */
static PyTypeObject * _pycsh_misc_param_t_type(param_t * param) {

	PyTypeObject * param_type = NULL;

	switch (param->type) {
		case PARAM_TYPE_UINT8:
		case PARAM_TYPE_XINT8:
		case PARAM_TYPE_UINT16:
		case PARAM_TYPE_XINT16:
		case PARAM_TYPE_UINT32:
		case PARAM_TYPE_XINT32:
		case PARAM_TYPE_UINT64:
		case PARAM_TYPE_XINT64:
		case PARAM_TYPE_INT8:
		case PARAM_TYPE_INT16:
		case PARAM_TYPE_INT32:
		case PARAM_TYPE_INT64: {
			param_type = &PyLong_Type;
			break;
		}
		case PARAM_TYPE_FLOAT:
		case PARAM_TYPE_DOUBLE: {
			param_type = &PyFloat_Type;
			break;
		}
		case PARAM_TYPE_STRING: {
			param_type = &PyUnicode_Type;
			break;
		}
		case PARAM_TYPE_DATA: {
			param_type = &PyByteArray_Type;
			break;
		}
		default:  // Raise NotImplementedError when param_type remains NULL.
			PyErr_SetString(PyExc_NotImplementedError, "Unsupported parameter type.");
			break;
	}

	return param_type;  // or NULL (for NotImplementedError).
}


/* Public interface for '_pycsh_misc_param_t_type()'
   Does not increment the reference count of the found type before returning. */
PyObject * pycsh_util_get_type(PyObject * self, PyObject * args) {

	PyObject * param_identifier;
	int node = pycsh_dfl_node;

	param_t * param;

	/* Function may be called either as method on 'Parameter' object or standalone function. */
	
	if (self && PyObject_TypeCheck(self, &ParameterType)) {
		ParameterObject *_self = (ParameterObject *)self;

		node = *_self->param->node;
		param = _self->param;

	} else {
		if (!PyArg_ParseTuple(args, "O|i", &param_identifier, &node)) {
			return NULL;  // TypeError is thrown
		}

		param = _pycsh_util_find_param_t(param_identifier, node);
	}

	if (param == NULL) {  // Did not find a match.
		return NULL;  // Raises either TypeError or ValueError.
	}


	return (PyObject *)_pycsh_misc_param_t_type(param);
}

ParameterObject * Parameter_wraps_param(param_t *param) {
	/* TODO Kevin: If it ever becomes possible to assert() the held state of the GIL,
		we would definitely want to do it here. We don't want to use PyGILState_Ensure()
		because the GIL should still be held after returning. */
    assert(param != NULL);

	/* PyCSH most likely not imported yet,
		so no way this can be a Python ParameterObject */
	if (param_callback_dict == NULL) {
		return NULL;
	}

	PyObject * const key AUTO_DECREF = PyLong_FromVoidPtr(param);
	assert(PyDict_Check(param_callback_dict));
    ParameterObject * const python_param = (ParameterObject*)PyDict_GetItem((PyObject*)param_callback_dict, key);

	return python_param;
}

/**
 * @brief Return a list of Parameter wrappers similar to the "list" slash command
 * 
 * @param node <0 for all nodes, otherwise only include parameters for the specified node.
 * @return PyObject* Py_NewRef(list[Parameter])
 */
PyObject * pycsh_util_parameter_list(uint32_t mask, int node, const char * globstr) {

	PyObject * list = PyObject_CallObject((PyObject *)&ParameterListType, NULL);

	param_t * param;
	param_list_iterator i = {};
	while ((param = param_list_iterate(&i)) != NULL) {

		if ((node >= 0) && (*param->node != node)) {
			continue;
		}
		if ((param->mask & mask) == 0) {
			continue;
		}
		int strmatch(const char *str, const char *pattern, int n, int m);  // TODO Kevin: Maybe strmatch() should be in the libparam public API?
		if ((globstr != NULL) && strmatch(param->name, globstr, strlen(param->name), strlen(globstr)) == 0) {
			continue;
		}

		/* CSH does not specify a paramver when listing parameters,
			so we just use 2 as the default version for the created instances. */
		PyObject * parameter AUTO_DECREF = pycsh_Parameter_from_param(&ParameterType, param, NULL, INT_MIN, pycsh_dfl_timeout, 1, 2, PY_PARAM_FREE_NO);
		if (parameter == NULL) {
			Py_DECREF(list);
			return NULL;
		}
		PyObject * argtuple AUTO_DECREF = PyTuple_Pack(1, parameter);
		Py_XDECREF(ParameterList_append(list, argtuple));  // TODO Kevin: DECREF on None doesn't seem right here...
	}

	return list;

}

typedef struct param_list_s {
	size_t cnt;
	param_t ** param_arr;
} param_list_t;

/* Do not apply parameters to the global list, only use the provided one */
static void pycsh_param_queue_apply_listless(param_queue_t * queue, param_list_t * param_list, int from, bool skip_list) {

    int atomic_write = 0;
	for (size_t i = 0; i < param_list->cnt; i++) {  /* Apply value to provided param_t* if they aren't in the list. */
		param_t * param = param_list->param_arr[i];

		/* Loop over paramid's in pull response */
		mpack_reader_t reader;
		mpack_reader_init_data(&reader, queue->buffer, queue->used);
		while(reader.data < reader.end) {
			int id, node, offset = -1;
			csp_timestamp_t timestamp = { .tv_sec = 0, .tv_nsec = 0 };
			param_deserialize_id(&reader, &id, &node, &timestamp, &offset, queue);
			if (node == 0)
				node = from;

			if (skip_list) {
				/* List parameters have already been applied by param_queue_apply(). */
				const param_t * const list_param = param_list_find_id(node, id);
				if (list_param != NULL) {
					/* Print the local RAM copy of the remote parameter (which is not in the list) */
					// if (verbose) {
					// 	param_print(param, -1, NULL, 0, verbose, 0);
					// }
					/* We need to discard the data field, to get to next paramid */
					mpack_discard(&reader);
					continue;
				}
			}

			/* This is not our parameter, let's hope it has been applied by `param_queue_apply(...)` */
			if (param->id != id) {
				/* We need to discard the data field, to get to next paramid */
				mpack_discard(&reader);
				continue;
			}

			if ((param->mask & PM_ATOMIC_WRITE) && (atomic_write == 0)) {
				atomic_write = 1;
				if (param_enter_critical)
					param_enter_critical();
			}

			param_deserialize_from_mpack_to_param(NULL, NULL, param, offset, &reader);

			/* Print the local RAM copy of the remote parameter (which is not in the list) */
			// if (verbose) {
			// 	param_print(param, -1, NULL, 0, verbose, 0);
			// }

		}
	}

	if (atomic_write) {
		if (param_exit_critical)
			param_exit_critical();
	}
}

static void pycsh_param_transaction_callback_pull(csp_packet_t *response, int verbose, int version, void * context) {

	int from = response->id.src;
	//csp_hex_dump("pull response", response->data, response->length);
	//printf("From %d\n", from);

	assert(context != NULL);
	param_list_t * param_list = (param_list_t *)context;

	param_queue_t queue;
	csp_timestamp_t time_now;
	csp_clock_get_time(&time_now);
	param_queue_init(&queue, &response->data[2], response->length - 2, response->length - 2, PARAM_QUEUE_TYPE_SET, version);
	queue.last_node = response->id.src;
	queue.client_timestamp = time_now;
	queue.last_timestamp = queue.client_timestamp;

	/* Even though we have been provided a `param_t * param`,
		we still call `param_queue_apply()` to support replies which unexpectedly contain multiple parameters.
		Although we are SOL if those unexpected parameters are not in the list.
		TODO Kevin: Make sure ParameterList accounts for this scenario. */
	param_queue_apply(&queue, from);

	/* For now we tolerate possibly setting parameters twice,
		as we have not had remote parameters with callbacks/side-effects yet.
		Although it is possible, I have tested it.
		We `assert()` that `pycsh_param_transaction_callback_pull()` is only used client-side.
		So PyCSH parameter servers will only use `param_queue_apply()` to apply parameters,
		meaning no worries about callbacks being set twice. */
	pycsh_param_queue_apply_listless(&queue, param_list, from, true);

	csp_buffer_free(response);
}


static int pycsh_param_push_single(param_t *param, int offset, int prio, void *value, int verbose, int host, int timeout, int version, bool ack_with_pull) {

	csp_packet_t * packet = csp_buffer_get(PARAM_SERVER_MTU);
	if (packet == NULL)
		return -1;

	packet->data[1] = 0;
	param_transaction_callback_f cb = NULL;

	if (ack_with_pull) {
		packet->data[1] = 1;
		cb = pycsh_param_transaction_callback_pull;
	}

	param_list_t param_list = {
		.param_arr = &param,
		.cnt = 1
	};

	if(version == 2) {
		packet->data[0] = PARAM_PUSH_REQUEST_V2;
	} else {
		packet->data[0] = PARAM_PUSH_REQUEST;
	}

	param_queue_t queue;
	param_queue_init(&queue, &packet->data[2], PARAM_SERVER_MTU - 2, 0, PARAM_QUEUE_TYPE_SET, version);
	param_queue_add(&queue, param, offset, value);

	packet->length = queue.used + 2;
	packet->id.pri = prio;
	int result = param_transaction(packet, host, timeout, cb, verbose, version, &param_list);

	if (result < 0) {
		return -1;
	}

	/* If it was a remote parameter, set the value after the ack but not if ack with push which sets param timestamp */
	if (*param->node != 0 && value != NULL && param->timestamp->tv_sec == 0)
	{
		if (offset < 0) {
			for (int i = 0; i < param->array_size; i++)
				param_set(param, i, value);
		} else {
			param_set(param, offset, value);
		}
	}

	return 0;
}


static int pycsh_param_pull_single(param_t *param, int offset, int prio, int verbose, int host, int timeout, int version) {

	csp_packet_t * packet = csp_buffer_get(PARAM_SERVER_MTU);
	if (packet == NULL)
		return -1;

	if (version == 2) {
		packet->data[0] = PARAM_PULL_REQUEST_V2;
	} else {
		packet->data[0] = PARAM_PULL_REQUEST;
	}
	packet->data[1] = 0;

	param_list_t param_list = {
		.param_arr = &param,
		.cnt = 1
	};

	param_queue_t queue;
	param_queue_init(&queue, &packet->data[2], PARAM_SERVER_MTU - 2, 0, PARAM_QUEUE_TYPE_GET, version);
	param_queue_add(&queue, param, offset, NULL);

	packet->length = queue.used + 2;
	packet->id.pri = prio;
	return param_transaction(packet, host, timeout, pycsh_param_transaction_callback_pull, verbose, version, &param_list);
}


static int pycsh_param_pull_queue(param_queue_t *queue, param_t *params, unsigned int param_cnt, int prio, int verbose, int host, int timeout, int version) {

	csp_packet_t * packet = csp_buffer_get(PARAM_SERVER_MTU);
	if (packet == NULL)
		return -1;

	if (version == 2) {
		packet->data[0] = PARAM_PULL_REQUEST_V2;
	} else {
		packet->data[0] = PARAM_PULL_REQUEST;
	}
	packet->data[1] = 0;

	param_list_t param_list = {
		.param_arr = &params,
		.cnt = param_cnt
	};

	packet->length = queue->used + 2;
	packet->id.pri = prio;
	return param_transaction(packet, host, timeout, pycsh_param_transaction_callback_pull, verbose, version, &param_list);
}


/**
 * @param ack_with_pull_params If specified, doubles as the `ack_with_pull` boolean.
 * 	It should then be a list of parameters that the remote is expected to ack with (typically the parameters in `queue`).
 */
static int pycsh_param_push_queue(param_queue_t *queue, int prio, int verbose, int host, int timeout, uint32_t hwid, param_list_t * ack_with_pull_params) {

	if ((queue == NULL) || (queue->used == 0))
		return 0;

	csp_packet_t * packet = csp_buffer_get(PARAM_SERVER_MTU);
	if (packet == NULL)
		return -2;

	if (queue->version == 2) {
		packet->data[0] = PARAM_PUSH_REQUEST_V2;
	} else {
		packet->data[0] = PARAM_PUSH_REQUEST;
	}

	packet->data[1] = 0;
	param_transaction_callback_f cb = NULL;

	if (ack_with_pull_params) {
		packet->data[1] = 1;
		cb = pycsh_param_transaction_callback_pull;
	} else if (timeout == 0) {
		packet->data[1] = PARAM_FLAG_NOACK;
	}

	memcpy(&packet->data[2], queue->buffer, queue->used);

	packet->length = queue->used + 2;
	packet->id.pri = prio;

	/* Append hwid, no care given to endian at this point */
	if (hwid > 0) {
		packet->data[0] = PARAM_PUSH_REQUEST_V2_HWID;
		//printf("Add hwid %x\n", hwid);
		//csp_hex_dump("tx", packet->data, packet->length);
		memcpy(&packet->data[packet->length], &hwid, sizeof(hwid));
		packet->length += sizeof(hwid);

	}

	int result = param_transaction(packet, host, timeout, cb, verbose, queue->version, ack_with_pull_params);

	if (result < 0) {
		printf("push queue error\n");
		return -1;
	}

	if(!ack_with_pull_params) {
        /* TODO Kevin: This will nok work with PyCSH parameters outside of the list. */
		param_queue_apply(queue, host);
	}

	return 0;
}


/* Checks that the specified index is within bounds of the sequence index, raises IndexError if not.
   Supports Python backwards subscriptions, mutates the index to a positive value in such cases. */
int _pycsh_util_index(int seqlen, int *index) {
	if (*index < 0)  // Python backwards subscription.
		*index += seqlen;
	if (*index < 0 || *index > seqlen - 1) {
		PyErr_SetString(PyExc_IndexError, "Array Parameter index out of range");
		return -1;
	}

	return 0;
}

/* Private interface for getting the value of single parameter
   Increases the reference count of the returned item before returning.
   Use INT_MIN for offset as no offset. */
PyObject * _pycsh_util_get_single(param_t *param, const int offset_orig, int autopull, int host, int timeout, int retries, int paramver, int verbose) {

	int offset = offset_orig;
	if (offset != INT_MIN) {
		if (_pycsh_util_index(param->array_size, &offset))  // Validate the offset.
			return NULL;  // Raises IndexError.
	} else
		offset = -1;

	if (autopull && (*param->node != 0)) {

		bool no_reply = false;
		Py_BEGIN_ALLOW_THREADS;
		for (size_t i = 0; i < (retries > 0 ? retries : 1); i++) {
			int param_pull_res;
			param_pull_res = pycsh_param_pull_single(param, offset, CSP_PRIO_NORM, 1, (host != INT_MIN ? host : *param->node), timeout, paramver);
			if (param_pull_res && i >= retries-1) {
				no_reply = true;
				break;
			}
		}	
		Py_END_ALLOW_THREADS;

		if (no_reply) {
			PyErr_Format(PyExc_ConnectionError, "No response from node %d", *param->node);
			return NULL;
		}
	}

	if (verbose > -1) {
		param_print(param, -1, NULL, 0, 0, 0);
	}

	switch (param->type) {
		case PARAM_TYPE_UINT8:
		case PARAM_TYPE_XINT8: {
			uint8_t val = (offset != -1) ? param_get_uint8_array(param, offset) : param_get_uint8(param);
			if (PyErr_Occurred()) {  // Error may occur during Parameter_getter()
				return NULL;
			}
			return Py_BuildValue("B", val);
		}
		case PARAM_TYPE_UINT16:
		case PARAM_TYPE_XINT16: {
			uint16_t val = (offset != -1) ? param_get_uint16_array(param, offset) :  param_get_uint16(param);
			if (PyErr_Occurred()) {  // Error may occur during Parameter_getter()
				return NULL;
			}
			return Py_BuildValue("H", val);
		}
		case PARAM_TYPE_UINT32:
		case PARAM_TYPE_XINT32: {
			uint32_t val = (offset != -1) ? param_get_uint32_array(param, offset) :  param_get_uint32(param);
			if (PyErr_Occurred()) {  // Error may occur during Parameter_getter()
				return NULL;
			}
			return Py_BuildValue("I", val);
		}
		case PARAM_TYPE_UINT64:
		case PARAM_TYPE_XINT64: {
			uint64_t val = (offset != -1) ? param_get_uint64_array(param, offset) :  param_get_uint64(param);
			if (PyErr_Occurred()) {  // Error may occur during Parameter_getter()
				return NULL;
			}
			return Py_BuildValue("K", val);
		}
		case PARAM_TYPE_INT8: {
			int8_t val = (offset != -1) ? param_get_int8_array(param, offset) : param_get_int8(param);
			if (PyErr_Occurred()) {  // Error may occur during Parameter_getter()
				return NULL;
			}
			return Py_BuildValue("b", val);	
		}
		case PARAM_TYPE_INT16: {
			int16_t val = (offset != -1) ? param_get_int16_array(param, offset) :  param_get_int16(param);
			if (PyErr_Occurred()) {  // Error may occur during Parameter_getter()
				return NULL;
			}
			return Py_BuildValue("h", val);	
		}
		case PARAM_TYPE_INT32: {
			int32_t val = (offset != -1) ? param_get_int32_array(param, offset) :  param_get_int32(param);
			if (PyErr_Occurred()) {  // Error may occur during Parameter_getter()
				return NULL;
			}
			return Py_BuildValue("i", val);	
		}
		case PARAM_TYPE_INT64: {
			int64_t val = (offset != -1) ? param_get_int64_array(param, offset) :  param_get_int64(param);
			if (PyErr_Occurred()) {  // Error may occur during Parameter_getter()
				return NULL;
			}
			return Py_BuildValue("k", val);	
		}
		case PARAM_TYPE_FLOAT: {
			float val = (offset != -1) ? param_get_float_array(param, offset) : param_get_float(param);
			if (PyErr_Occurred()) {  // Error may occur during Parameter_getter()
				return NULL;
			}
			return Py_BuildValue("f", val);	
		}
		case PARAM_TYPE_DOUBLE: {
			double val = (offset != -1) ? param_get_double_array(param, offset) : param_get_double(param);
			if (PyErr_Occurred()) {  // Error may occur during Parameter_getter()
				return NULL;
			}
			return Py_BuildValue("d", val);	
		}
		case PARAM_TYPE_STRING: {
			char buf[param->array_size];
			param_get_string(param, &buf, param->array_size);
			int offset_str = offset_orig;
			if (offset_str != INT_MIN) {
				if (_pycsh_util_index(strnlen(buf, param->array_size), &offset_str)) {  // Validate the offset.
					return NULL;  // Raises IndexError.
				}
			} else {
				offset_str = -1;
			}

			if (buf[offset_str] == '\0') {  /* TODO Kevin: Consider whether we should use `offset >= strnlen(buf, param->array_size)` here instead. */
				/* I don't much like raising an error here, but it's how we handle it in `_pycsh_util_get_array_indexes()`. */
				PyErr_Format(PyExc_IndexError, "IndexError: Index (%d) out of range of parameter ('%s@%d') value", offset_str, param->name, *param->node);
				return NULL;
			}
			if (offset_str != -1) {
				char charstrbuf[] = {buf[offset_str]};
				return Py_BuildValue("s", charstrbuf);
			}
			return Py_BuildValue("s", buf);
		}
		case PARAM_TYPE_DATA: {
			// TODO Kevin: No idea if this has any chance of working.
			//	I hope it will raise a reasonable exception if it doesn't,
			//	instead of just segfaulting :P
			unsigned int size = (param->array_size > 1) ? param->array_size : 1;
			char buf[size];
			param_get_data(param, buf, size);
			return Py_BuildValue("O&", buf);
		}
		default: {
			/* Default case to make the compiler happy. Set error and return */
			break;
		}
	}
	PyErr_SetString(PyExc_NotImplementedError, "Unsupported parameter type for get operation.");
	return NULL;
}

/* TODO Kevin: Consider how we want the internal API for pulling queues to be.
	We can't use `param_queue_t*` itself, because we need a way to find the `param_t*` for list-less parameters. */
#if 0
static int pycsh_param_pull_queue(param_queue_t *queue, uint8_t prio, int verbose, int host, int timeout) {

	if ((queue == NULL) || (queue->used == 0))
		return 0;

	csp_packet_t * packet = csp_buffer_get(PARAM_SERVER_MTU);
	if (packet == NULL)
		return -2;

	if (queue->version == 2) {
		packet->data[0] = PARAM_PULL_REQUEST_V2;
	} else {
		packet->data[0] = PARAM_PULL_REQUEST;
	}

	packet->data[1] = 0;

	memcpy(&packet->data[2], queue->buffer, queue->used);

	packet->length = queue->used + 2;
	packet->id.pri = prio;
	return param_transaction(packet, host, timeout, pycsh_param_transaction_callback_pull, verbose, queue->version, NULL);

}
#endif

static PyObject * _slice_to_range(PyObject * slice, int seqlen, int *whole_range) {

	assert(slice);
	if (!PySlice_Check(slice)) {
		PyErr_Format(PyExc_TypeError, "`slice` argument must be slice, not %s", slice->ob_type->tp_name);
		return NULL;
	}

	Py_ssize_t start, stop, step, slicelength;
	if (PySlice_GetIndicesEx(slice, seqlen, &start, &stop, &step, &slicelength) < 0) {
		// Error handling (e.g., seq_length is negative or invalid slice)
		return NULL;
	}

	if (whole_range) {
		if (start == 0 && step == 1 && slicelength == seqlen) {
			/* TODO Kevin: Pretty annoying that we may waste the returned `range()` object here,
				but oh well. */
			*whole_range = 1;
		}
	}

	return PyObject_CallFunction((PyObject *)&PyRange_Type, "nnn", start, stop, step);
}

static PyObject * _safePyObject_GetIter(PyObject *obj, bool decref) {

	if (!obj) {
		return NULL;
	}

	PyObject * _obj_decref AUTO_DECREF = NULL;
	if (decref) {
		_obj_decref = obj;
	}

	return PyObject_GetIter(obj);
}


/**
 * 
 * @param whole_range_out Will be set to 1 if indexes cover the entirety of seqlen, otherwise left untouched.
 */
static PyObject * _indices_to_iterator(PyObject *indexes, int * whole_range_out, int seqlen) {
	/* Used for reference counting.
		'indexes' is always a borrowed reference.
		So we create this other variable for when we're creating a new reference,
		so we can always `Py_DecRef()` it */
    PyObject * _newref_slice AUTO_DECREF = NULL;  
    if (!indexes || indexes == Py_None) {
        /* Default to setting the entire parameter. */
		*whole_range_out = 1;
        indexes = _newref_slice = PySlice_New(NULL, NULL, NULL);
        if (!indexes) {
            return NULL;
        }
    }

	/* Make slices iterable by converting to iterables. */
	if (PySlice_Check(indexes)) {

		/* TODO Kevin: Check handling of PARAM_TYPE_DATA */
		return _safePyObject_GetIter(_slice_to_range(indexes, seqlen, whole_range_out), true);
	}

	if (PyLong_Check(indexes)) {
		return _safePyObject_GetIter(PyTuple_Pack(1, indexes), true);
	}

	/* Try converting raw object to iterator. */
	return _safePyObject_GetIter(indexes, false);

	//PyErr_Format(PyExc_TypeError, "indices must be integers, slices, Iterable[int] or None, not `%s`", indexes->ob_type->tp_name);
	//return NULL;
}

static PyObject * _pycsh_get_str_value(PyObject * obj) {

	// This 'if' exists for cases where the value
	// of a parameter is assigned from that of another.
	// i.e: 				param1.value = param2
	// Which is equal to:	param1.value = param2.value
	if (PyObject_TypeCheck(obj, &ParameterType)) {
		// Return the value of the Parameter.

		ParameterObject *paramobj = ((ParameterObject *)obj);

		param_t * param = paramobj->param;
		int host = paramobj->host;
		int timeout = paramobj->timeout;
		int retries = paramobj->retries;
		int paramver = paramobj->paramver;

		PyObject * value AUTO_DECREF = param->array_size > 0 ?
			_pycsh_util_get_array(param, 0, host, timeout, retries, paramver, -1) :
			_pycsh_util_get_single(param, INT_MIN, 0, host, timeout, retries, paramver, -1);

		PyObject * strvalue = PyObject_Str(value);
		return strvalue;
	}
	else  // Otherwise use __str__.
		return PyObject_Str(obj);
}

/**
 * @brief Join an iterable[str] into a flat string
 * 
 * `('s', 't', 'r', 'i', 'n', 'g')` -> `"string"`
 */
static PyObject * _iter_to_str(PyObject * iterable) {
	PyObject * const empty_string AUTO_DECREF = PyUnicode_FromString("");  /* Needed to ''.join(value_tuple) */
	if (!empty_string) {
		return NULL;
	}
	return PyUnicode_Join(empty_string, iterable);
}

/* Private interface for getting the value of an array parameter
   Increases the reference count of the returned tuple before returning.  */
PyObject * _pycsh_util_get_array(param_t *param, int autopull, int host, int timeout, int retries, int paramver, int verbose) {

	// Pull the value for every index using a queue (if we're allowed to),
	// instead of pulling them individually.
	if (autopull && *param->node != 0) {
		#if 0  /* Pull array parameters with index -1, like CSH. We may change this in the future */
		uint8_t queuebuffer[PARAM_SERVER_MTU] = {0};
		param_queue_t queue = { };
		param_queue_init(&queue, queuebuffer, PARAM_SERVER_MTU, 0, PARAM_QUEUE_TYPE_GET, paramver);

		for (int i = 0; i < param->array_size; i++) {
			param_queue_add(&queue, param, i, NULL);
		}
		#endif

		bool no_reply = false;
		Py_BEGIN_ALLOW_THREADS;
		for (size_t i = 0; i < (retries > 0 ? retries : 1); i++) {
			if (pycsh_param_pull_single(param, -1, CSP_PRIO_NORM, 0, *param->node, timeout, paramver)) {
				no_reply = true;
				break;
			}
		}
		Py_END_ALLOW_THREADS;

		if (no_reply) {
			PyErr_Format(PyExc_ConnectionError, "No response from node %d", *param->node);
			return 0;
		}
	}

	/* TODO Kevin: How to PARAM_TYPE_DATA? */
	/* PARAM_TYPE_STRING only gets up to first `\0` */
	if (param->type == PARAM_TYPE_STRING) {
		char buf[param->array_size];
		param_get_string(param, &buf, param->array_size);
		return PyUnicode_FromStringAndSize(buf, strnlen(buf, param->array_size));
	}

	// We will populate this tuple with the values from the indexes.
	PyObject * value_tuple AUTO_DECREF = PyTuple_New(param->array_size);

	for (int i = 0; i < param->array_size; i++) {
		PyObject * item = _pycsh_util_get_single(param, i, 0, host, timeout, retries, paramver, -1);

		if (item == NULL) {  // Something went wrong, probably a ConnectionError. Let's abandon ship.
			return NULL;
		}
		
		PyTuple_SET_ITEM(value_tuple, i, item);
	}

	if (verbose > -1) {
		param_print(param, -1, NULL, 0, 2, 0);
	}

	/* TODO Kevin: Not sure if we can str.join() on PARAM_TYPE_DATA. */
	/* NOTE: By putting this here, we only ever return PARAM_TYPE_STRING as `"string"`,
		never `('s', 't', 'r', 'i', 'n', 'g')`. but this is probably only good. */
	if (param->type == PARAM_TYPE_STRING) {
		return _iter_to_str(value_tuple);
	}
	
	return Py_NewRef(value_tuple);
}


static void _pyval_to_param_valuebuf(char valuebuf[128] /*__attribute__((aligned(16)))*/, PyObject* value, param_type_e type) {
    PyObject * strvalue AUTO_DECREF = _pycsh_get_str_value(value);
    switch (type) {
        case PARAM_TYPE_XINT8:
        case PARAM_TYPE_XINT16:
        case PARAM_TYPE_XINT32:
        case PARAM_TYPE_XINT64:
            // If the destination parameter is expecting a hexadecimal value
            // and the Python object value is of Long type (int), then we need
            // to do a conversion here. Otherwise if the Python value is a string
            // type, then we must expect hexadecimal digits only (including 0x)
            if (PyLong_Check(value)) {
                // Convert the integer value to hexadecimal digits
                char tmp[64];
                snprintf(tmp,64,"0x%lX", PyLong_AsUnsignedLong(value));
                // Convert the hexadecimal C-string into a Python string object
                PyObject *py_long_str = PyUnicode_FromString(tmp);
                // De-reference the original strvalue before assigning a new
                Py_DECREF(strvalue);
                strvalue = py_long_str;
            }
            break;

        default:
            break;
    }
    param_str_to_value(type, (char*)PyUnicode_AsUTF8(strvalue), valuebuf);
}


/**
 * @returns <0 on exception set, otherwise >0 index.
 */
static int obj_to_index_in_range(PyObject *index, int seqlen) {
    if (!PyLong_Check(index)) {
        PyErr_Format(PyExc_TypeError, "indexes in `indexes` must be integers. Not '%s'", index->ob_type->tp_name);
        return -3;
    }

    /* TODO Kevin: Overflow check */
    assert(!PyErr_Occurred());  /* Assert that we don't override an existing exception. */
    int offset = PyLong_AsLong(index);
    assert(!PyErr_Occurred());  /* And assert that we don't create a new one. */

    /* Handle negative indexes */
    if (_pycsh_util_index(seqlen, &offset)) {  // Validate the offset.
        return -4;  // Raises IndexError.
    }

    assert(offset >= 0);
    return offset;
}


/* Only pulls from remote, does not construct value.
    Pull multiple specific indexes from a single parameter. */
static int _pycsh_param_pull_single_indexes(param_t *param, PyObject *indexes, int autopull, int host, int timeout, int retries, int paramver, int verbose) {

	if (param->type == PARAM_TYPE_STRING || param->type == PARAM_TYPE_DATA) {
		/* Always pull entirety of STRING and DATA parameters */
		PyObject *pull_res AUTO_DECREF = _pycsh_util_get_array(param, autopull, host, timeout, retries, paramver, verbose);
		return (pull_res == NULL) ? -1 : 0;
	}

	int whole_range = 0;
	PyObject * const index_iter = _indices_to_iterator(indexes, &whole_range, param->array_size);

	if (whole_range) {
		PyErr_Clear();  /* we don't care if we couldn't crate the `range()` objects here. */
		/* Return whole array, which may be more efficient. */
		/* TODO Kevin: We may consider removing `_pycsh_util_get_array()` in the future. */
		PyObject * const pull_res AUTO_DECREF = _pycsh_util_get_array(param, autopull, host, timeout, retries, paramver, verbose);
		return (pull_res == NULL) ? -1 : 0;
	}

	if (!index_iter) {
		return -9;
	}

	PyObject *index;

	uint8_t queuebuffer[PARAM_SERVER_MTU] = {0};
	param_queue_t queue = { };
	param_queue_init(&queue, queuebuffer, PARAM_SERVER_MTU, 0, PARAM_QUEUE_TYPE_GET, paramver);

	while ((index = PyIter_Next(index_iter)) != NULL) {
        PyObject * _index AUTO_DECREF = index;

        int offset = obj_to_index_in_range(index, param->array_size);
        if (offset < 0) {
            assert(PyErr_Occurred());
            return offset;  /* Error code */
        }

		if (param_queue_add(&queue, param, offset, NULL) < 0) {
			PyErr_SetString(PyExc_MemoryError, "Queue full");
			return -5;
		}
	}

    bool no_reply = false;
    Py_BEGIN_ALLOW_THREADS;
    for (size_t i = 0; i < (retries > 0 ? retries : 1); i++) {
        if (pycsh_param_pull_queue(&queue, param, 1, CSP_PRIO_NORM, verbose, host, timeout, paramver)) {
            no_reply = true;
            break;
        }
    }
    Py_END_ALLOW_THREADS;

    if (no_reply) {
        PyErr_Format(PyExc_ConnectionError, "No response from node %d", *param->node);
        return -6;
    }

    return 0;
}


#if 0
/* TODO Kevin: Is it worth making a callback iterator for `_pycsh_param_pull_single_indexes()` and `_pycsh_util_get_array_indexes()` ? */
/**
 * @brief Call the callback function with every element in the provided iterable
 *
 * @returns 0< on failure, otherwise returns number of iterations.
 */
typedef void (*pycsh_iter_foreach_f)(PyObject *element, void * context);
static int _pycsh_iter_foreach(PyObject * iterable, pycsh_iter_foreach_f callback, void * context) {

    PyObject *iter AUTO_DECREF = PyObject_GetIter(iterable);
	PyObject *index;

    size_t num_indexes = 0;
	while ((index = PyIter_Next(iter)) != NULL) {
        PyObject * _item AUTO_DECREF = index;  /* `Py_DecRef()` each element */

        if (!PyLong_Check(index)) {
			PyErr_Format(PyExc_TypeError, "indexes in `indexes` must be integers. Not '%s'", index->ob_type->tp_name);
			return NULL;
		}

        /* TODO Kevin: Overflow check */
        assert(!PyErr_Occurred());  /* Assert that we don't override an existing exception. */
        int offset = PyLong_AsLong(index);
        assert(!PyErr_Occurred());  /* And assert that we don't create a new one. */

        /* Handle negative indexes */
        if (_pycsh_util_index(param->array_size, &offset)) {  // Validate the offset.
			return NULL;  // Raises IndexError.
		}

		PyObject * value = _pycsh_util_get_single(param, offset, 0, host, timeout, retries, paramver, verbose);

		if (value == NULL) {  // Something went wrong, probably a ConnectionError. Let's abandon ship.
			return NULL;
		}

		PyTuple_SET_ITEM(value_tuple, offset, value);
        num_indexes++;
	}

}
#endif

#if 0
static PyObject* slice_c_string(const char* c_str, Py_ssize_t c_len, PyObject* slice) {

	/* Clamp to actual length to avoid `PyUnicode_FromStringAndSize()` padding with 0x00 bytes. */
    PyObject* py_str AUTO_DECREF = PyUnicode_FromStringAndSize(c_str, strnlen(c_str, c_len));
	if (!py_str) {
		return NULL;
	}

	return PyObject_GetItem(py_str, slice);
}
#endif


static PyObject * _index_zip(PyObject * value_array, PyObject *iter, int verbose, param_t * param) {

	Py_ssize_t value_len = PyObject_Length(value_array);
	if (value_len < 0) {
		return NULL;
	}

	/* We will populate this tuple with specified slice of values from the indexes.
        It cannot possibly be larger than the parameter itself. So we shrink it from that. */
	//PyObject * value_tuple AUTO_DECREF = PyTuple_New(param->array_size);
	PyObject * value_tuple AUTO_DECREF = PyTuple_New(value_len);
    if (!value_tuple) {
        return PyErr_NoMemory();
    }

	PyObject *index;
    size_t num_indexes = 0;
	while ((index = PyIter_Next(iter)) != NULL) {
        PyObject * _index AUTO_DECREF = index;  /* `Py_DecRef()` each element */

        int offset = obj_to_index_in_range(index, value_len);
        if (offset < 0) {
            assert(PyErr_Occurred());
            return NULL;
        }

		// if (param->type == PARAM_TYPE_STRING && ((char)param_get_uint8_array(param, offset)) == '\0') {
		// 	// PyErr_Format(PyExc_IndexError, "Index %d out of bounds of returned string %s", offset);
		// 	// return NULL;
		// 	break;
		// }

		PyObject * value = PyObject_GetItem(value_array, index);
		if (!value) {
			return NULL;
		}

		/* NOTE: It is tempting to `PyTuple_SET_ITEM(..., offset, ...)` here,
			but that only works with whole arrays anyway. */
		PyTuple_SET_ITEM(value_tuple, num_indexes, value);
        num_indexes++;
	}

    /* Shrink tuple to actual number of indexes. */
    if (_PyTuple_Resize(&value_tuple, num_indexes) < 0) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to resize tuple for ident replies");
        return NULL;
    }

	/* When verbose, print only once,
		whether or not we pull multiple indices,
		or whether remote or not. */
	/* NOTE: Not very nice that we assume what we should print here. */
	if (verbose > -1) {
		param_print(param, -1, NULL, 0, 2, 0);
	}

	/* TODO Kevin: Not sure if we can str.join() on PARAM_TYPE_DATA. */
	if (param->type == PARAM_TYPE_STRING) {
		return _iter_to_str(value_tuple);
	}

	return Py_NewRef(value_tuple);
}


/* Similar to `_pycsh_util_get_array()`, but accepts a `PyObject * indexes`,
	which will be iterated to map out specific indexes to retrieve/return. */
PyObject * _pycsh_util_get_array_indexes(param_t *param, PyObject * indicies_raw, int autopull, int host, int timeout, int retries, int paramver, int verbose) {

	assert(param);
	if (param->type == PARAM_TYPE_DATA) {
		/* No slicing support for `PARAM_TYPE_DATA`.
			just pull the whole parameter.
			We may consider returning slices in the future,
			even if we still have to pull all of it. */
		return _pycsh_util_get_array(param, autopull, host, timeout, retries, paramver, verbose);
	}

	int whole_range = 0;
	PyObject * const index_iter = _indices_to_iterator(indicies_raw, &whole_range, param->array_size);
	if (whole_range) {
		PyErr_Clear();  /* we don't care if we couldn't crate the `range()` objects here. */
		/* Return whole array, which may be more efficient. */
		/* TODO Kevin: We may consider removing `_pycsh_util_get_array()` in the future. */
		return _pycsh_util_get_array(param, autopull, host, timeout, retries, paramver, verbose);
	}

	if (!index_iter) {
		return NULL;
	}
	
	// Pull the value for every index using a queue (if we're allowed to),
	// instead of pulling them individually.
	if (autopull && *param->node != 0) {
        if (_pycsh_param_pull_single_indexes(param, indicies_raw, autopull, host, timeout, retries, paramver, -1)) {
            return NULL;
        }
	}

	PyObject * const value_array = _pycsh_util_get_array(param, false, host, timeout, retries, paramver, -1);
	if (value_array == NULL) {  // Something went wrong, probably a ConnectionError. Let's abandon ship.
		return NULL;
	}

	if (param->type == PARAM_TYPE_STRING) {  /* Exclude trailing NULL bytes from string parameter indexation. i.e "123\0\0"[-1] == 3 (not '\0') */
		Py_ssize_t str_len = PyObject_Length(value_array);
		if (str_len < 0) {
			return NULL;
		}
		PyObject * index_iter_str = _indices_to_iterator(indicies_raw, &whole_range, str_len);
		if (!index_iter_str) {
			return NULL;
		}
		return _index_zip(value_array, index_iter_str, verbose, param);
	}

	return _index_zip(value_array, index_iter, verbose, param);
}


PyObject * _pycsh_util_set_array_indexes(param_t *param, PyObject * values, PyObject * indexes, int autopush, int host, int timeout, int retries, int paramver, int verbose) {

    assert(param);
    assert(values);

    PyObject * _indexes_newref AUTO_DECREF = NULL;  /* Used for reference counting.*/
    if (!indexes || Py_IsNone(indexes)) {
        /* Default to setting the entire parameter. */
        indexes = _indexes_newref = PySlice_New(NULL, NULL, NULL);
		if (!indexes) {
			assert(PyErr_Occurred());
			return NULL;
		}
    }

	PyObject * const values_iter AUTO_DECREF = PyObject_GetIter(values);
    if (!values_iter) {
        assert(PyErr_Occurred());
		PyErr_Clear();
		if (_indexes_newref && param->array_size > 1)  {  /* No index specified. */
			PyObject * obj_str AUTO_DECREF = PyObject_Str(values);
			assert(obj_str);
			const char* c_str = PyUnicode_AsUTF8(obj_str);
			assert(c_str);
			/* TODO Kevin: We can't really assume we're using ValueProxy here. Could be `pycsh.set()` in the future? */
			PyErr_Format(PyExc_IndexError, "Use `[:]` to set all indices (of `%s`) from a single value (%s). i.e: `Parameter.value = 1` -> `Parameter.value[:] = 1`", param->name, c_str, c_str);
			return NULL;
		}
    }

	int whole_range = 0;
	if (PySlice_Check(indexes)) {  /* Make slices iterable by converting to iterables. */

		indexes = _indexes_newref = _slice_to_range(indexes, param->array_size, &whole_range);

		if (!indexes) {
			return NULL;
		}
	}

	if (!values_iter && whole_range) {
		/* Set whole array from 1 value without index, which should be more efficient. */
		if (_pycsh_util_set_single(param, values, INT_MIN, host, timeout, retries, paramver, autopush, verbose) < 0) {
			return NULL;
		}
		Py_RETURN_NONE;
	}

    uint8_t queuebuffer[PARAM_SERVER_MTU] = {0};
	param_queue_t queue = { };
	param_queue_init(&queue, queuebuffer, PARAM_SERVER_MTU, 0, PARAM_QUEUE_TYPE_SET, paramver);

    
    PyObject * const indexes_iter AUTO_DECREF = PyObject_GetIter(indexes);
    if (!indexes_iter) {
        assert(PyErr_Occurred());
        return NULL;
    }

	Py_ssize_t iter_cnt = 0;
    while (1) {
        assert(!PyErr_Occurred());
        PyObject * const value AUTO_DECREF = values_iter ? PyIter_Next(values_iter) : Py_NewRef(values);
        if (PyErr_Occurred()) {
            return NULL;
        }
        PyObject * const index AUTO_DECREF = PyIter_Next(indexes_iter);
        if (PyErr_Occurred()) {
            return NULL;
        }

        if (!value && !index) {
            break;
        } else if (!index) {
			if (!values_iter) {
				break;  /* non-iterable value specified, don't require matching lengths. */
			}
            PyErr_Format(PyExc_ValueError, "Received fewer indexes than values (number of indices: %ld, param->array_size: %d)", iter_cnt, param->array_size);
            return NULL;
        } else if (!value) {
            PyErr_Format(PyExc_ValueError, "Received fewer values than indexes (number of values: %ld, param->array_size: %d)", iter_cnt, param->array_size);
            return NULL;
        }
		iter_cnt++;

        int offset = obj_to_index_in_range(index, param->array_size);
        if (offset < 0) {
            assert(PyErr_Occurred());
            return NULL;
        }

        char valuebuf[128] __attribute__((aligned(16))) = { };
        _pyval_to_param_valuebuf(valuebuf, value, param->type);
        if (param_queue_add(&queue, param, offset, valuebuf) < 0) {
			PyErr_SetString(PyExc_MemoryError, "Queue full");
			return NULL;
		}
    }

	param_list_t param_list = {
		.param_arr = &param,
		.cnt = 1
	};
    if (host != 0) {
		if (pycsh_param_push_queue(&queue, 1, verbose, host, timeout, 0, &param_list) < 0) {  // TODO Kevin: We should probably have a parameter for hwid here.
			PyErr_Format(PyExc_ConnectionError, "No response from node %d", *param->node);
			return NULL;
		}
	} else {
        /* TODO Kevin: is `host` the correct from node here? */
        pycsh_param_queue_apply_listless(&queue, &param_list, host, false);
    }

    Py_RETURN_NONE;
}

/* Attempts a conversion to the specified type, by calling it. */
static PyObject * _pycsh_typeconvert(PyObject * strvalue, PyTypeObject * type, int check_only) {
	// TODO Kevin: Using this to check the types of object is likely against
	// PEP 20 -- The Zen of Python: "Explicit is better than implicit"

	PyObject * valuetuple AUTO_DECREF = PyTuple_Pack(1, strvalue);
	PyObject * converted_value = PyObject_CallObject((PyObject *)type, valuetuple);
	if (converted_value == NULL) {
		return NULL;  // We assume failed conversions to have set an exception string.
	}
	if (check_only) {
		Py_DECREF(converted_value);
		Py_RETURN_NONE;
	}
	return converted_value;
}

/* Iterates over the specified iterable, and checks the type of each object. */
static int _pycsh_typecheck_sequence(PyObject * sequence, PyTypeObject * type) {
	// This is likely not thread-safe however.

	// It seems that tuples pass PySequence_Check() but not PyIter_Check(),
	// which seems to indicate that not all sequences are inherently iterable.
	if (!PyIter_Check(sequence) && !PySequence_Check(sequence)) {
		PyErr_SetString(PyExc_TypeError, "Provided value is not a iterable");
		return -1;
	}

	PyObject *iter = PyObject_GetIter(sequence);

	PyObject *item;

	while ((item = PyIter_Next(iter)) != NULL) {
        PyObject * _item AUTO_DECREF = item;

		if (!_pycsh_typeconvert(item, type, 1)) {
#if 0  // Should we raise the exception from the failed conversion, or our own?
			PyObject * temppystr = PyObject_Str(item);
			char* tempstr = (char*)PyUnicode_AsUTF8(temppystr);
			char buf[70 + strlen(item->ob_type->tp_name) + strlen(tempstr)];
			sprintf(buf, "Iterable contains object of an incorrect/unconvertible type <%s: %s>.", item->ob_type->tp_name, tempstr);
			PyErr_SetString(PyExc_TypeError, buf);
			Py_DECREF(temppystr);
#endif
			return -2;
		}
	}

	// Raise an exception if we got an error while iterating.
	return PyErr_Occurred() ? -3 : 0;
}

/* Private interface for setting the value of a normal parameter. 
   Use INT_MIN as no offset. */
int _pycsh_util_set_single(param_t *param, PyObject *value, int offset, int host, int timeout, int retries, int paramver, int remote, int verbose) {
	
	if (offset == INT_MIN) {
        offset = -1;
    } else {
        if (param->type == PARAM_TYPE_STRING) {
			PyErr_SetString(PyExc_NotImplementedError, "Cannot set string parameters by index.");
			return -1;
		}

		if (_pycsh_util_index(param->array_size, &offset)) {  // Validate the offset.
			return -1;  // Raises IndexError.
        }
    }

	char valuebuf[128] __attribute__((aligned(16))) = { };
 	_pyval_to_param_valuebuf(valuebuf, value, param->type);

	int dest = (host != INT_MIN ? host : *param->node);

	// TODO Kevin: The way we set the parameters has been refactored,
	//	confirm that it still behaves like the original (especially for remote host parameters).
	if (remote && (dest != 0)) {  // When allowed, set remote parameter immediately.

		for (size_t i = 0; i < (retries > 0 ? retries : 1); i++) {
			int param_push_res;
			Py_BEGIN_ALLOW_THREADS;  // Only allow threads for remote parameters, as local ones could have Python callbacks.
			param_push_res = pycsh_param_push_single(param, offset, 0, valuebuf, 1, dest, timeout, paramver, true);
			Py_END_ALLOW_THREADS;
			if (param_push_res < 0) {
				if (i >= retries-1) {
					PyErr_Format(PyExc_ConnectionError, "No response from node %d", dest);
					return -2;
				}
			}
		}

		if (verbose > -1) {
			param_print(param, offset, NULL, 0, 2, 0);
		}

	} else {  // Otherwise; set local cached value.

		/* `param_set()` seems to have issues with PARAM_TYPE_STRING.
			i.e: `set test_str hello` `set test_str hi` == `hillo` */
		param_list_t param_list = {
			.param_arr = &param,
			.cnt = 1
		};
		uint8_t queuebuffer[PARAM_SERVER_MTU] = {0};
		param_queue_t queue;
		param_queue_init(&queue, queuebuffer, PARAM_SERVER_MTU - 2, 0, PARAM_QUEUE_TYPE_SET, paramver);
		if (param_queue_add(&queue, param, offset, valuebuf) < 0) {
			PyErr_SetString(PyExc_MemoryError, "Queue full");
			return -4;
		}
		pycsh_param_queue_apply_listless(&queue, &param_list, host, false);

		if (PyErr_Occurred()) {
			/* If the exception came from the callback, we should already have chained unto it. */
			// TODO Kevin: We could create a CallbackException class here, to be caught by us and in Python.
			return -3;
		}
	}

	return 0;
}

/* Private interface for setting the value of an array parameter. */
int _pycsh_util_set_array(param_t *param, PyObject *value, int host, int timeout, int retries, int paramver, int verbose) {

	PyObject * _value AUTO_DECREF = value;

	// Transform lazy generators and iterators into sequences,
	// such that their length may be retrieved in a uniform manner.
	// This comes at the expense of memory (and likely performance),
	// especially for very large sequences.
	if (!PySequence_Check(value)) {
		if (PyIter_Check(value)) {
			PyObject * temptuple AUTO_DECREF = PyTuple_Pack(1, value);
			value = PyObject_CallObject((PyObject *)&PyTuple_Type, temptuple);
		} else {
			PyErr_SetString(PyExc_TypeError, "Provided argument must be iterable.");
			return -1;
		}
	} else {
		Py_INCREF(value);  // Iterators will be 1 higher than needed so do the same for sequences.
	}

	Py_ssize_t seqlen = PySequence_Size(value);

	// We don't support assigning slices (or anything of the like) yet, so...
	if (seqlen != param->array_size) {
		if (param->array_size > 1) {  // Check that the lengths match.
			PyErr_Format(PyExc_ValueError, "Provided iterable's length does not match parameter's. <iterable length: %li> <param length: %i>", seqlen, param->array_size);
		} else {  // Check that the parameter is an array.
			PyErr_SetString(PyExc_TypeError, "Cannot assign iterable to non-array type parameter.");
		}
		return -2;
	}

	// Check that the iterable only contains valid types.
	if (_pycsh_typecheck_sequence(value, _pycsh_misc_param_t_type(param))) {
		return -3;  // Raises TypeError.
	}

	// TODO Kevin: This does not allow for queued operations on array parameters.
	//	This could be implemented by simply replacing 'param_queue_t queue = { };',
	//	with the global queue, but then we need to handle freeing the buffer.
	// TODO Kevin: Also this queue is not used for local parameters (and therefore wasted).
	//	Perhaps structure the function to avoid its unecessary instantiation.
	uint8_t queuebuffer[PARAM_SERVER_MTU] = {0};
	param_queue_t queue = { };
	param_queue_init(&queue, queuebuffer, PARAM_SERVER_MTU, 0, PARAM_QUEUE_TYPE_SET, paramver);
	
	for (int i = 0; i < seqlen; i++) {

		PyObject *item AUTO_DECREF = PySequence_GetItem(value, i);

		if(!item) {
			PyErr_SetString(PyExc_RuntimeError, "Iterator went outside the bounds of the iterable.");
			return -4;
		}

#if 0  /* TODO Kevin: When should we use queues with the new cmd system? */
		// Set local parameters immediately, use the global queue if autosend if off.
		param_queue_t *usequeue = (!autosend ? &param_queue_set : ((*param->node != 0) ? &queue : NULL));
#endif
		assert(!PyErr_Occurred());
		_pycsh_util_set_single(param, item, i, host, timeout, retries, paramver, 1, verbose);
		if (PyErr_Occurred()) {
			return -7;
		}
		
		// 'item' is a borrowed reference, so we don't need to decrement it.
	}

	param_queue_print(&queue);
	
	if (host != 0) {
		if (param_push_queue(&queue, 1, 0, host, 100, 0, false) < 0) {  // TODO Kevin: We should probably have a parameter for hwid here.
			PyErr_Format(PyExc_ConnectionError, "No response from node %d", *param->node);
			return -6;
		}
	}
	
	return 0;
}

int pycsh_parse_param_mask(PyObject * mask_in, uint32_t * mask_out) {

	assert(mask_in != NULL);

	if (PyUnicode_Check(mask_in)) {
		const char * include_mask_str = PyUnicode_AsUTF8(mask_in);
		*mask_out = param_maskstr_to_mask(include_mask_str);
	} else if (PyLong_Check(mask_in)) {
		*mask_out = PyLong_AsUnsignedLong(mask_in);
	} else {
		PyErr_SetString(PyExc_TypeError, "parameter mask must be either str or int");
		return -1;
	}

	return 0;
}
