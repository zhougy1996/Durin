#pragma once

#include "AssetBuild/BuildDefinition.h"
#include "Modules/ModularFeature.h"

namespace Durin::Asset::Build
{
	struct FBuildFunctionConfig
	{
		std::string CacheBucket;
		std::string ExpectedValueName;
		uint64 MaximumValueBytes = 0;
		uint64 CleanupBudgetBytes = 0;
		uint32 CleanupDeleteLimit = 0;
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
		ASSETBUILDCORE_API ~FBuildFunctionRegistration();
		FBuildFunctionRegistration(const FBuildFunctionRegistration&) = delete;
		auto operator=(const FBuildFunctionRegistration&) -> FBuildFunctionRegistration& = delete;
		ASSETBUILDCORE_API FBuildFunctionRegistration(FBuildFunctionRegistration&&) noexcept;
		ASSETBUILDCORE_API auto operator=(FBuildFunctionRegistration&&) noexcept
			-> FBuildFunctionRegistration&;
		ASSETBUILDCORE_API auto Reset() -> void;
		auto IsValid() const -> bool { return Generation != 0; }
	private:
		FBuildFunctionIdentity Identity;
		uint64 Generation = 0;
		friend ASSETBUILDCORE_API auto RegisterBuildFunction(
			FBuildFunctionIdentity, std::shared_ptr<IBuildFunction>,
			FModuleOwnedCallbackGate, std::string*) -> FBuildFunctionRegistration;
	};

	// Module providers pass their callback gate. A process-resident direct-linked
	// provider may omit it only when no unload boundary exists in that process.
	ASSETBUILDCORE_API auto RegisterBuildFunction(
		FBuildFunctionIdentity Identity, std::shared_ptr<IBuildFunction> Function,
		FModuleOwnedCallbackGate OwnerGate, std::string* OutError = nullptr)
		-> FBuildFunctionRegistration;
}
