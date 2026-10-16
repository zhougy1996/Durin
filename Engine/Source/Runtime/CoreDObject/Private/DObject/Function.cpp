#include "DObject/FunctionParameters.h"

#include "DObject/Object.h"
#include "Misc/StringHelper.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/PropertyValidation.h"
#include "GCReferenceSchema.h"
#include "Threading/RunnableThread.h"

namespace
{
	thread_local const Durin::FFunctionFrame* GCurrentFunctionFrame = nullptr;
	auto GetFunctionBuffers() -> std::unordered_set<Durin::FFunctionParameterBuffer*>&
	{
		static std::unordered_set<Durin::FFunctionParameterBuffer*> Buffers;
		return Buffers;
	}
}

namespace Durin
{
	namespace
	{
		auto ValidateFunctionProperty(const FProperty& Property, std::string Path, uint32 Depth = 0) -> std::optional<FPropertyValidationError>
		{
			using E = EPropertyValidationError;
			using K = DurinCodeGen::EPropertyGenFlags;
			auto Fail = [&](const char* Reason) -> std::optional<FPropertyValidationError> {
				return FPropertyValidationError{E::InvalidStorage, Path, Reason};
			};
			if (auto Error = ValidateNativeProperty(Property, {.RequireValueOperations = true})) { Error->PropertyPath = Path; return Error; }
			if (Depth > 4) return Fail("function containers exceed the maximum nesting depth of four");
			if (Property.GetArrayDim() != 1 || Property.HasValueAccessors()) return Fail("function values require one inline native slot without custom accessors");
			if (Depth && Property.GetOffset() != 0) return Fail("container element/key/value offset must be zero");
			switch (Property.GetKind())
			{
			case K::None: case K::Byte: case K::Blob: case K::BulkData: case K::Count:
				return Fail("property kind is unsupported in native function parameters");
			case K::Array:
			{
				const auto& Array = static_cast<const FArrayProperty&>(Property);
				if (Depth == 4) return Fail("function containers exceed the maximum nesting depth of four");
				if (Array.GetInner()->GetKind() == K::Bool) return Fail("std::vector<bool> storage is unsupported in native function parameters");
				return ValidateFunctionProperty(*Array.GetInner(), Path + ".Inner", Depth + 1);
			}
			case K::Map:
			{
				const auto& Map = static_cast<const FMapProperty&>(Property);
				if (Depth == 4) return Fail("function containers exceed the maximum nesting depth of four");
				const auto Kind = Map.GetKeyProp()->GetKind();
				if (Kind == K::Object || Kind == K::Struct || Kind == K::SoftObject || Kind == K::WeakObject || Kind == K::Array || Kind == K::Map)
					return FPropertyValidationError{E::InvalidType, Path + ".Key", "native function map keys must use scalar, string, name, GUID or enum storage"};
				if (auto Error = ValidateFunctionProperty(*Map.GetKeyProp(), Path + ".Key", Depth + 1)) return Error;
				return ValidateFunctionProperty(*Map.GetValueProp(), Path + ".Value", Depth + 1);
			}
			default: return {};
			}
		}
	}

	DFunction::DFunction(DClass* Owner, const DurinCodeGen::FFunctionParams& Params)
		: DStructBase(FObjectInitializer{.Class = DFunction::StaticClass(), .Outer = Owner, .Name = Params.Name, .Flags = EObjectFlags::Transient}),
		FunctionFlags(Params.Flags), Category(Params.Category ? Params.Category : "Actions"),
		DisplayName(Params.DisplayName && *Params.DisplayName ? Params.DisplayName : StringUtils::HumanizeName(Params.Name)),
		ToolTip(Params.ToolTip ? Params.ToolTip : ""), NativeFunction(Params.NativeFunction)
	{
		SetStructureLayout(Params.Layout ? Params.Layout->ParmsSize : 0, Params.Layout ? Params.Layout->ParmsAlignment : 1);
		if (const auto* Layout = Params.Layout)
		{
			InitializeParameters = Layout->Initialize;
			DestroyParameters = Layout->Destroy;
			DurinCodeGen::ConstructProperties(*this, Layout->PropertyParams, Layout->NumProperties);
		}
	}

	auto DFunction::RegisterMetadata() -> void { AddObject(GetFName()); }

	auto DFunction::GetOuterDClass() const -> DClass* { return static_cast<DClass*>(GetOuter()); }

	auto DFunction::GetReturnValueOffset() const -> uint32
	{
		require(ArePropertiesLinked());
		return ReturnProperty ? ReturnProperty->GetOffset() : std::numeric_limits<uint32>::max();
	}

	auto DFunction::Invoke(DObject& Target, FFunctionFrame& Frame, void* Result) const -> void
	{
		require(&Target == &Frame.GetObject() && this == &Frame.GetFunction());
		NativeFunction(Target, Frame, Result);
	}

	auto ToString(const FFunctionLinkDiagnostic& Diagnostic) -> std::string
	{
		if (!Diagnostic.HasError()) return {};
		return std::format("{}::{}{}: {}", Diagnostic.OwnerClass, Diagnostic.FunctionName,
			Diagnostic.PropertyPath.empty() ? std::string{} : "." + Diagnostic.PropertyPath, Diagnostic.Reason);
	}

	auto DFunction::MakeLinkDiagnostic(EFunctionLinkError Code, std::string Path, std::string Reason) const -> FFunctionLinkDiagnostic
	{
		const auto* Owner = Cast<DClass>(GetOuter());
		return {Code, Owner ? Owner->GetQualifiedName().ToString() : "<no declaring class>", GetName(), std::move(Path), std::move(Reason)};
	}

	auto DFunction::FailLink(EFunctionLinkError Code, std::string Path, std::string Reason) -> bool
	{
		LinkDiagnostic = MakeLinkDiagnostic(Code, std::move(Path), std::move(Reason));
		return false;
	}

	auto DFunction::Link() -> bool
	{
		LinkDiagnostic = {};
		const auto* Owner = Cast<DClass>(GetOuter());
		if (!Owner) return FailLink(EFunctionLinkError::InvalidDescriptor, {}, "declaring class is missing");
		auto* Parent = Owner->GetSuperClass();
		if (Parent && Parent->LinkFunctions() != EFunctionLinkError::None)
		{ LinkDiagnostic = Parent->GetFunctionLinkDiagnostic(); return false; }
		auto* SuperFunction = Parent ? const_cast<DFunction*>(Parent->FindFunctionByName(GetFName())) : nullptr;
		if (ArePropertiesLinked()) return GetSuperStructBase() == SuperFunction
			|| FailLink(EFunctionLinkError::InvalidDescriptor, {}, "declaring-class parent changed after function layout linking");
		SetSuperStructBase(SuperFunction);
		if (!InitializeDerivedMembers())
		{
			ParameterProperties.clear();
			ReturnProperty = nullptr;
			return false;
		}
		ParameterReferenceSchema = Private::FGCReferenceSchemaRegistry::CompileParameters(*this);
		return Super::Link();
	}

	auto DFunction::InitializeDerivedMembers() -> bool
	{
		ParameterProperties.clear();
		ReturnProperty = nullptr;
		struct FRange { uint32 Start, End; std::string Name; };
		std::vector<FRange> Ranges;
		std::unordered_set<FName> Names;
		auto Fail = [&](std::string Path, std::string Reason) { return FailLink(EFunctionLinkError::InvalidDescriptor, std::move(Path), std::move(Reason)); };
		if (!GetChildProperties())
			return (GetStructureSize() == 0 && GetMinAlignment() == 1 && !InitializeParameters && !DestroyParameters)
				|| Fail({}, "void/no-argument functions require an empty aggregate layout and no lifecycle callbacks");
		if (!GetStructureSize() || GetStructureSize() > std::numeric_limits<uint16>::max()) return Fail({}, "parameter aggregate size must be in [1, 65535]");
		if (!GetMinAlignment() || (GetMinAlignment() & (GetMinAlignment() - 1)) || GetStructureSize() % GetMinAlignment())
			return Fail({}, "parameter aggregate alignment must be a power of two and divide its size");
		if (!InitializeParameters || !DestroyParameters) return Fail({}, "parameter aggregate initialization or destruction callback is missing");
		for (auto* Field = GetChildProperties(); Field; Field = Field->GetNext())
		{
			const auto* Property = static_cast<const FProperty*>(Field);
			const auto Path = Property->NamePrivate.ToString();
			if (!Property->HasAnyPropertyFlags(EPropertyFlags::Parm)) return Fail(Path, "parameter property is missing the Parm flag");
			if (auto Error = ValidateFunctionProperty(*Property, Path)) return Fail(Error->PropertyPath, Error->Reason);
			if (Property->NamePrivate.IsNone()) return Fail(Path, "parameter property name is empty");
			const bool bReturn = Property->HasAnyPropertyFlags(EPropertyFlags::ReturnParm);
			if (!bReturn && !Names.insert(Property->NamePrivate).second) return Fail(Path, "parameter name is duplicated");
			const bool bReference = Property->HasAnyPropertyFlags(EPropertyFlags::ReferenceParm);
			const bool bConst = Property->HasAnyPropertyFlags(EPropertyFlags::ConstParm);
			const bool bOut = Property->HasAnyPropertyFlags(EPropertyFlags::OutParm);
			if (bReturn && ReturnProperty) return Fail(Path, "multiple return properties are present");
			if (bReturn ? (bReference || bConst || !bOut) : (bConst && !bReference) || (bOut != (bReference && !bConst)))
				return Fail(Path, "parameter ReferenceParm/ConstParm/OutParm/ReturnParm flags are inconsistent");
			if (bReturn) ReturnProperty = Property;
			else ParameterProperties.push_back(Property);
			const uint32 Offset = Property->GetOffset(), Size = Property->GetValueSize(), Alignment = Property->GetValueAlignment();
			if (Offset > GetStructureSize() || Size > GetStructureSize() - Offset) return Fail(Path, "parameter storage exceeds the aggregate bounds");
			if (GetMinAlignment() < Alignment || Offset % Alignment) return Fail(Path, "parameter offset or aggregate alignment does not satisfy native alignment");
			Ranges.push_back({Offset, Offset + Size, Path});
		}
		std::ranges::sort(Ranges, {}, &FRange::Start);
		for (size_t Index = 1; Index < Ranges.size(); ++Index)
			if (Ranges[Index - 1].End > Ranges[Index].Start) return Fail(Ranges[Index].Name, "parameter storage overlaps '" + Ranges[Index - 1].Name + "'");
		return true;
	}

	auto DFunction::GetSignatureMismatch(const DFunction& Other) const -> FFunctionLinkDiagnostic
	{
		auto Fail = [&](std::string Path, const char* Reason) { return MakeLinkDiagnostic(EFunctionLinkError::SignatureMismatch, std::move(Path), Reason); };
		if (!ArePropertiesLinked() || !Other.ArePropertiesLinked()) return Fail({}, "both function layouts must be linked before signature comparison");
		if (GetNumInputParms() != Other.GetNumInputParms()) return Fail({}, "parameter count differs from the overridden function");
		if (HasAnyFunctionFlags(EFunctionFlags::Const) != Other.HasAnyFunctionFlags(EFunctionFlags::Const)) return Fail({}, "method const qualification differs from the overridden function");
		constexpr auto Roles = EPropertyFlags::ReferenceParm | EPropertyFlags::ConstParm | EPropertyFlags::OutParm;
		for (size_t Index = 0; Index < ParameterProperties.size(); ++Index)
		{
			const auto* A = ParameterProperties[Index];
			const auto* B = Other.ParameterProperties[Index];
			if ((A->GetPropertyFlags() & Roles) != (B->GetPropertyFlags() & Roles)) return Fail(A->NamePrivate.ToString(), "parameter passing flags differ from the overridden function");
			if (!ArePropertyTypesSame(A, B)) return Fail(A->NamePrivate.ToString(), "native parameter type differs from the overridden function");
		}
		if (bool(ReturnProperty) != bool(Other.ReturnProperty) || (ReturnProperty && !ArePropertyTypesSame(ReturnProperty, Other.ReturnProperty)))
			return Fail("ReturnValue", "native return type differs from the overridden function");
		return {};
	}

	auto DFunction::IsSignatureCompatibleWith(const DFunction& Other) const -> bool
	{
		return !GetSignatureMismatch(Other).HasError();
	}

	FFunctionParameterBuffer::FFunctionParameterBuffer(const DFunction& Function) : Function(Function)
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		FGarbageCollectionDeferralScope Deferral;
		require(Function.ArePropertiesLinked());
		if (!Function.GetParmsSize()) return;
		const auto Alignment = std::align_val_t(std::max<size_t>(Function.GetMinAlignment(), alignof(std::max_align_t)));
		Storage = ::operator new(Function.GetParmsSize(), Alignment);
		try { Function.InitializeParameters(Storage); }
		catch (...) { ::operator delete(Storage, Alignment); throw; }
		try { GetFunctionBuffers().insert(this); }
		catch (...) { Function.DestroyParameters(Storage); ::operator delete(Storage, Alignment); throw; }
	}

	FFunctionParameterBuffer::~FFunctionParameterBuffer()
	{
		if (!Storage) return;
		if (GIsGameThreadIdInitialized) CheckGameThread();
		FGarbageCollectionDeferralScope Deferral;
		GetFunctionBuffers().erase(this);
		Function.DestroyParameters(Storage);
		::operator delete(Storage, std::align_val_t(std::max<size_t>(Function.GetMinAlignment(), alignof(std::max_align_t))));
	}

	namespace Private
	{
		auto AddFunctionParameterReferences(FReferenceCollector& Collector) -> void
		{
			for (auto* Buffer : GetFunctionBuffers())
			{
				FGCReferenceSchemaRegistry::Visit(*Buffer->Function.ParameterReferenceSchema, Buffer->Storage, Collector);
			}
		}
	}

	auto DFunction::GetSuperFunction() const -> const DFunction*
	{
		return static_cast<const DFunction*>(GetSuperStructBase());
	}

	auto DFunction::GetRootFunction() const -> const DFunction&
	{
		const auto* Super = GetSuperFunction();
		return Super ? Super->GetRootFunction() : *this;
	}

	FFunctionFrame::FFunctionFrame(DObject& InObject, const DFunction& InFunction, FMutableByteView InParameters)
		: Object(InObject), Function(InFunction), Parameters(InParameters), PreviousFrame(GCurrentFunctionFrame)
	{
		GCurrentFunctionFrame = this;
	}

	FFunctionFrame::~FFunctionFrame()
	{
		require(GCurrentFunctionFrame == this);
		GCurrentFunctionFrame = PreviousFrame;
	}

	auto FFunctionFrame::GetCurrentFrame() -> const FFunctionFrame*
	{
		return GCurrentFunctionFrame;
	}

	auto DFunction::ValidateInvocation(DObject& Target, FMutableByteView Parameters) const -> EFunctionInvokeError
	{
		if (!GetOuterDClass() || !Target.IsA(GetOuterDClass())) return EFunctionInvokeError::WrongClass;
		if (!bLinked) return EFunctionInvokeError::UnlinkedFunction;
		if (Parameters.size() != GetParmsSize() || (GetParmsSize()
			&& (!Parameters.data() || reinterpret_cast<uintptr_t>(Parameters.data()) % GetMinAlignment())))
			return EFunctionInvokeError::InvalidParameters;
		return EFunctionInvokeError::None;
	}

}

IMPLEMENT_INTRINSIC_CLASS(DFunction, DFunction, COREDOBJECT_API, DStructBase, DStructBase, COREDOBJECT_API, {})
