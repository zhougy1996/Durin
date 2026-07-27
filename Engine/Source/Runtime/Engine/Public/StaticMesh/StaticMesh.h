#pragma once

#include "EngineAPI.h"
#include "CookedAsset.h"
#include "DObject/CoreDObject.h"
#include "Source/SourcePath.h"

#include "StaticMesh.gen.h"

namespace Durin
{
	class DMaterialInterface;

	// Selects a signed source axis when converting imported geometry to Durin space.
	DENUM()
	enum class EStaticMeshImportAxis : int8
	{
		PositiveX,
		NegativeX,
		PositiveY,
		NegativeY,
		PositiveZ,
		NegativeZ
	};

	// Defines the orthogonal source basis used during static-mesh import.
	DSTRUCT()
	struct FStaticMeshImportSettings
	{
		GENERATED_BODY()

		DPROPERTY()
		EStaticMeshImportAxis ForwardAxis = EStaticMeshImportAxis::PositiveX;

		DPROPERTY()
		EStaticMeshImportAxis RightAxis = EStaticMeshImportAxis::PositiveY;

		DPROPERTY()
		EStaticMeshImportAxis UpAxis = EStaticMeshImportAxis::PositiveZ;

		ENGINE_API auto IsValid(std::string* OutError = nullptr) const -> bool;

		ENGINE_API static auto MakeDurin() -> FStaticMeshImportSettings;
		ENGINE_API static auto MakeYUpNegativeZForward() -> FStaticMeshImportSettings;

		auto operator==(const FStaticMeshImportSettings&) const -> bool = default;
	};

	// Stores optional portable source provenance used only for editor rebuild and reimport.
	DSTRUCT()
	struct FStaticMeshSourceImportData
	{
		GENERATED_BODY()

		// Empty means the asset has no source dependency; otherwise this is a complete mounted source path.
		DPROPERTY()
		FSourcePath SourcePath;

		// Lowercase XXH3-128 of the exact source bytes.
		DPROPERTY()
		std::string SourceContentHash;

		DPROPERTY()
		std::string ImporterId;

		DPROPERTY()
		uint32 ImporterVersion = 0;

		DPROPERTY()
		FStaticMeshImportSettings ImportSettings;

		auto HasSource() const -> bool { return !SourcePath.IsEmpty(); }
		auto operator==(const FStaticMeshSourceImportData&) const -> bool = default;
	};

	struct FStaticMeshBuildData;
	struct FStaticMeshRenderData;
	struct FStaticMeshImportResult;

	enum class EStaticMeshSourceStatus : uint8
	{
		NoSource,
		Available,
		Changed,
		Missing,
		Invalid
	};

	// Describes editor-facing source availability without making source data a runtime requirement.
	struct FStaticMeshSourceDiagnostic
	{
		EStaticMeshSourceStatus Status = EStaticMeshSourceStatus::NoSource;
		std::string ResolvedPath;
		std::string Message;

		auto IsAvailable() const -> bool
		{
			return Status == EStaticMeshSourceStatus::Available
				|| Status == EStaticMeshSourceStatus::Changed;
		}
	};

	enum class EStaticMeshDerivedDataStatus : uint8
	{
		None,
		Hit,
		Missing,
		Corrupt,
		Incompatible,
		Rebuilt,
		WriteFailure,
		SourceUnavailableCached,
		SourceUnavailable,
		CookedLoaded,
		CookedFailure
	};

	// Describes the most recent native-payload cache decision for editor diagnostics.
	struct FStaticMeshDerivedDataDiagnostic
	{
		EStaticMeshDerivedDataStatus Status = EStaticMeshDerivedDataStatus::None;
		std::string Key;
		std::string Message;
		bool bSourceImporterInvoked = false;
	};

	// Reports the deterministic work performed by one static-mesh component scan.
	struct FStaticMeshUpdateCounters
	{
		uint64 ObjectSnapshotCount = 0;
		uint64 ScannedObjectCount = 0;
		uint64 ScannedComponentCount = 0;
		uint64 MatchedComponentCount = 0;
		uint64 UpdatedComponentCount = 0;
	};

	// Returns the counters from the most recently completed static-mesh component scan.
	ENGINE_API auto GetLastStaticMeshUpdateCounters() -> FStaticMeshUpdateCounters;

	// Preserves one material slot's stable identity and source-import provenance.
	DSTRUCT()
	struct FStaticMeshMaterialSlotDefinition
	{
		GENERATED_BODY()

		// Stable identity used to retain component overrides across reimport and reordering.
		DPROPERTY()
		FGuid SlotId;

		DPROPERTY()
		FName Name;

		DPROPERTY()
		std::string SourceName;

		// Original importer index used only for source reconciliation.
		DPROPERTY()
		uint32 SourceMaterialIndex = 0;

		DPROPERTY()
		TObjectPtr<DMaterialInterface> DefaultMaterial;
	};

	// Owns imported mesh metadata, material slots, and rebuilt render resources.
	DCLASS()
	class DStaticMesh : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DStaticMesh(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DStaticMesh() override;
		ENGINE_API auto GetRenderData() const -> const FStaticMeshRenderData*;
		ENGINE_API auto GetRenderData() -> FStaticMeshRenderData*;
		auto GetSourceFile() const -> const std::string& { return SourceImportData.SourcePath.Path; }
		auto GetImportSettings() const -> const FStaticMeshImportSettings& { return SourceImportData.ImportSettings; }
		auto GetSourceImportData() const -> const FStaticMeshSourceImportData& { return SourceImportData; }
		auto GetNumMaterialSlots() const -> uint32 { return static_cast<uint32>(MaterialSlots.size()); }
		auto GetMaterialSlots() const -> std::span<const FStaticMeshMaterialSlotDefinition> { return MaterialSlots; }
		ENGINE_API auto GetMaterialSlot(uint32 SlotIndex) const -> const FStaticMeshMaterialSlotDefinition*;
		ENGINE_API auto FindMaterialSlot(const FGuid& SlotId) const -> const FStaticMeshMaterialSlotDefinition*;
		ENGINE_API auto FindMaterialSlot(FName Name) const -> const FStaticMeshMaterialSlotDefinition*;

		ENGINE_API auto SetRenderData(std::unique_ptr<FStaticMeshRenderData> InRenderData) -> void;
		ENGINE_API auto InspectSource() const -> FStaticMeshSourceDiagnostic;
		auto GetDerivedDataDiagnostic() const -> const FStaticMeshDerivedDataDiagnostic& { return DerivedDataDiagnostic; }
		auto GetCookedPayloadDescriptor() const -> const Asset::FCookedPayloadDescriptor& { return CookedPayload; }
		ENGINE_API auto ChangeSourceReference(
			std::string_view SourceVirtualPath, std::string& OutError) -> bool;
		ENGINE_API auto IngestAndChangeSource(
			std::string_view FilePath,
			std::string_view TargetSourceVirtualPath,
			std::string& OutError) -> bool;
		ENGINE_API auto RepairSourcePath(std::string_view FilePath, std::string& OutError) -> bool;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		// Contributes deterministic DMSH data and descriptor-bearing runtime metadata to a cook.
		ENGINE_API auto AddToCook(
			Asset::FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError,
			bool bRetainDiagnosticSourceMetadata = false) -> bool;

		ENGINE_API static auto CreateDebugTriangle(DObject* Outer = nullptr) -> DStaticMesh*;
		// Creates unpackaged geometry for tests and runtime-generated content; editor previews use retained assets.
		ENGINE_API static auto CreateTransientFromFile(
			std::string_view FilePath,
			DObject* Outer,
			std::string_view ObjectName,
			std::string& OutError,
			const FStaticMeshImportSettings& InImportSettings = {}
		) -> DStaticMesh*;
		ENGINE_API static auto ImportAsset(
			std::string_view FilePath,
			std::string_view AssetPath,
			const FStaticMeshImportSettings& InImportSettings = {},
			std::string_view SourceDestination = {}) -> FStaticMeshImportResult;

	private:
		auto BuildRenderData(std::string_view PhysicalFilePath, std::string& OutError) -> bool;
		auto BuildRenderDataCandidate(
			std::string_view PhysicalFilePath,
			std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
			std::vector<FStaticMeshMaterialSlotDefinition>& OutMaterialSlots,
			bool& bOutSlotMetadataChanged,
			std::string& OutError) -> bool;
		auto PublishRenderData(
			std::unique_ptr<FStaticMeshRenderData> InRenderData,
			std::vector<FStaticMeshMaterialSlotDefinition> InMaterialSlots,
			bool bSlotMetadataChanged) -> void;
		auto NotifyLoadedComponents() -> void;
		auto LoadCookedRenderData(std::string& OutError) -> bool;

		DPROPERTY()
		std::string SourceFile;

		DPROPERTY()
		FStaticMeshSourceImportData SourceImportData;

		DPROPERTY()
		float NormalizedSize = 1.5f;

		// Retained only so packages with the removed legacy schema can be diagnosed
		// and rejected without losing serialized field compatibility.
		DPROPERTY()
		FStaticMeshImportSettings ImportSettings;

		DPROPERTY()
		uint32 MaterialSlotsVersion = 0;

		DPROPERTY()
		std::vector<FStaticMeshMaterialSlotDefinition> MaterialSlots;

		DPROPERTY()
		Asset::FCookedPayloadDescriptor CookedPayload;

		std::unique_ptr<FStaticMeshRenderData> RenderData;
		FStaticMeshDerivedDataDiagnostic DerivedDataDiagnostic;
	};

	// Reports static-mesh import success and the created asset, when available.
	struct FStaticMeshImportResult
	{
		bool bSucceeded = false;
		std::string Message;
		DStaticMesh* Asset = nullptr;

		explicit operator bool() const { return bSucceeded; }
	};
}
