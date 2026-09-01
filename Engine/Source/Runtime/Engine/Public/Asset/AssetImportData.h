#pragma once

#include "EngineAPI.h"
#include "Asset/PackageInspection.h"
#include "DObject/Object.h"
#include "Hash/XxHash.h"

#include "AssetImportData.gen.h"

namespace Durin
{
	inline constexpr uint32 AssetImportDataSchemaVersion = 3;
	inline constexpr uint32 MaximumAssetImportSources = 8'192;
	inline constexpr size_t MaximumAssetImportStringBytes = 1'024;
	inline constexpr size_t MaximumAssetImportRoleBytes = 128;

	DENUM()
	enum class ESourceHintBase : uint8
	{
		AssetRelative,
		ProjectRelative,
		Absolute
	};

	DSTRUCT()
	struct FSourceFile
	{
		GENERATED_BODY()

		DPROPERTY()
		FName Role;

		DPROPERTY()
		std::string DisplayLabel;

		DPROPERTY()
		std::string Hint;

		DPROPERTY()
		ESourceHintBase HintBase = ESourceHintBase::AssetRelative;

		DPROPERTY()
		uint64 ContentHashLow = 0;

		DPROPERTY()
		uint64 ContentHashHigh = 0;

		DPROPERTY()
		uint64 ByteCount = 0;

		ENGINE_API auto IsEmpty() const -> bool;
		auto GetContentHash() const -> FXxHash128
		{
			return {ContentHashLow, ContentHashHigh};
		}
		ENGINE_API auto Validate(std::string& OutError) const -> bool;
		auto operator==(const FSourceFile&) const -> bool = default;
	};

	DSTRUCT()
	struct FAssetImportInfo
	{
		GENERATED_BODY()

		DPROPERTY()
		std::vector<FSourceFile> Sources;

		ENGINE_API auto Normalize() -> void;
		ENGINE_API auto Validate(std::string& OutError) const -> bool;
		ENGINE_API auto FindByRole(FName Role) const
			-> const FSourceFile*;
		ENGINE_API auto GetFingerprint() const -> FXxHash128;
		auto operator==(const FAssetImportInfo&) const -> bool = default;
	};

	struct FAssetImportDataState
	{
		uint32 SchemaVersion = AssetImportDataSchemaVersion;
		FAssetImportInfo SourceData;

		auto operator==(const FAssetImportDataState&) const -> bool = default;
	};

	// Stores editor-only source metadata shared by imported asset families.
	DCLASS()
	class DAssetImportData : public DObject
	{
		GENERATED_BODY()

	public:
		ENGINE_API explicit DAssetImportData(
			const FObjectInitializer& ObjectInitializer);

		auto GetSchemaVersion() const -> uint32 { return SchemaVersion; }
		auto GetSourceData() const -> const FAssetImportInfo& { return SourceData; }

		ENGINE_API virtual auto Validate(std::string& OutError) const -> bool;
		ENGINE_API auto SetState(
			FAssetImportDataState State, std::string& OutError) -> bool;
		auto GetState() const -> FAssetImportDataState
		{
			return {.SchemaVersion = SchemaVersion, .SourceData = SourceData};
		}
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;

	private:
		DPROPERTY()
		uint32 SchemaVersion = AssetImportDataSchemaVersion;

		DPROPERTY()
		FAssetImportInfo SourceData;
	};

	ENGINE_API auto InspectAssetImportInfo(
		const FAssetPackageInspection& Inspection,
		FAssetImportInfo& OutInfo,
		std::string& OutError) -> bool;
}
