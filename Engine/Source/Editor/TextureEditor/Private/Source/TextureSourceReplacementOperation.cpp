#include "Source/TextureSourceReplacementOperation.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/MountedSource.h"
#include "DObject/Package.h"
#include "DObject/WeakObjectPtr.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DAuthoring.h"
#include "AssetForge/Builtins/Texture2DImport.h"

namespace Durin::Editor::Texture
{
	namespace
	{
		auto StartTextureBuild(
			const TWeakObjectPtr<DTexture2D>& WeakTexture,
			FAsyncOperationCompletion Completion,
			FAsyncOperationCancel& OutCancel,
			std::string& OutError) -> bool
		{
			DTexture2D* Texture = WeakTexture.Get();
			if (!Texture)
			{
				OutError = "The Texture2D replacement target is unavailable.";
				return false;
			}
			const bool bStarted = AssetForge::Builtins::ReimportTexture2DSource(
				*Texture,
				{},
				OutError,
				[Completion = std::move(Completion)](
					Asset::FTexture2DAuthoringResult Result) mutable {
					Completion(Result.Succeeded(), std::move(Result.Diagnostic));
				});
			if (bStarted)
			{
				OutCancel = [WeakTexture] {
					if (DTexture2D* PendingTexture = WeakTexture.Get())
						FAssetCompilingManager::Get().MarkCompilationAsCanceled(
							*PendingTexture);
				};
			}
			return bStarted;
		}
	}

	auto MakeTextureSourceReplacementOperation(
		FTextureSourceReplacementRequest Request)
		-> FCompensatingAsyncOperation::FOperations
	{
		const TWeakObjectPtr<DTexture2D> WeakTexture(Request.Texture);
		const std::string PackagePath = Request.Texture && Request.Texture->GetPackage()
			? Request.Texture->GetPackage()->GetPackagePath() : std::string{};
		const auto Replacement = std::make_shared<Asset::FMountedSourceReplacement>();
		return {
			.Prepare = [Replacement,
				ReplacementPhysicalPath = std::move(Request.ReplacementPhysicalPath),
				SourceVirtualPath = std::move(Request.SourceVirtualPath),
				PackagePath](std::string& OutError) {
				if (PackagePath.empty())
				{
					OutError = "Shared source replacement requires a packaged Texture2D.";
					return false;
				}
				return Asset::PrepareMountedSourceReplacement(
					ReplacementPhysicalPath,
					PackagePath,
					SourceVirtualPath,
					*Replacement,
					OutError);
			},
			.StartApply = [WeakTexture](
				FAsyncOperationCompletion Completion,
				FAsyncOperationCancel& OutCancel,
				std::string& OutError) {
				return StartTextureBuild(
					WeakTexture, std::move(Completion), OutCancel, OutError);
			},
			.Commit = [Replacement, WeakTexture, Save = std::move(Request.Save)](
				std::string& OutError) {
				DTexture2D* Texture = WeakTexture.Get();
				if (!Texture)
				{
					OutError = "The Texture2D replacement target became unavailable.";
					return false;
				}
				if (!Save || !Save(*Texture, OutError)) return false;
				Asset::CommitMountedSourceReplacement(*Replacement);
				return true;
			},
			.Rollback = [Replacement] {
				Asset::RollbackMountedSourceReplacement(*Replacement);
			},
			.StartCompensation = [WeakTexture](
				FAsyncOperationCompletion Completion,
				FAsyncOperationCancel& OutCancel,
				std::string& OutError) {
				return StartTextureBuild(
					WeakTexture, std::move(Completion), OutCancel, OutError);
			},
			.Finished = std::move(Request.Finished),
		};
	}
}
