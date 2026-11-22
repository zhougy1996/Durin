#include "PythonReflectionInternal.h"
#if DURIN_WITH_PYTHON
#include "Editor/EditorEngine.h"
#include "Engine/Actor.h"
#include "Components/ActorComponent.h"
#include "PropertyEditor/PropertyHandle.h"

namespace Durin::PythonReflection::Private
{
    static auto IsEditorProperty(const FProperty& Property) -> bool
    {
        return Property.HasAnyPropertyFlags(EPropertyFlags::Edit)
            && !Property.HasAnyPropertyFlags(EPropertyFlags::ScriptNoExport);
    }

    static auto SupportsEditorPropertyWrite(const FProperty& Property) -> bool
    {
        return IsEditorProperty(Property) && Property.GetArrayDim() == 1
            && !Property.HasAnyPropertyFlags(EPropertyFlags::ReadOnly | EPropertyFlags::NonTransactional)
            && Supported(Property);
    }

    static auto HasEditorPropertyWriteAccess(FScopedEditorScriptCall& Call, DObject& Object) -> bool
    {
        return (Cast<AActor>(&Object) || Cast<DActorComponent>(&Object))
            && Call.ResolveEditAccess(Object) == EEditorScriptTargetAccess::Writable;
    }

    static auto FindEditorProperty(DClass* Class, PyObject* Name) -> const FProperty*
    {
        std::string Key;
        if (!String(Name, Key, Qualified(Class) + ".editor_property")) return nullptr;
        const FProperty* Found = nullptr;
        for (auto* Type = static_cast<DStructBase*>(Class); Type; Type = Type->GetSuperStructBase())
            for (auto* Field = Type->GetChildProperties(); Field; Field = Field->GetNext())
            {
                const auto* P = static_cast<const FProperty*>(Field);
                if (!IsEditorProperty(*P) || PropertyName(*P) != Key) continue;
                if (Found) { Error(PyExc_AttributeError, Qualified(Class) + ": ambiguous editor property " + Key); return nullptr; }
                Found = P;
            }
        if (!Found || !Supported(*Found))
        { Error(PyExc_AttributeError, Qualified(Class) + ": unknown or unsupported editor property " + Key); return nullptr; }
        return Found;
    }

    auto WriteObjectProperty(FObject* Self, const FProperty& P, PyObject* Value) -> PyObject*
    {
        auto C = Context(); if (!C) return nullptr; FScopedEditorScriptCall Call(*C);
        if (!Call.IsAdmitted()) return Error(Registry->AccessError, "Script admission closed.");
        auto* Object = Resolve(Self, *C); if (!Object) return nullptr;
        const auto Path = Qualified(Object->GetClass()) + "." + PropertyName(P);
        if (!SupportsEditorPropertyWrite(P))
            return Error(PyExc_AttributeError, Path + ": property does not support transactional script writes");
        if (!HasEditorPropertyWriteAccess(Call, *Object))
            return Error(Registry->AccessError, Path + ": target is not owned by the writable editor Level");
        // Complete all input conversion before opening a native edit/history scope.
        FReflectedValueStorage Draft;
        if (!Draft.CopyConstruct(&P, P.GetValuePtr(Object))) return Error(PyExc_ValueError, Path + ": detached property storage is unavailable");
        FBudget Budget;
        if (!Input(P, Draft.GetContainer(), Value, *C, Budget, Path)) return nullptr;
        FPropertyValueSnapshot Proposal;
        if (const auto Capture = CapturePropertyValue(&P, Draft.GetContainer(), 0, Proposal); !Capture)
            return Error(PyExc_ValueError, Path + ": " + ToString(Capture.error()));
        if (!Resolve(Self, *C)) return nullptr;
        if (Call.ResolveEditAccess(*Object) != EEditorScriptTargetAccess::Writable)
            return Error(Registry->AccessError, Path + ": target lost write admission");
        auto* Transactor = C->Host->GetTransactor();
        if (!Transactor) return Error(Registry->AccessError, Path + ": Host history is unavailable");
        Durin::PropertyEditor::FPropertyEdit Session;
        std::string EditError;
        if (!Session.Begin(FPropertyEditTarget::ForMember(Object, &P), {}, *Transactor, &EditError))
            return Error(Registry->AccessError, Path + ": " + EditError);
        auto CloseRejected = [&](std::string* CleanupError = nullptr) {
            return Session.HasChanges() ? Session.Cancel(CleanupError) : Session.CancelUnchanged(CleanupError);
        };
        try
        {
            if (!Session.Apply(Proposal, &EditError))
            {
                std::string CleanupError;
                const bool bCleanedUp = CloseRejected(&CleanupError);
                auto Message = Path + ": " + (Call.GetError() ? Call.GetError()->Message : EditError);
                if (!bCleanedUp) Message += " Cleanup failed: " + CleanupError;
                return Error(Call.GetError() ? Registry->AccessError : PyExc_ValueError, Message);
            }
            if (!Session.Commit(&EditError))
            {
                std::string CleanupError;
                if (!CloseRejected(&CleanupError)) EditError += " Cleanup failed: " + CleanupError;
                return Error(Registry->AccessError, Path + ": " + EditError);
            }
        }
        catch (...)
        {
            if (Session.IsActive()) (void)CloseRejected();
            throw;
        }
        Py_IncRef(Py_None); return Py_None;
    }

    auto GetEditorProperty(FObject* Self, PyObject* Name) -> PyObject*
    {
        try
        {
            auto C = Context(); if (!C) return nullptr; FScopedEditorScriptCall Call(*C);
            if (!Call.IsAdmitted()) return Error(Registry->AccessError, "Script admission closed.");
            auto* Object = Resolve(Self, *C); if (!Object) return nullptr;
            auto* P = FindEditorProperty(Object->GetClass(), Name); if (!P) return nullptr;
            FBudget B; return Output(*P, Object, *C, B, Qualified(Object->GetClass()) + "." + PropertyName(*P));
        }
        catch (const std::exception& E) { return Error(PyExc_RuntimeError, std::string("get_editor_property: ") + E.what()); }
        catch (...) { return Error(PyExc_RuntimeError, "Native editor property read threw an exception."); }
    }

    auto ListEditorProperties(FObject* Self, PyObject*) -> PyObject*
    {
        try
        {
            auto C = Context(); if (!C) return nullptr; FScopedEditorScriptCall Call(*C);
            if (!Call.IsAdmitted()) return Error(Registry->AccessError, "Script admission closed.");
            auto* Object = Resolve(Self, *C); if (!Object) return nullptr;
            const bool CanWrite = HasEditorPropertyWriteAccess(Call, *Object);
            if (!Resolve(Self, *C)) return nullptr;

            std::map<std::string, const FProperty*> Properties;
            Object->GetClass()->ForEachProperty([&](const FProperty* Property) {
                if (!IsEditorProperty(*Property)) return;
                const auto [Entry, Added] = Properties.emplace(PropertyName(*Property), Property);
                if (!Added) Entry->second = nullptr;
            });

            FPyOwner Result(PyDict_New());
            if (!Result.Value) return nullptr;
            for (const auto& [Name, Property] : Properties)
            {
                if (!Property || !Supported(*Property)) continue;
                FPyOwner Info(PyDict_New());
                FPyOwner Type(PyUnicode_FromString(EditorPropertyType(*Property).c_str()));
                FPyOwner Writable(PyBool_FromLong(CanWrite && SupportsEditorPropertyWrite(*Property)));
                if (!Info.Value || !Type.Value || !Writable.Value
                    || PyDict_SetItemString(Info.Value, "type", Type.Value) < 0
                    || PyDict_SetItemString(Info.Value, "writable", Writable.Value) < 0
                    || PyDict_SetItemString(Result.Value, Name.c_str(), Info.Value) < 0) return nullptr;
            }
            return Result.Release();
        }
        catch (const std::exception& E) { return Error(PyExc_RuntimeError, std::string("list_editor_properties: ") + E.what()); }
        catch (...) { return Error(PyExc_RuntimeError, "Native editor property discovery threw an exception."); }
    }

    auto SetEditorProperty(FObject* Self, PyObject* Args) -> PyObject*
    {
        try
        {
            PyObject *Name, *Value;
            if (!PyArg_ParseTuple(Args, "OO", &Name, &Value)) return nullptr;
            auto C = Context(); if (!C) return nullptr; FScopedEditorScriptCall Call(*C);
            if (!Call.IsAdmitted()) return Error(Registry->AccessError, "Script admission closed.");
            auto* Object = Resolve(Self, *C); if (!Object) return nullptr;
            auto* P = FindEditorProperty(Object->GetClass(), Name);
            return P ? WriteObjectProperty(Self, *P, Value) : nullptr;
        }
        catch (const std::exception& E) { return Error(PyExc_RuntimeError, std::string("set_editor_property: ") + E.what()); }
        catch (...) { return Error(PyExc_RuntimeError, "Native editor property write threw an exception."); }
    }
}
#endif
