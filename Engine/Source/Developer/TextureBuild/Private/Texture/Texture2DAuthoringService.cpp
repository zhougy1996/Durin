#include "Texture/Texture2DAuthoringService.h"

#include "AssetBuild/BuildHost.h"
#include "DObject/DObjectGlobals.h"
#include "Threading/RunnableThread.h"
#include "Texture/TextureBuildService.h"
#include "Texture/TextureBuilder.h"
#include "Texture/Texture2DPostLoad.h"

namespace Durin::Asset::Build
{
	namespace
	{
		struct FTexture2DAuthoringState
		{
			TWeakObjectPtr<DTexture2D> Texture;
			uint64 Generation = 0;
			uint64 ActiveRequestId = 0;
			uint64 LastRequestId = 0;
			bool bLastRequestFailed = false;
			FTexture2DPublicationContext PublicationContext;
			FAsyncBuildCompletion Completion;
		};

		std::mutex GTexture2DAuthoringMutex;
		std::unordered_map<std::string, FTexture2DAuthoringState> GTexture2DAuthoringStates;
		uint64 GNextTexture2DGeneration = 1;

		auto FindStateLocked(std::string_view Identity) -> FTexture2DAuthoringState*
		{
			const auto It = GTexture2DAuthoringStates.find(std::string(Identity));
			return It == GTexture2DAuthoringStates.end() ? nullptr : &It->second;
		}

		auto ApplyCompletion(FTexture2DQueuedBuildResult&& Result) -> void
		{
			CheckGameThread();
			TWeakObjectPtr<DTexture2D> WeakTexture;
			FTexture2DPublicationContext PublicationContext;
			FAsyncBuildCompletion Completion;
			{
				std::lock_guard Lock(GTexture2DAuthoringMutex);
				FTexture2DAuthoringState* State = FindStateLocked(Result.AssetIdentity);
				if (!State || State->Generation != Result.Generation
					|| State->ActiveRequestId != Result.RequestId) return;
				State->ActiveRequestId = 0;
				State->LastRequestId = Result.RequestId;
				State->bLastRequestFailed = Result.Phase == ETexture2DBuildPhase::Failed;
				WeakTexture = State->Texture;
				PublicationContext = State->PublicationContext;
				Completion = std::move(State->Completion);
			}
			DTexture2D* Texture = WeakTexture.Get();
			if (!Texture || Texture->GetObjectPath() != Result.AssetIdentity)
			{
				if (Completion) Completion({
					.Status = EAsyncBuildStatus::Failed,
					.Diagnostic = "The Texture2D authoring target is unavailable."});
				return;
			}
			if (Result.Phase != ETexture2DBuildPhase::UploadPending
				|| !Result.SourceData || !Result.PlatformData)
			{
				if (Completion) Completion({
					.Status = Result.Phase == ETexture2DBuildPhase::Cancelled
						? EAsyncBuildStatus::Canceled : EAsyncBuildStatus::Failed,
					.Diagnostic = Result.Error.empty()
						? "The Texture2D authoring build did not produce a publishable product."
						: std::move(Result.Error)});
				return;
			}

			FTexture2DBuildProduct Product{
				.SourceData = std::move(*Result.SourceData),
				.PlatformData = std::move(*Result.PlatformData),
				.DerivedDataKey = std::move(Result.DerivedDataKey),
				.SourceContentHashLow = Result.SourceHash.HashLow,
				.SourceContentHashHigh = Result.SourceHash.HashHigh,
				.Settings = {
					.Usage = Result.Settings.Usage,
					.CompressionQuality = Result.Settings.CompressionQuality,
					.AlphaMipMode = Result.Settings.AlphaMipMode,
					.AlphaCoverageThreshold = Result.Settings.AlphaCoverageThreshold,
					.MaxResolution = Result.Settings.MaxResolution,
					.bSRGB = Result.Settings.bSRGB},
				.bSRGB = Result.Settings.bSRGB};
			std::string Error;
			if (!PublishTexture2DProduct(
				*Texture, std::move(Product), PublicationContext, Error))
			{
				{
					std::lock_guard Lock(GTexture2DAuthoringMutex);
					if (FTexture2DAuthoringState* State = FindStateLocked(Result.AssetIdentity);
						State && State->Generation == Result.Generation)
						State->bLastRequestFailed = true;
				}
				DURIN_ERROR("Texture2D authoring publication failed for {}: {}",
					Result.AssetIdentity, Error);
				if (Completion) Completion({
					.Status = EAsyncBuildStatus::Failed,
					.Diagnostic = std::move(Error)});
				return;
			}
			if (Completion) Completion({.Status = EAsyncBuildStatus::Succeeded});
		}
	}

	auto SubmitTexture2DBuild(
		DTexture2D& Texture,
		FTexture2DAuthoringRequest Request,
		std::string& OutError,
		FAsyncBuildCompletion Completion) -> bool
	{
		CheckGameThread();
		if (!Request.SourceData.IsValid() || Request.SourcePath.IsEmpty()
			|| Request.DecoderId.empty())
		{
			OutError = "Texture2D authoring submission requires normalized source and provenance.";
			return false;
		}
		if (!GetTexture2DBuildCoordinator()
			|| !GetBuildHostSnapshot().bAcceptingRequests)
		{
			OutError = "The authoring host or TextureBuild coordinator is unavailable.";
			return false;
		}
		FTexture2DBuildCoordinator* Coordinator = GetTexture2DBuildCoordinator();
		if (!Coordinator)
		{
			OutError = "The TextureBuild coordinator is unavailable.";
			return false;
		}

		const std::string Identity = Texture.GetObjectPath();
		uint64 Generation = 0;
		uint64 PreviousRequestId = 0;
		FAsyncBuildCompletion SupersededCompletion;
		{
			std::lock_guard Lock(GTexture2DAuthoringMutex);
			FTexture2DAuthoringState& State = GTexture2DAuthoringStates[Identity];
			PreviousRequestId = State.ActiveRequestId;
			if (PreviousRequestId != 0)
				SupersededCompletion = std::move(State.Completion);
			Generation = GNextTexture2DGeneration++;
			State = {
				.Texture = TWeakObjectPtr<DTexture2D>(&Texture),
				.Generation = Generation,
				.PublicationContext = {
					.SourcePath = Request.SourcePath,
					.DecoderId = Request.DecoderId,
					.DecoderVersion = Request.DecoderVersion,
					.SourceFileSize = Request.SourceFileSize,
					.SourceLastWriteTime = Request.SourceLastWriteTime,
					.bMarkPackageDirty = Request.bMarkPackageDirty,
					.bReportLoadMutation = Request.bReportLoadMutation},
				.Completion = std::move(Completion)};
		}
		if (PreviousRequestId != 0) Coordinator->Cancel(PreviousRequestId);

		const FTexture2DBuildSettings Settings = Request.Settings;
		const bool bSRGB = Settings.bSRGB.value_or(
			TextureBuilder::GetDefaultSRGB(Settings.Usage));
		const uint32 Width = Request.SourceData.Width;
		const uint32 Height = Request.SourceData.Height;
		const uint64 RequestId = Coordinator->Submit({
			.AssetIdentity = Identity,
			.SourcePath = Request.SourcePath,
			.SourceData = std::move(Request.SourceData),
			.SourceHash = {
				.HashLow = Request.SourceContentHashLow,
				.HashHigh = Request.SourceContentHashHigh},
			.Settings = {
				.Usage = Settings.Usage,
				.bSRGB = bSRGB,
				.MaxResolution = Settings.MaxResolution,
				.CompressionQuality = Settings.CompressionQuality,
				.AlphaMipMode = Settings.AlphaMipMode,
				.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold},
			.Generation = Generation,
			.EstimatedWidth = Width,
			.EstimatedHeight = Height,
			.Priority = Request.Priority,
			.bPersistDerivedData = Request.bPersistDerivedData}, ApplyCompletion);
		if (RequestId == 0)
		{
			{
				std::lock_guard Lock(GTexture2DAuthoringMutex);
				if (FTexture2DAuthoringState* State = FindStateLocked(Identity);
					State && State->Generation == Generation)
					GTexture2DAuthoringStates.erase(Identity);
			}
			if (SupersededCompletion) SupersededCompletion({
				.Status = EAsyncBuildStatus::Superseded,
				.Diagnostic = "The Texture2D authoring build was superseded by a newer request."});
			OutError = "The TextureBuild coordinator rejected the request.";
			return false;
		}
		{
			std::lock_guard Lock(GTexture2DAuthoringMutex);
			FTexture2DAuthoringState* State = FindStateLocked(Identity);
			if (State && State->Generation == Generation)
			{
				State->ActiveRequestId = RequestId;
				State->LastRequestId = RequestId;
			}
		}
		if (SupersededCompletion) SupersededCompletion({
			.Status = EAsyncBuildStatus::Superseded,
			.Diagnostic = "The Texture2D authoring build was superseded by a newer request."});
		OutError.clear();
		return true;
	}

	auto GetTexture2DBuildDiagnostic(const DTexture2D& Texture)
		-> FTexture2DBuildDiagnostic
	{
		uint64 RequestId = 0;
		{
			std::lock_guard Lock(GTexture2DAuthoringMutex);
			if (FTexture2DAuthoringState* State = FindStateLocked(Texture.GetObjectPath()))
				RequestId = State->ActiveRequestId != 0
					? State->ActiveRequestId : State->LastRequestId;
		}
		FTexture2DBuildCoordinator* Coordinator = GetTexture2DBuildCoordinator();
		return Coordinator && RequestId != 0
			? Coordinator->GetDiagnostic(RequestId) : FTexture2DBuildDiagnostic{};
	}

	auto HasPendingTexture2DBuild(const DTexture2D& Texture) -> bool
	{
		std::lock_guard Lock(GTexture2DAuthoringMutex);
		const FTexture2DAuthoringState* State = FindStateLocked(Texture.GetObjectPath());
		return State && State->Texture.Get() == &Texture && State->ActiveRequestId != 0;
	}

	auto CancelTexture2DBuild(DTexture2D& Texture) -> bool
	{
		uint64 RequestId = 0;
		{
			std::lock_guard Lock(GTexture2DAuthoringMutex);
			FTexture2DAuthoringState* State = FindStateLocked(Texture.GetObjectPath());
			if (State && State->Texture.Get() == &Texture) RequestId = State->ActiveRequestId;
		}
		FTexture2DBuildCoordinator* Coordinator = GetTexture2DBuildCoordinator();
		return Coordinator && RequestId != 0 && Coordinator->Cancel(RequestId);
	}

	auto WaitForTexture2DBuild(DTexture2D& Texture, double TimeoutSeconds) -> bool
	{
		uint64 RequestId = 0;
		{
			std::lock_guard Lock(GTexture2DAuthoringMutex);
			FTexture2DAuthoringState* State = FindStateLocked(Texture.GetObjectPath());
			if (State && State->Texture.Get() == &Texture) RequestId = State->ActiveRequestId;
		}
		if (RequestId == 0)
		{
			if (const auto Interchange = TryWaitForTexture2DInterchangeRecovery(
				Texture, TimeoutSeconds)) return *Interchange;
			return true;
		}
		FTexture2DBuildCoordinator* Coordinator = GetTexture2DBuildCoordinator();
		if (!Coordinator || !Coordinator->WaitForRequest(RequestId, TimeoutSeconds)) return false;
		PumpBuildHostCompletions(std::numeric_limits<uint32>::max());
		bool bFailed = false;
		{
			std::lock_guard Lock(GTexture2DAuthoringMutex);
			if (FTexture2DAuthoringState* State = FindStateLocked(Texture.GetObjectPath()))
				bFailed = State->bLastRequestFailed;
		}
		return !HasPendingTexture2DBuild(Texture) && !bFailed
			&& Texture.GetBuildStatus() == ETextureBuildStatus::Ready;
	}
}
