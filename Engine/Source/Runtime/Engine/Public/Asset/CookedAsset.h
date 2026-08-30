#pragma once

#include "EngineAPI.h"
#include "AssetRegistry/Result.h"
#include "DObject/DObjectFwd.h"
#include "DObject/ObjectMacros.h"
#include "Misc/Guid.h"

#include "CookedAsset.gen.h"

namespace Durin::Asset
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

	enum class ECookedPayloadCompression : uint32
	{
		None = 0,
		Zstandard = 1
	};

	enum class ECookedPayloadLocationKind : uint32
	{
		Invalid = 0,
		PackageCompanion = 1
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

	// Selects one logical payload without persisting a workstation or companion path.
	DSTRUCT()
	struct FCookedPayloadDescriptor
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid PayloadId;

		DPROPERTY()
		uint32 LocationKind = 0;

		DPROPERTY()
		uint64 Offset = 0;

		DPROPERTY()
		uint64 StoredSize = 0;

		DPROPERTY()
		uint64 UncompressedSize = 0;

		DPROPERTY()
		uint32 Alignment = 0;

		DPROPERTY()
		uint64 PayloadHashLow = 0;

		DPROPERTY()
		uint64 PayloadHashHigh = 0;

		DPROPERTY()
		uint32 PayloadSchemaVersion = 0;

		DPROPERTY()
		uint32 TargetPlatform = 0;

		DPROPERTY()
		uint32 TargetProfile = 0;

		DPROPERTY()
		uint32 CompressionMethod = 0;

		auto operator==(const FCookedPayloadDescriptor&) const -> bool = default;
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

	// Owns validated DBLK entries and their uncompressed payload bytes.
	struct FCookedBulkContainer
	{
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		std::vector<FCookedPayloadDescriptor> Entries;
		std::vector<std::vector<std::byte>> Payloads;
	};

	ENGINE_API auto DecodeCookedBulk(
		std::span<const std::byte> Bytes,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		FCookedBulkContainer& OutContainer,
		std::string* OutError = nullptr
	) -> bool;

	ENGINE_API auto LoadCookedBulkFile(
		const std::filesystem::path& Path,
		ECookTargetPlatform ExpectedPlatform,
		ECookTargetProfile ExpectedProfile,
		FCookedBulkContainer& OutContainer,
		std::string* OutError = nullptr
	) -> bool;

	ENGINE_API auto ResolveCookedPayload(
		const FCookedBulkContainer& Container,
		const FCookedPayloadDescriptor& Descriptor,
		std::span<const std::byte>& OutPayload,
		std::string* OutError = nullptr
	) -> bool;

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

} // namespace Durin::Asset
