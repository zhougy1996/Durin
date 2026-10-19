#include "PythonReflectionInternal.h"
#if DURIN_WITH_PYTHON
#include "Editor/EditorEngine.h"
#include "Editor/EditorSubsystem.h"

namespace Durin::PythonReflection::Private
{
    auto Resolve(FObject* Self, const FEditorScriptCallContext& Context) -> DObject*
    {
        auto& State = *Self->State;
        if (State.Purged || !State.Root.IsValid()) { State.Purge(); Error(PyExc_ReferenceError, "Native object has been destroyed or purged."); return nullptr; }
        if (State.Host != FObjectKey(Context.Host) || State.Gate != Context.Gate)
        { Error(Registry->AccessError, "Object belongs to another editor Host or endpoint generation."); return nullptr; }
        if (!State.Gate->IsOpen() || (State.ServiceGate && !State.ServiceGate->IsOpen()))
        { State.Purge(); Error(PyExc_ReferenceError, "Native object service has retired."); return nullptr; }
        if (!ModuleAvailable(State.Root.Get()->GetClass()))
        { Error(Registry->AccessError, "Native type module is stopped or unavailable."); return nullptr; }
        return State.Root.Get();
    }
    auto Dealloc(FObject* Self) -> void
    {
        if (Registry) std::erase(Registry->Objects, Self);
        delete Self->State;
        auto* Type = Py_TYPE(Self); Type->tp_free(reinterpret_cast<PyObject*>(Self)); Py_DECREF(Type);
    }
    auto Construct(PyTypeObject*, PyObject*, PyObject*) -> PyObject*
    { return Error(PyExc_TypeError, "Native objects cannot be constructed from Python; use an editor service."); }
    auto Subclass(PyObject*, PyObject*) -> PyObject*
    { return Error(PyExc_TypeError, "Python subclassing of native reflected types is unsupported."); }
    auto Repr(FObject*) -> PyObject* { return PyUnicode_FromString("<durin native object>"); }
    auto Hash(FObject* Self) -> Py_hash_t
    {
        const auto& S = *Self->State;
        const auto Value = S.Host.GetHash() ^ S.Target.GetHash() ^ std::hash<const void*>{}(S.Gate.get());
        const auto Result = static_cast<Py_hash_t>(Value); return Result == -1 ? -2 : Result;
    }
    auto Compare(PyObject* Left, PyObject* Right, int Op) -> PyObject*
    {
        bool IsObject = false;
        if (Registry) for (const auto& [Native, Type] : Registry->Types)
            if (Cast<DClass>(Native) && (Py_TYPE(Right) == reinterpret_cast<PyTypeObject*>(Type.Python) || PyType_IsSubtype(Py_TYPE(Right), reinterpret_cast<PyTypeObject*>(Type.Python)))) { IsObject = true; break; }
        if (!IsObject || (Op != Py_EQ && Op != Py_NE))
        { Py_IncRef(Py_NotImplemented); return Py_NotImplemented; }
        const auto& A = *reinterpret_cast<FObject*>(Left)->State;
        const auto& B = *reinterpret_cast<FObject*>(Right)->State;
        const bool Equal = A.Host == B.Host && A.Target == B.Target && A.Gate == B.Gate;
        return PyBool_FromLong(Op == Py_EQ ? Equal : !Equal);
    }
    auto GetName(FObject* Self, PyObject*) -> PyObject*
    {
        auto Value = Context(); if (!Value) return nullptr;
        FScopedEditorScriptCall Call(*Value); if (!Call.IsAdmitted()) return Error(Registry->AccessError, "Script admission closed.");
        auto* Object = Resolve(Self, *Value); return Object ? PyUnicode_FromString(Object->GetName().c_str()) : nullptr;
    }
    auto GetClass(FObject* Self, PyObject*) -> PyObject*
    {
        auto Value = Context(); if (!Value) return nullptr;
        FScopedEditorScriptCall Call(*Value); if (!Call.IsAdmitted()) return Error(Registry->AccessError, "Script admission closed.");
        auto* Object = Resolve(Self, *Value); return Object ? Build(Object->GetClass()) : nullptr;
    }
    PyMethodDef ObjectMethods[] = {
        {"get_name", reinterpret_cast<PyCFunction>(GetName), METH_NOARGS, "Return the native object name after Host/lifetime admission."},
        {"get_class", reinterpret_cast<PyCFunction>(GetClass), METH_NOARGS, "Return the actual reflected Python class."},
        {"get_editor_property", reinterpret_cast<PyCFunction>(GetEditorProperty), METH_O, "Read an editable native property as a detached value."},
        {"set_editor_property", reinterpret_cast<PyCFunction>(SetEditorProperty), METH_VARARGS, "Validate and edit a native property through editor history."},
        {"__init_subclass__", Subclass, METH_CLASS | METH_NOARGS, nullptr}, {nullptr, nullptr, 0, nullptr}};
    auto Wrap(DObject* Object, const FEditorScriptCallContext& Context) -> PyObject*
    {
        if (!IsValid(Object)) return Error(PyExc_ReferenceError, "Native object is unavailable.");
        if (!ModuleAvailable(Object->GetClass())) return Error(Registry->AccessError, "Native object module is unavailable.");
        if (auto* Service = Cast<DSubsystem>(Object); Service && !Service->GetWorkGate()->IsOpen()) return Error(PyExc_ReferenceError, "Native object service has retired.");
        for (auto* Existing : Registry->Objects)
        {
            const auto& S = *Existing->State;
            if (!S.Purged && S.Host == FObjectKey(Context.Host) && S.Gate == Context.Gate && S.Target == FObjectKey(Object))
            { Py_IncRef(reinterpret_cast<PyObject*>(Existing)); return reinterpret_cast<PyObject*>(Existing); }
        }
        auto* PythonType = Build(Object->GetClass()); if (!PythonType) return nullptr;
        auto* Self = reinterpret_cast<FObject*>(reinterpret_cast<PyTypeObject*>(PythonType)->tp_alloc(reinterpret_cast<PyTypeObject*>(PythonType), 0));
        Py_DECREF(PythonType); if (!Self) return nullptr;
        Self->State = new FObjectState{FObjectKey(Context.Host), FObjectKey(Object), Context.Gate, {}, FStrongObjectPtr(Object)};
        if (auto* Service = Cast<DSubsystem>(Object)) Self->State->ServiceGate = Service->GetWorkGate();
        Registry->Objects.push_back(Self); return reinterpret_cast<PyObject*>(Self);
    }

    auto ObjectTypeSlots() -> std::span<const PyType_Slot>
    {
        static const PyType_Slot Slots[] = {
            {Py_tp_new, reinterpret_cast<void*>(Construct)}, {Py_tp_dealloc, reinterpret_cast<void*>(Dealloc)},
            {Py_tp_repr, reinterpret_cast<void*>(Repr)}, {Py_tp_hash, reinterpret_cast<void*>(Hash)},
            {Py_tp_richcompare, reinterpret_cast<void*>(Compare)}, {Py_tp_methods, reinterpret_cast<void*>(ObjectMethods)}, {0, nullptr}};
        return Slots;
    }
}
#endif
