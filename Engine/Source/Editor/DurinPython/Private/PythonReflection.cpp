#include "PythonReflectionInternal.h"
#if DURIN_WITH_PYTHON
#include "Editor/EditorEngine.h"
#include "Editor/EditorSubsystem.h"
#include "DObject/Function.h"
#include "DObject/DObjectArray.h"
#include "Modules/ModuleManager.h"

namespace Durin::PythonReflection::Private
{
    std::unique_ptr<FRegistry> Registry;
    auto Error(PyObject* Kind, const std::string& Message) -> PyObject*
    { if (!PyErr_Occurred()) PyErr_SetString(Kind, Message.c_str()); return nullptr; }
    auto IsUpper(char C) -> bool { return C >= 'A' && C <= 'Z'; }
    auto IsLower(char C) -> bool { return C >= 'a' && C <= 'z'; }
    auto Name(std::string Value, bool Type) -> std::string
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
            || Type == DStructBase::StaticClass() || Type == DClass::StaticClass() || Type == DStruct::StaticClass()
            || Type == DEnum::StaticClass() || Type == DFunction::StaticClass()
            || Type == DSubsystem::StaticClass() || Type == DEditorSubsystem::StaticClass();
        // CoreDObject is a linked process-lifetime foundation with no IModuleInterface.
        if (Module == "CoreDObject") return true;
        const auto Info = FModuleManager::Get().FindModule(FName(Module));
        return Info && Info->State == EModuleState::Active && Info->Module && !Info->Module->SupportsDynamicReloading();
    }
    auto IsExcluded(DType* Type) -> bool
    {
        for (auto* Current = Type; Current;)
        {
            if (Current->GetScriptExposure().Has(EScriptExposure::NoExport)) return true;
            auto* Class = Cast<DClass>(Current);
            Current = Class ? Class->GetSuperClass() : nullptr;
        }
        return false;
    }
    static auto Eligible(DType* Type) -> bool
    {
        if (IsExcluded(Type)) return false;
        if (Type->GetScriptExposure().Has(EScriptExposure::Type)) return true;
        bool Exposed = false;
        if (auto* Class = Cast<DClass>(Type))
        {
            for (auto* Parent = Class->GetSuperClass(); Parent; Parent = Parent->GetSuperClass())
                if (Parent->GetScriptExposure().Has(EScriptExposure::Type)) return true;
            Class->ForEachFunction([&](const DFunction& Function) {
                Exposed |= Function.GetScriptExposure().Has(EScriptExposure::Callable) && !Function.GetScriptExposure().Has(EScriptExposure::NoExport);
            });
        }
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
    auto NativeType(PyObject* Python) -> DType*
    {
        if (!Registry) return nullptr;
        const auto Found = Registry->NativeTypes.find(Python);
        return Found != Registry->NativeTypes.end() ? Found->second : nullptr;
    }
    auto ValueType(PyObject* Value) -> DType* { return NativeType(reinterpret_cast<PyObject*>(Py_TYPE(Value))); }
    auto Intrinsic(DType* Type) -> bool
    { return Cast<DStruct>(Type) && Type->HasAnyObjectFlags(EObjectFlags::Intrinsic) && Type->GetScriptExposure().Module == "CoreDObject"; }
    auto PropertyName(const FProperty& P) -> std::string
    {
        auto Native = P.NamePrivate.ToString(); auto Script = P.GetMetaData("ScriptName");
        if (Script.empty() && P.GetKind() == DurinCodeGen::EPropertyGenFlags::Bool && Native.size() > 1 && Native[0] == 'b' && IsUpper(Native[1])) Native.erase(0, 1);
        return Name(Script.empty() ? Native : Script);
    }
    static auto ValidateMembers(DType* Type) -> bool
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
                Success = Add(P.NamePrivate.ToString(), PropertyName(P), "property ");
            }
        return Success;
    }
    static auto VisitPropertyType(const FProperty& Property, const std::function<bool(DType*)>& Visit) -> bool
    {
        DType* Dependency = nullptr;
        using K = DurinCodeGen::EPropertyGenFlags;
        switch (Property.GetKind())
        {
        case K::Object: Dependency = Property.GetReferencedClass(); break;
        case K::Struct: Dependency = static_cast<const FStructProperty&>(Property).GetStruct(); break;
        case K::Enum: Dependency = static_cast<const FEnumProperty&>(Property).GetEnum(); break;
        case K::Array: return VisitPropertyType(*static_cast<const FArrayProperty&>(Property).GetInner(), Visit);
        case K::Map:
            return VisitPropertyType(*static_cast<const FMapProperty&>(Property).GetKeyProp(), Visit)
                && VisitPropertyType(*static_cast<const FMapProperty&>(Property).GetValueProp(), Visit);
        default: break;
        }
        if (!Dependency) return true;
        return Visit(Dependency);
    }
    static auto VisitDependencies(DType* Type, const std::function<bool(DType*)>& Visit) -> bool
    {
        bool Success = true;
        if (auto* Struct = Cast<DStructBase>(Type))
            for (auto* Field = Struct->GetChildProperties(); Field; Field = Field->GetNext())
            {
                const auto& Property = *static_cast<const FProperty*>(Field);
                if (!Property.HasAnyPropertyFlags(EPropertyFlags::ScriptNoExport) &&
                    (Intrinsic(Type) || Property.HasAnyPropertyFlags(EPropertyFlags::ScriptReadOnly | EPropertyFlags::ScriptReadWrite)) && Supported(Property))
                    Success = Success && VisitPropertyType(Property, Visit);
            }
        if (auto* Class = Cast<DClass>(Type)) Class->ForEachFunction([&](const DFunction& Function) {
            if (!Success || !Function.GetScriptExposure().Has(EScriptExposure::Callable)
                || Function.GetScriptExposure().Has(EScriptExposure::NoExport)) return;
            if (Function.GetReturnProperty() && !Supported(*Function.GetReturnProperty())) return;
            for (size_t Index = 0; Index < Function.GetNumInputParms(); ++Index)
                if (!Supported(*Function.GetParameterProperty(Index))) return;
            for (size_t Index = 0; Index < Function.GetNumInputParms() && Success; ++Index)
                Success = VisitPropertyType(*Function.GetParameterProperty(Index), Visit);
            if (Success && Function.GetReturnProperty()) Success = VisitPropertyType(*Function.GetReturnProperty(), Visit);
        }, false);
        return Success;
    }
    static auto RegisterDependencies(DType* Type) -> bool
    {
        return VisitDependencies(Type, [](DType* Dependency) {
            auto* Wrapped = Build(Dependency); if (!Wrapped) return false; Py_DECREF(Wrapped); return true;
        });
    }
    static auto UnavailableDependency(DType* Type, std::unordered_set<DType*>& Visited) -> DType*
    {
        if (!ModuleAvailable(Type)) return Type;
        if (!Visited.insert(Type).second) return nullptr;
        if (auto* Class = Cast<DClass>(Type); Class && Class->GetSuperClass())
            if (auto* Missing = UnavailableDependency(Class->GetSuperClass(), Visited)) return Missing;
        DType* Missing = nullptr;
        VisitDependencies(Type, [&](DType* Dependency) {
            Missing = UnavailableDependency(Dependency, Visited); return Missing == nullptr;
        });
        return Missing;
    }
    auto Build(DType* Type) -> PyObject*
    {
        if (!Type || IsExcluded(Type)) return Error(PyExc_TypeError, "Type is excluded from scripts.");
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
        const auto Slots = ObjectType ? ObjectTypeSlots() : ValueTypeSlots();
        std::vector<PyType_Slot> ActiveSlots(Slots.begin(), Slots.end());
        PyType_Spec Spec{PythonName.c_str(), static_cast<int>(ObjectType ? sizeof(FObject) : sizeof(FValue)), 0,
            Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_IMMUTABLETYPE | (ObjectType ? 0UL : Py_TPFLAGS_HAVE_GC), ActiveSlots.data()};
        auto* Python = PyType_FromSpecWithBases(&Spec, Bases); Py_XDECREF(Bases); if (!Python) return nullptr;
        Registry->Types.emplace(Type, FType{Python, QualifiedName, Alias});
        Registry->NativeTypes.emplace(Python, Type);
        if (!PublishMembers(Type, Python)) return nullptr;
        if (!RegisterDependencies(Type))
        { Registry->NativeTypes.erase(Python); Registry->Types.erase(Type); Py_DECREF(Python); return nullptr; }
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
}

namespace Durin::PythonReflection
{
    using namespace Private;
    auto Start(PyObject* Module, PyObject* AccessError,
        std::function<std::optional<FEditorScriptCallContext>()> GetContext) -> bool
    {
        Registry = std::make_unique<FRegistry>(); Registry->Module = Module; Registry->AccessError = AccessError;
        Registry->Context = std::move(GetContext);
        Registry->PurgeRegistration = std::make_unique<FObjectReferencePurgeRegistration>([](DObject& Object) {
            const FObjectKey Key(&Object);
            for (auto* Proxy : Registry->Objects)
                if (Proxy->State->Target == Key || Proxy->State->Host == Key) Proxy->State->Purge();
            for (auto* Value : Registry->Values)
                if (Value->State->Host == Key || std::ranges::any_of(Value->State->Roots, [&](const auto& Root) { return Root.GetKey() == Key; })) Value->State->Purge();
        });
        return StartMemberType() && StartValueTypes() && Publish();
    }
    auto Stop() -> void
    {
        if (!Registry) return;
        Registry->Active = false; Registry->PurgeRegistration.reset();
        for (auto* Proxy : Registry->Objects) Proxy->State->Purge();
        for (auto* Value : Registry->Values) Value->State->Purge();
        auto Types = std::move(Registry->Types);
        Registry->NativeTypes.clear();
        for (auto& [Type, Wrapped] : Types) Py_DECREF(Wrapped.Python);
        Py_XDECREF(Registry->MemberType); Py_XDECREF(Registry->NameType); Py_XDECREF(Registry->GuidType);
        Registry.reset();
    }
    auto Publish() -> bool
    {
        if (!Registry) return true;
        for (auto* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeTemplates))
            if (auto* Type = Cast<DType>(Object); Type && Eligible(Type))
            {
                // Linked metadata can precede module activation in lean native hosts.
                // Defer the whole dependency graph without publishing a partial type.
                std::unordered_set<DType*> Visited;
                if (auto* Missing = UnavailableDependency(Type, Visited))
                {
                    if (Registry->DeferredTypes.insert(Type).second)
                        DURIN_WARN("Deferring Python type {}: native dependency {} is unresolved, stopped or unloadable.", Qualified(Type), Qualified(Missing));
                    continue;
                }
                Registry->DeferredTypes.erase(Type);
                auto* Wrapped = Build(Type); if (!Wrapped) return false; Py_DECREF(Wrapped);
            }
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
        auto* Class = Cast<DClass>(NativeType(PythonType));
        if (!Class || !Class->IsChildOf(DEditorSubsystem::StaticClass())) return Error(PyExc_TypeError, "Expected a reflected EditorSubsystem class.");
        auto* Service = Value->Host->GetEditorSubsystem(Class);
        if (!Service) return Error(Registry->AccessError, "Editor subsystem is unavailable or retiring.");
        return Wrap(Service, *Value);
    }
    auto PurgeGate(const FSubsystemWorkGate* Gate) -> void
    {
        if (Registry)
        {
            for (auto* Proxy : Registry->Objects) if (Proxy->State->Gate.get() == Gate) Proxy->State->Purge();
            for (auto* Value : Registry->Values) if (Value->State->Gate.get() == Gate) Value->State->Purge();
        }
    }
}
#endif
