#pragma once

#include "HAL/PlatformLibrary.h"

// Private, pinned CPython ABI table. No Python loader dependency reaches editor startup.
namespace Durin::PythonABI
{
    struct FAPI
    {
        decltype(&::PyObject_GC_UnTrack) pPyObject_GC_UnTrack = nullptr;
        decltype(&::PyTuple_SetItem) pPyTuple_SetItem = nullptr;
        decltype(&::PyTuple_New) pPyTuple_New = nullptr;
        decltype(&::PyTuple_Size) pPyTuple_Size = nullptr;
        decltype(&::PyTuple_GetItem) pPyTuple_GetItem = nullptr;
        decltype(&::PySequence_Size) pPySequence_Size = nullptr;
        decltype(&::_Py_NotImplementedStruct) p_Py_NotImplementedStruct = nullptr;
        decltype(&::PyExc_AttributeError) pPyExc_AttributeError = nullptr;
        decltype(&::PyExc_ReferenceError) pPyExc_ReferenceError = nullptr;
        decltype(&::PyExc_TypeError) pPyExc_TypeError = nullptr;
        decltype(&::PyDict_GetItemString) pPyDict_GetItemString = nullptr;
        decltype(&::PyTuple_Pack) pPyTuple_Pack = nullptr;
        decltype(&::PyObject_Free) pPyObject_Free = nullptr;
        decltype(&::PyType_GenericAlloc) pPyType_GenericAlloc = nullptr;
        decltype(&::PyType_FromSpecWithBases) pPyType_FromSpecWithBases = nullptr;
        decltype(&::PyFloat_AsDouble) pPyFloat_AsDouble = nullptr;
        decltype(&::PyGC_Collect) pPyGC_Collect = nullptr;
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
        auto Load(FModuleHandle Library) -> std::expected<void, FPlatformLibraryError>
        {
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyGC_Collect"); Symbol)
                pPyGC_Collect = reinterpret_cast<decltype(pPyGC_Collect)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyFloat_AsDouble"); Symbol)
                pPyFloat_AsDouble = reinterpret_cast<decltype(pPyFloat_AsDouble)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "_Py_Dealloc"); Symbol)
                p_Py_Dealloc = reinterpret_cast<decltype(p_Py_Dealloc)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "_Py_NoneStruct"); Symbol)
                p_Py_NoneStruct = reinterpret_cast<decltype(p_Py_NoneStruct)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "_Py_TrueStruct"); Symbol)
                p_Py_TrueStruct = reinterpret_cast<decltype(p_Py_TrueStruct)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "Py_BuildValue"); Symbol)
                pPy_BuildValue = reinterpret_cast<decltype(pPy_BuildValue)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "Py_CompileStringExFlags"); Symbol)
                pPy_CompileStringExFlags = reinterpret_cast<decltype(pPy_CompileStringExFlags)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "Py_DecRef"); Symbol)
                pPy_DecRef = reinterpret_cast<decltype(pPy_DecRef)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "Py_FinalizeEx"); Symbol)
                pPy_FinalizeEx = reinterpret_cast<decltype(pPy_FinalizeEx)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "Py_IncRef"); Symbol)
                pPy_IncRef = reinterpret_cast<decltype(pPy_IncRef)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "Py_InitializeFromConfig"); Symbol)
                pPy_InitializeFromConfig = reinterpret_cast<decltype(pPy_InitializeFromConfig)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "Py_IsInitialized"); Symbol)
                pPy_IsInitialized = reinterpret_cast<decltype(pPy_IsInitialized)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyArg_ParseTuple"); Symbol)
                pPyArg_ParseTuple = reinterpret_cast<decltype(pPyArg_ParseTuple)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyBool_FromLong"); Symbol)
                pPyBool_FromLong = reinterpret_cast<decltype(pPyBool_FromLong)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyBool_Type"); Symbol)
                pPyBool_Type = reinterpret_cast<decltype(pPyBool_Type)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyConfig_Clear"); Symbol)
                pPyConfig_Clear = reinterpret_cast<decltype(pPyConfig_Clear)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyConfig_InitIsolatedConfig"); Symbol)
                pPyConfig_InitIsolatedConfig = reinterpret_cast<decltype(pPyConfig_InitIsolatedConfig)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyDict_New"); Symbol)
                pPyDict_New = reinterpret_cast<decltype(pPyDict_New)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyDict_Next"); Symbol)
                pPyDict_Next = reinterpret_cast<decltype(pPyDict_Next)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyDict_SetItem"); Symbol)
                pPyDict_SetItem = reinterpret_cast<decltype(pPyDict_SetItem)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyDict_SetItemString"); Symbol)
                pPyDict_SetItemString = reinterpret_cast<decltype(pPyDict_SetItemString)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyDict_Size"); Symbol)
                pPyDict_Size = reinterpret_cast<decltype(pPyDict_Size)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyDict_Type"); Symbol)
                pPyDict_Type = reinterpret_cast<decltype(pPyDict_Type)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyErr_Clear"); Symbol)
                pPyErr_Clear = reinterpret_cast<decltype(pPyErr_Clear)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyErr_GetRaisedException"); Symbol)
                pPyErr_GetRaisedException = reinterpret_cast<decltype(pPyErr_GetRaisedException)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyErr_NewException"); Symbol)
                pPyErr_NewException = reinterpret_cast<decltype(pPyErr_NewException)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyErr_Occurred"); Symbol)
                pPyErr_Occurred = reinterpret_cast<decltype(pPyErr_Occurred)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyErr_SetObject"); Symbol)
                pPyErr_SetObject = reinterpret_cast<decltype(pPyErr_SetObject)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyErr_SetString"); Symbol)
                pPyErr_SetString = reinterpret_cast<decltype(pPyErr_SetString)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyEval_EvalCode"); Symbol)
                pPyEval_EvalCode = reinterpret_cast<decltype(pPyEval_EvalCode)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyEval_RestoreThread"); Symbol)
                pPyEval_RestoreThread = reinterpret_cast<decltype(pPyEval_RestoreThread)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyEval_SaveThread"); Symbol)
                pPyEval_SaveThread = reinterpret_cast<decltype(pPyEval_SaveThread)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyEval_SetTrace"); Symbol)
                pPyEval_SetTrace = reinterpret_cast<decltype(pPyEval_SetTrace)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyExc_BaseException"); Symbol)
                pPyExc_BaseException = reinterpret_cast<decltype(pPyExc_BaseException)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyExc_OverflowError"); Symbol)
                pPyExc_OverflowError = reinterpret_cast<decltype(pPyExc_OverflowError)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyExc_RuntimeError"); Symbol)
                pPyExc_RuntimeError = reinterpret_cast<decltype(pPyExc_RuntimeError)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyExc_ValueError"); Symbol)
                pPyExc_ValueError = reinterpret_cast<decltype(pPyExc_ValueError)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyFloat_FromDouble"); Symbol)
                pPyFloat_FromDouble = reinterpret_cast<decltype(pPyFloat_FromDouble)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyFloat_Type"); Symbol)
                pPyFloat_Type = reinterpret_cast<decltype(pPyFloat_Type)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyImport_AppendInittab"); Symbol)
                pPyImport_AppendInittab = reinterpret_cast<decltype(pPyImport_AppendInittab)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyImport_ImportModule"); Symbol)
                pPyImport_ImportModule = reinterpret_cast<decltype(pPyImport_ImportModule)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyList_Append"); Symbol)
                pPyList_Append = reinterpret_cast<decltype(pPyList_Append)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyList_GetItem"); Symbol)
                pPyList_GetItem = reinterpret_cast<decltype(pPyList_GetItem)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyList_New"); Symbol)
                pPyList_New = reinterpret_cast<decltype(pPyList_New)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyList_Size"); Symbol)
                pPyList_Size = reinterpret_cast<decltype(pPyList_Size)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyList_Type"); Symbol)
                pPyList_Type = reinterpret_cast<decltype(pPyList_Type)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyLong_AsDouble"); Symbol)
                pPyLong_AsDouble = reinterpret_cast<decltype(pPyLong_AsDouble)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyLong_AsLongLongAndOverflow"); Symbol)
                pPyLong_AsLongLongAndOverflow = reinterpret_cast<decltype(pPyLong_AsLongLongAndOverflow)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyLong_AsUnsignedLong"); Symbol)
                pPyLong_AsUnsignedLong = reinterpret_cast<decltype(pPyLong_AsUnsignedLong)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyLong_AsUnsignedLongLong"); Symbol)
                pPyLong_AsUnsignedLongLong = reinterpret_cast<decltype(pPyLong_AsUnsignedLongLong)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyLong_FromLongLong"); Symbol)
                pPyLong_FromLongLong = reinterpret_cast<decltype(pPyLong_FromLongLong)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyLong_FromSsize_t"); Symbol)
                pPyLong_FromSsize_t = reinterpret_cast<decltype(pPyLong_FromSsize_t)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyLong_FromUnsignedLongLong"); Symbol)
                pPyLong_FromUnsignedLongLong = reinterpret_cast<decltype(pPyLong_FromUnsignedLongLong)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyLong_Type"); Symbol)
                pPyLong_Type = reinterpret_cast<decltype(pPyLong_Type)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyModule_AddObjectRef"); Symbol)
                pPyModule_AddObjectRef = reinterpret_cast<decltype(pPyModule_AddObjectRef)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyModule_Create2"); Symbol)
                pPyModule_Create2 = reinterpret_cast<decltype(pPyModule_Create2)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyModule_GetDict"); Symbol)
                pPyModule_GetDict = reinterpret_cast<decltype(pPyModule_GetDict)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyObject_CallFunction"); Symbol)
                pPyObject_CallFunction = reinterpret_cast<decltype(pPyObject_CallFunction)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyObject_CallMethod"); Symbol)
                pPyObject_CallMethod = reinterpret_cast<decltype(pPyObject_CallMethod)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyObject_GetAttrString"); Symbol)
                pPyObject_GetAttrString = reinterpret_cast<decltype(pPyObject_GetAttrString)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyObject_SetAttrString"); Symbol)
                pPyObject_SetAttrString = reinterpret_cast<decltype(pPyObject_SetAttrString)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyObject_Str"); Symbol)
                pPyObject_Str = reinterpret_cast<decltype(pPyObject_Str)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyRun_StringFlags"); Symbol)
                pPyRun_StringFlags = reinterpret_cast<decltype(pPyRun_StringFlags)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyStatus_Exception"); Symbol)
                pPyStatus_Exception = reinterpret_cast<decltype(pPyStatus_Exception)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyTuple_Type"); Symbol)
                pPyTuple_Type = reinterpret_cast<decltype(pPyTuple_Type)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyType_IsSubtype"); Symbol)
                pPyType_IsSubtype = reinterpret_cast<decltype(pPyType_IsSubtype)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyUnicode_AsUTF8"); Symbol)
                pPyUnicode_AsUTF8 = reinterpret_cast<decltype(pPyUnicode_AsUTF8)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyUnicode_AsUTF8AndSize"); Symbol)
                pPyUnicode_AsUTF8AndSize = reinterpret_cast<decltype(pPyUnicode_AsUTF8AndSize)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyUnicode_DecodeUTF8"); Symbol)
                pPyUnicode_DecodeUTF8 = reinterpret_cast<decltype(pPyUnicode_DecodeUTF8)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyUnicode_FromString"); Symbol)
                pPyUnicode_FromString = reinterpret_cast<decltype(pPyUnicode_FromString)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyUnicode_Join"); Symbol)
                pPyUnicode_Join = reinterpret_cast<decltype(pPyUnicode_Join)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyUnicode_Type"); Symbol)
                pPyUnicode_Type = reinterpret_cast<decltype(pPyUnicode_Type)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyWideStringList_Append"); Symbol)
                pPyWideStringList_Append = reinterpret_cast<decltype(pPyWideStringList_Append)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyType_FromSpecWithBases"); Symbol)
                pPyType_FromSpecWithBases = reinterpret_cast<decltype(pPyType_FromSpecWithBases)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyType_GenericAlloc"); Symbol)
                pPyType_GenericAlloc = reinterpret_cast<decltype(pPyType_GenericAlloc)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyObject_Free"); Symbol)
                pPyObject_Free = reinterpret_cast<decltype(pPyObject_Free)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyTuple_Pack"); Symbol)
                pPyTuple_Pack = reinterpret_cast<decltype(pPyTuple_Pack)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyDict_GetItemString"); Symbol)
                pPyDict_GetItemString = reinterpret_cast<decltype(pPyDict_GetItemString)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyExc_TypeError"); Symbol)
                pPyExc_TypeError = reinterpret_cast<decltype(pPyExc_TypeError)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyExc_ReferenceError"); Symbol)
                pPyExc_ReferenceError = reinterpret_cast<decltype(pPyExc_ReferenceError)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyExc_AttributeError"); Symbol)
                pPyExc_AttributeError = reinterpret_cast<decltype(pPyExc_AttributeError)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "_Py_NotImplementedStruct"); Symbol)
                p_Py_NotImplementedStruct = reinterpret_cast<decltype(p_Py_NotImplementedStruct)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PySequence_Size"); Symbol)
                pPySequence_Size = reinterpret_cast<decltype(pPySequence_Size)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyTuple_GetItem"); Symbol)
                pPyTuple_GetItem = reinterpret_cast<decltype(pPyTuple_GetItem)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyTuple_Size"); Symbol)
                pPyTuple_Size = reinterpret_cast<decltype(pPyTuple_Size)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyTuple_New"); Symbol)
                pPyTuple_New = reinterpret_cast<decltype(pPyTuple_New)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyTuple_SetItem"); Symbol)
                pPyTuple_SetItem = reinterpret_cast<decltype(pPyTuple_SetItem)>(*Symbol);
            else return std::unexpected(Symbol.error());
            if (const auto Symbol = FPlatformLibrary::FindSymbol(Library, "PyObject_GC_UnTrack"); Symbol)
                pPyObject_GC_UnTrack = reinterpret_cast<decltype(pPyObject_GC_UnTrack)>(*Symbol);
            else return std::unexpected(Symbol.error());
            return {};
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

#define PyGC_Collect Durin::PythonABI::API.pPyGC_Collect

#define PyType_FromSpecWithBases Durin::PythonABI::API.pPyType_FromSpecWithBases

#define PyType_GenericAlloc Durin::PythonABI::API.pPyType_GenericAlloc

#define PyObject_Free Durin::PythonABI::API.pPyObject_Free

#define PyTuple_Pack Durin::PythonABI::API.pPyTuple_Pack

#define PyDict_GetItemString Durin::PythonABI::API.pPyDict_GetItemString

#define PyExc_TypeError (*Durin::PythonABI::API.pPyExc_TypeError)

#define PyExc_ReferenceError (*Durin::PythonABI::API.pPyExc_ReferenceError)

#define PyExc_AttributeError (*Durin::PythonABI::API.pPyExc_AttributeError)

#define _Py_NotImplementedStruct (*Durin::PythonABI::API.p_Py_NotImplementedStruct)

#define PySequence_Size Durin::PythonABI::API.pPySequence_Size

#define PyTuple_GetItem Durin::PythonABI::API.pPyTuple_GetItem

#define PyTuple_Size Durin::PythonABI::API.pPyTuple_Size

#define PyTuple_New Durin::PythonABI::API.pPyTuple_New

#define PyTuple_SetItem Durin::PythonABI::API.pPyTuple_SetItem

#define PyObject_GC_UnTrack Durin::PythonABI::API.pPyObject_GC_UnTrack

// Python.h's inline helper otherwise retains a direct import-library reference.
#undef PyObject_TypeCheck
#define PyObject_TypeCheck(object, type) (Py_IS_TYPE((object), (type)) || PyType_IsSubtype(Py_TYPE(object), (type)))
