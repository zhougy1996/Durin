#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"

#include "StaticMesh.gen.h"

namespace Durin
{
	class DMaterialInterface;
	class DStaticMeshComponent;

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

	DSTRUCT()
	struct ENGINE_API FStaticMeshImportSettings
	{
		GENERATED_BODY()

		DPROPERTY()
		EStaticMeshImportAxis ForwardAxis = EStaticMeshImportAxis::PositiveX;

		DPROPERTY()
		EStaticMeshImportAxis RightAxis = EStaticMeshImportAxis::PositiveY;

		DPROPERTY()
		EStaticMeshImportAxis UpAxis = EStaticMeshImportAxis::PositiveZ;

		auto IsValid(std::string* OutError = nullptr) const -> bool;

		static auto MakeDurin() -> FStaticMeshImportSettings;
		static auto MakeYUpNegativeZForward() -> FStaticMeshImportSettings;

		auto operator==(const FStaticMeshImportSettings&) const -> bool = default;
	};

	struct FStaticMeshBuildData;
	struct FStaticMeshRenderData;
	struct FStaticMeshImportResult;

	DSTRUCT()
	struct ENGINE_API FStaticMeshMaterialSlotDefinition
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid SlotId;

		DPROPERTY()
		FName Name;

		DPROPERTY()
		std::string SourceName;

		DPROPERTY()
		uint32 SourceMaterialIndex = 0;

		DPROPERTY()
		TObjectPtr<DMaterialInterface> DefaultMaterial;
	};

	DCLASS()
	class ENGINE_API DStaticMesh : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DStaticMesh(const FObjectInitializer& ObjectInitializer);
		~DStaticMesh() override;
		auto GetRenderData() const -> const FStaticMeshRenderData*;
		auto GetRenderData() -> FStaticMeshRenderData*;
		auto GetSourceFile() const -> const std::string& { return SourceFile; }
		auto GetImportSettings() const -> const FStaticMeshImportSettings& { return ImportSettings; }
		auto GetNumMaterialSlots() const -> uint32 { return static_cast<uint32>(MaterialSlots.size()); }
		auto GetMaterialSlots() const -> std::span<const FStaticMeshMaterialSlotDefinition> { return MaterialSlots; }
		auto GetMaterialSlot(uint32 SlotIndex) const -> const FStaticMeshMaterialSlotDefinition*;
		auto FindMaterialSlot(const FGuid& SlotId) const -> const FStaticMeshMaterialSlotDefinition*;
		auto FindMaterialSlot(FName Name) const -> const FStaticMeshMaterialSlotDefinition*;

		auto SetRenderData(std::unique_ptr<FStaticMeshRenderData> InRenderData) -> void;
		auto PostLoad(std::string& OutError) -> bool override;

		static auto CreateDebugTriangle(DObject* Outer = nullptr) -> DStaticMesh*;
		// Editor preview geometry stays as ordinary source content; this creates only the transient runtime mesh.
		static auto CreateTransientFromFile(
			std::string_view FilePath,
			DObject* Outer,
			std::string_view ObjectName,
			std::string& OutError,
			const FStaticMeshImportSettings& InImportSettings = {}
		) -> DStaticMesh*;
		static auto ImportAsset(
			std::string_view FilePath,
			std::string_view AssetPath,
			const FStaticMeshImportSettings& InImportSettings = {}) -> FStaticMeshImportResult;

	private:
		auto BuildRenderData(std::string_view PhysicalFilePath, std::string& OutError) -> bool;
		auto AddBoundComponent(DStaticMeshComponent* Component) -> void;
		auto RemoveBoundComponent(DStaticMeshComponent* Component) -> void;

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
		std::vector<DStaticMeshComponent*> BoundComponents;

		friend class DStaticMeshComponent;
	};

	struct FStaticMeshImportResult
	{
		bool bSucceeded = false;
		std::string Message;
		DStaticMesh* Asset = nullptr;

		explicit operator bool() const { return bSucceeded; }
	};
}
