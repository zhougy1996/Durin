#pragma once

#include "EngineAPI.h"
#include "Asset/AssetDefinitions.h"

namespace Durin
{
	enum class ECookTargetPlatform : uint32
	{
		Invalid = 0,
		Win64 = 1
	};

	enum class ECookTargetProfile : uint32
	{
		Invalid = 0,
		Game = 1,
		EditorValidation = 2
	};

	enum class EAssetExecutionDomain : uint8
	{
		Authored,
		Cooked
	};

	enum class EAssetPayloadPolicy : uint8
	{
		SourceAndDerivedDataAllowed,
		CookedPayloadRequired
	};

	// Fixes the process asset domain and payload policy for one runtime lifetime.
	class FAssetRuntimeConfiguration
	{
	public:
		ENGINE_API static auto Authored() -> FAssetRuntimeConfiguration;
		// Leaves OutConfiguration unchanged when the cook root is not absolute and normalized.
		ENGINE_API static auto Cooked(
			std::filesystem::path CookRoot,
			FAssetRuntimeConfiguration& OutConfiguration
		) -> FAssetResult;

		auto GetExecutionDomain() const -> EAssetExecutionDomain { return ExecutionDomain; }
		auto GetPayloadPolicy() const -> EAssetPayloadPolicy { return PayloadPolicy; }
		auto GetCookRoot() const -> const std::filesystem::path& { return CookRoot; }
		auto IsAuthored() const -> bool
		{
			return ExecutionDomain == EAssetExecutionDomain::Authored;
		}
		auto IsCooked() const -> bool
		{
			return ExecutionDomain == EAssetExecutionDomain::Cooked;
		}
		auto AllowsSourceFallback() const -> bool
		{
			return PayloadPolicy == EAssetPayloadPolicy::SourceAndDerivedDataAllowed;
		}
		auto AllowsDerivedDataFallback() const -> bool { return AllowsSourceFallback(); }
		auto RequiresCookedPayload() const -> bool
		{
			return PayloadPolicy == EAssetPayloadPolicy::CookedPayloadRequired;
		}
		auto operator==(const FAssetRuntimeConfiguration&) const -> bool = default;

	private:
		FAssetRuntimeConfiguration() = default;

		EAssetExecutionDomain ExecutionDomain = EAssetExecutionDomain::Authored;
		EAssetPayloadPolicy PayloadPolicy = EAssetPayloadPolicy::SourceAndDerivedDataAllowed;
		std::filesystem::path CookRoot;
	};

	ENGINE_API auto ResolveCookedPackagePath(
		const std::filesystem::path& CookRoot,
		std::string_view VirtualPackagePath,
		std::filesystem::path& OutPackagePath,
		std::string* OutError = nullptr
	) -> bool;

	ENGINE_API auto ResolveCookedCompanionPath(
		const std::filesystem::path& CookRoot,
		const std::filesystem::path& PackagePath,
		std::filesystem::path& OutCompanionPath,
		std::string* OutError = nullptr
	) -> bool;

} // namespace Durin
