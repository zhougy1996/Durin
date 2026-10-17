#pragma once

#include "CoreMinimal.h"
#include "CoreDObjectAPI.h"

namespace Durin
{
	class DObject;
	class DFunction;
	class FFunctionFrame;
	class FProperty;
	enum class EFunctionDefaultState : uint8 { None, NativeOnly, Reflected };
	// Sparse generated record copied into its function. Values are immutable native
	// objects; Matches checks exact property identity and the supported value subset.
	struct FFunctionParameterDefault
	{
		size_t ParameterIndex = 0;
		EFunctionDefaultState State = EFunctionDefaultState::NativeOnly;
		std::string Expression;
		std::shared_ptr<const void> Value;
		bool (*Matches)(const FProperty&, const void*) = nullptr;
	};
	enum class EFunctionDefaultError : uint8 { InvalidIndex, DuplicateIndex, NotInput, MissingDefault, NativeOnly, AssignmentFailed };
	struct FFunctionDefaultError
	{
		EFunctionDefaultError Code;
		size_t ParameterIndex;
	};
	using FNativeFunctionPtr = void (*)(DObject&, FFunctionFrame&, void* Result);
	enum class EFunctionFlags : uint32 { None = 0, CallInEditor = 1, Virtual = 2, Native = 4, Final = 8, Const = 16 };
	ENUM_CLASS_FLAGS(EFunctionFlags);
	enum class EFunctionInvokeError : uint8
	{
		None, InvalidTarget, WrongClass, UnlinkedFunction,
		InvalidFunction, InvalidParameters
	};
	// Linking runs after all local descriptors in a registration batch are installed.
	enum class EFunctionLinkError : uint8 { None, InheritanceCycle, InvalidDescriptor, NonVirtualOverride, FinalOverride, SignatureMismatch };

	// Owned diagnostic for a failed function/class link attempt; empty after success.
	struct FFunctionLinkDiagnostic
	{
		EFunctionLinkError Code = EFunctionLinkError::None;
		std::string OwnerClass;
		std::string FunctionName;
		std::string PropertyPath;
		std::string Reason;
		auto HasError() const -> bool { return Code != EFunctionLinkError::None; }
	};
	COREDOBJECT_API auto ToString(const FFunctionLinkDiagnostic& Diagnostic) -> std::string;

}
