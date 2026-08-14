#pragma once

#include "AssetBuild/BuildRequest.h"

namespace Durin::Asset::Build
{
	using FLocalBuildFunction = std::function<FBuildFunctionResult(
		const FBuildDefinition&, const FBuildPolicy&, const FBuildRequestOwner&)>;
	using FBuildTerminalCallback = std::function<void(const FBuildFunctionResult&)>;

	// Lifetime token for one local-only function registration.
	class FBuildFunctionRegistration
	{
	public:
		FBuildFunctionRegistration() = default;
		ASSETBUILDCORE_API ~FBuildFunctionRegistration();
		FBuildFunctionRegistration(const FBuildFunctionRegistration&) = delete;
		auto operator=(const FBuildFunctionRegistration&) -> FBuildFunctionRegistration& = delete;
		ASSETBUILDCORE_API FBuildFunctionRegistration(FBuildFunctionRegistration&& Other) noexcept;
		ASSETBUILDCORE_API auto operator=(FBuildFunctionRegistration&& Other) noexcept
			-> FBuildFunctionRegistration&;

		ASSETBUILDCORE_API auto Reset() -> void;
		auto IsValid() const -> bool { return Generation != 0; }

	private:
		FBuildFunctionIdentity Identity;
		uint64 Generation = 0;
		std::shared_ptr<FBuildRequestOwner> Owner;

		friend ASSETBUILDCORE_API auto RegisterLocalBuildFunction(
			FBuildFunctionIdentity, FLocalBuildFunction,
			FModuleOwnedCallbackGate, std::shared_ptr<FBuildRequestOwner>, std::string*)
			-> FBuildFunctionRegistration;
	};

	// Registers a process-local implementation behind a portable function identity.
	ASSETBUILDCORE_API auto RegisterLocalBuildFunction(
		FBuildFunctionIdentity Identity,
		FLocalBuildFunction Function,
		FModuleOwnedCallbackGate OwnerGate,
		std::shared_ptr<FBuildRequestOwner> Owner = {},
		std::string* OutError = nullptr) -> FBuildFunctionRegistration;

	ASSETBUILDCORE_API auto ExecuteLocalBuildFunction(
		const FBuildDefinition& Definition,
		const FBuildPolicy& Policy,
		FBuildFunctionResult& OutResult,
		FBuildTerminalCallback TerminalCallback = {},
		std::string* OutError = nullptr) -> bool;

	ASSETBUILDCORE_API auto IsLocalBuildFunctionRegistered(
		const FBuildFunctionIdentity& Identity) -> bool;
	ASSETBUILDCORE_API auto GetRegisteredLocalBuildFunctionCount() -> uint32;
}
