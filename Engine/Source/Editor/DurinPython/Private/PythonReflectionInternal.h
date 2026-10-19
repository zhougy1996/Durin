#pragma once
#include "CoreMinimal.h"
#if DURIN_WITH_PYTHON
#pragma push_macro("_DEBUG")
#undef _DEBUG
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#pragma pop_macro("_DEBUG")
#include "PythonABI.h"
#include "PythonReflection.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/StrongObjectPtr.h"

// Interpreter implementation only. No CPython type or private state reaches native public APIs.
namespace Durin::PythonReflection::Private
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
    struct FValueState;
    struct FValue { PyObject_HEAD FValueState* State; };
    struct FType { PyObject* Python = nullptr; std::string Qualified, Alias; };
    struct FRegistry
    {
        PyObject* Module;
        PyObject* AccessError;
        std::function<std::optional<FEditorScriptCallContext>()> Context;
        std::map<DType*, FType> Types;
        std::map<std::string, DType*> Aliases;
        // Borrowed reverse index; Types owns the Python type references.
        std::unordered_map<PyObject*, DType*> NativeTypes;
        // Non-owning observations, removed synchronously by tp_dealloc (UE factory model).
        std::vector<FObject*> Objects;
        std::vector<FValue*> Values;
        std::unordered_set<DType*> DeferredTypes;
        PyObject* MemberType = nullptr;
        PyObject* NameType = nullptr;
        PyObject* GuidType = nullptr;
        std::unique_ptr<FObjectReferencePurgeRegistration> PurgeRegistration;
        bool Active = true;
    };
    struct FValueState
    {
        DType* Type = nullptr;
        void* Data = nullptr;
        uint64 Enum = 0;
        std::string Text;
        FName NativeName;
        bool IsName = false;
        FObjectKey Host;
        std::shared_ptr<const FSubsystemWorkGate> Gate;
        std::vector<FStrongObjectPtr> Roots;
        bool Purged = false;
        auto Purge() -> void;
        ~FValueState();
    };
    struct FPyOwner
    {
        PyObject* Value = nullptr;
        explicit FPyOwner(PyObject* In = nullptr) : Value(In) {}
        ~FPyOwner() { Py_XDECREF(Value); }
        FPyOwner(const FPyOwner&) = delete;
        auto Release() -> PyObject* { return std::exchange(Value, nullptr); }
    };
    struct FBudget
    {
        size_t Nodes = 0, Bytes = 0;
        std::unordered_set<PyObject*> Active;
        auto Charge(size_t Size, const std::string& Path, unsigned Depth) -> bool;
    };

    // Registry and exposure policy. Python types are interpreter-local.
    extern std::unique_ptr<FRegistry> Registry;
    auto Error(PyObject* Kind, const std::string& Message) -> PyObject*;
    auto IsUpper(char C) -> bool;
    auto IsLower(char C) -> bool;
    auto Name(std::string Value, bool Type = false) -> std::string;
    auto ValidName(const std::string& Value) -> bool;
    auto Qualified(DType* Type) -> std::string;
    auto ModuleAvailable(DType* Type) -> bool;
    auto IsExcluded(DType* Type) -> bool;
    auto Context() -> std::optional<FEditorScriptCallContext>;
    auto NativeType(PyObject* Python) -> DType*;
    auto ValueType(PyObject* Value) -> DType*;
    auto Intrinsic(DType* Type) -> bool;
    auto PropertyName(const FProperty& Property) -> std::string;
    auto Build(DType* Type) -> PyObject*;

    // Native object identity, admission and Python slots.
    auto Resolve(FObject* Self, const FEditorScriptCallContext& Context) -> DObject*;
    auto Wrap(DObject* Object, const FEditorScriptCallContext& Context) -> PyObject*;
    auto Construct(PyTypeObject*, PyObject*, PyObject*) -> PyObject*;
    auto Subclass(PyObject*, PyObject*) -> PyObject*;
    auto ObjectTypeSlots() -> std::span<const PyType_Slot>;

    // Owned value storage and reference retirement.
    auto RefreshRoots(FValueState& State) -> void;
    auto ValidateValue(FValueState& State, const FEditorScriptCallContext& Context) -> bool;
    auto MakeStruct(DStruct* Struct, const void* Source, const FEditorScriptCallContext& Context) -> PyObject*;
    auto ScalarValue(PyObject* Python, DType* Type, uint64 Enum, std::string Text = {}) -> PyObject*;
    auto ValueTypeSlots() -> std::span<const PyType_Slot>;
    auto StartValueTypes() -> bool;

    // Schema support and value conversion, sharing one budget across a native call.
    auto String(PyObject* Value, std::string& Out, const std::string& Path) -> bool;
    auto CheckText(std::string& Text, bool Guid, const std::string& Path) -> bool;
    auto Supported(const FProperty& Property, unsigned Containers = 0, unsigned Depth = 0,
        std::unordered_set<const DStruct*>* Visiting = nullptr) -> bool;
    auto Input(const FProperty& Property, void* Container, PyObject* Value, const FEditorScriptCallContext& Context,
        FBudget& Budget, const std::string& Path, unsigned Depth = 0, uint32 Index = 0, bool Single = false) -> bool;
    auto Output(const FProperty& Property, const void* Container, const FEditorScriptCallContext& Context,
        FBudget& Budget, const std::string& Path, unsigned Depth = 0, uint32 Index = 0, bool Single = false) -> PyObject*;

    // Descriptor ownership, member publication and reflected invocation.
    auto StartMemberType() -> bool;
    auto PublishMembers(DType* Type, PyObject* Python) -> bool;
    auto VisitPublishedMembers(DType* Type,
        const std::function<void(const std::string&, const DFunction*, const FProperty*)>& Visit) -> void;
    auto MemberDocumentation(DType* Type, const DFunction* Function, const FProperty* Property) -> std::string;
    auto InitializeValueField(PyTypeObject* Python, PyObject* Instance, const std::string& Field, PyObject* Value) -> int;

    // Editor-object writes share the native property session and Host history.
    auto WriteObjectProperty(FObject* Self, const FProperty& Property, PyObject* Value) -> PyObject*;
    auto GetEditorProperty(FObject* Self, PyObject* Name) -> PyObject*;
    auto SetEditorProperty(FObject* Self, PyObject* Args) -> PyObject*;
}
#endif
