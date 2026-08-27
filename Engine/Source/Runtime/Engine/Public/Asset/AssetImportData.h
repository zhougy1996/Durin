#pragma once

#include "EngineAPI.h"
#include "Asset/PackageInspection.h"
#include "Asset/SourcePath.h"
#include "DObject/Object.h"
#include "Hash/XxHash.h"

#include "AssetImportData.gen.h"

namespace Durin::AssetImport
{
	inline constexpr uint32 AssetImportDataSchemaVersion = 2;
	inline constexpr uint32 MaximumAssetImportSources = 8'192;
	inline constexpr size_t MaximumAssetImportStringBytes = 1'024;

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
		std::string StableIdentity;

		DPROPERTY()
		std::string Role;

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
		ENGINE_API auto FindByStableIdentity(
			std::string_view StableIdentity) const -> const FSourceFile*;
		ENGINE_API auto FindByRole(std::string_view Role) const
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

	DCLASS(Abstract)
	class DAssetImportData : public DObject
	{
		GENERATED_BODY()

	public:
		auto GetSchemaVersion() const -> uint32 { return SchemaVersion; }
		auto GetSourceData() const -> const FAssetImportInfo& { return SourceData; }

		ENGINE_API virtual auto Validate(std::string& OutError) const -> bool;
		virtual auto GetState() const -> FAssetImportDataState = 0;
		virtual auto CloneToOwner(DObject* Owner, FName Name, std::string& OutError) const
			-> DAssetImportData* = 0;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;

	protected:
		ENGINE_API explicit DAssetImportData(
			const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto ApplyState(
			FAssetImportDataState State, std::string& OutError) -> bool;

	private:
		DPROPERTY()
		uint32 SchemaVersion = AssetImportDataSchemaVersion;

		DPROPERTY()
		FAssetImportInfo SourceData;
	};

	ENGINE_API auto InspectAssetImportInfo(
		const Asset::FAssetPackageInspection& Inspection,
		FAssetImportInfo& OutInfo,
		std::string& OutError) -> bool;
}
