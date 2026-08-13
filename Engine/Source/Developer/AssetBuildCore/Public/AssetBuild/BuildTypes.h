#pragma once

#include "AssetBuildCoreAPI.h"
#include "Hash/XxHash.h"

namespace Durin::AssetBuild
{
	// Stable owner-qualified identity of one Build function.
	struct FBuildFunctionIdentity
	{
		std::string Owner;
		std::string Name;

		auto operator==(const FBuildFunctionIdentity&) const -> bool = default;
	};

	// Immutable named bytes exchanged by Build definitions and functions.
	class FBuildValue
	{
	public:
		ASSETBUILDCORE_API static auto FromOwned(
			std::string Name, std::vector<uint8> Bytes) -> FBuildValue;

		auto GetName() const -> std::string_view { return Name; }
		auto GetContentIdentity() const -> const FXxHash128& { return ContentIdentity; }
		auto GetBytes() const -> std::span<const uint8>
		{
			return Bytes ? std::span<const uint8>(*Bytes) : std::span<const uint8>();
		}
		auto GetSize() const -> uint64 { return Bytes ? Bytes->size() : 0; }
		auto IsValid() const -> bool { return !Name.empty() && Bytes != nullptr; }

	private:
		std::string Name;
		FXxHash128 ContentIdentity;
		std::shared_ptr<const std::vector<uint8>> Bytes;
	};

	// Portable immutable recipe invocation facts. Local callbacks are deliberately absent.
	struct FBuildDefinition
	{
		FBuildFunctionIdentity Function;
		std::string ImplementationIdentity;
		std::string RecipeIdentity;
		std::string TargetPlatform;
		std::string TargetProfile;
		std::vector<FBuildValue> Inputs;
		bool bExportable = true;
	};

	// Explicit cache/build/data-return intent for one request.
	struct FBuildPolicy
	{
		bool bQueryCache = true;
		bool bAllowLocalBuild = true;
		bool bStoreBuildResult = true;
		bool bRequireStoreSuccess = false;
		bool bReturnData = true;
		int32 Priority = 0;
	};

	// Detached terminal result returned by a local Build function.
	struct FBuildFunctionResult
	{
		bool bSucceeded = false;
		bool bCanceled = false;
		std::vector<FBuildValue> Values;
		std::string Diagnostic;
	};

	ASSETBUILDCORE_API auto IsValidBuildFunctionIdentity(
		const FBuildFunctionIdentity& Identity, std::string* OutError = nullptr) -> bool;
	ASSETBUILDCORE_API auto BuildFunctionIdentityString(
		const FBuildFunctionIdentity& Identity) -> std::string;
	ASSETBUILDCORE_API auto ValidateBuildDefinition(
		const FBuildDefinition& Definition, std::string* OutError = nullptr) -> bool;
}
