#pragma once
#include "Asset/CookInputResult.h"

#include "EngineAPI.h"
#include "Asset/AssetReadResult.h"
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

	enum class ECookDependencyCodecError : uint8
	{
		None, RecordLimit, InvalidRecord, Duplicate, ByteLimit, Header, TruncatedRecord, Noncanonical, TrailingBytes
	};
	struct FCookDependencyCodecResult
	{
		ECookDependencyCodecError Error = ECookDependencyCodecError::None;
		ECookBuildDependencyKind Kind = ECookBuildDependencyKind::SourcePackage;
		std::string LogicalName;
		uint64 RecordIndex = 0;
		uint64 Actual = 0;
		uint64 Maximum = 0;
		uint64 RemainingBytes = 0;
		uint32 Version = 0;
		explicit operator bool() const { return Error == ECookDependencyCodecError::None; }
	};
	ENGINE_API auto FormatCookDependencyCodecError(const FCookDependencyCodecResult& Result) -> std::string;

	// Kind/name identities must be unique. Encoding sorts; decoding requires canonical order.
	ENGINE_API auto EncodeCookBuildDependencies(std::span<const FCookBuildDependency> Records,
		FByteBuffer& OutBytes) -> FCookDependencyCodecResult;
	ENGINE_API auto DecodeCookBuildDependencies(FByteView Bytes,
		std::vector<FCookBuildDependency>& OutRecords) -> FCookDependencyCodecResult;
	ENGINE_API auto FingerprintCookBuildDependencies(std::span<const FCookBuildDependency> Records,
		FXxHash128& OutFingerprint) -> FCookDependencyCodecResult;

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

	enum class ECookDependencyGraphError : uint8
	{
		None, GraphLimit, Package, Codec, GraphBound, Declaration, SourceIdentity,
		Conflict, ExpansionBound, MissingPackage, MissingSource
	};
	struct FCookDependencyGraphResult
	{
		ECookDependencyGraphError Error = ECookDependencyGraphError::None;
		FPackagePath Package;
		FPackagePath Dependency;
		ECookBuildDependencyKind Kind = ECookBuildDependencyKind::SourcePackage;
		std::string LogicalName;
		uint64 Bytes = 0;
		uint64 Records = 0;
		std::optional<FCookDependencyCodecResult> CodecCause;
		explicit operator bool() const { return Error == ECookDependencyGraphError::None; }
		ENGINE_API auto ToInputResult() const -> FCookInputResult;
	};
	ENGINE_API auto FormatCookDependencyGraphError(const FCookDependencyGraphResult& Result) -> std::string;

	// Prepare once per run: source identities and validated nodes are reused by every root.
	class FCookBuildDependencyGraph
	{
	public:
		ENGINE_API auto Initialize(std::span<const FCookPackageBuildInputs> Graph) -> FCookDependencyGraphResult;
		ENGINE_API auto Expand(const FPackagePath& Root,
			std::vector<FCookBuildDependency>& OutRecords) const -> FCookDependencyGraphResult;
	private:
		std::unordered_map<FPackagePath, FCookPackageBuildInputs> Nodes;
		std::unordered_map<FPackagePath, FByteBuffer> SourceIdentities;
	};

	ENGINE_API auto ExpandCookBuildDependencies(const FPackagePath& Root,
		std::span<const FCookPackageBuildInputs> Graph,
		std::vector<FCookBuildDependency>& OutRecords) -> FCookDependencyGraphResult;

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
		// Evaluated once per run; empty means the provider supplied no reusable identity.
		std::string_view ShaderBuildIdentity;
	};

	// Called once per package before cache lookup, without loading authored objects.
	struct FCookContributionResult;
	using FCookDependencyDeclarationCallback = std::function<FCookContributionResult(
		const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>&)>;
}
