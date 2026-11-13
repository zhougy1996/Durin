#include "Thumbnail/ThumbnailDrawContext.h"
#include "DObject/Object.h"
#include "Thumbnail/ThumbnailCapture.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Engine/StreamableManager.h"
#include "DObject/WeakObjectPtr.h"
#include "DObject/Package.h"
#include "DObject/Class.h"

namespace Durin
{
	struct FThumbnailDrawContext::FImpl
	{
		TWeakObjectPtr<DObject> Object;
		uint64 Revision = 0;
		FTopLevelAssetPath Path;
		std::unordered_map<std::string, std::shared_ptr<FStreamableHandle>> Dependencies;
		FThumbnailDrawResult Result{.State = EThumbnailDrawStatus::WaitingForResources};
		FThumbnailPreviewScene* Scene = nullptr;
		std::optional<FThumbnailPreviewView> View;
		std::optional<FViewEnvironmentOverride> Environment;
		FThumbnailImageRenderer Image;
		std::function<std::expected<void, std::string>()> Validator;
	};
	FThumbnailDrawContext::FThumbnailDrawContext(DObject* Object, FTopLevelAssetPath Path) : Impl(std::make_unique<FImpl>())
	{
		Impl->Object = Object; Impl->Path = std::move(Path);
		if (Object && Object->GetPackage()) Impl->Revision = Object->GetPackage()->GetEditRevision();
	}
	FThumbnailDrawContext::~FThumbnailDrawContext()
	{
		Impl->Validator = {}; State.reset();
		if (Impl->Scene) Impl->Scene->BindingOwner = nullptr;
		for (auto& [Path, Load] : Impl->Dependencies) Load->CancelHandle();
	}
	auto FThumbnailDrawContext::GetAssetPath() const -> const FTopLevelAssetPath& { return Impl->Path; }
	auto FThumbnailDrawContext::LoadAsset(const FSoftObjectPath& Path, DClass* Class) -> DObject*
	{
		auto& Load = Impl->Dependencies[Path.ToString()];
		if (!Load) Load = GetStreamableManager().RequestAsyncLoad(Path, {}, Class);
		if (!Load || !Load->IsComplete()) { Impl->Result = {.State = EThumbnailDrawStatus::WaitingForResources}; return nullptr; }
		if (Load->HasError()) { Impl->Result = {.Diagnostic = Load->GetError().Message}; return nullptr; }
		return Load->GetLoadedAsset();
	}
	auto FThumbnailDrawContext::SetResult(FThumbnailDrawResult Result) -> void
	{
		if (Result.State == EThumbnailDrawStatus::Failed && Result.Diagnostic.empty())
			Result.Diagnostic = "Thumbnail drawing failed.";
		Impl->Result = std::move(Result);
	}
	auto FThumbnailDrawContext::GetResult() const -> FThumbnailDrawResult { return Impl->Result; }
	auto FThumbnailDrawContext::SetValidation(std::function<std::expected<void, std::string>()> Validator) -> void { Impl->Validator = std::move(Validator); }
	auto FThumbnailDrawContext::Validate() const -> std::expected<void, std::string>
	{
		const auto* Object = Impl->Object.Get();
		if (Impl->Result.State != EThumbnailDrawStatus::ReadyToRender || !Object)
			return std::unexpected("The thumbnail draw is not ready.");
		if (Object->GetPackage() && Object->GetPackage()->GetEditRevision() != Impl->Revision)
			return std::unexpected("The asset changed while its thumbnail was being generated.");
		return Impl->Validator ? Impl->Validator() : std::expected<void, std::string>{};
	}
	auto FThumbnailDrawContext::SetScene(FThumbnailPreviewScene& Scene) -> std::expected<void, std::string>
	{
		if (Impl->Scene == &Scene) return {};
		if (Impl->Scene || Scene.BindingOwner)
		{
			Impl->Result = {.State = EThumbnailDrawStatus::WaitingForResources};
			return std::unexpected("The thumbnail preview scene is already in use.");
		}
		if (!Scene.IsAvailable())
		{
			Impl->Result = {.Diagnostic = Scene.GetDiagnostic()};
			return std::unexpected(Scene.GetDiagnostic());
		}
		Scene.BindingOwner = this; Impl->Scene = &Scene; return {};
	}
	auto FThumbnailDrawContext::SetView(const FThumbnailPreviewView& View) -> void { Impl->View = View; }
	auto FThumbnailDrawContext::SetViewEnvironment(const FViewEnvironmentOverride& Value) -> void { Impl->Environment = Value; }
	auto FThumbnailDrawContext::SetImageRenderer(FThumbnailImageRenderer Image) -> void { Impl->Image = std::move(Image); }
	auto FThumbnailDrawContext::BindCapture(FThumbnailCapture& Capture) -> std::expected<void, std::string>
	{
		if (auto Valid = Validate(); !Valid) return Valid;
		if (Impl->Scene) Capture.BindDrawScene(*Impl->Scene);
		if (Impl->View) Capture.SetView(*Impl->View);
		if (Impl->Environment) Capture.SetViewEnvironment(*Impl->Environment);
		if (Impl->Image) Capture.SetImageRenderer(Impl->Image);
		return {};
	}
}
