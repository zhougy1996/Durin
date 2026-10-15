#pragma once

#include "CoreMinimal.h"
#include "CoreDObjectAPI.h"

namespace Durin
{
	class DClass;
	class DObject;

	enum class EFunctionFlags : uint8 { None = 0, CallInEditor = 1, Virtual = 2 };
	ENUM_CLASS_FLAGS(EFunctionFlags);
	enum class EFunctionSignature : uint8 { VoidNoArgs };
	enum class EFunctionInvokeError : uint8 { None, InvalidTarget, WrongClass, UnsupportedSignature, MissingInvoker };

	// Immutable class-owned native metadata, independent of serialized properties.
	class FFunction
	{
	public:
		using FInvoker = void (*)(DObject&);
		FFunction(DClass* InOwner, FName InName, EFunctionFlags InFlags,
			std::string InCategory, std::string InDisplayName, std::string InToolTip,
			FInvoker InInvoker)
			: Owner(InOwner), Name(InName), Flags(InFlags), Category(std::move(InCategory)),
			DisplayName(std::move(InDisplayName)), ToolTip(std::move(InToolTip)), Invoker(InInvoker) {}
		auto GetOwnerClass() const -> DClass* { return Owner; }
		auto GetName() const -> FName { return Name; }
		auto GetFlags() const -> EFunctionFlags { return Flags; }
		auto HasAnyFlags(EFunctionFlags InFlags) const -> bool { return EnumHasAnyFlags(Flags, InFlags); }
		auto GetSignature() const -> EFunctionSignature { return EFunctionSignature::VoidNoArgs; }
		auto GetCategory() const -> const std::string& { return Category; }
		auto GetDisplayName() const -> const std::string& { return DisplayName; }
		auto GetToolTip() const -> const std::string& { return ToolTip; }
		COREDOBJECT_API auto GetRootFunction() const -> const FFunction&;
		COREDOBJECT_API auto InvokeVoidNoArgs(DObject& Target) const -> EFunctionInvokeError;
	private:
		DClass* Owner;
		FName Name;
		EFunctionFlags Flags;
		std::string Category, DisplayName, ToolTip;
		FInvoker Invoker;
	};
}
