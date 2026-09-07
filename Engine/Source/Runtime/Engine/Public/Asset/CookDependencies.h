#pragma once

#include "EngineAPI.h"
#include "Asset/AssetDefinitions.h"
#include "Asset/CookedAsset.h"
#include "DObject/AssetPath.h"
#include "Hash/XxHash.h"

namespace Durin
{
	enum class ECookBuildDependencyKind : uint8
	{
		SourcePackage = 1, OwnedBulk, DirectPackage, TransitivePackage,
		ExternalFile, ConfigurationValue, SchemaProducerVersion
	};

	struct FCookBuildDependency
	{
		ECookBuildDependencyKind Kind = ECookBuildDependencyKind::SourcePackage;
		std::string LogicalName;
		FByteBuffer Value;
		auto operator==(const FCookBuildDependency&) const -> bool = default;
	};

	inline constexpr uint32 MaximumCookDependencyRecords = 65536;
	inline constexpr uint64 MaximumCookDependencyBytes = 64ull * 1024 * 1024;
	inline constexpr uint64 MaximumCookDependencyValueBytes = 1024 * 1024;

	// Kind/name identities must be unique. Encoding sorts; decoding requires canonical order.
	ENGINE_API auto EncodeCookBuildDependencies(std::span<const FCookBuildDependency> Records,
		FByteBuffer& OutBytes, std::string* OutError = nullptr) -> bool;
	ENGINE_API auto DecodeCookBuildDependencies(FByteView Bytes,
		std::vector<FCookBuildDependency>& OutRecords, std::string* OutError = nullptr) -> bool;
	ENGINE_API auto FingerprintCookBuildDependencies(std::span<const FCookBuildDependency> Records,
		FXxHash128& OutFingerprint, std::string* OutError = nullptr) -> bool;

	struct FCookPackageBuildDependency
	{
		FPackagePath Package;
		bool bTransitive = false;
	};

	// Inputs already own evaluated values. Graph expansion performs no I/O or loading.
	struct FCookPackageBuildInputs
	{
		FPackagePath Package;
		std::vector<FCookBuildDependency> Inputs;
		std::vector<FCookPackageBuildDependency> Packages;
	};

	// Prepare once per run: source identities and validated nodes are reused by every root.
	class FCookBuildDependencyGraph
	{
	public:
		ENGINE_API auto Initialize(std::span<const FCookPackageBuildInputs> Graph,
			std::string* OutError = nullptr) -> bool;
		ENGINE_API auto Expand(const FPackagePath& Root,
			std::vector<FCookBuildDependency>& OutRecords, std::string* OutError = nullptr) const -> bool;
	private:
		std::unordered_map<FPackagePath, FCookPackageBuildInputs> Nodes;
		std::unordered_map<FPackagePath, FByteBuffer> SourceIdentities;
	};

	ENGINE_API auto ExpandCookBuildDependencies(const FPackagePath& Root,
		std::span<const FCookPackageBuildInputs> Graph,
		std::vector<FCookBuildDependency>& OutRecords, std::string* OutError = nullptr) -> bool;

	// Only declared values of the current capture package are accessible. Failure is sticky.
	ENGINE_API auto IsCookInputCaptureActive() -> bool;
	ENGINE_API auto ReadCapturedCookInput(ECookBuildDependencyKind Kind,
		std::string_view LogicalName, FByteBuffer& OutBytes) -> FAssetResult;

	struct FCookDependencyDeclaration
	{
		ECookBuildDependencyKind Kind = ECookBuildDependencyKind::ConfigurationValue;
		std::string LogicalName;
		// Acquisition locator only, excluded from persistent identity. ExternalFile only.
		std::filesystem::path FilePath;
		// Owned canonical value for ConfigurationValue or SchemaProducerVersion.
		FByteBuffer Value;
	};

	struct FCookDependencyRequest
	{
		FPackagePath Package;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		bool bRetainEditorOnlyData = false;
	};

	// Called once per package before cache lookup, without loading authored objects.
	using FCookDependencyDeclarationCallback = std::function<FAssetResult(
		const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>&)>;
}
