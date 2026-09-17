#include "Preview/PreviewMeshResources.h"

#include "Asset/AssetCompilingManager.h"
#include "DynamicRHI.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMesh.h"
#include "Threading/RunnableThread.h"

namespace Durin::Editor
{
	auto FPreviewMeshResources::Initialize(std::string& OutError) -> bool
	{
		checkf(IsInGameThread(), "Preview mesh warmup must run on the game thread.");
		if (bInitialized)
		{
			OutError = Diagnostic;
			return Diagnostic.empty();
		}
		bInitialized = true;
		constexpr std::array Paths{SphereAssetPath, BoxAssetPath};
		auto Report = [&](size_t Index, std::string_view Error) {
			if (!Diagnostic.empty()) Diagnostic += "\n";
			Diagnostic += std::format("Preview mesh '{}': {}", Paths[Index], Error);
		};
		std::vector<DObject*> LoadedMeshes;
		for (size_t Index = 0; Index < Meshes.size(); ++Index)
		{
			FObjectPath Path;
			std::string Error;
			if (const auto PathValidation = FObjectPath::TryCreate(Paths[Index], Path); !PathValidation)
			{
				Report(Index, FormatObjectError(PathValidation.Error));
				continue;
			}
			if (!FAssetRetentionService::Acquire(Path, Meshes[Index], Error))
			{
				Report(Index, Error);
				continue;
			}
			if (auto* Mesh = Cast<DStaticMesh>(Meshes[Index].Get()))
				LoadedMeshes.push_back(Mesh);
			else
			{
				Meshes[Index] = {};
				Report(Index, "The asset is not a static mesh.");
			}
		}
		// Load both first so their compilation can overlap; wait only at startup.
		if (!LoadedMeshes.empty())
			FAssetCompilingManager::Get().FinishCompilationForObjects(LoadedMeshes);
		for (size_t Index = 0; Index < Meshes.size(); ++Index)
		{
			auto* Mesh = Cast<DStaticMesh>(Meshes[Index].Get());
			if (!Mesh) continue;
			if (!Mesh->GetRenderData())
			{
				const FCookedMeshBlockingResult Result = Mesh->EnsureRenderDataLoadedBlocking();
				if (!Result)
				{
					Report(Index, Result.Message);
					continue;
				}
			}
			if (GDynamicRHI) Mesh->InitResources();
		}
		if (GDynamicRHI)
		{
			// One startup fence for both meshes. Rendering uploads remain ordered on RHI.
			FlushRenderingCommands();
			for (size_t Index = 0; Index < Meshes.size(); ++Index)
			{
				auto* Mesh = Cast<DStaticMesh>(Meshes[Index].Get());
				if (Mesh && Mesh->GetRenderData()
					&& Mesh->GetRenderResourceStatus().Readiness != EStaticMeshRenderResourceReadiness::Ready)
					Report(Index, "GPU render resources could not be initialized.");
			}
		}
		OutError = Diagnostic;
		return Diagnostic.empty();
	}

	auto FPreviewMeshResources::Reset() -> void
	{
		checkf(IsInGameThread(), "Preview mesh retirement must run on the game thread.");
		Meshes = {};
		Diagnostic.clear();
		bInitialized = false;
	}

	auto FPreviewMeshResources::GetSphere() const -> DStaticMesh*
	{
		return Cast<DStaticMesh>(Meshes[0].Get());
	}

	auto FPreviewMeshResources::GetBox() const -> DStaticMesh*
	{
		return Cast<DStaticMesh>(Meshes[1].Get());
	}
}
