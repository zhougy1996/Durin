#pragma once

#include "DerivedDataCache/DerivedDataBuildDefinition.h"
#include "Modules/ModularFeature.h"

namespace Durin::DerivedData
{
	struct FBuildFunctionConfig
	{
		// Changes whenever the function's output semantics change. Family build
		// keys encode this same version to invalidate incompatible cache results.
		uint32 Version = 0;
		std::string CacheBucket;
		std::string ExpectedValueName;
		uint64 MaximumValueBytes = 0;
	};

	class FBuildContext
	{
	public:
		FBuildContext(const FBuildDefinition& InDefinition,
			const FBuildCancellationToken* InCancellation)
			: Definition(InDefinition), Cancellation(InCancellation) {}
		auto GetDefinition() const -> const FBuildDefinition& { return Definition; }
		auto GetInput(std::string_view Name) const -> const FBuildValue*
			{ return Definition.GetInput(Name); }
		auto IsCanceled() const -> bool
			{ return Cancellation && Cancellation->IsCanceled(); }
	private:
		const FBuildDefinition& Definition;
		const FBuildCancellationToken* Cancellation = nullptr;
	};

	class IBuildFunction
	{
	public:
		virtual ~IBuildFunction() = default;
		virtual auto GetConfig() const -> FBuildFunctionConfig = 0;
		virtual auto Validate(const FBuildDefinition& Definition,
			const FBuildValue& Value, std::string& OutError) const -> bool = 0;
		virtual auto Build(const FBuildContext& Context,
			FBuildValue& OutValue, std::string& OutError) const -> bool = 0;
	};

	class FBuildFunctionRegistration
	{
	public:
		FBuildFunctionRegistration() = default;
		DERIVEDDATACACHE_API ~FBuildFunctionRegistration();
		FBuildFunctionRegistration(const FBuildFunctionRegistration&) = delete;
		auto operator=(const FBuildFunctionRegistration&) -> FBuildFunctionRegistration& = delete;
		DERIVEDDATACACHE_API FBuildFunctionRegistration(FBuildFunctionRegistration&&) noexcept;
		DERIVEDDATACACHE_API auto operator=(FBuildFunctionRegistration&&) noexcept
			-> FBuildFunctionRegistration&;
		DERIVEDDATACACHE_API auto Reset() -> void;
		auto IsValid() const -> bool { return Generation != 0; }
	private:
		FBuildFunctionName Name;
		uint64 Generation = 0;
		friend DERIVEDDATACACHE_API auto RegisterBuildFunction(
			FBuildFunctionName, std::shared_ptr<IBuildFunction>,
			FModuleOwnedCallbackGate, std::string*) -> FBuildFunctionRegistration;
	};

	// Module providers pass their callback gate. A process-resident direct-linked
	// provider may omit it only when no unload boundary exists in that process.
	DERIVEDDATACACHE_API auto RegisterBuildFunction(
		FBuildFunctionName Name, std::shared_ptr<IBuildFunction> Function,
		FModuleOwnedCallbackGate OwnerGate, std::string* OutError = nullptr)
		-> FBuildFunctionRegistration;

	// Returns the current registered implementation version for a function name,
	// or zero when no local function is registered.
	DERIVEDDATACACHE_API auto FindBuildFunctionVersion(
		const FBuildFunctionName& Name) -> uint32;
}
