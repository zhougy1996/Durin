#include "PythonReflectionInternal.h"
#if DURIN_WITH_PYTHON
#include "Editor/EditorEngine.h"

namespace Durin::PythonReflection::Private
{
    using K = DurinCodeGen::EPropertyGenFlags;
    auto FValueState::Purge() -> void
    {
        Purged = true; Roots.clear();
        if (Data)
        {
            auto* Struct = Cast<DStruct>(Type);
            if (Struct->NeedsDestroy()) Struct->GetOps().Destroy(Data);
            ::operator delete(Data, std::align_val_t(Struct->GetMinAlignment())); Data = nullptr;
        }
    }

    FValueState::~FValueState() { Purge(); }
    auto RefreshRoots(FValueState& State) -> void
    {
        State.Roots.clear();
        if (!State.Data) return;
        struct FCollector : FReferenceCollector
        {
            std::vector<FStrongObjectPtr>& Roots;
            explicit FCollector(std::vector<FStrongObjectPtr>& In) : Roots(In) {}
            auto AddReferencedObject(DObject*& Object) -> void override { if (Object) Roots.emplace_back(Object); }
        } Collector(State.Roots);
        // Walk owning containers so generated accessors and fixed-array indexes remain valid.
        // The collector observes references; it never mutates native slots.
        std::function<void(DStruct*, void*, unsigned)> StructReferences;
        std::function<void(const FProperty&, const void*, unsigned)> PropertyReferences;
        FBudget Budget;
        PropertyReferences = [&](const FProperty& P, const void* Container, unsigned Depth)
        {
            if (!Budget.Charge(P.GetValueSize(), Qualified(State.Type), Depth)) throw std::runtime_error("Reflected value reference budget exceeded");
            for (uint32 Index = 0; Index < P.GetArrayDim(); ++Index)
            {
                if (P.GetKind() == K::Object)
                {
                    auto* Object = static_cast<const FObjectProperty&>(P).GetObjectPropertyValue(Container, Index);
                    Collector.AddReferencedObject(Object);
                }
                else if (P.GetKind() == K::Struct)
                    StructReferences(static_cast<const FStructProperty&>(P).GetStruct(), const_cast<void*>(P.GetValuePtr(Container, Index)), Depth + 1);
                else if (P.GetKind() == K::Array)
                {
                    const auto& A = static_cast<const FArrayProperty&>(P);
                    for (uint64 I = 0; I < A.Num(Container, Index); ++I)
                        PropertyReferences(*A.GetInner(), A.GetElementPtr(Container, I, Index), Depth + 1);
                }
                else if (P.GetKind() == K::Map)
                {
                    const auto& M = static_cast<const FMapProperty&>(P);
                    struct FVisit { const FMapProperty& M; decltype(PropertyReferences)& Visit; unsigned Depth; } V{M, PropertyReferences, Depth};
                    M.VisitEntries(Container, [](void* Context, const void* Key, const void* Value) {
                        auto& V = *static_cast<FVisit*>(Context);
                        V.Visit(*V.M.GetKeyProp(), Key, V.Depth + 1); V.Visit(*V.M.GetValueProp(), Value, V.Depth + 1); return true;
                    }, &V, Index);
                }
            }
        };
        StructReferences = [&](DStruct* Struct, void* Data, unsigned Depth) {
            for (auto* S = static_cast<DStructBase*>(Struct); S; S = S->GetSuperStructBase())
                for (auto* F = S->GetChildProperties(); F; F = F->GetNext()) PropertyReferences(*static_cast<FProperty*>(F), Data, Depth);
            if (Struct->HasReferenceCollector()) Struct->GetOps().CollectReferences(Data, Collector);
        };
        StructReferences(Cast<DStruct>(State.Type), State.Data, 0);
    }
    auto ValidateValue(FValueState& State, const FEditorScriptCallContext& C) -> bool
    {
        if (State.Purged) { Error(PyExc_ReferenceError, "Reflected value has been purged."); return false; }
        if (State.Type && !ModuleAvailable(State.Type)) { Error(Registry->AccessError, "Value type module is unavailable."); return false; }
        if (!State.Roots.empty() && (State.Host != FObjectKey(C.Host) || State.Gate != C.Gate))
        { Error(Registry->AccessError, "Value references belong to another editor Host."); return false; }
        for (const auto& Root : State.Roots) if (!Root.IsValid())
        { State.Purge(); Error(PyExc_ReferenceError, "Value contains a destroyed object."); return false; }
        return true;
    }
    static auto AllocateValue(PyObject* Python, DType* Type) -> FValue*
    {
        auto* T = reinterpret_cast<PyTypeObject*>(Python);
        auto* Self = reinterpret_cast<FValue*>(T->tp_alloc(T, 0));
        if (!Self) return nullptr;
        Self->State = new FValueState; Self->State->Type = Type;
        Registry->Values.push_back(Self); return Self;
    }
    static auto DeallocValue(FValue* Self) -> void
    {
        PyObject_GC_UnTrack(Self);
        if (Registry) std::erase(Registry->Values, Self);
        delete Self->State;
        auto* Type = Py_TYPE(Self); Type->tp_free(reinterpret_cast<PyObject*>(Self)); Py_DECREF(Type);
    }
    static auto TraverseValue(FValue* Self, visitproc Visit, void* Argument) -> int
    { return Visit(reinterpret_cast<PyObject*>(Py_TYPE(Self)), Argument); }
    auto MakeStruct(DStruct* Struct, const void* Source, const FEditorScriptCallContext& C) -> PyObject*
    {
        FPyOwner Python(Build(Struct)); if (!Python.Value) return nullptr;
        auto* Self = AllocateValue(Python.Value, Struct); if (!Self) return nullptr;
        FPyOwner Owner(reinterpret_cast<PyObject*>(Self));
        auto& S = *Self->State;
        void* Data = ::operator new(Struct->GetStructureSize(), std::align_val_t(Struct->GetMinAlignment()));
        try { if (Source) Struct->GetOps().CopyConstruct(Data, Source); else Struct->GetOps().DefaultConstruct(Data); }
        catch (...) { ::operator delete(Data, std::align_val_t(Struct->GetMinAlignment())); throw; }
        S.Data = Data;
        S.Host = FObjectKey(C.Host); S.Gate = C.Gate; RefreshRoots(S);
        return Owner.Release();
    }
    auto ScalarValue(PyObject* Python, DType* Type, uint64 Enum, std::string Text) -> PyObject*
    {
        auto* Self = AllocateValue(Python, Type); if (!Self) return nullptr;
        Self->State->Enum = Enum; Self->State->Text = std::move(Text);
        Self->State->IsName = Python == Registry->NameType;
        if (Self->State->IsName) Self->State->NativeName = FName(Self->State->Text);
        return reinterpret_cast<PyObject*>(Self);
    }
    static auto ConstructValueImpl(PyTypeObject* Python, PyObject* Args, PyObject* Kwargs) -> PyObject*
    {
        auto C = Context(); if (!C) return nullptr; FScopedEditorScriptCall Call(*C);
        if (!Call.IsAdmitted()) return Error(Registry->AccessError, "Script admission closed.");
        auto* Type = NativeType(reinterpret_cast<PyObject*>(Python));
        if (reinterpret_cast<PyObject*>(Python) == Registry->NameType || reinterpret_cast<PyObject*>(Python) == Registry->GuidType)
        {
            if (PyTuple_Size(Args) != 1 || (Kwargs && PyDict_Size(Kwargs))) return Error(PyExc_TypeError, "Name/Guid requires one string.");
            std::string Text; if (!String(PyTuple_GetItem(Args, 0), Text, "Name/Guid") || !CheckText(Text, reinterpret_cast<PyObject*>(Python) == Registry->GuidType, "Name/Guid")) return nullptr;
            return ScalarValue(reinterpret_cast<PyObject*>(Python), nullptr, 0, Text);
        }
        auto* Struct = Cast<DStruct>(Type);
        if (!Struct || (!Intrinsic(Type) && !Type->GetScriptExposure().Has(EScriptExposure::Type))) return Error(PyExc_TypeError, "Native value construction is not exposed.");
        bool Writable = Intrinsic(Type);
        for (auto* F = Struct->GetChildProperties(); F; F = F->GetNext())
            Writable |= static_cast<FProperty*>(F)->HasAnyPropertyFlags(EPropertyFlags::ScriptReadWrite);
        if (!Writable || !Struct->CanDefaultConstruct() || PyTuple_Size(Args)) return Error(PyExc_TypeError, "Struct requires exposed writable named fields and native defaults.");
        FPyOwner Result(MakeStruct(Struct, nullptr, *C)); if (!Result.Value) return nullptr;
        Py_ssize_t Pos = 0; PyObject *Key, *Value;
        while (Kwargs && PyDict_Next(Kwargs, &Pos, &Key, &Value))
        {
            std::string Text; if (!String(Key, Text, Qualified(Type))) return nullptr;
            if (InitializeValueField(Python, Result.Value, Text, Value) < 0) return nullptr;
        }
        return Result.Release();
    }
    static auto ConstructValue(PyTypeObject* Python, PyObject* Args, PyObject* Kwargs) -> PyObject*
    {
        try { return ConstructValueImpl(Python, Args, Kwargs); }
        catch (const std::exception& E) { return Error(PyExc_RuntimeError, std::string("Native value construction: ") + E.what()); }
        catch (...) { return Error(PyExc_RuntimeError, "Native value construction threw an exception."); }
    }
    static auto ValueString(FValue* Self) -> PyObject*
    { return PyUnicode_FromString(Self->State->Text.c_str()); }
    static auto ValueHash(FValue* Self) -> Py_hash_t
    {
        if (Cast<DStruct>(Self->State->Type)) { Error(PyExc_TypeError, "Mutable reflected structs are unhashable."); return -1; }
        auto Hash = static_cast<Py_hash_t>((Self->State->IsName ? std::hash<FName>{}(Self->State->NativeName) : std::hash<std::string>{}(Self->State->Text)) ^ std::hash<void*>{}(Self->State->Type) ^ Self->State->Enum);
        return Hash == -1 ? -2 : Hash;
    }
    static auto ValueCompare(PyObject* A, PyObject* B, int Op) -> PyObject*
    {
        if (Py_TYPE(A) != Py_TYPE(B) || (Op != Py_EQ && Op != Py_NE)) { Py_IncRef(Py_NotImplemented); return Py_NotImplemented; }
        auto& L = *reinterpret_cast<FValue*>(A)->State; auto& R = *reinterpret_cast<FValue*>(B)->State;
        const bool Equal = Cast<DStruct>(L.Type) ? A == B : L.IsName ? L.NativeName == R.NativeName : L.Enum == R.Enum && L.Text == R.Text;
        return PyBool_FromLong(Op == Py_EQ ? Equal : !Equal);
    }
    static PyMethodDef ValueTypeMethods[] = {
        {"__init_subclass__", Subclass, METH_CLASS | METH_NOARGS, nullptr}, {nullptr, nullptr, 0, nullptr}};
    auto StartValueTypes() -> bool
    {
        PyType_Slot ValueSlots[] = {{Py_tp_new, reinterpret_cast<void*>(ConstructValue)}, {Py_tp_dealloc, reinterpret_cast<void*>(DeallocValue)},
            {Py_tp_traverse, reinterpret_cast<void*>(TraverseValue)},
            {Py_tp_str, reinterpret_cast<void*>(ValueString)}, {Py_tp_hash, reinterpret_cast<void*>(ValueHash)},
            {Py_tp_richcompare, reinterpret_cast<void*>(ValueCompare)}, {0, nullptr}};
        for (const auto& [Name, Target] : {std::pair{"durin.Name", &Registry->NameType}, std::pair{"durin.Guid", &Registry->GuidType}})
        {
            PyType_Spec ValueSpec{Name, sizeof(FValue), 0, Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_HAVE_GC, ValueSlots};
            *Target = PyType_FromSpecWithBases(&ValueSpec, nullptr);
            if (!*Target || PyModule_AddObjectRef(Registry->Module, Name + 6, *Target) < 0) return false;
        }
        return true;
    }

    auto ValueTypeSlots() -> std::span<const PyType_Slot>
    {
        static const PyType_Slot Slots[] = {
            {Py_tp_new, reinterpret_cast<void*>(ConstructValue)}, {Py_tp_dealloc, reinterpret_cast<void*>(DeallocValue)},
            {Py_tp_hash, reinterpret_cast<void*>(ValueHash)}, {Py_tp_richcompare, reinterpret_cast<void*>(ValueCompare)},
            {Py_tp_traverse, reinterpret_cast<void*>(TraverseValue)}, {Py_tp_methods, reinterpret_cast<void*>(ValueTypeMethods)}, {0, nullptr}};
        return Slots;
    }
}
#endif
