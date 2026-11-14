#include "Thumbnail/ThumbnailDrawContext.h"
#include "DObject/Object.h"
#include "Thumbnail/ThumbnailCapture.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
#include "DObject/WeakObjectPtr.h"
#include "DObject/Package.h"

namespace Durin
{
	struct FThumbnailDrawContext::FImpl
	{
		TWeakObjectPtr<DObject> Object;
		uint64 Revision = 0;
		FThumbnailPreviewScene* Scene = nullptr;
		std::optional<FThumbnailPreviewView> View;
		std::optional<FViewEnvironmentOverride> Environment;
		FThumbnailImageRenderer Image;
		std::function<bool()> Validator;
	};
	FThumbnailDrawContext::FThumbnailDrawContext(DObject* Object) : Impl(std::make_unique<FImpl>())
	{
		Impl->Object = Object;
		if (Object && Object->GetPackage()) Impl->Revision = Object->GetPackage()->GetEditRevision();
	}
	FThumbnailDrawContext::~FThumbnailDrawContext()
	{
		Impl->Validator = {};
		if (Impl->Scene)
		{
			Impl->Scene->ResetPreview();
			Impl->Scene->BindingOwner = nullptr;
		}
	}
	auto FThumbnailDrawContext::MarkReady() -> void
	{
		Status = EState::Ready; Diagnostic.clear();
	}
	auto FThumbnailDrawContext::Fail(std::string Diagnostic) -> void
	{
		Status = EState::Failed;
		this->Diagnostic = Diagnostic.empty() ? "Thumbnail drawing failed." : std::move(Diagnostic);
	}
	auto FThumbnailDrawContext::SetValidation(std::function<bool()> Validator) -> void { Impl->Validator = std::move(Validator); }
	auto FThumbnailDrawContext::Validate() const -> std::expected<void, std::string>
	{
		const auto* Object = Impl->Object.Get();
		if (Status != EState::Ready || !Object)
			return std::unexpected("The thumbnail draw is not ready.");
		if (Object->GetPackage() && Object->GetPackage()->GetEditRevision() != Impl->Revision)
			return std::unexpected("The asset changed while its thumbnail was being generated.");
		if (Impl->Validator && !Impl->Validator())
			return std::unexpected("The thumbnail resources changed while rendering.");
		return {};
	}
	auto FThumbnailDrawContext::TrySetScene(FThumbnailPreviewScene& Scene) -> bool
	{
		if (Impl->Scene == &Scene) return true;
		if (Impl->Scene || Scene.BindingOwner) return false;
		if (!Scene.IsAvailable())
		{
			Fail(Scene.GetDiagnostic());
			return false;
		}
		Scene.BindingOwner = this;
		Impl->Scene = &Scene;
		return true;
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
