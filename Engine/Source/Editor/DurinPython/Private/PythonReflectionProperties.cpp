#include "PythonReflectionInternal.h"
#if DURIN_WITH_PYTHON
#include "Editor/EditorEngine.h"
#include "Engine/Actor.h"
#include "Components/ActorComponent.h"
#include "PropertyEditor/PropertyEditing.h"

namespace Durin::PythonReflection::Private
{
    static auto FindEditorProperty(DClass* Class, PyObject* Name) -> const FProperty*
    {
        std::string Key;
        if (!String(Name, Key, Qualified(Class) + ".editor_property")) return nullptr;
        const FProperty* Found = nullptr;
        for (auto* Type = static_cast<DStructBase*>(Class); Type; Type = Type->GetSuperStructBase())
            for (auto* Field = Type->GetChildProperties(); Field; Field = Field->GetNext())
            {
                const auto* P = static_cast<const FProperty*>(Field);
                if (!P->HasAnyPropertyFlags(EPropertyFlags::Edit)
                    || P->HasAnyPropertyFlags(EPropertyFlags::ScriptNoExport) || PropertyName(*P) != Key) continue;
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
        if (!P.HasAnyPropertyFlags(EPropertyFlags::Edit)
            || P.HasAnyPropertyFlags(EPropertyFlags::ReadOnly | EPropertyFlags::NonTransactional | EPropertyFlags::ScriptNoExport)
            || P.GetArrayDim() != 1 || !Supported(P))
            return Error(PyExc_AttributeError, Path + ": property does not support transactional script writes");
        if ((!Cast<AActor>(Object) && !Cast<DActorComponent>(Object))
            || Call.ResolveEditAccess(*Object) != EEditorScriptTargetAccess::Writable)
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
        Editor::FPropertyEditSession Session;
        if (const auto Begun = Session.Begin(Editor::FPropertyEditTarget::ForMember(Object, &P), {}, *Transactor); !Begun)
            return Error(Registry->AccessError, Path + ": " + Begun.Message);
        auto CloseRejected = [&]() {
            return !Session.HasChanges() && !Session.HasPendingDeferredEdit() ? Session.CancelUnchanged() : Session.Cancel();
        };
        try
        {
            const auto Applied = Session.Apply(Proposal);
            if (!Applied || Applied.Status == Editor::EPropertyEditResult::Pending)
            {
                const auto Cleanup = CloseRejected();
                auto Message = Path + ": " + (Call.GetError() ? Call.GetError()->Message : Applied.Message);
                if (!Cleanup) Message += " Cleanup failed: " + Cleanup.Message;
                return Error(Call.GetError() ? Registry->AccessError : PyExc_ValueError, Message);
            }
            if (const auto Committed = Session.Commit(); !Committed)
            {
                const auto Cleanup = CloseRejected();
                return Error(Registry->AccessError, Path + ": " + Committed.Message + (Cleanup ? "" : " Cleanup failed: " + Cleanup.Message));
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
