#include "CoreMinimal.h"
#if DURIN_WITH_PYTHON
#pragma push_macro("_DEBUG")
#undef _DEBUG
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#pragma pop_macro("_DEBUG")
#include "PythonABI.h"
#include "PythonReflection.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorSubsystem.h"
#include "DObject/Class.h"
#include "DObject/Function.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/StrongObjectPtr.h"
#include "Modules/ModuleManager.h"

namespace Durin::PythonReflection
{
    namespace
    {
        struct FObjectState
        {
            FObjectKey Host, Target;
            std::shared_ptr<const FSubsystemWorkGate> Gate;
            std::shared_ptr<const FSubsystemWorkGate> ServiceGate;
            FStrongObjectPtr Root;
            bool Purged = false;
            auto Purge() -> void { Purged = true; Root.Reset(); }
        };
        struct FObject { PyObject_HEAD FObjectState* State; };
        struct FType { PyObject* Python = nullptr; std::string Qualified, Alias; };
        struct FRegistry
        {
            PyObject* Module;
            PyObject* AccessError;
            std::function<std::optional<FEditorScriptCallContext>()> Context;
            std::map<DType*, FType> Types;
            std::map<std::string, DType*> Aliases;
            // Non-owning observations, removed synchronously by tp_dealloc (UE factory model).
            std::vector<FObject*> Objects;
            std::unique_ptr<FObjectReferencePurgeRegistration> PurgeRegistration;
            bool Active = true;
        };
        std::unique_ptr<FRegistry> Registry;
        auto Error(PyObject* Kind, const std::string& Message) -> PyObject*
        { if (!PyErr_Occurred()) PyErr_SetString(Kind, Message.c_str()); return nullptr; }
        auto IsUpper(char C) -> bool { return C >= 'A' && C <= 'Z'; }
        auto IsLower(char C) -> bool { return C >= 'a' && C <= 'z'; }
        auto Name(std::string Value, bool Type = false) -> std::string
        {
            if (Type)
            {
                if (Value.size() > 1 && std::string_view("DAFE").contains(Value[0]) && IsUpper(Value[1])) Value.erase(0, 1);
                return Value;
            }
            std::string Result;
            for (size_t Index = 0; Index < Value.size(); ++Index)
            {
                const auto C = Value[Index];
                if (IsUpper(C) && Index && Value[Index - 1] != '_' &&
                    (IsLower(Value[Index - 1]) || (Index + 1 < Value.size() && IsLower(Value[Index + 1])))) Result += '_';
                Result += IsUpper(C) ? static_cast<char>(C + ('a' - 'A')) : C;
            }
            return Result;
        }
        auto ValidName(const std::string& Value) -> bool
        {
            static const std::unordered_set<std::string> Keywords{"False", "None", "True", "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del", "elif", "else", "except", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try", "while", "with", "yield"};
            FScriptExposure Test; Test.Name = Value;
            return !Value.empty() && Test.Validate() && !Keywords.contains(Value) && !Value.starts_with('_');
        }
        auto Qualified(DType* Type) -> std::string
        {
            if (auto* Class = Cast<DClass>(Type)) return Class->GetQualifiedName().ToString();
            if (auto* Struct = Cast<DStruct>(Type)) return Struct->GetQualifiedName().ToString();
            if (auto* Enum = Cast<DEnum>(Type)) return Enum->GetQualifiedName().ToString();
            return Type->GetName();
        }
        auto ModuleAvailable(DType* Type) -> bool
        {
            const auto& Module = Type->GetScriptExposure().Module;
            if (Module.empty()) return Type == DObject::StaticClass() || Type == DType::StaticClass()
                || Type == DStructBase::StaticClass() || Type == DSubsystem::StaticClass() || Type == DEditorSubsystem::StaticClass();
            // CoreDObject is a linked process-lifetime foundation with no IModuleInterface.
            if (Module == "CoreDObject") return true;
            const auto Info = FModuleManager::Get().FindModule(FName(Module));
            return Info && Info->State == EModuleState::Active && Info->Module && !Info->Module->SupportsDynamicReloading();
        }
        auto Eligible(DType* Type) -> bool
        {
            if (Type->GetScriptExposure().Has(EScriptExposure::NoExport)) return false;
            if (Type->GetScriptExposure().Has(EScriptExposure::Type)) return true;
            bool Exposed = false;
            if (auto* Class = Cast<DClass>(Type))
                Class->ForEachFunction([&](const DFunction& Function) {
                    Exposed |= Function.GetScriptExposure().Has(EScriptExposure::Callable) && !Function.GetScriptExposure().Has(EScriptExposure::NoExport);
                });
            if (auto* Struct = Cast<DStructBase>(Type))
                for (auto* Field = Struct->GetChildProperties(); Field; Field = Field->GetNext())
                    if (auto* Property = static_cast<FProperty*>(Field); !Property->HasAnyPropertyFlags(EPropertyFlags::ScriptNoExport))
                        Exposed |= Property->HasAnyPropertyFlags(EPropertyFlags::ScriptReadOnly | EPropertyFlags::ScriptReadWrite);
            return Exposed;
        }
        auto Context() -> std::optional<FEditorScriptCallContext>
        {
            if (!Registry || !Registry->Active) { Error(PyExc_RuntimeError, "Reflection interpreter is unavailable."); return {}; }
            auto Value = Registry->Context();
            if (!Value) Error(Registry->AccessError, "Reflection access requires an active script job.");
            return Value;
        }
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
        auto ValidateMembers(DType* Type) -> bool
        {
            std::map<std::string, std::string> Members;
            auto Add = [&](std::string Native, std::string Script, std::string Kind) {
                const auto Alias = Name(Script.empty() ? Native : Script);
                static const std::unordered_set<std::string> Reserved{"get_name", "get_class", "get_editor_property", "set_editor_property"};
                if (!ValidName(Alias) || Reserved.contains(Alias))
                { Error(PyExc_ValueError, "Invalid or reserved reflected member " + Qualified(Type) + "::" + Native); return false; }
                const auto Identity = Kind + Native;
                auto [It, Inserted] = Members.emplace(Alias, Identity);
                if (!Inserted && It->second != Identity)
                { Error(PyExc_ValueError, "Reflected member name collision on " + Qualified(Type) + ": " + It->second + " and " + Identity); return false; }
                return true;
            };
            bool Success = true;
            if (auto* Class = Cast<DClass>(Type)) Class->ForEachFunction([&](const DFunction& Function) {
                const auto& Exposure = Function.GetScriptExposure();
                if (Success && Exposure.Has(EScriptExposure::Callable) && !Exposure.Has(EScriptExposure::NoExport))
                    Success = Add(Function.GetName(), Exposure.Name, "function ");
            });
            for (auto* Struct = Cast<DStructBase>(Type); Struct && Success; Struct = Struct->GetSuperStructBase())
                for (auto* Field = Struct->GetChildProperties(); Field && Success; Field = Field->GetNext())
                {
                    const auto& P = *static_cast<const FProperty*>(Field);
                    if (P.HasAnyPropertyFlags(EPropertyFlags::ScriptNoExport)
                        || !P.HasAnyPropertyFlags(EPropertyFlags::ScriptReadOnly | EPropertyFlags::ScriptReadWrite)) continue;
                    auto Native = P.NamePrivate.ToString();
                    auto Script = P.GetMetaData("ScriptName");
                    if (Script.empty() && P.GetKind() == DurinCodeGen::EPropertyGenFlags::Bool && Native.size() > 1 && Native[0] == 'b' && IsUpper(Native[1]))
                        Script = Native.substr(1);
                    Success = Add(Native, Script, "property ");
                }
            return Success;
        }
        auto Build(DType* Type) -> PyObject*;
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
            {"__init_subclass__", Subclass, METH_CLASS | METH_NOARGS, nullptr}, {nullptr, nullptr, 0, nullptr}};
        auto ExposablePropertyType(const FProperty& Property) -> bool
        {
            using K = DurinCodeGen::EPropertyGenFlags;
            const DType* Type = nullptr;
            switch (Property.GetKind())
            {
            case K::Object: Type = Property.GetReferencedClass(); break;
            case K::Struct: Type = static_cast<const FStructProperty&>(Property).GetStruct(); break;
            case K::Enum: Type = static_cast<const FEnumProperty&>(Property).GetEnum(); break;
            case K::Array: return ExposablePropertyType(*static_cast<const FArrayProperty&>(Property).GetInner());
            case K::Map:
                return ExposablePropertyType(*static_cast<const FMapProperty&>(Property).GetKeyProp())
                    && ExposablePropertyType(*static_cast<const FMapProperty&>(Property).GetValueProp());
            default: break;
            }
            return !Type || !Type->GetScriptExposure().Has(EScriptExposure::NoExport);
        }
        PyMethodDef ValueTypeMethods[] = {
            {"__init_subclass__", Subclass, METH_CLASS | METH_NOARGS, nullptr}, {nullptr, nullptr, 0, nullptr}};
        auto RegisterPropertyType(const FProperty& Property) -> bool
        {
            DType* Dependency = nullptr;
            using K = DurinCodeGen::EPropertyGenFlags;
            switch (Property.GetKind())
            {
            case K::Object: Dependency = Property.GetReferencedClass(); break;
            case K::Struct: Dependency = static_cast<const FStructProperty&>(Property).GetStruct(); break;
            case K::Enum: Dependency = static_cast<const FEnumProperty&>(Property).GetEnum(); break;
            case K::Array: return RegisterPropertyType(*static_cast<const FArrayProperty&>(Property).GetInner());
            case K::Map:
                return RegisterPropertyType(*static_cast<const FMapProperty&>(Property).GetKeyProp())
                    && RegisterPropertyType(*static_cast<const FMapProperty&>(Property).GetValueProp());
            default: break;
            }
            if (!Dependency) return true;
            auto* Wrapped = Build(Dependency); if (!Wrapped) return false; Py_DECREF(Wrapped); return true;
        }
        auto RegisterDependencies(DType* Type) -> bool
        {
            bool Success = true;
            if (auto* Struct = Cast<DStructBase>(Type))
                for (auto* Field = Struct->GetChildProperties(); Field; Field = Field->GetNext())
                {
                    const auto& Property = *static_cast<const FProperty*>(Field);
                    if (!Property.HasAnyPropertyFlags(EPropertyFlags::ScriptNoExport) &&
                        Property.HasAnyPropertyFlags(EPropertyFlags::ScriptReadOnly | EPropertyFlags::ScriptReadWrite) && ExposablePropertyType(Property))
                        Success = Success && RegisterPropertyType(Property);
                }
            if (auto* Class = Cast<DClass>(Type)) Class->ForEachFunction([&](const DFunction& Function) {
                if (!Success || !Function.GetScriptExposure().Has(EScriptExposure::Callable)
                    || Function.GetScriptExposure().Has(EScriptExposure::NoExport)) return;
                if (Function.GetReturnProperty() && !ExposablePropertyType(*Function.GetReturnProperty())) return;
                for (size_t Index = 0; Index < Function.GetNumInputParms(); ++Index)
                    if (!ExposablePropertyType(*Function.GetParameterProperty(Index))) return;
                for (size_t Index = 0; Index < Function.GetNumInputParms() && Success; ++Index)
                    Success = RegisterPropertyType(*Function.GetParameterProperty(Index));
                if (Success && Function.GetReturnProperty()) Success = RegisterPropertyType(*Function.GetReturnProperty());
            }, false);
            return Success;
        }
        auto Build(DType* Type) -> PyObject*
        {
            if (!Type || Type->GetScriptExposure().Has(EScriptExposure::NoExport)) return Error(PyExc_TypeError, "Type is excluded from scripts.");
            if (auto It = Registry->Types.find(Type); It != Registry->Types.end())
            { Py_IncRef(It->second.Python); return It->second.Python; }
            if (!ModuleAvailable(Type)) return Error(Registry->AccessError, "Cannot export type with unresolved, stopped or unloadable native module: " + Qualified(Type));
            if (!ValidateMembers(Type)) return nullptr;
            const auto QualifiedName = Qualified(Type);
            auto ShortName = QualifiedName.substr(QualifiedName.rfind("::") == std::string::npos ? 0 : QualifiedName.rfind("::") + 2);
            const auto Alias = Type->GetScriptExposure().Name.empty() ? Name(ShortName, true) : Type->GetScriptExposure().Name;
            if (!ValidName(Alias)) return Error(PyExc_ValueError, "Invalid script type name: " + QualifiedName);
            PyObject* Bases = nullptr;
            const bool ObjectType = Cast<DClass>(Type) != nullptr;
            if (auto* Class = Cast<DClass>(Type); Class && Class->GetSuperClass())
            {
                auto* Base = Build(Class->GetSuperClass()); if (!Base) return nullptr;
                Bases = PyTuple_Pack(1, Base); Py_DECREF(Base); if (!Bases) return nullptr;
            }
            const auto PythonName = "durin." + Alias;
            PyType_Slot Slots[] = {{Py_tp_new, reinterpret_cast<void*>(Construct)},
                {Py_tp_dealloc, ObjectType ? reinterpret_cast<void*>(Dealloc) : nullptr},
                {Py_tp_repr, ObjectType ? reinterpret_cast<void*>(Repr) : nullptr},
                {Py_tp_hash, ObjectType ? reinterpret_cast<void*>(Hash) : nullptr},
                {Py_tp_richcompare, ObjectType ? reinterpret_cast<void*>(Compare) : nullptr},
                {Py_tp_methods, ObjectType ? reinterpret_cast<void*>(ObjectMethods) : reinterpret_cast<void*>(ValueTypeMethods)}, {0, nullptr}};
            // Value types are registered now; their construction/conversion comes in Stage 2.
            std::vector<PyType_Slot> ActiveSlots;
            for (auto Slot : Slots) if (Slot.pfunc || Slot.slot == 0) ActiveSlots.push_back(Slot);
            PyType_Spec Spec{PythonName.c_str(), static_cast<int>(ObjectType ? sizeof(FObject) : sizeof(PyObject)), 0,
                Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_IMMUTABLETYPE, ActiveSlots.data()};
            auto* Python = PyType_FromSpecWithBases(&Spec, Bases); Py_XDECREF(Bases); if (!Python) return nullptr;
            Registry->Types.emplace(Type, FType{Python, QualifiedName, Alias});
            if (!RegisterDependencies(Type))
            { Registry->Types.erase(Type); Py_DECREF(Python); return nullptr; }
            auto [It, Inserted] = Registry->Aliases.emplace(Alias, Type);
            if (!Inserted && It->second != Type)
            {
                if (It->second) DURIN_WARN("Ambiguous Python type alias {}: {} and {}; use durin.get_type.", Alias, Qualified(It->second), QualifiedName);
                It->second = nullptr;
                if (PyDict_GetItemString(PyModule_GetDict(Registry->Module), Alias.c_str()))
                    if (PyObject_SetAttrString(Registry->Module, Alias.c_str(), nullptr) < 0) return nullptr;
            }
            else if (Inserted)
            {
                if (PyDict_GetItemString(PyModule_GetDict(Registry->Module), Alias.c_str()))
                    return Error(PyExc_ValueError, "Script type collides with reserved durin member: " + Alias);
                if (PyModule_AddObjectRef(Registry->Module, Alias.c_str(), Python) < 0) return nullptr;
            }
            Py_IncRef(Python); return Python;
        }
        auto Wrap(DObject* Object, const FEditorScriptCallContext& Context) -> PyObject*
        {
            if (!IsValid(Object)) return Error(PyExc_ReferenceError, "Native object is unavailable.");
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
    }
    auto Start(PyObject* Module, PyObject* AccessError,
        std::function<std::optional<FEditorScriptCallContext>()> GetContext) -> bool
    {
        Registry = std::make_unique<FRegistry>(); Registry->Module = Module; Registry->AccessError = AccessError;
        Registry->Context = std::move(GetContext);
        Registry->PurgeRegistration = std::make_unique<FObjectReferencePurgeRegistration>([](DObject& Object) {
            const FObjectKey Key(&Object);
            for (auto* Proxy : Registry->Objects)
                if (Proxy->State->Target == Key || Proxy->State->Host == Key) Proxy->State->Purge();
        });
        return Publish();
    }
    auto Stop() -> void
    {
        if (!Registry) return;
        Registry->Active = false; Registry->PurgeRegistration.reset();
        for (auto* Proxy : Registry->Objects) Proxy->State->Purge();
        auto Types = std::move(Registry->Types);
        for (auto& [Type, Wrapped] : Types) Py_DECREF(Wrapped.Python);
        Registry.reset();
    }
    auto Publish() -> bool
    {
        if (!Registry) return true;
        for (auto* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeTemplates))
            if (auto* Type = Cast<DType>(Object); Type && Eligible(Type))
            { auto* Wrapped = Build(Type); if (!Wrapped) return false; Py_DECREF(Wrapped); }
        return true;
    }
    auto GetType(std::string_view QualifiedName) -> PyObject*
    {
        DType* Type = FindClassByQualifiedName(FName(QualifiedName));
        if (!Type) Type = FindStructByQualifiedName(FName(QualifiedName));
        if (!Type) Type = FindEnumByQualifiedName(FName(QualifiedName));
        if (!Type || (!Eligible(Type) && !Registry->Types.contains(Type))) return Error(PyExc_AttributeError, "Unknown or unexposed native type: " + std::string(QualifiedName));
        return Build(Type);
    }
    auto GetEditorSubsystem(PyObject* PythonType) -> PyObject*
    {
        auto Value = Context(); if (!Value) return nullptr;
        FScopedEditorScriptCall Call(*Value); if (!Call.IsAdmitted()) return Error(Registry->AccessError, "Script admission closed.");
        DClass* Class = nullptr;
        for (const auto& [Native, Type] : Registry->Types) if (Type.Python == PythonType) Class = Cast<DClass>(Native);
        if (!Class || !Class->IsChildOf(DEditorSubsystem::StaticClass())) return Error(PyExc_TypeError, "Expected a reflected EditorSubsystem class.");
        auto* Service = Value->Host->GetEditorSubsystem(Class);
        if (!Service) return Error(Registry->AccessError, "Editor subsystem is unavailable or retiring.");
        return Wrap(Service, *Value);
    }
    auto PurgeGate(const FSubsystemWorkGate* Gate) -> void
    {
        if (Registry) for (auto* Proxy : Registry->Objects) if (Proxy->State->Gate.get() == Gate) Proxy->State->Purge();
    }
}
#endif
