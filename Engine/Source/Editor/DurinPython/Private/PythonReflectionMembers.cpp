#include "PythonReflectionInternal.h"
#if DURIN_WITH_PYTHON
#include "Editor/EditorEngine.h"
#include "DObject/FunctionParameters.h"

namespace Durin::PythonReflection::Private
{
    using K = DurinCodeGen::EPropertyGenFlags;
    struct FMember
    {
        PyObject_HEAD
        const DFunction* Function;
        const FProperty* Property;
        DType* Declaring;
        PyObject* Owner;
    };
    static auto ClearMember(FMember* Self) -> int
    { auto* Owner = std::exchange(Self->Owner, nullptr); Py_XDECREF(Owner); return 0; }
    static auto TraverseMember(FMember* Self, visitproc Visit, void* Argument) -> int
    { if (const auto Result = Visit(reinterpret_cast<PyObject*>(Py_TYPE(Self)), Argument)) return Result; return Self->Owner ? Visit(Self->Owner, Argument) : 0; }
    static auto DeallocMember(FMember* Self) -> void
    { PyObject_GC_UnTrack(Self); ClearMember(Self); auto* Type = Py_TYPE(Self); Type->tp_free(reinterpret_cast<PyObject*>(Self)); Py_DECREF(Type); }
    static auto Member(const DFunction* Function, const FProperty* Property, DType* Declaring, PyObject* Owner = nullptr) -> PyObject*
    {
        auto* T = reinterpret_cast<PyTypeObject*>(Registry->MemberType);
        auto* Self = reinterpret_cast<FMember*>(T->tp_alloc(T, 0)); if (!Self) return nullptr;
        Self->Function = Function; Self->Property = Property; Self->Declaring = Declaring; Self->Owner = Owner;
        Py_XINCREF(Owner); return reinterpret_cast<PyObject*>(Self);
    }
    static auto Effective(FMember* Self, DObject* Object) -> const DFunction*
    {
        auto* F = Object->GetClass()->FindFunctionByName(FName(Self->Function->GetName()));
        if (!F || !F->GetScriptExposure().Has(EScriptExposure::Callable) || F->GetScriptExposure().Has(EScriptExposure::NoExport))
        { Error(PyExc_AttributeError, Qualified(Object->GetClass()) + "::" + Self->Function->GetName() + ": effective override is not script callable"); return nullptr; }
        if (!ModuleAvailable(F->GetOuterDClass())) { Error(Registry->AccessError, "Native function provider is unavailable."); return nullptr; }
        return F;
    }
    static auto CallMemberImpl(FMember* Self, PyObject* Args, PyObject* Kwargs) -> PyObject*
    {
        if (!Self->Function) return Error(PyExc_TypeError, "Reflected property is not callable.");
        if (!Self->Owner)
        {
            if (PyTuple_Size(Args) == 0) return Error(PyExc_TypeError, "Reflected method requires a native object.");
            auto* Owner = PyTuple_GetItem(Args, 0); auto* Class = Cast<DClass>(ValueType(Owner));
            if (!Class || !Class->IsChildOf(Cast<DClass>(Self->Declaring))) return Error(PyExc_TypeError, "Incompatible unbound native method target.");
            FPyOwner Bound(Member(Self->Function, nullptr, Self->Declaring, Owner));
            FPyOwner Rest(PyTuple_New(PyTuple_Size(Args) - 1)); if (!Bound.Value || !Rest.Value) return nullptr;
            for (Py_ssize_t I = 1; I < PyTuple_Size(Args); ++I)
            { auto* Item = PyTuple_GetItem(Args, I); Py_IncRef(Item); PyTuple_SetItem(Rest.Value, I - 1, Item); }
            return CallMemberImpl(reinterpret_cast<FMember*>(Bound.Value), Rest.Value, Kwargs);
        }
        auto C = Context(); if (!C) return nullptr; FScopedEditorScriptCall Call(*C);
        if (!Call.IsAdmitted()) return Error(Registry->AccessError, "Script admission closed.");
        if (!ModuleAvailable(Self->Declaring)) return Error(Registry->AccessError, "Native member provider is unavailable.");
        auto* Object = Resolve(reinterpret_cast<FObject*>(Self->Owner), *C); if (!Object) return nullptr;
        const auto* F = Effective(Self, Object); if (!F) return nullptr;
        const auto Path = Qualified(Object->GetClass()) + "::" + F->GetName();
        const auto Count = F->GetNumInputParms();
        if (static_cast<size_t>(PyTuple_Size(Args)) > Count) return Error(PyExc_TypeError, Path + ": too many positional arguments");
        std::vector<PyObject*> Inputs(Count); std::vector<size_t> Omitted;
        for (Py_ssize_t I = 0; I < PyTuple_Size(Args); ++I) Inputs[I] = PyTuple_GetItem(Args, I);
        if (Kwargs)
        {
            Py_ssize_t Pos = 0; PyObject *Key, *Value;
            while (PyDict_Next(Kwargs, &Pos, &Key, &Value))
            {
                std::string KeyName; if (!String(Key, KeyName, Path)) return nullptr;
                size_t I = 0; while (I < Count && PropertyName(*F->GetParameterProperty(I)) != KeyName) ++I;
                if (I == Count || Inputs[I]) return Error(PyExc_TypeError, Path + ": unknown or duplicate argument " + KeyName);
                Inputs[I] = Value;
            }
        }
        for (size_t I = 0; I < Count; ++I) if (!Inputs[I])
        {
            const auto* Default = F->GetParameterDefault(I);
            if (!Default || Default->State != EFunctionDefaultState::Reflected)
                return Error(PyExc_TypeError, Path + ": missing required argument " + PropertyName(*F->GetParameterProperty(I)));
            Omitted.push_back(I);
        }
        FFunctionParameterBuffer Buffer(*F); FBudget B;
        if (!Buffer.ApplyDefaults(Omitted)) return Error(PyExc_TypeError, Path + ": reflected defaults unavailable");
        for (size_t I = 0; I < Count; ++I) if (Inputs[I] && !Input(*F->GetParameterProperty(I), Buffer.GetView().data(), Inputs[I], *C, B, Path + "." + PropertyName(*F->GetParameterProperty(I)))) return nullptr;
        // Input conversion cannot run arbitrary iterators or user conversion hooks. Recheck before effects.
        if (!Call.IsAdmitted()) return Error(Registry->AccessError, Path + ": admission closed before native dispatch");
        if (!Resolve(reinterpret_cast<FObject*>(Self->Owner), *C)) return nullptr;
        if (Object->ProcessEvent(F, Buffer.GetView()) != EFunctionInvokeError::None) return Error(Registry->AccessError, Path + ": native dispatch rejected");
        if (Call.GetError()) return Error(Registry->AccessError, Path + ": " + Call.GetError()->Message);
        if (!Call.IsAdmitted()) return Error(Registry->AccessError, Path + ": admission closed during native dispatch");
        std::vector<const FProperty*> Outputs;
        for (size_t I = 0; I < Count; ++I)
        { auto* P = F->GetParameterProperty(I); if (P->HasAnyPropertyFlags(EPropertyFlags::OutParm) && !P->HasAnyPropertyFlags(EPropertyFlags::ConstParm)) Outputs.push_back(P); }
        const auto* Return = F->GetReturnProperty();
        if (Return && Return->GetKind() == K::Bool && !Outputs.empty())
        { if (!*static_cast<const bool*>(Return->GetValuePtr(Buffer.GetView().data()))) { Py_IncRef(Py_None); return Py_None; } }
        else if (Return) Outputs.insert(Outputs.begin(), Return);
        if (Outputs.empty()) { Py_IncRef(Py_None); return Py_None; }
        if (Outputs.size() == 1) return Output(*Outputs[0], Buffer.GetView().data(), *C, B, Path + ".result");
        FPyOwner Tuple(PyTuple_New(Outputs.size())); if (!Tuple.Value) return nullptr;
        for (size_t I = 0; I < Outputs.size(); ++I)
        {
            auto* Value = Output(*Outputs[I], Buffer.GetView().data(), *C, B, Path + "." + PropertyName(*Outputs[I]));
            if (!Value) return nullptr; PyTuple_SetItem(Tuple.Value, I, Value);
        }
        return Tuple.Release();
    }
    static auto CallMember(FMember* Self, PyObject* Args, PyObject* Kwargs) -> PyObject*
    {
        try { return CallMemberImpl(Self, Args, Kwargs); }
        catch (const std::exception& E) { return Error(PyExc_RuntimeError, Qualified(Self->Declaring) + "::" + Self->Function->GetName() + ": " + E.what()); }
        catch (...) { return Error(PyExc_RuntimeError, "Native reflected invocation threw an exception."); }
    }
    static auto GetMemberImpl(FMember* Self, PyObject* Owner, PyObject*) -> PyObject*
    {
        if (!Owner || Owner == Py_None) { Py_IncRef(reinterpret_cast<PyObject*>(Self)); return reinterpret_cast<PyObject*>(Self); }
        auto C = Context(); if (!C) return nullptr; FScopedEditorScriptCall Call(*C);
        if (!Call.IsAdmitted()) return Error(Registry->AccessError, "Script admission closed.");
        if (!ModuleAvailable(Self->Declaring)) return Error(Registry->AccessError, "Native member provider is unavailable.");
        void* Data = nullptr;
        if (Cast<DClass>(Self->Declaring))
        {
            auto* Type = Cast<DClass>(ValueType(Owner));
            if (!Type || !Type->IsChildOf(Cast<DClass>(Self->Declaring))) return Error(PyExc_TypeError, "Incompatible native descriptor target.");
            auto* Object = Resolve(reinterpret_cast<FObject*>(Owner), *C); if (!Object) return nullptr;
            if (Self->Function) return Effective(Self, Object) ? Member(Self->Function, nullptr, Self->Declaring, Owner) : nullptr;
            Data = Object;
        }
        else
        {
            if (ValueType(Owner) != Self->Declaring) return Error(PyExc_TypeError, "Incompatible value descriptor target.");
            auto& S = *reinterpret_cast<FValue*>(Owner)->State; if (!ValidateValue(S, *C)) return nullptr; Data = S.Data;
        }
        FBudget B; return Output(*Self->Property, Data, *C, B, Qualified(Self->Declaring) + "." + PropertyName(*Self->Property));
    }
    static auto GetMember(FMember* Self, PyObject* Owner, PyObject* Type) -> PyObject*
    {
        try { return GetMemberImpl(Self, Owner, Type); }
        catch (const std::exception& E) { return Error(PyExc_RuntimeError, Qualified(Self->Declaring) + ": " + E.what()); }
        catch (...) { return Error(PyExc_RuntimeError, "Native reflected member access threw an exception."); }
    }
    static auto SetMemberImpl(FMember* Self, PyObject* Owner, PyObject* Value) -> int
    {
        if (Value && Self->Property && Cast<DClass>(Self->Declaring))
        {
            auto* Type = Cast<DClass>(ValueType(Owner));
            if (!Type || !Type->IsChildOf(Cast<DClass>(Self->Declaring)))
            { Error(PyExc_TypeError, "Incompatible native descriptor target."); return -1; }
            if (!Self->Property->HasAnyPropertyFlags(EPropertyFlags::ScriptReadWrite))
            { Error(PyExc_AttributeError, "Reflected field is read-only."); return -1; }
            FPyOwner Result(WriteObjectProperty(reinterpret_cast<FObject*>(Owner), *Self->Property, Value));
            return Result.Value ? 0 : -1;
        }
        if (!Value || !Self->Property || !Cast<DStruct>(Self->Declaring)
            || (!Intrinsic(Self->Declaring) && !Self->Property->HasAnyPropertyFlags(EPropertyFlags::ScriptReadWrite)))
        { Error(PyExc_AttributeError, "Reflected field is read-only."); return -1; }
        if (ValueType(Owner) != Self->Declaring) { Error(PyExc_TypeError, "Incompatible value descriptor target."); return -1; }
        auto C = Context(); if (!C) return -1; FScopedEditorScriptCall Call(*C);
        if (!Call.IsAdmitted()) { Error(Registry->AccessError, "Script admission closed."); return -1; }
        auto& S = *reinterpret_cast<FValue*>(Owner)->State; if (!ValidateValue(S, *C)) return -1;
        FPyOwner Temporary(MakeStruct(Cast<DStruct>(Self->Declaring), S.Data, *C));
        if (!Temporary.Value) return -1;
        auto* TemporaryData = reinterpret_cast<FValue*>(Temporary.Value)->State->Data;
        FBudget B; const auto Path = Qualified(Self->Declaring) + "." + PropertyName(*Self->Property);
        if (!Input(*Self->Property, TemporaryData, Value, *C, B, Path)) return -1;
        auto& Draft = *reinterpret_cast<FValue*>(Temporary.Value)->State;
        RefreshRoots(Draft);
        // Publish the fully validated detached value by swapping owned storage, including roots.
        std::swap(S.Data, Draft.Data); std::swap(S.Roots, Draft.Roots);
        S.Host = FObjectKey(C->Host); S.Gate = C->Gate; return 0;
    }
    static auto SetMember(FMember* Self, PyObject* Owner, PyObject* Value) -> int
    {
        try { return SetMemberImpl(Self, Owner, Value); }
        catch (const std::exception& E) { Error(PyExc_RuntimeError, Qualified(Self->Declaring) + ": " + E.what()); return -1; }
        catch (...) { Error(PyExc_RuntimeError, "Native reflected field edit threw an exception."); return -1; }
    }

    auto InitializeValueField(PyTypeObject* Python, PyObject* Instance, const std::string& Field, PyObject* Value) -> int
    {
        auto* Descriptor = PyDict_GetItemString(Python->tp_dict, Field.c_str());
        if (!Descriptor || Py_TYPE(Descriptor) != reinterpret_cast<PyTypeObject*>(Registry->MemberType))
        { Error(PyExc_AttributeError, Qualified(NativeType(reinterpret_cast<PyObject*>(Python))) + ": unknown field " + Field); return -1; }
        return SetMember(reinterpret_cast<FMember*>(Descriptor), Instance, Value);
    }
    auto StartMemberType() -> bool
    {
        static PyGetSetDef Documentation[] = {
            {const_cast<char*>("__doc__"), reinterpret_cast<getter>(+[](FMember* Self, void*) -> PyObject* {
                const auto Text = MemberDocumentation(Self->Declaring, Self->Function, Self->Property);
                return PyUnicode_FromString(Text.c_str());
            }), nullptr, nullptr, nullptr},
            {nullptr, nullptr, nullptr, nullptr, nullptr}};
        PyType_Slot Slots[] = {{Py_tp_new, reinterpret_cast<void*>(Construct)}, {Py_tp_dealloc, reinterpret_cast<void*>(DeallocMember)}, {Py_tp_call, reinterpret_cast<void*>(CallMember)},
            {Py_tp_getset, Documentation},
            {Py_tp_traverse, reinterpret_cast<void*>(TraverseMember)}, {Py_tp_clear, reinterpret_cast<void*>(ClearMember)},
            {Py_tp_descr_get, reinterpret_cast<void*>(GetMember)}, {Py_tp_descr_set, reinterpret_cast<void*>(SetMember)}, {0, nullptr}};
        PyType_Spec Spec{"durin._NativeMember", sizeof(FMember), 0, Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE, Slots};
        Registry->MemberType = PyType_FromSpecWithBases(&Spec, nullptr); if (!Registry->MemberType) return false;
        return true;
    }
    auto PublishMembers(DType* Type, PyObject* Python) -> bool
    {
        auto* Dict = reinterpret_cast<PyTypeObject*>(Python)->tp_dict;
        const auto Description = Qualified(Type) + "\n\nNative reflected type. Use dir() to discover its published members.";
        FPyOwner Doc(PyUnicode_FromString(Description.c_str()));
        if (!Doc.Value || PyDict_SetItemString(Dict, "__doc__", Doc.Value) < 0) return false;
        std::unordered_set<std::string> Names;
        auto Add = [&](const std::string& Name, PyObject* Value) {
            FPyOwner Owner(Value); if (!Value) return false;
            if (!ValidName(Name) || !Names.insert(Name).second) { Error(PyExc_ValueError, Qualified(Type) + ": script member collision " + Name); return false; }
            return PyDict_SetItemString(Dict, Name.c_str(), Value) == 0;
        };
        if (auto* Enum = Cast<DEnum>(Type))
        {
            for (const auto& E : Enum->GetValues())
            {
                if (E.ScriptExposure.Has(EScriptExposure::NoExport)) continue;
                auto N = Name(E.ScriptExposure.Name.empty() ? E.Name.ToString() : E.ScriptExposure.Name);
                for (char& C : N) if (IsLower(C)) C -= 'a' - 'A';
                if (!Add(N, ScalarValue(Python, Enum, E.Value))) return false;
            }
        }
        if (auto* Struct = Cast<DStructBase>(Type))
            for (auto* F = Struct->GetChildProperties(); F; F = F->GetNext())
            {
                const auto& P = *static_cast<FProperty*>(F);
                if (P.HasAnyPropertyFlags(EPropertyFlags::ScriptNoExport)
                    || (!Intrinsic(Type) && !P.HasAnyPropertyFlags(EPropertyFlags::ScriptReadOnly | EPropertyFlags::ScriptReadWrite))) continue;
                if (!Supported(P)) { DURIN_WARN("Omitting unsupported Python property {}.{}", Qualified(Type), P.NamePrivate.ToString()); continue; }
                if (!Add(PropertyName(P), Member(nullptr, &P, Type))) return false;
            }
        bool Success = true;
        if (auto* Class = Cast<DClass>(Type)) Class->ForEachFunction([&](const DFunction& F) {
            if (!Success || !F.GetScriptExposure().Has(EScriptExposure::Callable) || F.GetScriptExposure().Has(EScriptExposure::NoExport)) return;
            if (F.GetReturnProperty() && !Supported(*F.GetReturnProperty())) { DURIN_WARN("Omitting unsupported Python signature {}::{}.return", Qualified(Type), F.GetName()); return; }
            std::unordered_set<std::string> Parameters;
            for (size_t I = 0; I < F.GetNumInputParms(); ++I)
            {
                const auto& P = *F.GetParameterProperty(I); auto N = PropertyName(P);
                if (!Supported(P)) { DURIN_WARN("Omitting unsupported Python signature {}::{}.{}", Qualified(Type), F.GetName(), P.NamePrivate.ToString()); return; }
                if (!ValidName(N) || !Parameters.insert(N).second) { Error(PyExc_ValueError, Qualified(Type) + "::" + F.GetName() + ": invalid/duplicate script parameter " + N); Success = false; return; }
            }
            Success = Add(Name(F.GetScriptExposure().Name.empty() ? F.GetName() : F.GetScriptExposure().Name), Member(&F, nullptr, Type));
        }, false);
        return Success;
    }
    auto VisitPublishedMembers(DType* Type,
        const std::function<void(const std::string&, const DFunction*, const FProperty*)>& Visit) -> void
    {
        auto It = Registry->Types.find(Type); if (It == Registry->Types.end()) return;
        PyObject* Key; PyObject* Value; Py_ssize_t Position = 0;
        auto* Dict = reinterpret_cast<PyTypeObject*>(It->second.Python)->tp_dict;
        while (PyDict_Next(Dict, &Position, &Key, &Value))
        {
            if (Py_TYPE(Value) != reinterpret_cast<PyTypeObject*>(Registry->MemberType)) continue;
            const auto* Member = reinterpret_cast<FMember*>(Value);
            Visit(PyUnicode_AsUTF8(Key), Member->Function, Member->Property);
        }
    }
}
#endif
