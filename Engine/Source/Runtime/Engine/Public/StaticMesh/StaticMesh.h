#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"

#include "StaticMesh.gen.h"

namespace Durin
{
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
	class DStaticMesh;

	struct FStaticMeshImportResult
	{
		bool bSucceeded = false;
		std::string Message;
		DStaticMesh* Asset = nullptr;

		explicit operator bool() const { return bSucceeded; }
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

		auto SetRenderData(std::unique_ptr<FStaticMeshRenderData> InRenderData) -> void;
		auto PostLoad(std::string& OutError) -> bool override;

		static auto CreateDebugTriangle(DObject* Outer = nullptr) -> DStaticMesh*;
		static auto ImportAsset(
			std::string_view FilePath,
			std::string_view AssetPath,
			const FStaticMeshImportSettings& InImportSettings = {}) -> FStaticMeshImportResult;

	private:
		auto BuildRenderData(std::string_view PhysicalFilePath, std::string& OutError) -> bool;

		DPROPERTY()
		std::string SourceFile;

		DPROPERTY()
		float NormalizedSize = 1.5f;

		// Import settings are source metadata: PostLoad must rebuild with the same basis
		// that was used when the asset package was first created.
		DPROPERTY()
		FStaticMeshImportSettings ImportSettings;

		std::unique_ptr<FStaticMeshRenderData> RenderData;
	};
}
