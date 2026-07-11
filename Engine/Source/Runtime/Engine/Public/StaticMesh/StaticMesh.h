#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"

#include "StaticMesh.gen.h"

namespace Durin
{
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

		auto SetRenderData(std::unique_ptr<FStaticMeshRenderData> InRenderData) -> void;
		auto PostLoad(std::string& OutError) -> bool override;

		static auto CreateDebugTriangle(DObject* Outer = nullptr) -> DStaticMesh*;
		static auto ImportAsset(std::string_view FilePath, std::string_view AssetPath) -> FStaticMeshImportResult;

	private:
		auto BuildRenderData(std::string_view PhysicalFilePath, std::string& OutError) -> bool;

		DPROPERTY()
		std::string SourceFile;

		DPROPERTY()
		float NormalizedSize = 1.5f;

		std::unique_ptr<FStaticMeshRenderData> RenderData;
	};
}
