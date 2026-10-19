#include "PythonReflectionInternal.h"
#if DURIN_WITH_PYTHON
#include "DObject/FunctionParameters.h"

namespace Durin::PythonReflection::Private
{
    using K = DurinCodeGen::EPropertyGenFlags;
    using FNames = std::map<DType*, std::string>;

    static auto TypeName(DType* Type, const FNames& Names) -> std::string
    {
        auto It = Names.find(Type);
        return It == Names.end() ? "Any" : It->second;
    }
    static auto Annotation(const FProperty& P, const FNames& Names, bool Input = false, bool Element = false) -> std::string
    {
        if (!Element && P.GetArrayDim() > 1)
        {
            const auto Item = Annotation(P, Names, Input, true);
            return Input ? "list[" + Item + "] | tuple[" + Item + ", ...]" : "list[" + Item + "]";
        }
        switch (P.GetKind())
        {
        case K::Bool: return "bool";
        case K::Float: case K::Double: return Input ? "float | int" : "float";
        case K::String: return "str";
        case K::Name: return Input ? "Name | str" : "Name";
        case K::Guid: return Input ? "Guid | str" : "Guid";
        case K::Enum: return TypeName(static_cast<const FEnumProperty&>(P).GetEnum(), Names);
        case K::Struct: return TypeName(static_cast<const FStructProperty&>(P).GetStruct(), Names);
        case K::Object:
            return P.GetReferencedClass() == DClass::StaticClass() ? "type[Any] | None"
                : TypeName(P.GetReferencedClass(), Names) + " | None";
        case K::Array:
        {
            const auto Item = Annotation(*static_cast<const FArrayProperty&>(P).GetInner(), Names, Input);
            return Input ? "list[" + Item + "] | tuple[" + Item + ", ...]" : "list[" + Item + "]";
        }
        case K::Map:
        {
            const auto& Map = static_cast<const FMapProperty&>(P);
            return "dict[" + Annotation(*Map.GetKeyProp(), Names, Input) + ", "
                + Annotation(*Map.GetValueProp(), Names, Input) + "]";
        }
        default: return "int";
        }
    }
    static auto Signature(const std::string& Name, const DFunction& F, const FNames& Names) -> std::string
    {
        bool HasSelfParameter = false;
        for (size_t I = 0; I < F.GetNumInputParms(); ++I)
            HasSelfParameter |= PropertyName(*F.GetParameterProperty(I)) == "self";
        std::string Result = Name + (HasSelfParameter ? "(_self, /" : "(self, /");
        bool DefaultSeen = false, KeywordOnly = false;
        std::vector<const FProperty*> Outputs;
        for (size_t I = 0; I < F.GetNumInputParms(); ++I)
        {
            const auto& P = *F.GetParameterProperty(I);
            const auto* Default = F.GetParameterDefault(I);
            const bool Optional = Default && Default->State == EFunctionDefaultState::Reflected;
            // C++ native-only defaults remain required to Python. Keep valid Python syntax
            // even when a reflected default precedes such a parameter.
            if (DefaultSeen && !Optional && !KeywordOnly) { Result += ", *"; KeywordOnly = true; }
            Result += ", " + PropertyName(P) + ": " + Annotation(P, Names, true);
            if (Optional) { Result += " = ..."; DefaultSeen = true; }
            if (P.HasAnyPropertyFlags(EPropertyFlags::OutParm) && !P.HasAnyPropertyFlags(EPropertyFlags::ConstParm)) Outputs.push_back(&P);
        }
        const auto* Return = F.GetReturnProperty();
        const bool OptionalResult = Return && Return->GetKind() == K::Bool && !Outputs.empty();
        if (Return && !OptionalResult) Outputs.insert(Outputs.begin(), Return);
        Result += ") -> ";
        if (Outputs.empty()) Result += "None";
        else if (Outputs.size() == 1) Result += Annotation(*Outputs[0], Names);
        else
        {
            Result += "tuple[";
            for (size_t I = 0; I < Outputs.size(); ++I)
            { if (I) Result += ", "; Result += Annotation(*Outputs[I], Names); }
            Result += "]";
        }
        if (OptionalResult) Result += " | None";
        return Result;
    }
    auto MemberDocumentation(DType* Type, const DFunction* Function, const FProperty* Property) -> std::string
    {
        FNames Names;
        for (const auto& [Native, Entry] : Registry->Types) Names.emplace(Native, Entry.Alias);
        if (Function)
        {
            const auto Method = Name(Function->GetScriptExposure().Name.empty() ? Function->GetName() : Function->GetScriptExposure().Name);
            auto Text = Signature(Method, *Function, Names) + "\n\n" + Qualified(Type) + "::" + Function->GetName();
            if (!Function->GetToolTip().empty()) Text += "\n" + Function->GetToolTip();
            return Text;
        }
        return PropertyName(*Property) + ": " + Annotation(*Property, Names)
            + (Intrinsic(Type) || Property->HasAnyPropertyFlags(EPropertyFlags::ScriptReadWrite) ? " (writable)" : " (read-only)")
            + "\n\n" + Qualified(Type) + "." + Property->NamePrivate.ToString();
    }
}

namespace Durin::PythonReflection
{
    using namespace Private;
    auto GenerateStubs() -> PyObject*
    {
        if (!Registry || !Registry->Active) return Error(PyExc_RuntimeError, "Reflection registry is unavailable.");
        // Snapshot the published registry. Never register new types or invoke native business code.
        std::map<std::string, DType*> Sorted;
        for (const auto& [Type, Entry] : Registry->Types) Sorted.emplace(Entry.Qualified, Type);
        FNames Names;
        size_t PrivateIndex = 0;
        for (const auto& [QualifiedName, Type] : Sorted)
        {
            const auto& Alias = Registry->Types.at(Type).Alias;
            const auto It = Registry->Aliases.find(Alias);
            Names.emplace(Type, It != Registry->Aliases.end() && It->second == Type ? Alias : "_Native" + std::to_string(PrivateIndex++));
        }
        std::string Text = "# Generated by durin.generate_stubs(); reflects loaded native modules.\n"
            "from __future__ import annotations\nfrom typing import Any, ClassVar, Literal, Never, Self, TypeVar, overload\n\n"
            "_T = TypeVar('_T')\ntools: Any\n"
            "class ScriptAccessError(RuntimeError): ...\nclass ToolError(RuntimeError): ...\n"
            "class CancelledError(BaseException): ...\nclass NotReady(RuntimeError): ...\n"
            "class ScopedEditorTransaction:\n"
            "    def __init__(self, desc: str) -> None: ...\n"
            "    def __enter__(self) -> Self: ...\n"
            "    def __exit__(self, type: Any, value: Any, traceback: Any) -> None: ...\n"
            "    def cancel(self) -> None: ...\n"
            "def defer(work: _T) -> _T: ...\n"
            "def get_editor_subsystem(subsystem_type: type[_T]) -> _T: ...\n"
            "def generate_stubs() -> str: ...\n\n"
            "class Name:\n    def __init__(self, value: str, /) -> None: ...\n    def __str__(self) -> str: ...\n"
            "class Guid:\n    def __init__(self, value: str, /) -> None: ...\n    def __str__(self) -> str: ...\n\n";
        // Qualified lookup preserves type identity when public aliases collide.
        for (const auto& [QualifiedName, Type] : Sorted)
            Text += "@overload\ndef get_type(qualified_name: Literal['" + QualifiedName + "']) -> type[" + Names.at(Type) + "]: ...\n";
        if (!Sorted.empty()) Text += "@overload\n";
        Text += "def get_type(qualified_name: str) -> type[Any]: ...\n\n";
        for (const auto& [QualifiedName, Type] : Sorted)
        {
            auto* Class = Cast<DClass>(Type);
            const auto Base = Class && Class->GetSuperClass() ? "(" + TypeName(Class->GetSuperClass(), Names) + ")" : "";
            Text += "# " + QualifiedName + "\nclass " + Names.at(Type) + Base + ":\n";
            std::map<std::string, std::pair<const DFunction*, const FProperty*>> Members;
            VisitPublishedMembers(Type, [&](const std::string& N, const DFunction* F, const FProperty* P) { Members.emplace(N, std::make_pair(F, P)); });
            auto* Struct = Cast<DStruct>(Type);
            bool Writable = Intrinsic(Type);
            if (Struct) for (auto* Field = Struct->GetChildProperties(); Field; Field = Field->GetNext())
                Writable |= static_cast<FProperty*>(Field)->HasAnyPropertyFlags(EPropertyFlags::ScriptReadWrite);
            if (Struct && (Intrinsic(Type) || Type->GetScriptExposure().Has(EScriptExposure::Type)) && Writable && Struct->CanDefaultConstruct())
            {
                Text += "    def __init__(_self, /";
                bool First = true;
                for (const auto& [N, Member] : Members) if (Member.second && (Intrinsic(Type) || Member.second->HasAnyPropertyFlags(EPropertyFlags::ScriptReadWrite)))
                { Text += First ? ", *" : ""; First = false; Text += ", " + N + ": " + Annotation(*Member.second, Names, true) + " = ..."; }
                Text += ") -> None: ...\n";
            }
            else Text += "    def __new__(cls, *args: Never, **kwargs: Never) -> Never: ...\n";
            if (Class && !Class->GetSuperClass())
                Text += "    def get_name(self) -> str: ...\n    def get_class(self) -> type[Self]: ...\n"
                    "    def get_editor_property(self, name: str) -> Any: ...\n"
                    "    def set_editor_property(self, name: str, value: Any) -> None: ...\n";
            for (const auto& [N, Member] : Members)
            {
                const auto [Function, Property] = Member;
                if (Function) Text += "    def " + Signature(N, *Function, Names) + ": ...\n";
                else
                {
                    Text += "    @property\n    def " + N + "(self) -> " + Annotation(*Property, Names) + ": ...\n";
                    if (Intrinsic(Type) || Property->HasAnyPropertyFlags(EPropertyFlags::ScriptReadWrite))
                        Text += "    @" + N + ".setter\n    def " + N + "(self, value: " + Annotation(*Property, Names, true) + ") -> None: ...\n";
                }
            }
            // Enum values are read from the actual Python dictionary as well.
            if (Cast<DEnum>(Type))
            {
                std::map<std::string, bool> Constants;
                PyObject* Key; PyObject* Value; Py_ssize_t Position = 0;
                while (PyDict_Next(reinterpret_cast<PyTypeObject*>(Registry->Types.at(Type).Python)->tp_dict, &Position, &Key, &Value))
                    if (Py_TYPE(Value) == reinterpret_cast<PyTypeObject*>(Registry->Types.at(Type).Python)) Constants.emplace(PyUnicode_AsUTF8(Key), true);
                for (const auto& [N, Unused] : Constants) Text += "    " + N + ": ClassVar[" + Names.at(Type) + "]\n";
            }
            Text += "\n";
        }
        return PyUnicode_FromString(Text.c_str());
    }
}
#endif
