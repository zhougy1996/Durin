#include "Thumbnail/ThumbnailRenderJob.h"
#include "Thumbnail/ThumbnailCapture.h"
#include "Engine/StreamableManager.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/Package.h"
#include "DObject/Class.h"

namespace Durin
{
	struct FThumbnailRenderJob::FImpl
	{
		TStrongObjectPtr<DThumbnailRenderer> Renderer;
		FAssetThumbnailPackageFingerprint Asset;
		DClass* AssetClass = nullptr;
		TStrongObjectPtr<DObject> LoadedOverride;
		std::shared_ptr<FStreamableHandle> Load;
		std::unique_ptr<FThumbnailDrawContext> Context;
		bool bStarted = false;
		~FImpl() { Context.reset(); if (Load) Load->CancelHandle(); }
	};
	FThumbnailRenderJob::FThumbnailRenderJob(DThumbnailRenderer* Renderer,
		FAssetThumbnailPackageFingerprint Asset, DClass* AssetClass, DObject* LoadedOverride)
		: Impl(std::make_unique<FImpl>())
	{
		Impl->Renderer = TStrongObjectPtr<DThumbnailRenderer>(Renderer); Impl->Asset = std::move(Asset);
		Impl->AssetClass = AssetClass; Impl->LoadedOverride = TStrongObjectPtr<DObject>(LoadedOverride);
	}
	FThumbnailRenderJob::~FThumbnailRenderJob() = default;
	auto FThumbnailRenderJob::Update(bool bAllowDraw) -> FThumbnailDrawResult
	{
		if (!Impl->bStarted)
		{
			Impl->bStarted = true;
			if (!Impl->LoadedOverride) Impl->Load = GetStreamableManager().RequestAsyncLoad(Impl->Asset.AssetPath, {}, Impl->AssetClass);
			return {.State = EThumbnailDrawStatus::WaitingForResources};
		}
		DObject* Object = Impl->LoadedOverride.Get();
		if (!Object)
		{
			if (!Impl->Load) return {.Diagnostic = "The thumbnail asset load is unavailable."};
			if (!Impl->Load->IsComplete()) return {.State = EThumbnailDrawStatus::WaitingForResources};
			if (Impl->Load->HasError()) return {.Diagnostic = Impl->Load->GetError().Message.empty() ? "The thumbnail asset could not be loaded." : Impl->Load->GetError().Message};
			Object = Impl->Load->GetLoadedAsset();
			if (!Object || Object->GetClass()->GetQualifiedName().ToString() != Impl->Asset.AssetClassName)
				return {.Diagnostic = "The thumbnail asset does not match its catalog class."};
		}
		if (!Impl->Renderer->CanVisualizeAsset(Object)) return {.Diagnostic = "The renderer cannot visualize this asset."};
		if (!Impl->Context) Impl->Context = std::make_unique<FThumbnailDrawContext>(Object, Impl->Asset.AssetPath);
		if (Impl->Context->GetResult().State == EThumbnailDrawStatus::ReadyToRender)
		{
			if (auto Valid = Impl->Context->Validate(); !Valid) return {.Diagnostic = Valid.error()};
			return Impl->Context->GetResult();
		}
		if (!bAllowDraw) return {.State = EThumbnailDrawStatus::WaitingForResources};
		Impl->Context->ResetResult();
		Impl->Renderer->Draw(Object, *Impl->Context);
		return Impl->Context->GetResult();
	}
	auto FThumbnailRenderJob::Validate() const -> std::expected<void, std::string>
	{
		return Impl->Context ? Impl->Context->Validate() : std::unexpected("The thumbnail has not been drawn.");
	}
	auto FThumbnailRenderJob::BindCapture(FThumbnailCapture& Capture) -> std::expected<void, std::string>
	{
		return Impl->Context ? Impl->Context->BindCapture(Capture) : std::unexpected("The thumbnail has not been drawn.");
	}
}
