#include "PythonReflectionInternal.h"
#if DURIN_WITH_PYTHON
#include "DObject/PropertyValueIterator.h"

namespace Durin::PythonReflection::Private
{
    using K = DurinCodeGen::EPropertyGenFlags;
    auto FBudget::Charge(size_t Size, const std::string& Path, unsigned Depth) -> bool
    {
        if (++Nodes > 65536 || Size > 16 * 1024 * 1024 - Bytes || Depth > 64)
        { Error(PyExc_ValueError, Path + ": reflected conversion budget exceeded"); return false; }
        Bytes += Size; return true;
    }
    static auto ExposablePropertyType(const FProperty& Property) -> bool
    {
        using K = DurinCodeGen::EPropertyGenFlags;
        DType* Type = nullptr;
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
        return !Type || !IsExcluded(Type);
    }
    auto String(PyObject* Value, std::string& Out, const std::string& Path) -> bool
    {
        if (!PyUnicode_Check(Value)) { Error(PyExc_TypeError, Path + ": expected str"); return false; }
        Py_ssize_t Size = 0; const char* Text = PyUnicode_AsUTF8AndSize(Value, &Size);
        if (!Text) { PyErr_Clear(); Error(PyExc_ValueError, Path + ": string is not valid UTF-8"); return false; }
        Out.assign(Text, Size); return true;
    }
    auto CheckText(std::string& Text, bool Guid, const std::string& Path) -> bool
    {
        if (Text.find('\0') != std::string::npos) { Error(PyExc_ValueError, Path + ": embedded NUL"); return false; }
        if (!Guid && Text.size() >= FName::MaxSize) { Error(PyExc_ValueError, Path + ": name exceeds native limit"); return false; }
        if (Guid)
        {
            FGuid Parsed; if (!FGuid::Parse(Text, Parsed)) { Error(PyExc_ValueError, Path + ": invalid GUID"); return false; }
            Text = Parsed.ToString();
        }
        return true;
    }
    auto Supported(const FProperty& P, unsigned Containers, unsigned Depth, std::unordered_set<const DStruct*>* Visiting) -> bool
    {
        if (Depth > 64 || !ExposablePropertyType(P) || !P.HasValueLifecycle() || !P.CanCopyConstructValue() || !P.CanCopyAssignValue()) return false;
        std::unordered_set<const DStruct*> Local;
        if (!Visiting) Visiting = &Local;
        switch (P.GetKind())
        {
        case K::Bool: case K::Int8: case K::Int16: case K::Int32: case K::Int64:
        case K::UInt8: case K::UInt16: case K::UInt32: case K::UInt64:
        case K::Float: case K::Double: case K::String: case K::Name: case K::Guid: case K::Enum: case K::Object: return true;
        case K::Struct:
        {
            auto* S = static_cast<const FStructProperty&>(P).GetStruct();
            if (!S || !S->CanDefaultConstruct() || !S->CanDestroy() || !S->CanCopyConstruct() || !S->CanCopyAssign()) return false;
            if (!Visiting->insert(S).second) return true;
            bool Result = true;
            for (auto* Base = static_cast<DStructBase*>(S); Base && Result; Base = Base->GetSuperStructBase())
                for (auto* F = Base->GetChildProperties(); F && Result; F = F->GetNext())
                    Result = Supported(*static_cast<FProperty*>(F), Containers, Depth + 1, Visiting);
            Visiting->erase(S); return Result;
        }
        case K::Array:
        {
            const auto& A = static_cast<const FArrayProperty&>(P);
            return Containers < 4 && A.HasArrayOps() && A.HasCapability(EArrayOpsFlags::Count | EArrayOpsFlags::RandomAccess | EArrayOpsFlags::DefaultGrow | EArrayOpsFlags::Shrink)
                && Supported(*A.GetInner(), Containers + 1, Depth + 1, Visiting);
        }
        case K::Map:
        {
            const auto& M = static_cast<const FMapProperty&>(P);
            const auto Kind = M.GetKeyProp()->GetKind();
            return Containers < 4 && M.HasMapOps() && M.HasCapability(EMapOpsFlags::Count | EMapOpsFlags::ConstTraversal | EMapOpsFlags::Clear | EMapOpsFlags::Lookup | EMapOpsFlags::Insert)
                && Kind != K::Object && Kind != K::Struct && Kind != K::Array && Kind != K::Map
                && Supported(*M.GetKeyProp(), Containers + 1, Depth + 1, Visiting) && Supported(*M.GetValueProp(), Containers + 1, Depth + 1, Visiting);
        }
        default: return false;
        }
    }
    static auto AccountStruct(DStruct* Struct, const void* Data, FBudget& B, const std::string& Path, unsigned Depth) -> bool
    {
        FPropertyValueIterator It(Struct, Data);
        for (; It; ++It)
        {
            const auto Chain = It.GetPropertyChain(); unsigned Containers = 0;
            for (const auto* P : Chain) Containers += P->GetKind() == K::Array || P->GetKind() == K::Map || P->GetArrayDim() > 1;
            const auto FieldPath = Path + "." + It.GetPropertyPathDebugString();
            if (Containers > 4) { Error(PyExc_ValueError, FieldPath + ": container nesting limit exceeded"); return false; }
            if (!B.Charge(It.Key()->GetValueSize(), FieldPath, Depth + Chain.size())) return false;
            if (It.Key()->GetKind() == K::String && !B.Charge(static_cast<const std::string*>(It.Value())->size(), FieldPath, Depth)) return false;
            if ((It.Key()->GetKind() == K::Float && !std::isfinite(*static_cast<const float*>(It.Value())))
                || (It.Key()->GetKind() == K::Double && !std::isfinite(*static_cast<const double*>(It.Value()))))
            { Error(PyExc_ValueError, FieldPath + ": native number is not finite"); return false; }
            if (It.Key()->GetKind() == K::Enum)
            {
                const auto& E = *static_cast<const FEnumProperty*>(It.Key()); FReflectedValueStorage Copy;
                if (!Copy.CopyConstruct(&E, It.Value())) { Error(PyExc_ValueError, FieldPath + ": enum storage unavailable"); return false; }
                const auto* Record = E.GetEnum()->FindValueRecordByValue(E.GetValueAsUInt64(Copy.GetContainer()));
                if (!Record || Record->ScriptExposure.Has(EScriptExposure::NoExport)) { Error(PyExc_ValueError, FieldPath + ": invalid enum value"); return false; }
            }
        }
        if (It.GetStatus() != EContainerOpResult::Success) { Error(PyExc_ValueError, Path + ": native value traversal failed"); return false; }
        return true;
    }
    template<typename T> auto NumberIn(void* Address, PyObject* Value, const std::string& Path) -> bool
    {
        if (PyBool_Check(Value) || (!PyLong_Check(Value) && !(std::is_floating_point_v<T> && PyFloat_Check(Value))))
        { Error(PyExc_TypeError, Path + ": expected numeric value (bool is not accepted)"); return false; }
        if constexpr (std::is_floating_point_v<T>)
        {
            double Number = PyLong_Check(Value) ? PyLong_AsDouble(Value) : PyFloat_AsDouble(Value);
            if (PyErr_Occurred()) { PyErr_Clear(); Error(PyExc_OverflowError, Path + ": numeric overflow"); return false; }
            if (!std::isfinite(Number)) { Error(PyExc_ValueError, Path + ": expected finite number"); return false; }
            if (std::abs(Number) > std::numeric_limits<T>::max()) { Error(PyExc_OverflowError, Path + ": numeric overflow"); return false; }
            *static_cast<T*>(Address) = static_cast<T>(Number);
        }
        else if constexpr (std::is_signed_v<T>)
        {
            int Overflow = 0; auto Number = PyLong_AsLongLongAndOverflow(Value, &Overflow);
            if (Overflow || Number < std::numeric_limits<T>::min() || Number > std::numeric_limits<T>::max())
            { Error(PyExc_OverflowError, Path + ": integer overflow"); return false; }
            if (PyErr_Occurred()) return false; *static_cast<T*>(Address) = static_cast<T>(Number);
        }
        else
        {
            auto Number = PyLong_AsUnsignedLongLong(Value);
            if (PyErr_Occurred()) { PyErr_Clear(); Error(PyExc_OverflowError, Path + ": unsigned integer overflow"); return false; }
            if (Number > std::numeric_limits<T>::max()) { Error(PyExc_OverflowError, Path + ": integer overflow"); return false; }
            *static_cast<T*>(Address) = static_cast<T>(Number);
        }
        return true;
    }
    auto Input(const FProperty& P, void* Container, PyObject* Value, const FEditorScriptCallContext& C,
        FBudget& B, const std::string& Path, unsigned Depth, uint32 Index, bool Single) -> bool
    {
        if (!B.Charge(P.GetValueSize(), Path, Depth)) return false;
        if (B.Active.contains(Value)) { Error(PyExc_ValueError, Path + ": cyclic container"); return false; }
        if (!Single && P.GetArrayDim() > 1)
        {
            if ((!PyList_CheckExact(Value) && !PyTuple_CheckExact(Value)) || PySequence_Size(Value) != P.GetArrayDim())
            { Error(PyExc_TypeError, Path + ": expected exact fixed array length"); return false; }
            for (uint32 I = 0; I < P.GetArrayDim(); ++I)
                if (!Input(P, Container, PyList_CheckExact(Value) ? PyList_GetItem(Value, I) : PyTuple_GetItem(Value, I), C, B, Path + "[" + std::to_string(I) + "]", Depth + 1, I, true)) return false;
            return true;
        }
        void* Address = P.GetValuePtr(Container, Index);
        switch (P.GetKind())
        {
        case K::Bool:
            if (!PyBool_Check(Value)) { Error(PyExc_TypeError, Path + ": expected bool"); return false; }
            *static_cast<bool*>(Address) = Value == Py_True; return true;
#define DURIN_PY_NUMBER(Kind, Type) case K::Kind: return NumberIn<Type>(Address, Value, Path)
        DURIN_PY_NUMBER(Int8, int8); DURIN_PY_NUMBER(Int16, int16); DURIN_PY_NUMBER(Int32, int32); DURIN_PY_NUMBER(Int64, int64);
        DURIN_PY_NUMBER(UInt8, uint8); DURIN_PY_NUMBER(UInt16, uint16); DURIN_PY_NUMBER(UInt32, uint32); DURIN_PY_NUMBER(UInt64, uint64);
        DURIN_PY_NUMBER(Float, float); DURIN_PY_NUMBER(Double, double);
#undef DURIN_PY_NUMBER
        case K::String: case K::Name: case K::Guid:
        {
            std::string Text;
            auto* Expected = P.GetKind() == K::Name ? Registry->NameType : Registry->GuidType;
            if (P.GetKind() != K::String && reinterpret_cast<PyObject*>(Py_TYPE(Value)) == Expected)
                Text = reinterpret_cast<FValue*>(Value)->State->Text;
            else if (!String(Value, Text, Path)) return false;
            if (!B.Charge(Text.size(), Path, Depth)) return false;
            if (P.GetKind() != K::String && !CheckText(Text, P.GetKind() == K::Guid, Path)) return false;
            if (P.GetKind() == K::String) *static_cast<std::string*>(Address) = Text;
            else if (P.GetKind() == K::Name) *static_cast<FName*>(Address) = FName(Text);
            else if (!FGuid::Parse(Text, *static_cast<FGuid*>(Address))) return false;
            return true;
        }
        case K::Enum:
        {
            auto& E = static_cast<const FEnumProperty&>(P);
            if (ValueType(Value) != E.GetEnum()) { Error(PyExc_TypeError, Path + ": expected exact reflected enum"); return false; }
            auto Number = reinterpret_cast<FValue*>(Value)->State->Enum;
            auto* Record = E.GetEnum()->FindValueRecordByValue(Number);
            if (!Record || Record->ScriptExposure.Has(EScriptExposure::NoExport)) { Error(PyExc_ValueError, Path + ": invalid enum value"); return false; }
            E.SetValueFromUInt64(Container, Number, Index); return true;
        }
        case K::Object:
        {
            const auto& O = static_cast<const FObjectProperty&>(P); DObject* Object = nullptr;
            if (Value != Py_None)
            {
                if (P.GetReferencedClass() == DClass::StaticClass())
                {
                    Object = Cast<DClass>(NativeType(Value));
                    if (Object && !ModuleAvailable(static_cast<DClass*>(Object))) { Error(Registry->AccessError, Path + ": class module is unavailable"); return false; }
                }
                else if (Cast<DClass>(ValueType(Value))) Object = Resolve(reinterpret_cast<FObject*>(Value), C);
                if (!Object || !Object->IsA(P.GetReferencedClass())) { Error(PyExc_TypeError, Path + ": incompatible native object/class"); return false; }
            }
            O.SetObjectPropertyValue(Container, Object, Index); return true;
        }
        case K::Struct:
        {
            auto* S = static_cast<const FStructProperty&>(P).GetStruct();
            if (ValueType(Value) != S) { Error(PyExc_TypeError, Path + ": expected exact reflected struct"); return false; }
            auto& State = *reinterpret_cast<FValue*>(Value)->State;
            if (!ValidateValue(State, C)) return false;
            if (!AccountStruct(S, State.Data, B, Path, Depth)) return false;
            S->GetOps().CopyAssign(Address, State.Data); return true;
        }
        case K::Array: case K::Map:
        {
            if (!B.Active.insert(Value).second) { Error(PyExc_ValueError, Path + ": cyclic container"); return false; }
            struct FPop { FBudget& B; PyObject* V; ~FPop() { B.Active.erase(V); } } Pop{B, Value};
            if (P.GetKind() == K::Array)
            {
                if (!PyList_CheckExact(Value) && !PyTuple_CheckExact(Value)) { Error(PyExc_TypeError, Path + ": expected list or tuple"); return false; }
                const auto Count = PySequence_Size(Value); const auto& A = static_cast<const FArrayProperty&>(P);
                if (Count > 65536) { Error(PyExc_ValueError, Path + ": container node limit exceeded"); return false; }
                if (!B.Charge(Count * A.GetInner()->GetValueSize(), Path, Depth)) return false;
                if (!A.Resize(Container, Count, Index)) { Error(PyExc_ValueError, Path + ": native array resize rejected"); return false; }
                for (Py_ssize_t I = 0; I < Count; ++I)
                    if (!Input(*A.GetInner(), A.GetMutableElementPtr(Container, I, Index), PyList_CheckExact(Value) ? PyList_GetItem(Value, I) : PyTuple_GetItem(Value, I), C, B, Path + "[" + std::to_string(I) + "]", Depth + 1)) return false;
                return true;
            }
            if (!PyDict_CheckExact(Value)) { Error(PyExc_TypeError, Path + ": expected dict"); return false; }
            const auto& M = static_cast<const FMapProperty&>(P); M.Clear(Container, Index);
            Py_ssize_t Pos = 0; PyObject *Key, *Item;
            while (PyDict_Next(Value, &Pos, &Key, &Item))
            {
                FReflectedValueStorage KStorage, VStorage;
                if (!KStorage.DefaultConstruct(M.GetKeyProp()) || !VStorage.DefaultConstruct(M.GetValueProp())) { Error(PyExc_ValueError, Path + ": map storage unavailable"); return false; }
                if (!Input(*M.GetKeyProp(), KStorage.GetContainer(), Key, C, B, Path + ".key", Depth + 1)
                    || !Input(*M.GetValueProp(), VStorage.GetContainer(), Item, C, B, Path + ".value", Depth + 1)) return false;
                if (M.Contains(Container, KStorage.GetValue(), Index)) { Error(PyExc_ValueError, Path + ": converted map key collision"); return false; }
                if (!M.Insert(Container, KStorage.GetValue(), VStorage.GetValue(), Index)) { Error(PyExc_ValueError, Path + ": map insertion rejected"); return false; }
            }
            return true;
        }
        default: Error(PyExc_TypeError, Path + ": unsupported reflected kind"); return false;
        }
    }
    auto Output(const FProperty& P, const void* Container, const FEditorScriptCallContext& C,
        FBudget& B, const std::string& Path, unsigned Depth, uint32 Index, bool Single) -> PyObject*
    {
        if (!B.Charge(P.GetValueSize(), Path, Depth)) return nullptr;
        if (!Single && P.GetArrayDim() > 1)
        {
            FPyOwner List(PyList_New(0)); if (!List.Value) return nullptr;
            for (uint32 I = 0; I < P.GetArrayDim(); ++I)
            { FPyOwner Item(Output(P, Container, C, B, Path, Depth + 1, I, true)); if (!Item.Value || PyList_Append(List.Value, Item.Value) < 0) return nullptr; }
            return List.Release();
        }
        const void* Address = P.GetValuePtr(Container, Index);
        switch (P.GetKind())
        {
        case K::Bool: return PyBool_FromLong(*static_cast<const bool*>(Address));
#define DURIN_PY_SIGNED(Kind, Type) case K::Kind: return PyLong_FromLongLong(*static_cast<const Type*>(Address))
        DURIN_PY_SIGNED(Int8, int8); DURIN_PY_SIGNED(Int16, int16); DURIN_PY_SIGNED(Int32, int32); DURIN_PY_SIGNED(Int64, int64);
#undef DURIN_PY_SIGNED
#define DURIN_PY_UNSIGNED(Kind, Type) case K::Kind: return PyLong_FromUnsignedLongLong(*static_cast<const Type*>(Address))
        DURIN_PY_UNSIGNED(UInt8, uint8); DURIN_PY_UNSIGNED(UInt16, uint16); DURIN_PY_UNSIGNED(UInt32, uint32); DURIN_PY_UNSIGNED(UInt64, uint64);
#undef DURIN_PY_UNSIGNED
        case K::Float: case K::Double:
        {
            const double Number = P.GetKind() == K::Float ? *static_cast<const float*>(Address) : *static_cast<const double*>(Address);
            return std::isfinite(Number) ? PyFloat_FromDouble(Number) : Error(PyExc_ValueError, Path + ": native number is not finite");
        }
        case K::String:
        {
            const auto& Text = *static_cast<const std::string*>(Address);
            return B.Charge(Text.size(), Path, Depth) ? PyUnicode_DecodeUTF8(Text.data(), Text.size(), "strict") : nullptr;
        }
        case K::Name: return ScalarValue(Registry->NameType, nullptr, 0, static_cast<const FName*>(Address)->ToString());
        case K::Guid: return ScalarValue(Registry->GuidType, nullptr, 0, static_cast<const FGuid*>(Address)->ToString());
        case K::Enum:
        {
            const auto& E = static_cast<const FEnumProperty&>(P); auto Number = E.GetValueAsUInt64(Container, Index);
            auto* Record = E.GetEnum()->FindValueRecordByValue(Number);
            if (!Record || Record->ScriptExposure.Has(EScriptExposure::NoExport)) return Error(PyExc_ValueError, Path + ": invalid native enum value");
            FPyOwner Type(Build(E.GetEnum())); return Type.Value ? ScalarValue(Type.Value, E.GetEnum(), Number) : nullptr;
        }
        case K::Object:
        {
            auto* Object = static_cast<const FObjectProperty&>(P).GetObjectPropertyValue(Container, Index);
            if (!Object) { Py_IncRef(Py_None); return Py_None; }
            if (auto* Class = Cast<DClass>(Object)) return Build(Class);
            return Wrap(Object, C);
        }
        case K::Struct:
        {
            auto* S = static_cast<const FStructProperty&>(P).GetStruct();
            if (!AccountStruct(S, Address, B, Path, Depth)) return nullptr;
            return MakeStruct(S, Address, C);
        }
        case K::Array:
        {
            const auto& A = static_cast<const FArrayProperty&>(P); auto Count = A.Num(Container, Index);
            if (!B.Charge(Count * A.GetInner()->GetValueSize(), Path, Depth)) return nullptr;
            FPyOwner List(PyList_New(0)); if (!List.Value) return nullptr;
            for (uint64 I = 0; I < Count; ++I)
            {
                FPyOwner Item(Output(*A.GetInner(), A.GetElementPtr(Container, I, Index), C, B, Path + "[" + std::to_string(I) + "]", Depth + 1));
                if (!Item.Value || PyList_Append(List.Value, Item.Value) < 0) return nullptr;
            }
            return List.Release();
        }
        case K::Map:
        {
            const auto& M = static_cast<const FMapProperty&>(P); FPyOwner Dict(PyDict_New()); if (!Dict.Value) return nullptr;
            struct FVisit { const FMapProperty& M; const FEditorScriptCallContext& C; FBudget& B; const std::string& Path; unsigned Depth; PyObject* Dict; bool Success = true; } V{M, C, B, Path, Depth, Dict.Value};
            const auto Visited = M.VisitEntries(Container, [](void* Context, const void* Key, const void* Value) {
                auto& V = *static_cast<FVisit*>(Context);
                FPyOwner K(Output(*V.M.GetKeyProp(), Key, V.C, V.B, V.Path + ".key", V.Depth + 1));
                FPyOwner Item(Output(*V.M.GetValueProp(), Value, V.C, V.B, V.Path + ".value", V.Depth + 1));
                V.Success = K.Value && Item.Value && PyDict_SetItem(V.Dict, K.Value, Item.Value) == 0; return V.Success;
            }, &V, Index);
            if (Visited != EContainerOpResult::Success && V.Success) return Error(PyExc_ValueError, Path + ": native map traversal failed");
            return V.Success ? Dict.Release() : nullptr;
        }
        default: return Error(PyExc_TypeError, Path + ": unsupported reflected kind");
        }
    }
}
#endif
