#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"

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

		// Empty means the asset has no source dependency; otherwise this is project- or engine-relative.
		DPROPERTY()
		std::string SourcePath;

		// Lowercase XXH3-128 of the exact source bytes, or empty until legacy metadata is upgraded.
		DPROPERTY()
		std::string SourceContentHash;

		DPROPERTY()
		std::string ImporterId;

		DPROPERTY()
		uint32 ImporterVersion = 0;

		DPROPERTY()
		FStaticMeshImportSettings ImportSettings;

		auto HasSource() const -> bool { return !SourcePath.empty(); }
		auto operator==(const FStaticMeshSourceImportData&) const -> bool = default;
	};

	struct FStaticMeshBuildData;
	struct FStaticMeshRenderData;
	struct FStaticMeshImportResult;

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
		auto GetSourceFile() const -> const std::string& { return SourceFile; }
		auto GetImportSettings() const -> const FStaticMeshImportSettings& { return ImportSettings; }
		auto GetNumMaterialSlots() const -> uint32 { return static_cast<uint32>(MaterialSlots.size()); }
		auto GetMaterialSlots() const -> std::span<const FStaticMeshMaterialSlotDefinition> { return MaterialSlots; }
		ENGINE_API auto GetMaterialSlot(uint32 SlotIndex) const -> const FStaticMeshMaterialSlotDefinition*;
		ENGINE_API auto FindMaterialSlot(const FGuid& SlotId) const -> const FStaticMeshMaterialSlotDefinition*;
		ENGINE_API auto FindMaterialSlot(FName Name) const -> const FStaticMeshMaterialSlotDefinition*;

		ENGINE_API auto SetRenderData(std::unique_ptr<FStaticMeshRenderData> InRenderData) -> void;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;

		ENGINE_API static auto CreateDebugTriangle(DObject* Outer = nullptr) -> DStaticMesh*;
		// Editor preview geometry stays as ordinary source content; this creates only the transient runtime mesh.
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
			const FStaticMeshImportSettings& InImportSettings = {}) -> FStaticMeshImportResult;

	private:
		auto BuildRenderData(std::string_view PhysicalFilePath, std::string& OutError) -> bool;
		auto NotifyLoadedComponents() -> void;

		DPROPERTY()
		std::string SourceFile;

		DPROPERTY()
		float NormalizedSize = 1.5f;

		// Import settings are source metadata: PostLoad must rebuild with the same basis
		// that was used when the asset package was first created.
		DPROPERTY()
		FStaticMeshImportSettings ImportSettings;

		DPROPERTY()
		uint32 MaterialSlotsVersion = 0;

		DPROPERTY()
		std::vector<FStaticMeshMaterialSlotDefinition> MaterialSlots;

		std::unique_ptr<FStaticMeshRenderData> RenderData;
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
