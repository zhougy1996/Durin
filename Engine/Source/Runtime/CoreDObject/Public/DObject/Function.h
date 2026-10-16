#pragma once

#include "CoreMinimal.h"
#include "CoreDObjectAPI.h"

namespace Durin
{
	class DClass;
	class DObject;
	class FFunction;
	class FFunctionFrame;
	class FProperty;
	class FReferenceCollector;
	namespace Private
	{
		// Called by the GC root pass for live initialized native parameter buffers.
		COREDOBJECT_API auto AddFunctionParameterReferences(FReferenceCollector& Collector) -> void;
	}

	enum class EFunctionFlags : uint32 { None = 0, CallInEditor = 1, Virtual = 2, Native = 4, Final = 8, Const = 16 };
	ENUM_CLASS_FLAGS(EFunctionFlags);
	// Reference parameters bind to initialized value slots in the caller's parameter storage.
	enum class EFunctionParameterPassing : uint8 { Value, ConstReference, Reference };
	struct FFunctionParameter
	{
		std::shared_ptr<const FProperty> Property;
		EFunctionParameterPassing Passing = EFunctionParameterPassing::Value;
	};

	// Owns detached parameter fields and the compiler-generated aggregate storage lifecycle.
	// These fields never enter a DClass's serialized property chain or GC schema.
	struct FFunctionSignature
	{
		std::vector<FFunctionParameter> Parameters;
		std::shared_ptr<const FProperty> ReturnProperty;
		uint32 ParmsSize = 0;
		uint32 ParmsAlignment = 1;
		void (*Initialize)(void*) = nullptr;
		void (*Destroy)(void*) = nullptr;
		auto IsVoidNoArgs() const -> bool { return Parameters.empty() && !ReturnProperty; }
		COREDOBJECT_API auto IsValid() const -> bool;
		COREDOBJECT_API auto IsCompatibleWith(const FFunctionSignature& Other) const -> bool;
	private:
		// Call only after validating both signatures. FFunction caches that result.
		COREDOBJECT_API auto HasCompatibleTypes(const FFunctionSignature& Other) const -> bool;
		friend class FFunction;
	};
	enum class EFunctionInvokeError : uint8
	{
		None, InvalidTarget, WrongClass, UnsupportedSignature, MissingInvoker, UnlinkedFunction,
		InvalidFunction, InvalidParameters
	};
	// Linking runs after all local descriptors in a registration batch are installed.
	enum class EFunctionLinkError : uint8 { None, InheritanceCycle, InvalidDescriptor, NonVirtualOverride, FinalOverride, SignatureMismatch };

	// Immutable class-owned native metadata, independent of serialized properties.
	class FFunction
	{
	public:
		using FNativeFuncPtr = void (*)(DObject&, FFunctionFrame&, void* Result);
		FFunction(DClass* InOwner, FName InName, FFunctionSignature InSignature, EFunctionFlags InFlags,
			std::string InCategory, std::string InDisplayName, std::string InToolTip,
			FNativeFuncPtr InNativeFunction)
			: Owner(InOwner), Name(InName), Signature(std::move(InSignature)), bValidSignature(Signature.IsValid()),
			Flags(InFlags), Category(std::move(InCategory)),
			DisplayName(std::move(InDisplayName)), ToolTip(std::move(InToolTip)), NativeFunction(InNativeFunction) {}
		auto GetOwnerClass() const -> DClass* { return Owner; }
		auto GetName() const -> FName { return Name; }
		auto GetFlags() const -> EFunctionFlags { return Flags; }
		auto HasAnyFlags(EFunctionFlags InFlags) const -> bool { return EnumHasAnyFlags(Flags, InFlags); }
		auto GetSignature() const -> const FFunctionSignature& { return Signature; }
		auto IsSignatureCompatibleWith(const FFunction& Other) const -> bool
		{
			return bValidSignature && Other.bValidSignature
				&& HasAnyFlags(EFunctionFlags::Const) == Other.HasAnyFlags(EFunctionFlags::Const)
				&& Signature.HasCompatibleTypes(Other.Signature);
		}
		auto GetCategory() const -> const std::string& { return Category; }
		auto GetDisplayName() const -> const std::string& { return DisplayName; }
		auto GetToolTip() const -> const std::string& { return ToolTip; }
		// Parent and root identities are stable after the owning class has linked.
		COREDOBJECT_API auto GetSuperFunction() const -> const FFunction*;
		COREDOBJECT_API auto GetRootFunction() const -> const FFunction&;
	private:
		// ProcessEvent is the public dispatch path; only it admits and constructs frames.
		COREDOBJECT_API auto ValidateInvocation(DObject& Target, FMutableByteView Parameters) const -> EFunctionInvokeError;
		COREDOBJECT_API auto Invoke(FFunctionFrame& Frame, void* Result) const -> void;
		DClass* Owner;
		FName Name;
		FFunctionSignature Signature;
		bool bValidSignature;
		EFunctionFlags Flags;
		std::string Category, DisplayName, ToolTip;
		FNativeFuncPtr NativeFunction;
		const FFunction* SuperFunction = nullptr;
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
		auto GetFunction() const -> const FFunction& { return Function; }
		auto GetParameters() const -> FMutableByteView { return Parameters; }
		auto GetPreviousFrame() const -> const FFunctionFrame* { return PreviousFrame; }
		// Typed access is defined in FunctionParameters.h and checks the reflected value kind.
		template<typename T> auto GetParameter(size_t Index) const -> T&;
		COREDOBJECT_API static auto GetCurrentFrame() -> const FFunctionFrame*;
	private:
		COREDOBJECT_API FFunctionFrame(DObject& InObject, const FFunction& InFunction, FMutableByteView InParameters);
		COREDOBJECT_API ~FFunctionFrame();
		DObject& Object;
		const FFunction& Function;
		FMutableByteView Parameters;
		const FFunctionFrame* PreviousFrame;
		friend class DObject;
	};
}
