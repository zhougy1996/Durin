#pragma once

// Private, pinned CPython ABI table. No Python loader dependency reaches editor startup.
namespace Durin::PythonABI
{
    struct FAPI
    {
        decltype(&::PyFloat_AsDouble) pPyFloat_AsDouble = nullptr;
        decltype(&::_Py_Dealloc) p_Py_Dealloc = nullptr;
        decltype(&::_Py_NoneStruct) p_Py_NoneStruct = nullptr;
        decltype(&::_Py_TrueStruct) p_Py_TrueStruct = nullptr;
        decltype(&::Py_BuildValue) pPy_BuildValue = nullptr;
        decltype(&::Py_CompileStringExFlags) pPy_CompileStringExFlags = nullptr;
        decltype(&::Py_DecRef) pPy_DecRef = nullptr;
        decltype(&::Py_FinalizeEx) pPy_FinalizeEx = nullptr;
        decltype(&::Py_IncRef) pPy_IncRef = nullptr;
        decltype(&::Py_InitializeFromConfig) pPy_InitializeFromConfig = nullptr;
        decltype(&::Py_IsInitialized) pPy_IsInitialized = nullptr;
        decltype(&::PyArg_ParseTuple) pPyArg_ParseTuple = nullptr;
        decltype(&::PyBool_FromLong) pPyBool_FromLong = nullptr;
        decltype(&::PyBool_Type) pPyBool_Type = nullptr;
        decltype(&::PyConfig_Clear) pPyConfig_Clear = nullptr;
        decltype(&::PyConfig_InitIsolatedConfig) pPyConfig_InitIsolatedConfig = nullptr;
        decltype(&::PyDict_New) pPyDict_New = nullptr;
        decltype(&::PyDict_Next) pPyDict_Next = nullptr;
        decltype(&::PyDict_SetItem) pPyDict_SetItem = nullptr;
        decltype(&::PyDict_SetItemString) pPyDict_SetItemString = nullptr;
        decltype(&::PyDict_Size) pPyDict_Size = nullptr;
        decltype(&::PyDict_Type) pPyDict_Type = nullptr;
        decltype(&::PyErr_Clear) pPyErr_Clear = nullptr;
        decltype(&::PyErr_GetRaisedException) pPyErr_GetRaisedException = nullptr;
        decltype(&::PyErr_NewException) pPyErr_NewException = nullptr;
        decltype(&::PyErr_Occurred) pPyErr_Occurred = nullptr;
        decltype(&::PyErr_SetObject) pPyErr_SetObject = nullptr;
        decltype(&::PyErr_SetString) pPyErr_SetString = nullptr;
        decltype(&::PyEval_EvalCode) pPyEval_EvalCode = nullptr;
        decltype(&::PyEval_RestoreThread) pPyEval_RestoreThread = nullptr;
        decltype(&::PyEval_SaveThread) pPyEval_SaveThread = nullptr;
        decltype(&::PyEval_SetTrace) pPyEval_SetTrace = nullptr;
        decltype(&::PyExc_BaseException) pPyExc_BaseException = nullptr;
        decltype(&::PyExc_OverflowError) pPyExc_OverflowError = nullptr;
        decltype(&::PyExc_RuntimeError) pPyExc_RuntimeError = nullptr;
        decltype(&::PyExc_TimeoutError) pPyExc_TimeoutError = nullptr;
        decltype(&::PyExc_ValueError) pPyExc_ValueError = nullptr;
        decltype(&::PyFloat_FromDouble) pPyFloat_FromDouble = nullptr;
        decltype(&::PyFloat_Type) pPyFloat_Type = nullptr;
        decltype(&::PyImport_AppendInittab) pPyImport_AppendInittab = nullptr;
        decltype(&::PyImport_ImportModule) pPyImport_ImportModule = nullptr;
        decltype(&::PyList_Append) pPyList_Append = nullptr;
        decltype(&::PyList_GetItem) pPyList_GetItem = nullptr;
        decltype(&::PyList_New) pPyList_New = nullptr;
        decltype(&::PyList_Size) pPyList_Size = nullptr;
        decltype(&::PyList_Type) pPyList_Type = nullptr;
        decltype(&::PyLong_AsDouble) pPyLong_AsDouble = nullptr;
        decltype(&::PyLong_AsLongLongAndOverflow) pPyLong_AsLongLongAndOverflow = nullptr;
        decltype(&::PyLong_AsUnsignedLong) pPyLong_AsUnsignedLong = nullptr;
        decltype(&::PyLong_AsUnsignedLongLong) pPyLong_AsUnsignedLongLong = nullptr;
        decltype(&::PyLong_FromLongLong) pPyLong_FromLongLong = nullptr;
        decltype(&::PyLong_FromSsize_t) pPyLong_FromSsize_t = nullptr;
        decltype(&::PyLong_FromUnsignedLongLong) pPyLong_FromUnsignedLongLong = nullptr;
        decltype(&::PyLong_Type) pPyLong_Type = nullptr;
        decltype(&::PyModule_AddObjectRef) pPyModule_AddObjectRef = nullptr;
        decltype(&::PyModule_Create2) pPyModule_Create2 = nullptr;
        decltype(&::PyModule_GetDict) pPyModule_GetDict = nullptr;
        decltype(&::PyObject_CallFunction) pPyObject_CallFunction = nullptr;
        decltype(&::PyObject_CallMethod) pPyObject_CallMethod = nullptr;
        decltype(&::PyObject_GetAttrString) pPyObject_GetAttrString = nullptr;
        decltype(&::PyObject_SetAttrString) pPyObject_SetAttrString = nullptr;
        decltype(&::PyObject_Str) pPyObject_Str = nullptr;
        decltype(&::PyRun_StringFlags) pPyRun_StringFlags = nullptr;
        decltype(&::PyStatus_Exception) pPyStatus_Exception = nullptr;
        decltype(&::PyTuple_Type) pPyTuple_Type = nullptr;
        decltype(&::PyType_IsSubtype) pPyType_IsSubtype = nullptr;
        decltype(&::PyUnicode_AsUTF8) pPyUnicode_AsUTF8 = nullptr;
        decltype(&::PyUnicode_AsUTF8AndSize) pPyUnicode_AsUTF8AndSize = nullptr;
        decltype(&::PyUnicode_DecodeUTF8) pPyUnicode_DecodeUTF8 = nullptr;
        decltype(&::PyUnicode_FromString) pPyUnicode_FromString = nullptr;
        decltype(&::PyUnicode_Join) pPyUnicode_Join = nullptr;
        decltype(&::PyUnicode_Type) pPyUnicode_Type = nullptr;
        decltype(&::PyWideStringList_Append) pPyWideStringList_Append = nullptr;
        auto Load(HMODULE Library) -> bool
        {
            pPyFloat_AsDouble = reinterpret_cast<decltype(pPyFloat_AsDouble)>(GetProcAddress(Library, "PyFloat_AsDouble"));
            if (!pPyFloat_AsDouble) return false;
            p_Py_Dealloc = reinterpret_cast<decltype(p_Py_Dealloc)>(GetProcAddress(Library, "_Py_Dealloc"));
            if (!p_Py_Dealloc) return false;
            p_Py_NoneStruct = reinterpret_cast<decltype(p_Py_NoneStruct)>(GetProcAddress(Library, "_Py_NoneStruct"));
            if (!p_Py_NoneStruct) return false;
            p_Py_TrueStruct = reinterpret_cast<decltype(p_Py_TrueStruct)>(GetProcAddress(Library, "_Py_TrueStruct"));
            if (!p_Py_TrueStruct) return false;
            pPy_BuildValue = reinterpret_cast<decltype(pPy_BuildValue)>(GetProcAddress(Library, "Py_BuildValue"));
            if (!pPy_BuildValue) return false;
            pPy_CompileStringExFlags = reinterpret_cast<decltype(pPy_CompileStringExFlags)>(GetProcAddress(Library, "Py_CompileStringExFlags"));
            if (!pPy_CompileStringExFlags) return false;
            pPy_DecRef = reinterpret_cast<decltype(pPy_DecRef)>(GetProcAddress(Library, "Py_DecRef"));
            if (!pPy_DecRef) return false;
            pPy_FinalizeEx = reinterpret_cast<decltype(pPy_FinalizeEx)>(GetProcAddress(Library, "Py_FinalizeEx"));
            if (!pPy_FinalizeEx) return false;
            pPy_IncRef = reinterpret_cast<decltype(pPy_IncRef)>(GetProcAddress(Library, "Py_IncRef"));
            if (!pPy_IncRef) return false;
            pPy_InitializeFromConfig = reinterpret_cast<decltype(pPy_InitializeFromConfig)>(GetProcAddress(Library, "Py_InitializeFromConfig"));
            if (!pPy_InitializeFromConfig) return false;
            pPy_IsInitialized = reinterpret_cast<decltype(pPy_IsInitialized)>(GetProcAddress(Library, "Py_IsInitialized"));
            if (!pPy_IsInitialized) return false;
            pPyArg_ParseTuple = reinterpret_cast<decltype(pPyArg_ParseTuple)>(GetProcAddress(Library, "PyArg_ParseTuple"));
            if (!pPyArg_ParseTuple) return false;
            pPyBool_FromLong = reinterpret_cast<decltype(pPyBool_FromLong)>(GetProcAddress(Library, "PyBool_FromLong"));
            if (!pPyBool_FromLong) return false;
            pPyBool_Type = reinterpret_cast<decltype(pPyBool_Type)>(GetProcAddress(Library, "PyBool_Type"));
            if (!pPyBool_Type) return false;
            pPyConfig_Clear = reinterpret_cast<decltype(pPyConfig_Clear)>(GetProcAddress(Library, "PyConfig_Clear"));
            if (!pPyConfig_Clear) return false;
            pPyConfig_InitIsolatedConfig = reinterpret_cast<decltype(pPyConfig_InitIsolatedConfig)>(GetProcAddress(Library, "PyConfig_InitIsolatedConfig"));
            if (!pPyConfig_InitIsolatedConfig) return false;
            pPyDict_New = reinterpret_cast<decltype(pPyDict_New)>(GetProcAddress(Library, "PyDict_New"));
            if (!pPyDict_New) return false;
            pPyDict_Next = reinterpret_cast<decltype(pPyDict_Next)>(GetProcAddress(Library, "PyDict_Next"));
            if (!pPyDict_Next) return false;
            pPyDict_SetItem = reinterpret_cast<decltype(pPyDict_SetItem)>(GetProcAddress(Library, "PyDict_SetItem"));
            if (!pPyDict_SetItem) return false;
            pPyDict_SetItemString = reinterpret_cast<decltype(pPyDict_SetItemString)>(GetProcAddress(Library, "PyDict_SetItemString"));
            if (!pPyDict_SetItemString) return false;
            pPyDict_Size = reinterpret_cast<decltype(pPyDict_Size)>(GetProcAddress(Library, "PyDict_Size"));
            if (!pPyDict_Size) return false;
            pPyDict_Type = reinterpret_cast<decltype(pPyDict_Type)>(GetProcAddress(Library, "PyDict_Type"));
            if (!pPyDict_Type) return false;
            pPyErr_Clear = reinterpret_cast<decltype(pPyErr_Clear)>(GetProcAddress(Library, "PyErr_Clear"));
            if (!pPyErr_Clear) return false;
            pPyErr_GetRaisedException = reinterpret_cast<decltype(pPyErr_GetRaisedException)>(GetProcAddress(Library, "PyErr_GetRaisedException"));
            if (!pPyErr_GetRaisedException) return false;
            pPyErr_NewException = reinterpret_cast<decltype(pPyErr_NewException)>(GetProcAddress(Library, "PyErr_NewException"));
            if (!pPyErr_NewException) return false;
            pPyErr_Occurred = reinterpret_cast<decltype(pPyErr_Occurred)>(GetProcAddress(Library, "PyErr_Occurred"));
            if (!pPyErr_Occurred) return false;
            pPyErr_SetObject = reinterpret_cast<decltype(pPyErr_SetObject)>(GetProcAddress(Library, "PyErr_SetObject"));
            if (!pPyErr_SetObject) return false;
            pPyErr_SetString = reinterpret_cast<decltype(pPyErr_SetString)>(GetProcAddress(Library, "PyErr_SetString"));
            if (!pPyErr_SetString) return false;
            pPyEval_EvalCode = reinterpret_cast<decltype(pPyEval_EvalCode)>(GetProcAddress(Library, "PyEval_EvalCode"));
            if (!pPyEval_EvalCode) return false;
            pPyEval_RestoreThread = reinterpret_cast<decltype(pPyEval_RestoreThread)>(GetProcAddress(Library, "PyEval_RestoreThread"));
            if (!pPyEval_RestoreThread) return false;
            pPyEval_SaveThread = reinterpret_cast<decltype(pPyEval_SaveThread)>(GetProcAddress(Library, "PyEval_SaveThread"));
            if (!pPyEval_SaveThread) return false;
            pPyEval_SetTrace = reinterpret_cast<decltype(pPyEval_SetTrace)>(GetProcAddress(Library, "PyEval_SetTrace"));
            if (!pPyEval_SetTrace) return false;
            pPyExc_BaseException = reinterpret_cast<decltype(pPyExc_BaseException)>(GetProcAddress(Library, "PyExc_BaseException"));
            if (!pPyExc_BaseException) return false;
            pPyExc_OverflowError = reinterpret_cast<decltype(pPyExc_OverflowError)>(GetProcAddress(Library, "PyExc_OverflowError"));
            if (!pPyExc_OverflowError) return false;
            pPyExc_RuntimeError = reinterpret_cast<decltype(pPyExc_RuntimeError)>(GetProcAddress(Library, "PyExc_RuntimeError"));
            if (!pPyExc_RuntimeError) return false;
            pPyExc_TimeoutError = reinterpret_cast<decltype(pPyExc_TimeoutError)>(GetProcAddress(Library, "PyExc_TimeoutError"));
            if (!pPyExc_TimeoutError) return false;
            pPyExc_ValueError = reinterpret_cast<decltype(pPyExc_ValueError)>(GetProcAddress(Library, "PyExc_ValueError"));
            if (!pPyExc_ValueError) return false;
            pPyFloat_FromDouble = reinterpret_cast<decltype(pPyFloat_FromDouble)>(GetProcAddress(Library, "PyFloat_FromDouble"));
            if (!pPyFloat_FromDouble) return false;
            pPyFloat_Type = reinterpret_cast<decltype(pPyFloat_Type)>(GetProcAddress(Library, "PyFloat_Type"));
            if (!pPyFloat_Type) return false;
            pPyImport_AppendInittab = reinterpret_cast<decltype(pPyImport_AppendInittab)>(GetProcAddress(Library, "PyImport_AppendInittab"));
            if (!pPyImport_AppendInittab) return false;
            pPyImport_ImportModule = reinterpret_cast<decltype(pPyImport_ImportModule)>(GetProcAddress(Library, "PyImport_ImportModule"));
            if (!pPyImport_ImportModule) return false;
            pPyList_Append = reinterpret_cast<decltype(pPyList_Append)>(GetProcAddress(Library, "PyList_Append"));
            if (!pPyList_Append) return false;
            pPyList_GetItem = reinterpret_cast<decltype(pPyList_GetItem)>(GetProcAddress(Library, "PyList_GetItem"));
            if (!pPyList_GetItem) return false;
            pPyList_New = reinterpret_cast<decltype(pPyList_New)>(GetProcAddress(Library, "PyList_New"));
            if (!pPyList_New) return false;
            pPyList_Size = reinterpret_cast<decltype(pPyList_Size)>(GetProcAddress(Library, "PyList_Size"));
            if (!pPyList_Size) return false;
            pPyList_Type = reinterpret_cast<decltype(pPyList_Type)>(GetProcAddress(Library, "PyList_Type"));
            if (!pPyList_Type) return false;
            pPyLong_AsDouble = reinterpret_cast<decltype(pPyLong_AsDouble)>(GetProcAddress(Library, "PyLong_AsDouble"));
            if (!pPyLong_AsDouble) return false;
            pPyLong_AsLongLongAndOverflow = reinterpret_cast<decltype(pPyLong_AsLongLongAndOverflow)>(GetProcAddress(Library, "PyLong_AsLongLongAndOverflow"));
            if (!pPyLong_AsLongLongAndOverflow) return false;
            pPyLong_AsUnsignedLong = reinterpret_cast<decltype(pPyLong_AsUnsignedLong)>(GetProcAddress(Library, "PyLong_AsUnsignedLong"));
            if (!pPyLong_AsUnsignedLong) return false;
            pPyLong_AsUnsignedLongLong = reinterpret_cast<decltype(pPyLong_AsUnsignedLongLong)>(GetProcAddress(Library, "PyLong_AsUnsignedLongLong"));
            if (!pPyLong_AsUnsignedLongLong) return false;
            pPyLong_FromLongLong = reinterpret_cast<decltype(pPyLong_FromLongLong)>(GetProcAddress(Library, "PyLong_FromLongLong"));
            if (!pPyLong_FromLongLong) return false;
            pPyLong_FromSsize_t = reinterpret_cast<decltype(pPyLong_FromSsize_t)>(GetProcAddress(Library, "PyLong_FromSsize_t"));
            if (!pPyLong_FromSsize_t) return false;
            pPyLong_FromUnsignedLongLong = reinterpret_cast<decltype(pPyLong_FromUnsignedLongLong)>(GetProcAddress(Library, "PyLong_FromUnsignedLongLong"));
            if (!pPyLong_FromUnsignedLongLong) return false;
            pPyLong_Type = reinterpret_cast<decltype(pPyLong_Type)>(GetProcAddress(Library, "PyLong_Type"));
            if (!pPyLong_Type) return false;
            pPyModule_AddObjectRef = reinterpret_cast<decltype(pPyModule_AddObjectRef)>(GetProcAddress(Library, "PyModule_AddObjectRef"));
            if (!pPyModule_AddObjectRef) return false;
            pPyModule_Create2 = reinterpret_cast<decltype(pPyModule_Create2)>(GetProcAddress(Library, "PyModule_Create2"));
            if (!pPyModule_Create2) return false;
            pPyModule_GetDict = reinterpret_cast<decltype(pPyModule_GetDict)>(GetProcAddress(Library, "PyModule_GetDict"));
            if (!pPyModule_GetDict) return false;
            pPyObject_CallFunction = reinterpret_cast<decltype(pPyObject_CallFunction)>(GetProcAddress(Library, "PyObject_CallFunction"));
            if (!pPyObject_CallFunction) return false;
            pPyObject_CallMethod = reinterpret_cast<decltype(pPyObject_CallMethod)>(GetProcAddress(Library, "PyObject_CallMethod"));
            if (!pPyObject_CallMethod) return false;
            pPyObject_GetAttrString = reinterpret_cast<decltype(pPyObject_GetAttrString)>(GetProcAddress(Library, "PyObject_GetAttrString"));
            if (!pPyObject_GetAttrString) return false;
            pPyObject_SetAttrString = reinterpret_cast<decltype(pPyObject_SetAttrString)>(GetProcAddress(Library, "PyObject_SetAttrString"));
            if (!pPyObject_SetAttrString) return false;
            pPyObject_Str = reinterpret_cast<decltype(pPyObject_Str)>(GetProcAddress(Library, "PyObject_Str"));
            if (!pPyObject_Str) return false;
            pPyRun_StringFlags = reinterpret_cast<decltype(pPyRun_StringFlags)>(GetProcAddress(Library, "PyRun_StringFlags"));
            if (!pPyRun_StringFlags) return false;
            pPyStatus_Exception = reinterpret_cast<decltype(pPyStatus_Exception)>(GetProcAddress(Library, "PyStatus_Exception"));
            if (!pPyStatus_Exception) return false;
            pPyTuple_Type = reinterpret_cast<decltype(pPyTuple_Type)>(GetProcAddress(Library, "PyTuple_Type"));
            if (!pPyTuple_Type) return false;
            pPyType_IsSubtype = reinterpret_cast<decltype(pPyType_IsSubtype)>(GetProcAddress(Library, "PyType_IsSubtype"));
            if (!pPyType_IsSubtype) return false;
            pPyUnicode_AsUTF8 = reinterpret_cast<decltype(pPyUnicode_AsUTF8)>(GetProcAddress(Library, "PyUnicode_AsUTF8"));
            if (!pPyUnicode_AsUTF8) return false;
            pPyUnicode_AsUTF8AndSize = reinterpret_cast<decltype(pPyUnicode_AsUTF8AndSize)>(GetProcAddress(Library, "PyUnicode_AsUTF8AndSize"));
            if (!pPyUnicode_AsUTF8AndSize) return false;
            pPyUnicode_DecodeUTF8 = reinterpret_cast<decltype(pPyUnicode_DecodeUTF8)>(GetProcAddress(Library, "PyUnicode_DecodeUTF8"));
            if (!pPyUnicode_DecodeUTF8) return false;
            pPyUnicode_FromString = reinterpret_cast<decltype(pPyUnicode_FromString)>(GetProcAddress(Library, "PyUnicode_FromString"));
            if (!pPyUnicode_FromString) return false;
            pPyUnicode_Join = reinterpret_cast<decltype(pPyUnicode_Join)>(GetProcAddress(Library, "PyUnicode_Join"));
            if (!pPyUnicode_Join) return false;
            pPyUnicode_Type = reinterpret_cast<decltype(pPyUnicode_Type)>(GetProcAddress(Library, "PyUnicode_Type"));
            if (!pPyUnicode_Type) return false;
            pPyWideStringList_Append = reinterpret_cast<decltype(pPyWideStringList_Append)>(GetProcAddress(Library, "PyWideStringList_Append"));
            if (!pPyWideStringList_Append) return false;
            return true;
        }
    };
    inline FAPI API;
}
#define _Py_Dealloc Durin::PythonABI::API.p_Py_Dealloc
#define _Py_NoneStruct (*Durin::PythonABI::API.p_Py_NoneStruct)
#define _Py_TrueStruct (*Durin::PythonABI::API.p_Py_TrueStruct)
#define Py_BuildValue Durin::PythonABI::API.pPy_BuildValue
#define Py_CompileStringExFlags Durin::PythonABI::API.pPy_CompileStringExFlags
#define Py_DecRef Durin::PythonABI::API.pPy_DecRef
#define Py_FinalizeEx Durin::PythonABI::API.pPy_FinalizeEx
#define Py_IncRef Durin::PythonABI::API.pPy_IncRef
#define Py_InitializeFromConfig Durin::PythonABI::API.pPy_InitializeFromConfig
#define Py_IsInitialized Durin::PythonABI::API.pPy_IsInitialized
#define PyArg_ParseTuple Durin::PythonABI::API.pPyArg_ParseTuple
#define PyBool_FromLong Durin::PythonABI::API.pPyBool_FromLong
#define PyBool_Type (*Durin::PythonABI::API.pPyBool_Type)
#define PyConfig_Clear Durin::PythonABI::API.pPyConfig_Clear
#define PyConfig_InitIsolatedConfig Durin::PythonABI::API.pPyConfig_InitIsolatedConfig
#define PyDict_New Durin::PythonABI::API.pPyDict_New
#define PyDict_Next Durin::PythonABI::API.pPyDict_Next
#define PyDict_SetItem Durin::PythonABI::API.pPyDict_SetItem
#define PyDict_SetItemString Durin::PythonABI::API.pPyDict_SetItemString
#define PyDict_Size Durin::PythonABI::API.pPyDict_Size
#define PyDict_Type (*Durin::PythonABI::API.pPyDict_Type)
#define PyErr_Clear Durin::PythonABI::API.pPyErr_Clear
#define PyErr_GetRaisedException Durin::PythonABI::API.pPyErr_GetRaisedException
#define PyErr_NewException Durin::PythonABI::API.pPyErr_NewException
#define PyErr_Occurred Durin::PythonABI::API.pPyErr_Occurred
#define PyErr_SetObject Durin::PythonABI::API.pPyErr_SetObject
#define PyErr_SetString Durin::PythonABI::API.pPyErr_SetString
#define PyEval_EvalCode Durin::PythonABI::API.pPyEval_EvalCode
#define PyEval_RestoreThread Durin::PythonABI::API.pPyEval_RestoreThread
#define PyEval_SaveThread Durin::PythonABI::API.pPyEval_SaveThread
#define PyEval_SetTrace Durin::PythonABI::API.pPyEval_SetTrace
#define PyExc_BaseException (*Durin::PythonABI::API.pPyExc_BaseException)
#define PyExc_OverflowError (*Durin::PythonABI::API.pPyExc_OverflowError)
#define PyExc_RuntimeError (*Durin::PythonABI::API.pPyExc_RuntimeError)
#define PyExc_TimeoutError (*Durin::PythonABI::API.pPyExc_TimeoutError)
#define PyExc_ValueError (*Durin::PythonABI::API.pPyExc_ValueError)
#define PyFloat_FromDouble Durin::PythonABI::API.pPyFloat_FromDouble
#define PyFloat_Type (*Durin::PythonABI::API.pPyFloat_Type)
#define PyImport_AppendInittab Durin::PythonABI::API.pPyImport_AppendInittab
#define PyImport_ImportModule Durin::PythonABI::API.pPyImport_ImportModule
#define PyList_Append Durin::PythonABI::API.pPyList_Append
#define PyList_GetItem Durin::PythonABI::API.pPyList_GetItem
#define PyList_New Durin::PythonABI::API.pPyList_New
#define PyList_Size Durin::PythonABI::API.pPyList_Size
#define PyList_Type (*Durin::PythonABI::API.pPyList_Type)
#define PyLong_AsDouble Durin::PythonABI::API.pPyLong_AsDouble
#define PyLong_AsLongLongAndOverflow Durin::PythonABI::API.pPyLong_AsLongLongAndOverflow
#define PyLong_AsUnsignedLong Durin::PythonABI::API.pPyLong_AsUnsignedLong
#define PyLong_AsUnsignedLongLong Durin::PythonABI::API.pPyLong_AsUnsignedLongLong
#define PyLong_FromLongLong Durin::PythonABI::API.pPyLong_FromLongLong
#define PyLong_FromSsize_t Durin::PythonABI::API.pPyLong_FromSsize_t
#define PyLong_FromUnsignedLongLong Durin::PythonABI::API.pPyLong_FromUnsignedLongLong
#define PyLong_Type (*Durin::PythonABI::API.pPyLong_Type)
#define PyModule_AddObjectRef Durin::PythonABI::API.pPyModule_AddObjectRef
#define PyModule_Create2 Durin::PythonABI::API.pPyModule_Create2
#define PyModule_GetDict Durin::PythonABI::API.pPyModule_GetDict
#define PyObject_CallFunction Durin::PythonABI::API.pPyObject_CallFunction
#define PyObject_CallMethod Durin::PythonABI::API.pPyObject_CallMethod
#define PyObject_GetAttrString Durin::PythonABI::API.pPyObject_GetAttrString
#define PyObject_SetAttrString Durin::PythonABI::API.pPyObject_SetAttrString
#define PyObject_Str Durin::PythonABI::API.pPyObject_Str
#define PyRun_StringFlags Durin::PythonABI::API.pPyRun_StringFlags
#define PyStatus_Exception Durin::PythonABI::API.pPyStatus_Exception
#define PyTuple_Type (*Durin::PythonABI::API.pPyTuple_Type)
#define PyType_IsSubtype Durin::PythonABI::API.pPyType_IsSubtype
#define PyUnicode_AsUTF8 Durin::PythonABI::API.pPyUnicode_AsUTF8
#define PyUnicode_AsUTF8AndSize Durin::PythonABI::API.pPyUnicode_AsUTF8AndSize
#define PyUnicode_DecodeUTF8 Durin::PythonABI::API.pPyUnicode_DecodeUTF8
#define PyUnicode_FromString Durin::PythonABI::API.pPyUnicode_FromString
#define PyUnicode_Join Durin::PythonABI::API.pPyUnicode_Join
#define PyUnicode_Type (*Durin::PythonABI::API.pPyUnicode_Type)
#define PyWideStringList_Append Durin::PythonABI::API.pPyWideStringList_Append
#define PyFloat_AsDouble Durin::PythonABI::API.pPyFloat_AsDouble
#undef Py_DECREF
#define Py_DECREF(object) Py_DecRef(reinterpret_cast<PyObject*>(object))
#undef Py_XDECREF
#define Py_XDECREF(object) do { if (object) Py_DECREF(object); } while (false)
