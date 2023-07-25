/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

// #include <assert.h>
// #include <stdio.h>
// #include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <math.h>

#include "boolobject.h"
#include "longobject.h"
#include "object.h"
#include "osdep/io.h"

#include "mpv_talloc.h"

#include "common/common.h"
#include "common/global.h"
#include "options/m_property.h"
#include "common/msg.h"
#include "common/msg_control.h"
#include "common/stats.h"
#include "options/m_option.h"
#include "input/input.h"
#include "options/path.h"
#include "misc/bstr.h"
#include "misc/json.h"
#include "osdep/subprocess.h"
#include "osdep/timer.h"
#include "osdep/threads.h"
#include "pyerrors.h"
#include "stream/stream.h"
#include "sub/osd.h"
#include "core.h"
#include "command.h"
#include "client.h"
#include "libmpv/client.h"
#include "ta/ta_talloc.h"

// List of builtin modules and their contents as strings.
// All these are generated from player/python/*.py
static const char *const builtin_files[][2] = {
    {"@/defaults.py",
#   include "generated/player/python/defaults.py.inc"
    },
    {"@/mpv_main_event_loop.py",
#   include "generated/player/python/mpv_main_event_loop.py.inc"
    },
    {0}
};


// Represents a loaded script. Each has its own Python state.
typedef struct {
    PyObject_HEAD

    char                **scripts;
    size_t              script_count;
    struct mpv_handle   *client;
    struct MPContext    *mpctx;
    struct mp_log       *log;
    void                *ta_ctx;
    struct stats_ctx    *stats;
} PyScriptCtx;


static PyTypeObject PyScriptCtx_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "script_ctx",
    .tp_basicsize = sizeof(PyScriptCtx),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "script_ctx object",
};


// prototypes
static void makenode(void *ta_ctx, PyObject *obj, struct mpv_node *node);
static PyObject *deconstructnode(struct mpv_node *node);
static PyObject *check_error(int res);

/*
* Separation of concern
* =====================
* * Get a list of all python scripts.
* * Initialize python in it's own thread, as a single client. (call Py_Initialize)
* * Run scripts in sub interpreters. (This where the scripts are isolated as virtual clients)
* * Run a event loop on the the mainThread created from Py_Initialize.
* * Delegate event actions to the sub interpreters.
* * Destroy all sub interpreters on MPV_EVENT_SHUTDOWN
* * Shutdown python. (call Py_Finalize)
*/
// module and type def
/* ========================================================================== */

PyThreadState *mainThread;
PyObject *PyInit_mpv(void);
PyObject *PyInit_mpvmainloop(void);

static PyObject *MpvError;

typedef struct {
    PyObject_HEAD
    char                **scripts;
    size_t              script_count;
    struct mpv_handle   *client;
    struct MPContext    *mpctx;
    struct mp_log       *log;
    struct stats_ctx    *stats;
    void                *ta_ctx;
    PyObject            *pympv_attr;
    PyObject            *pyclient;
    PyThreadState       *threadState;
} PyMpvObject;

static PyTypeObject PyMpv_Type;

#define PyCtxObject_Check(v)      Py_IS_TYPE(v, &PyMpv_Type)

PyMpvObject **clients;

static void
PyMpv_dealloc(PyMpvObject *self)
{
    Py_XDECREF(self->pympv_attr);
    PyObject_Free(self);
}


static PyObject *
setup(PyObject *self, PyObject *args)
{
    return Py_NewRef(Py_NotImplemented);
}


static PyMethodDef PyMpv_methods[] = {
    {"setup", (PyCFunction)setup, METH_VARARGS,
     PyDoc_STR("Just a test method to see if extending is working.")},
    {NULL, NULL, 0, NULL}                                                 /* Sentinal */
};


static PyObject *
PyMpv_getattro(PyMpvObject *self, PyObject *name)
{
    if (self->pympv_attr != NULL) {
        PyObject *v = PyDict_GetItemWithError(self->pympv_attr, name);
        if (v != NULL) {
            return Py_NewRef(v);
        }
        else if (PyErr_Occurred()) {
            return NULL;
        }
    }
    return PyObject_GenericGetAttr((PyObject *)self, name);
}


static int
PyMpv_setattr(PyMpvObject *self, const char *name, PyObject *v)
{
    if (self->pympv_attr == NULL) {
        self->pympv_attr = PyDict_New();
        if (self->pympv_attr == NULL)
            return -1;
    }
    if (v == NULL) {
        int rv = PyDict_DelItemString(self->pympv_attr, name);
        if (rv < 0 && PyErr_ExceptionMatches(PyExc_KeyError))
            PyErr_SetString(PyExc_AttributeError,
                "delete non-existing PyMpv attribute");
        return rv;
    }
    else
        return PyDict_SetItemString(self->pympv_attr, name, v);
}


static PyTypeObject PyMpv_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "mpv.Mpv",
    .tp_basicsize = sizeof(PyMpvObject),
    .tp_dealloc = (destructor)PyMpv_dealloc,
    .tp_getattr = (getattrfunc)0,
    .tp_setattr = (setattrfunc)PyMpv_setattr,
    .tp_getattro = (getattrofunc)PyMpv_getattro,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = PyMpv_methods,
};


/*
* args[1]: DEFAULT_TIMEOUT
* returns: PyLongObject event_id
*/
static PyObject *
mpvmainloop_wait_event(PyObject *mpvmainloop, PyObject *args)
{
    PyScriptCtx *ctx = (PyScriptCtx *)PyObject_GetAttrString(mpvmainloop, "context");
    int timeout;
    PyArg_ParseTuple(args, "i", &timeout);
    mpv_event *event = mpv_wait_event(ctx->client, timeout);
    Py_DECREF(ctx);
    PyObject *ret = PyTuple_New(2);
    PyTuple_SetItem(ret, 0, PyLong_FromLong(event->event_id));
    if (event->event_id == MPV_EVENT_CLIENT_MESSAGE) {
        mpv_event_client_message *m = (mpv_event_client_message *)event->data;
        PyObject *data = PyTuple_New(m->num_args);
        for (int i = 0; i < m->num_args; i++) {
            PyTuple_SetItem(data, i, PyUnicode_DecodeFSDefault(m->args[i]));
        }
        PyTuple_SetItem(ret, 1, data);
        return ret;
    }
    PyTuple_SetItem(ret, 1, Py_NewRef(Py_None));
    return ret;
}


static PyObject *
mpv_extension_ok(PyObject *self, PyObject *args)
{
    return Py_NewRef(Py_True);
}


// args: log level, varargs
static PyObject *script_log(struct mp_log *log, PyObject *args)
{
    // Parse args to list_obj
    PyObject* list_obj;
    if (!PyArg_ParseTuple(args, "O", &list_obj)) {
        Py_RETURN_NONE;
    }
    if (!PyList_Check(list_obj)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a list of strings");
        Py_RETURN_NONE;
    }
    int length = PyList_Size(list_obj);

    if(length<1) {
        PyErr_SetString(PyExc_TypeError, "Insufficient Args");
        Py_RETURN_NONE;
    }

    PyObject* log_obj = PyList_GetItem(list_obj, 0);
    if (!PyUnicode_Check(log_obj)) {
        PyErr_SetString(PyExc_TypeError, "List must contain only strings");
        Py_RETURN_NONE;
    }

    int msgl = mp_msg_find_level(PyUnicode_AsUTF8(log_obj));
    if (msgl < 0) {
        PyErr_SetString(PyExc_TypeError,PyUnicode_AsUTF8(log_obj));
        Py_RETURN_NONE;
    }

    if(length>1) {
        for (Py_ssize_t i = 1; i < length; i++) {
            PyObject* str_obj = PyList_GetItem(list_obj, i);
            if (!PyUnicode_Check(str_obj)) {
                PyErr_SetString(PyExc_TypeError, "List must contain only strings");
                Py_RETURN_NONE;
            }
            mp_msg(log, msgl, (i == 2 ? "%s" : " %s"), PyUnicode_AsUTF8(str_obj));
        }
        mp_msg(log, msgl, "\n");
        Py_RETURN_NONE;
    }
    Py_RETURN_NONE;
}

static PyObject *
mainloop_log_handle(PyObject *mpvmainloop, PyObject *args)
{
    PyScriptCtx *ctx = (PyScriptCtx *)PyObject_GetAttrString(mpvmainloop, "context");
    struct mp_log *log = ctx->log;
    Py_DECREF(ctx);
    return script_log(log, args);
}

static PyMpvObject *
get_client_context(PyObject *module)
{
    PyMpvObject *cctx = (PyMpvObject *)PyObject_GetAttrString(module, "context");
    return cctx;
}

static PyObject *
handle_log(PyObject *mpv, PyObject *args)
{
    PyMpvObject *pyMpv = get_client_context(mpv);
    return script_log(pyMpv->log, args);
}

static PyObject *
command(PyObject *mpv, PyObject *args)
{
    PyMpvObject *ctx = get_client_context(mpv);
    mpv_node cmd;
    makenode(ctx->ta_ctx, PyTuple_GetItem(args, 0), &cmd);
    mpv_node *result = talloc_zero(ctx->ta_ctx, mpv_node);
    if (check_error(mpv_command_node(ctx->client, &cmd, result)) != Py_True) {
        mp_msg(ctx->log, mp_msg_find_level("error"), "failed to run node command\n");
        Py_RETURN_NONE;
    }
    return deconstructnode(result);
}

// args: string
static PyObject *
command_string(PyObject* mpv, PyObject* args)
{
    PyMpvObject *ctx = get_client_context(mpv);

    const char *s;

    if (!PyArg_ParseTuple(args, "s", &s))
        return NULL;

    int res = mpv_command_string(ctx->client, s);
    return check_error(res);
}

static PyObject *
commandv(PyObject *mpv, PyObject *args)
{
    Py_ssize_t arg_length = PyTuple_Size(args);
    const char **argv = talloc_array(NULL, const char *, arg_length + 1);
    for (Py_ssize_t i = 0; i < arg_length; i++) {
        char *carg;
        PyArg_Parse(PyTuple_GetItem(args, i), "s", &carg);
        argv[i] = talloc_strdup(argv, carg);
    }
    argv[arg_length] = NULL;
    PyMpvObject *ctx = get_client_context(mpv);
    int ret = mpv_command(ctx->client, argv);
    talloc_free(argv);
    Py_DECREF(args);
    return check_error(ret);
}

// args: string -> string
static PyObject*
find_config_file(PyObject* mpv, PyObject* args)
{
    PyMpvObject *ctx = get_client_context(mpv);

    const char *fname;

    if (!PyArg_ParseTuple(args, "s", &fname))
        return NULL;

    char *path = mp_find_config_file(ctx->ta_ctx, ctx->mpctx->global, fname);
    if (path) {
        PyObject* ret =  PyUnicode_FromString(path);
        talloc_free(path);
        return ret;
    } else {
        talloc_free(path);
        PyErr_SetString(PyExc_FileNotFoundError, "Not found");
        return NULL;
    }

    Py_RETURN_NONE;
}

// args: string, bool
static PyObject*
request_event(PyObject* mpv, PyObject* args)
{
    PyMpvObject *ctx = get_client_context(mpv);

    int event_id, enable;

    if (!PyArg_ParseTuple(args, "ii", &event_id, &enable)) {
        return NULL;
    }

    return check_error(mpv_request_event(ctx->client, event_id, enable));
}

// args: string
static PyObject*
enable_messages(PyObject* mpv, PyObject* args)
{
    PyMpvObject *ctx = get_client_context(mpv);

    const char *level;

    if (!PyArg_ParseTuple(args, "s", &level))
        return NULL;


    int res = mpv_request_log_messages(ctx->client, level);
    if (res == MPV_ERROR_INVALID_PARAMETER) {
        PyErr_SetString(PyExc_Exception, "Invalid Log Error");
        return NULL;
    }
    return check_error(res);
}


// args: name, native value
static PyObject*
set_property(PyObject* mpv, PyObject* args)
{
    PyMpvObject *ctx = get_client_context(mpv);
    mpv_node node;

    char *name;
    PyObject *property_name = PyTuple_GetItem(args, 0);
    PyArg_Parse(property_name, "s", &name);

    makenode(ctx->ta_ctx, PyTuple_GetItem(args, 1), &node);
    int res = mpv_set_property(ctx->client, name, MPV_FORMAT_NODE, &node);
    return check_error(res);
}


// args: string
static PyObject*
del_property(PyObject* mpv, PyObject* args)
{
    PyMpvObject *ctx = get_client_context(mpv);

    const char *p;

    if (!PyArg_ParseTuple(args, "s", &p))
        return NULL;


    int res = mpv_del_property(ctx->client, p);
    return check_error(res);
}


static PyObject *
get_property(PyObject* mpv, PyObject* args)
{
    PyMpvObject *ctx = get_client_context(mpv);

    const char *name;

    if (!PyArg_ParseTuple(args, "s", &name))
        return NULL;

    mpv_node *node = NULL;
    int err = mpv_get_property(ctx->client, name, MPV_FORMAT_NODE, node);
    if (err >= 0) {
        return deconstructnode(node);
    }
    return check_error(err);
}

static PyObject *
get_property_string(PyObject *mpv, PyObject *args)
{
    PyMpvObject *ctx = get_client_context(mpv);

    const char *name;

    if (!PyArg_ParseTuple(args, "s", &name))
        return NULL;

    char *prop = mpv_get_property_string(ctx->client, name);
    return PyUnicode_DecodeFSDefault(prop);
}

static PyObject *
get_property_osd(PyObject *mpv, PyObject *args)
{
    Py_RETURN_NONE;
}

static PyObject *
observe_property(PyObject *mpv, PyObject *args)
{
    PyMpvObject *ctx = get_client_context(mpv);
    const char *name;
    PyArg_ParseTuple(args, "s", &name);
    uint64_t reply_userdata = 0;
    return check_error(mpv_observe_property(
        ctx->client, reply_userdata, name, MPV_FORMAT_NODE));
}

static PyObject *
unobserve_property(PyObject *mpv, PyObject *args)
{
    PyMpvObject *ctx = get_client_context(mpv);
    uint64_t reply_userdata = 0;
    return check_error(mpv_unobserve_property(ctx->client, reply_userdata));
}

static PyObject *
mpv_input_define_section(PyObject *mpv, PyObject *args)
{
    char *name, *location, *contents, *owner;
    bool builtin;
    PyArg_ParseTuple(args, "sssps", &name, &location, &contents, &builtin, &owner);

    // why the f**k do I need this!!!
    PyArg_Parse(PyTuple_GetItem(args, 0), "s", &name);

    PyMpvObject *ctx = get_client_context(mpv);
    mp_input_define_section(ctx->mpctx->input, name, location, contents, builtin, owner);
    mp_msg(ctx->log, mp_msg_find_level("debug"), "c duh\n");
    Py_RETURN_NONE;
}


static PyObject *
mpv_input_enable_section(PyObject *mpv, PyObject *args)
{
    char *name;
    int flags;
    PyArg_ParseTuple(args, "si", &name, &flags);
    PyMpvObject *ctx = get_client_context(mpv);
    mp_input_enable_section(ctx->mpctx->input, name, flags);
    Py_RETURN_NONE;
}


static PyMethodDef Mpv_methods[] = {
    {"extension_ok", (PyCFunction)mpv_extension_ok, METH_VARARGS,             /* METH_VARARGS | METH_KEYWORDS (PyObject *self, PyObject *args, PyObject **kwargs) */
     PyDoc_STR("Just a test method to see if extending is working.")},
    {"handle_log", (PyCFunction)handle_log, METH_VARARGS,
     PyDoc_STR("handles log records emitted from python thread.")},
    {"find_config_file", (PyCFunction)find_config_file, METH_VARARGS,
     PyDoc_STR("")},
    {"request_event", (PyCFunction)request_event, METH_VARARGS,
     PyDoc_STR("")},
    {"enable_messages", (PyCFunction)enable_messages, METH_VARARGS,
     PyDoc_STR("")},
    {"set_property", (PyCFunction)set_property, METH_VARARGS,
     PyDoc_STR("")},
    {"del_property", (PyCFunction)del_property, METH_VARARGS,
     PyDoc_STR("")},
    {"get_property", (PyCFunction)get_property, METH_VARARGS,
     PyDoc_STR("")},
    {"get_property_string", (PyCFunction)get_property_string, METH_VARARGS,
     PyDoc_STR("")},
    {"observe_property", (PyCFunction)observe_property, METH_VARARGS,
     PyDoc_STR("")},
    {"unobserve_property", (PyCFunction)unobserve_property, METH_VARARGS,
     PyDoc_STR("")},
    {"mpv_input_define_section", (PyCFunction)mpv_input_define_section, METH_VARARGS,
     PyDoc_STR("")},
    {"mpv_input_enable_section", (PyCFunction)mpv_input_enable_section, METH_VARARGS,
     PyDoc_STR("")},
    {"commandv", (PyCFunction)commandv, METH_VARARGS,
     PyDoc_STR("runs mpv_command given command name and args.")},
    {"command_string", (PyCFunction)command_string, METH_VARARGS,
     PyDoc_STR("runs mpv_command_string given a string as the only argument.")},
    {"command", (PyCFunction)command, METH_VARARGS,
     PyDoc_STR("runs mpv_command_node given py structure(s, as in list) convertible to mpv_node as the only argument.")},
    {NULL, NULL, 0, NULL}                                                     /* Sentinal */
};


static int
pympv_exec(PyObject *m)
{
    if (PyType_Ready(&PyMpv_Type) < 0)
        return -1;

    if (MpvError == NULL) {
        MpvError = PyErr_NewException("mpv.error", NULL, NULL);
        if (MpvError == NULL)
            return -1;
    }
    int rc = PyModule_AddType(m, (PyTypeObject *)MpvError);
    Py_DECREF(MpvError);
    if (rc < 0)
        return -1;

    if (PyModule_AddType(m, &PyMpv_Type) < 0)
        return -1;

    return 0;
}


static struct PyModuleDef_Slot pympv_slots[] = {
    {Py_mod_exec, pympv_exec},
    {0, NULL}
};


// mpv python module
static struct PyModuleDef mpv_module_def = {
    PyModuleDef_HEAD_INIT,
    "mpv",
    NULL,
    0,
    Mpv_methods,
    pympv_slots,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC PyInit_mpv(void)
{
    return PyModuleDef_Init(&mpv_module_def);
}


static PyObject *
notify_clients(PyObject *mpvmainloop, PyObject *args)
{
    PyScriptCtx *ctx = (PyScriptCtx *)PyObject_GetAttrString(mpvmainloop, "context");
    mainThread = PyThreadState_Swap(NULL);
    for (size_t i = 0; i < ctx->script_count; i++) {
        PyThreadState_Swap(clients[i]->threadState);
        PyObject *mpv = PyObject_GetAttrString(clients[i]->pyclient, "mpv");
        PyObject *event_processor = PyObject_GetAttrString(mpv, "process_event");
        Py_INCREF(args);
        PyObject_Call(event_processor, args, NULL);
        Py_DECREF(event_processor);
        Py_DECREF(mpv);
    }
    PyThreadState_Swap(mainThread);

    Py_RETURN_NONE;
}


static PyObject *
init_clients(PyObject *mpvmainloop, PyObject *args)
{
    PyScriptCtx *ctx = (PyScriptCtx *)PyObject_GetAttrString(mpvmainloop, "context");
    mainThread = PyThreadState_Swap(NULL);
    for (size_t i = 0; i < ctx->script_count; i++) {
        PyThreadState_Swap(clients[i]->threadState);
        PyObject *mpv = PyObject_GetAttrString(clients[i]->pyclient, "mpv");
        PyObject_CallMethod(mpv, "flush", NULL);
        Py_DECREF(mpv);
    }
    PyThreadState_Swap(mainThread);
    Py_RETURN_NONE;
}


static PyMethodDef MpvMainLoop_methods[] = {
    {"wait_event", (PyCFunction)mpvmainloop_wait_event, METH_VARARGS,
     PyDoc_STR("Wrapper around the mpv_wait_event")},
    {"notify_clients", (PyCFunction)notify_clients, METH_VARARGS,
     PyDoc_STR("Notifies the clients in each thread")},
    {"init_clients", (PyCFunction)init_clients, METH_VARARGS,
     PyDoc_STR("")},
    {"handle_log", (PyCFunction)mainloop_log_handle, METH_VARARGS,
     PyDoc_STR("handles log records emitted from python thread.")},
    {NULL, NULL, 0, NULL}
};


static int
mpvmainloop_exec(PyObject *m)
{
    if (PyType_Ready(&PyScriptCtx_Type) < 0)
        return -1;

    if (PyModule_AddType(m, &PyScriptCtx_Type) < 0)
        return -1;

    return 0;
}


static struct PyModuleDef_Slot mpvmainloop_slots[] = {
    {Py_mod_exec, mpvmainloop_exec},
    {0, NULL}
};


// mpv python module
static struct PyModuleDef mpv_main_loop_module_def = {
    PyModuleDef_HEAD_INIT,
    "mpvmainloop",
    NULL,
    0,
    MpvMainLoop_methods,
    mpvmainloop_slots,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC PyInit_mpvmainloop(void)
{
    return PyModuleDef_Init(&mpv_main_loop_module_def);
}


/* ========================================================================== */

static PyObject *
load_local_pystrings(const char *string, char *module_name)
{
    PyObject *defaults = Py_CompileString(string, module_name, Py_file_input);
    if (defaults == NULL) {
        return NULL;
    }
    PyObject *defaults_mod = PyImport_ExecCodeModule(module_name, defaults);
    Py_DECREF(defaults);
    if (defaults_mod == NULL) {
        return NULL;
    }
    return defaults_mod;
}


static PyObject *
load_script(char *script_name, PyObject *defaults, char *client_name)
{
    PyObject *mpv = PyObject_GetAttrString(defaults, "mpv");

    char *string;
    char *pathname;
    PyObject *args = PyObject_CallMethod(mpv, "read_script", "s", script_name);
    PyArg_ParseTuple(args, "ss", &pathname, &string);
    Py_DECREF(args);

    PyObject *client = Py_CompileString(string, client_name, Py_file_input);
    if (client == NULL) {
        return NULL;
    }
    PyObject *client_mod = PyImport_ExecCodeModuleEx(client_name, client, pathname);
    if (client_mod == NULL) {
        return NULL;
    }
    Py_DECREF(client);
    return client_mod;
}

static int
initialize_python(PyScriptCtx *ctx)
{
    if (PyImport_AppendInittab("mpv", PyInit_mpv) == -1) {
        mp_msg(ctx->log, mp_msg_find_level("error"), "could not extend in-built modules table\n");
        return -1;
    }

    if (PyImport_AppendInittab("mpvmainloop", PyInit_mpvmainloop) == -1) {
        mp_msg(ctx->log, mp_msg_find_level("error"), "could not extend in-built modules table\n");
        return -1;
    }

    char **client_names = talloc_array(ctx->ta_ctx, char *, ctx->script_count);
    clients = talloc_array(ctx->ta_ctx, PyMpvObject *, ctx->script_count + 1);

    Py_Initialize();

    // Keep an extra dummy interpreter to repopulate the the global interpreter
    // in case we through away the last interpreter which seem to erase some
    // essential stuff from the main thread
    for (size_t i = 0; i <= ctx->script_count; i++) {
        PyMpvObject *pyMpv = PyObject_New(PyMpvObject, &PyMpv_Type);
        pyMpv->log = ctx->log;
        pyMpv->client = ctx->client;
        pyMpv->mpctx = ctx->mpctx;
        pyMpv->ta_ctx = talloc_new(ctx->ta_ctx);
        pyMpv->threadState = Py_NewInterpreter();
        clients[i] = pyMpv;
    }

    size_t discarded_client = 0;

    PyMpvObject *pyMpv;
    mainThread = PyThreadState_Swap(NULL);

    for (size_t i = 0; i < ctx->script_count; i++) {
        pyMpv = clients[i];
        PyThreadState_Swap(pyMpv->threadState);
        clients[i] = NULL;

        PyObject *filename = PyUnicode_DecodeFSDefault(ctx->scripts[i]);

        PyObject *pympv = PyImport_ImportModule("mpv");
        if (PyModule_AddObjectRef(pympv, "context", (PyObject *)pyMpv) < 0) {
            discarded_client++;
            mp_msg(ctx->log, mp_msg_find_level("error"), "cound not set up context for the module mpv.\n");
            Py_DECREF(filename);
            Py_DECREF(pympv);
            Py_EndInterpreter(pyMpv->threadState);
            talloc_free(pyMpv->ta_ctx);
            PyObject_Del(pyMpv);
            PyThreadState_Swap(NULL);
            continue;
        };

        PyModule_AddObjectRef(pympv, "filename", filename);

        PyObject *defaults = load_local_pystrings(builtin_files[0][1], "mpvclient");

        if (defaults == NULL) {
            discarded_client++;
            mp_msg(ctx->log, mp_msg_find_level("error"), "failed to load defaults (AKA. mpvclient) module.\n");
            Py_DECREF(filename);
            Py_DECREF(pympv);
            Py_DECREF(defaults);
            Py_EndInterpreter(pyMpv->threadState);
            talloc_free(pyMpv->ta_ctx);
            PyObject_Del(pyMpv);
            PyThreadState_Swap(NULL);
            continue;
        }

        PyObject *client_name = PyObject_GetAttrString(defaults, "client_name");

        PyObject *os = PyImport_ImportModule("os");
        PyObject *path = PyObject_GetAttrString(os, "path");
        Py_DECREF(os);
        if (PyObject_CallMethod(path, "exists", "s", ctx->scripts[i]) == Py_False) {
            discarded_client++;
            mp_msg(ctx->log, mp_msg_find_level("error"), "%s does not exists.\n", ctx->scripts[i]);
            Py_DECREF(filename);
            Py_DECREF(path);
            Py_DECREF(pympv);
            Py_DECREF(defaults);
            Py_DECREF(client_name);
            Py_EndInterpreter(pyMpv->threadState);
            talloc_free(pyMpv->ta_ctx);
            PyObject_Del(pyMpv);
            PyThreadState_Swap(NULL);
            continue;
        }
        Py_DECREF(path);

        char *cname;
        PyArg_Parse(client_name, "s", &cname);
        Py_DECREF(client_name);
        PyObject *client = load_script(ctx->scripts[i], defaults, cname);
        if (client == NULL) {
            discarded_client++;
            mp_msg(ctx->log, mp_msg_find_level("error"), "could not load client. discarding: %s.\n", cname);
            Py_DECREF(filename);
            Py_DECREF(pympv);
            Py_DECREF(defaults);
            Py_EndInterpreter(pyMpv->threadState);
            talloc_free(pyMpv->ta_ctx);
            PyObject_Del(pyMpv);
            PyThreadState_Swap(NULL);
            continue;
        }

        if (PyObject_HasAttrString(client, "mpv") == 0) {
            discarded_client++;
            mp_msg(ctx->log, mp_msg_find_level("error"), "illegal client. does not have an 'mpv' instance (use: from mpvclient import mpv). discarding: %s.\n", cname);
            Py_DECREF(filename);
            Py_XDECREF(filename);
            Py_DECREF(pympv);
            Py_DECREF(defaults);
            Py_DECREF(client);
            Py_EndInterpreter(pyMpv->threadState);
            talloc_free(pyMpv->ta_ctx);
            PyObject_Del(pyMpv);
            PyThreadState_Swap(NULL);
            continue;
        }

        PyObject *mpv = PyObject_GetAttrString(client, "mpv");
        PyObject_CallMethod(mpv, "flush", NULL);
        Py_DECREF(mpv);

        pyMpv->pyclient = Py_NewRef(client);

        Py_DECREF(filename);
        Py_DECREF(pympv);
        Py_DECREF(defaults);
        Py_DECREF(client);

        clients[i - discarded_client] = pyMpv;
        clients[i - discarded_client]->threadState = PyThreadState_Swap(NULL);
        client_names[i - discarded_client] = cname;
    }
    talloc_free(ctx->scripts);

    // Swap with the clean thread to repopulate the global interpreter in case
    // the last sub interpreter was deleted
    pyMpv = clients[ctx->script_count];
    PyThreadState_Swap(pyMpv->threadState);
    PyObject_Del(pyMpv);
    PyThreadState_Swap(NULL);
    clients[ctx->script_count] = NULL;

    if (ctx->script_count == discarded_client) {
        mp_msg(ctx->log, mp_msg_find_level("warn"), "no active client found.");
        PYcINITIALIZED = true;
        talloc_free(clients);
        talloc_free(client_names);
        return -1;
    }

    PyThreadState_Swap(mainThread);

    if (discarded_client > 0) {
        ctx->script_count = ctx->script_count - discarded_client;
        clients = talloc_realloc(ctx->ta_ctx, clients, PyMpvObject *, ctx->script_count);
    }

    PyObject *mpvmainloop = PyImport_ImportModule("mpvmainloop");

    if (PyModule_AddObjectRef(mpvmainloop, "context", (PyObject *)ctx) < 0) {
        mp_msg(ctx->log, mp_msg_find_level("error"), "%s.\n", "cound not set up context for the module mpvmainloop");
        return -1;
    };

    PyObject *clnts = PyList_New(ctx->script_count);
    for (size_t i = 0; i < ctx->script_count; i++) {
        PyObject *name = PyUnicode_FromString(client_names[i]);
        PyList_SetItem(clnts, i, name);
        Py_DECREF(name);
    }

    talloc_free(client_names);

    PyModule_AddObjectRef(mpvmainloop, "clients", clnts);
    Py_XDECREF(clnts);

    PyObject *mainloop = load_local_pystrings(builtin_files[1][1], "mainloop");
    PyObject *ml = PyObject_GetAttrString(mainloop, "ml");

    PYcINITIALIZED = true;

    PyObject_CallMethod(ml, "run", NULL);
    Py_DECREF(ml);
    Py_DECREF(mainloop);
    Py_DECREF(mpvmainloop);

    return 0;
}

// Main Entrypoint (We want only one call here.)
static int s_load_python(struct mp_script_args *args)
{
    PyScriptCtx *ctx = PyObject_New(PyScriptCtx, &PyScriptCtx_Type);
    ctx->client = args->client;
    ctx->mpctx = args->mpctx;
    ctx->log = args->log;
    ctx->scripts = args->py_scripts;
    ctx->script_count = args->script_count;
    ctx->ta_ctx = talloc_new(NULL);
    ctx->stats = stats_ctx_create(ctx->ta_ctx, ctx->mpctx->global, "script/python");
    stats_register_thread_cputime(ctx->stats, "cpu");

    if (initialize_python(ctx) < 0) {
        return -1;
    };

    for (size_t i = 0; i < ctx->script_count; i++) {
        talloc_free(clients[i]->ta_ctx);
    }

    talloc_free(clients);

    Py_Finalize(); // closes the sub interpreters, no need for doing it manually
    return 0;
}



/**********************************************************************
 *  Main mp.* scripting APIs and helpers
 *********************************************************************/

static PyObject* check_error(int err)
{
    if (err >= 0) {
        Py_RETURN_TRUE;
    }
    const char *errstr = mpv_error_string(err);
    printf("%s\n", errstr);
    PyErr_SetString(PyExc_Exception, errstr);
    Py_RETURN_NONE;
}

static void makenode(void *ta_ctx, PyObject *obj, struct mpv_node *node) {
    if (obj == Py_None) {
        node->format = MPV_FORMAT_NONE;
    }
    else if (PyBool_Check(obj)) {
        node->format = MPV_FORMAT_FLAG;
        node->u.flag = (int) PyObject_IsTrue(obj);
    }
    else if (PyLong_Check(obj)) {
        node->format = MPV_FORMAT_INT64;
        node->u.int64 = (int64_t) PyLong_AsLongLong(obj);
    }
    else if (PyFloat_Check(obj)) {
        node->format = MPV_FORMAT_DOUBLE;
        node->u.double_ = PyFloat_AsDouble(obj);
    }
    else if (PyUnicode_Check(obj)) {
        node->format = MPV_FORMAT_STRING;
        node->u.string = talloc_strdup(ta_ctx, (char *)PyUnicode_AsUTF8(obj));
    }
    else if (PyList_Check(obj)) {
        node->format = MPV_FORMAT_NODE_ARRAY;
        node->u.list = talloc(ta_ctx, struct mpv_node_list);
        int l = (int) PyList_Size(obj);
        node->u.list->num = l;
        node->u.list->keys = NULL;
        node->u.list->values = talloc_array(ta_ctx, struct mpv_node, l);
        for (int i = 0; i < l; i++) {
            PyObject *child = PyList_GetItem(obj, i);
            makenode(ta_ctx, child, &node->u.list->values[i]);
        }
    }
    else if (PyDict_Check(obj)) {
        node->format = MPV_FORMAT_NODE_MAP;
        node->u.list = talloc(ta_ctx, struct mpv_node_list);
        int l = (int) PyDict_Size(obj);
        node->u.list->num = l;
        node->u.list->keys = talloc_array(ta_ctx, char *, l);
        node->u.list->values = talloc_array(ta_ctx, struct mpv_node, l);

        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(obj, &pos, &key, &value)) {
            if (!PyUnicode_Check(key)) {
                PyErr_Format(PyExc_TypeError, "node keys must be 'str'");
            }
            int i = (int) pos;
            node->u.list->keys[i] = talloc_strdup(ta_ctx, (char *)PyUnicode_AsUTF8(key));
            makenode(ta_ctx, value, &node->u.list->values[i]);
        }
    }
}


static PyObject *
deconstructnode(struct mpv_node *node)
{
    if (node->format == MPV_FORMAT_NONE) {
        Py_RETURN_NONE;
    }
    else if (node->format == MPV_FORMAT_FLAG) {
        if (node->u.flag == 1) {
            Py_RETURN_TRUE;
        }
        Py_RETURN_FALSE;
    }
    else if (node->format == MPV_FORMAT_INT64) {
        return PyLong_FromLongLong(node->u.int64);
    }
    else if (node->format == MPV_FORMAT_DOUBLE) {
        return PyFloat_FromDouble(node->u.double_);
    }
    else if (node->format == MPV_FORMAT_STRING) {
        return PyUnicode_FromString(node->u.string);
    }
    else if (node->format == MPV_FORMAT_NODE_ARRAY) {
        PyObject *lnode = PyList_New(node->u.list->num);
        for (int i = 0; i < node->u.list->num; i++) {
            PyList_SetItem(lnode, i, deconstructnode(&node->u.list->values[i]));
        }
        return lnode;
    }
    else if (node->format == MPV_FORMAT_NODE_MAP) {
        PyObject *dnode = PyDict_New();
        for (int i = 0; i < node->u.list->num; i++) {
            PyDict_SetItemString(dnode, node->u.list->keys[i], deconstructnode(&node->u.list->values[i]));
        }
        return dnode;
    }
    Py_RETURN_NONE;
}


/************************************************************************************************/

// main export of this file, used by cplayer to load js scripts
const struct mp_scripting mp_scripting_py = {
    .name = "python",
    .file_ext = "py",
    .load = s_load_python,
};
