#pragma once

#include "CoreMinimal.h"
#include "DObject/FunctionTypes.h"
#include "DObject/StructBase.h"

namespace Durin
{
	class DClass;
	class DObject;
	class FFunctionFrame;
	class FProperty;
	class FReferenceCollector;
	namespace Private
	{
		// Called by the GC root pass for live initialized native parameter buffers.
		COREDOBJECT_API auto AddFunctionParameterReferences(FReferenceCollector& Collector) -> void;
	}

	// Class-owned reflected metadata. Parameter properties and role flags are the
	// authoritative signature; cached indexes borrow fields owned by DStructBase.
	class DFunction : public DStructBase
	{
	public:
		DECLARE_CLASS_INTRINSIC_API(DFunction, DStructBase, COREDOBJECT_API)
		COREDOBJECT_API auto Link() -> bool override;
		auto GetLinkDiagnostic() const -> const FFunctionLinkDiagnostic& { return LinkDiagnostic; }
		auto GetInheritanceSuper() const -> DStructBase* override { return nullptr; }
		COREDOBJECT_API auto GetOuterDClass() const -> DClass*;
		COREDOBJECT_API auto IsSignatureCompatibleWith(const DFunction& Other) const -> bool;
		auto GetReturnProperty() const -> const FProperty* { require(ArePropertiesLinked()); return ReturnProperty; }
		auto GetParameterProperty(size_t Index) const -> const FProperty* { require(ArePropertiesLinked()); require(Index < ParameterProperties.size()); return ParameterProperties[Index]; }
		auto GetNumInputParms() const -> size_t { require(ArePropertiesLinked()); return ParameterProperties.size(); }
		auto GetNumParms() const -> size_t { require(ArePropertiesLinked()); return ParameterProperties.size() + bool(ReturnProperty); }
		COREDOBJECT_API auto GetParameterDefault(size_t Index) const -> const FFunctionParameterDefault*;
		template<typename T> auto GetParameterDefaultValue(size_t Index) const -> const T&;
		auto GetParmsSize() const -> uint32 { return GetStructureSize(); }
		COREDOBJECT_API auto GetReturnValueOffset() const -> uint32;
		auto GetFunctionFlags() const -> EFunctionFlags { return FunctionFlags; }
		auto HasAnyFunctionFlags(EFunctionFlags Flags) const -> bool { return EnumHasAnyFlags(FunctionFlags, Flags); }
		auto HasAllFunctionFlags(EFunctionFlags Flags) const -> bool { return EnumHasAllFlags(FunctionFlags, Flags); }
		auto GetNativeFunc() const -> FNativeFunctionPtr { return NativeFunction; }
		COREDOBJECT_API auto Invoke(DObject& Target, FFunctionFrame& Frame, void* Result) const -> void;
		auto GetCategory() const -> const std::string& { return Category; }
		auto GetDisplayName() const -> const std::string& { return DisplayName; }
		auto GetToolTip() const -> const std::string& { return ToolTip; }
		COREDOBJECT_API auto GetSuperFunction() const -> const DFunction*;
		COREDOBJECT_API auto GetRootFunction() const -> const DFunction&;
	private:
		DFunction(DClass* Owner, const DurinCodeGen::FFunctionParams& Params);
		auto InitializeDerivedMembers() -> bool;
		auto MakeLinkDiagnostic(EFunctionLinkError Code, std::string Path, std::string Reason) const -> FFunctionLinkDiagnostic;
		auto FailLink(EFunctionLinkError Code, std::string Path, std::string Reason) -> bool;
		auto GetSignatureMismatch(const DFunction& Other) const -> FFunctionLinkDiagnostic;
		auto RegisterMetadata() -> void;
		auto ValidateInvocation(DObject& Target, FMutableByteView Parameters) const -> EFunctionInvokeError;
		FFunctionLinkDiagnostic LinkDiagnostic;
		std::shared_ptr<const Private::FGCReferenceSchema> ParameterReferenceSchema;
		std::vector<const FProperty*> ParameterProperties;
		std::vector<FFunctionParameterDefault> ParameterDefaults;
		bool bInvalidDefaultRecords = false;
		const FProperty* ReturnProperty = nullptr;
		void (*InitializeParameters)(void*) = nullptr;
		void (*DestroyParameters)(void*) = nullptr;
		bool bLinked = false;
		EFunctionFlags FunctionFlags = EFunctionFlags::None;
		std::string Category, DisplayName, ToolTip;
		FNativeFunctionPtr NativeFunction = nullptr;
		friend auto DurinCodeGen::ConstructDFunction(DClass*, const DurinCodeGen::FFunctionParams&) -> DFunction*;
		friend auto Private::AddFunctionParameterReferences(FReferenceCollector&) -> void;
		friend class FFunctionParameterBuffer;
		friend class DClass;
		friend class DObject;
	};

	// Borrowed synchronous native-call context. Frames form a thread-local stack;
	// neither the frame nor its parameter storage may be retained after dispatch.
	class FFunctionFrame final
	{
	public:
		FFunctionFrame(const FFunctionFrame&) = delete;
		auto operator=(const FFunctionFrame&) -> FFunctionFrame& = delete;
		auto GetObject() const -> DObject& { return Object; }
		auto GetFunction() const -> const DFunction& { return Function; }
		auto GetParameters() const -> FMutableByteView { return Parameters; }
		auto GetPreviousFrame() const -> const FFunctionFrame* { return PreviousFrame; }
		// Typed access is defined in FunctionParameters.h and checks the reflected value kind.
		template<typename T> auto GetParameter(size_t Index) const -> T&;
		COREDOBJECT_API static auto GetCurrentFrame() -> const FFunctionFrame*;
	private:
		COREDOBJECT_API FFunctionFrame(DObject& InObject, const DFunction& InFunction, FMutableByteView InParameters);
		COREDOBJECT_API ~FFunctionFrame();
		DObject& Object;
		const DFunction& Function;
		FMutableByteView Parameters;
		const FFunctionFrame* PreviousFrame;
		friend class DObject;
	};
}
