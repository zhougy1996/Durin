#pragma once

#include "CoreMinimal.h"
#include "CoreDObjectAPI.h"

namespace Durin
{
	class DObject;
	class DFunction;
	class FFunctionFrame;
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
