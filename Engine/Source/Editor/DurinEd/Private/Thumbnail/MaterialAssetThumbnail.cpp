#include "Thumbnail/MaterialAssetThumbnail.h"

#include "AssetSystem.h"
#include "DynamicRHI.h"
#include "ImageDecoder.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DRenderResource.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Thumbnail/RenderedAssetThumbnailPipeline.h"
#include "Thumbnail/StaticMeshAssetThumbnail.h"
#include "Thumbnail/TextureCubeAssetThumbnail.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 MaterialThumbnailGeneratorSchema = 3;
		constexpr uint32 MaterialThumbnailShaderContract = 2;
		constexpr float MaterialThumbnailSphereScale = 1.65f;

		// Carries only the immutable asset identity across provider-neutral boundaries.
		class FMaterialThumbnailGenerationInput final : public IAssetThumbnailGenerationInput
		{
		public:
			explicit FMaterialThumbnailGenerationInput(FAssetPath InAssetPath)
				: AssetPath(std::move(InAssetPath))
			{
			}

			FAssetPath AssetPath;
		};

		auto MakeFingerprint(const Asset::FAssetData& Data) -> FAssetThumbnailPackageFingerprint
		{
			return {
				.VirtualPath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks};
		}

		auto GetMaterialResourceRevision(
			DMaterialInterface* Material,
			bool& bOutReady,
			std::string& OutError) -> uint64
		{
			bOutReady = false;
			OutError.clear();
			if (Material == nullptr)
			{
				OutError = "The material asset is unavailable.";
				return 0;
			}
			if (auto* Instance = Cast<DMaterialInstance>(Material); Instance != nullptr
				&& Instance->GetParent() == nullptr)
			{
				OutError = "The material instance has no valid parent.";
				return 0;
			}

			uint64 Revision = Material->GetRenderStateVersion();
			DTexture2D* Texture = nullptr;
			if (!Material->GetTextureParameterValue(
					MaterialParameters::BaseColorTextureName(), Texture)
				|| Texture == nullptr)
			{
				bOutReady = true;
				return Revision;
			}
			if (Texture->GetBuildStatus() != ETextureBuildStatus::Ready)
			{
				OutError = Texture->GetLastBuildError().empty()
					? "A referenced material texture is not built."
					: Texture->GetLastBuildError();
				return 0;
			}
			const ERenderResourceState State =
				Texture->GetRenderResourceState();
			if (State == ERenderResourceState::Failed)
			{
				OutError = "A referenced material texture render resource failed.";
				return 0;
			}
			bOutReady = State == ERenderResourceState::Ready;
			Revision ^= Texture->GetBuildRevision() + 0x9e3779b97f4a7c15ull
				+ (Revision << 6) + (Revision >> 2);
			return Revision == 0 ? 1 : Revision;
		}

		auto GetTextureCubeResourceRevision(
			DTextureCube* TextureCube,
			bool& bOutReady,
			std::string& OutError) -> uint64
		{
			bOutReady = false;
			OutError.clear();
			if (TextureCube == nullptr)
			{
				OutError = "The TextureCube asset is unavailable.";
				return 0;
			}
			if (TextureCube->GetBuildStatus() != ETextureBuildStatus::Ready)
			{
				OutError = TextureCube->GetLastBuildError();
				if (OutError.empty())
				{
					switch (TextureCube->GetBuildStatus())
					{
					case ETextureBuildStatus::Unbuilt:
						OutError = "The TextureCube is not built.";
						break;
					case ETextureBuildStatus::MissingSource:
						OutError = "The TextureCube source is missing.";
						break;
					case ETextureBuildStatus::UnsupportedFormat:
						OutError = "The TextureCube format is unsupported.";
						break;
					default:
						OutError = "The TextureCube build failed.";
						break;
					}
				}
				return 0;
			}
			if (TextureCube->GetTextureReferenceRHI() == nullptr)
			{
				OutError = "The TextureCube has no texture reference.";
				return 0;
			}
			const ERenderResourceState State =
				TextureCube->GetRenderResourceState();
			if (State == ERenderResourceState::Failed)
			{
				OutError = "The TextureCube render resource failed.";
				return 0;
			}
			if (State == ERenderResourceState::Released)
			{
				OutError = "The TextureCube render resource was released.";
				return 0;
			}
			bOutReady = State == ERenderResourceState::Ready;
			return TextureCube->GetBuildRevision();
		}

		auto GetStaticMeshResourceRevision(
			DStaticMesh* StaticMesh,
			bool& bOutReady,
			std::string& OutError) -> uint64
		{
			bOutReady = false;
			OutError.clear();
			if (StaticMesh == nullptr)
			{
				OutError = "The StaticMesh asset is unavailable.";
				return 0;
			}
			if (!StaticMesh->GetLOD0LocalBounds())
			{
				OutError = std::format(
					"StaticMesh '{}' has no valid non-degenerate LOD 0 bounds.",
					StaticMesh->GetObjectPath());
				return 0;
			}
			const FStaticMeshRenderResourceStatus Status =
				StaticMesh->GetRenderResourceStatus();
			switch (Status.Readiness)
			{
			case EStaticMeshRenderResourceReadiness::Ready:
				bOutReady = true;
				break;
			case EStaticMeshRenderResourceReadiness::Queued:
				break;
			case EStaticMeshRenderResourceReadiness::Failed:
				OutError = std::format(
					"StaticMesh '{}' render resource failed.",
					StaticMesh->GetObjectPath());
				break;
			case EStaticMeshRenderResourceReadiness::Unavailable:
				OutError = std::format(
					"StaticMesh '{}' render resource is unavailable.",
					StaticMesh->GetObjectPath());
				break;
			}
			return Status.Revision;
		}

		struct FRenderedThumbnailUploadResult
		{
			FAssetPath AssetPath;
			uint64 Serial = 0;
			FTextureRHIRef Texture;
			uint32 Width = 0;
			uint32 Height = 0;
			std::string Error;
		};

		struct FRenderedThumbnailAsyncState
		{
			std::mutex Mutex;
			std::vector<FRenderedThumbnailUploadResult> Uploads;
		};
	} // namespace

	FMaterialAssetThumbnailProvider::FMaterialAssetThumbnailProvider(std::string InAssetClassName)
		: AssetClassName(std::move(InAssetClassName))
	{
	}

	auto FMaterialAssetThumbnailProvider::GetRegistration() const
		-> FAssetThumbnailProviderRegistration
	{
		return {
			.AssetClassName = AssetClassName,
			.ProviderName = "MaterialRenderedThumbnail",
			.GeneratorSchemaVersion = MaterialThumbnailGeneratorSchema};
	}

	auto FMaterialAssetThumbnailProvider::CaptureGenerationRequest(
		const FAssetThumbnailRequest& Request,
		uint64 ProviderGeneration,
		FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> bool
	{
		OutRequest = {};
		OutError.clear();
		if (Request.Asset.AssetClassName != AssetClassName)
		{
			OutError = "The material thumbnail provider received the wrong asset class.";
			return false;
		}

		const Asset::FAssetRegistry& Registry = Asset::GetAssetRegistry();
		const Asset::FAssetData* Root = Registry.FindAssetExact(Request.Asset.VirtualPath);
		if (Root == nullptr)
		{
			OutError = std::format(
				"Material thumbnail registry data is missing for {}.",
				Request.Asset.VirtualPath.ToString());
			return false;
		}
		if (MakeFingerprint(*Root) != Request.Asset)
		{
			OutError = std::format(
				"Material thumbnail registry data changed for {}; refresh the request snapshot.",
				Request.Asset.VirtualPath.ToString());
			return false;
		}
		std::vector<FAssetThumbnailDependencyNode> Nodes;
		Nodes.reserve(Registry.GetAssets().size());
		for (const auto& [Path, Data] : Registry.GetAssets())
		{
			Nodes.push_back({
				.Package = MakeFingerprint(Data),
				.Dependencies = Data.Dependencies});
		}
		std::vector<FAssetThumbnailPackageFingerprint> Dependencies;
		if (!BuildAssetThumbnailDependencyClosure(
				Request.Asset.VirtualPath, Nodes, Dependencies, OutError))
			return false;

		const FRenderedAssetThumbnailVisualContract Visual;
		OutRequest.KeyInput = {
			.Output = Visual.Output,
			.PreviewFixtureIdentity = std::string(
				FRenderedAssetThumbnailVisualContract::SphereVirtualPath),
			.PreviewFixtureVersion =
				FRenderedAssetThumbnailVisualContract::SphereFixtureVersion,
			.ShaderContractVersion = MaterialThumbnailShaderContract,
			.Dependencies = std::move(Dependencies)};
		OutRequest.Input =
			std::make_shared<FMaterialThumbnailGenerationInput>(Request.Asset.VirtualPath);
		OutRequest.ProviderGeneration = ProviderGeneration;
		OutRequest.RequestSerial = Request.RequestSerial;
		return true;
	}

	struct FRenderedAssetThumbnailCache::FImpl
	{
		struct FEntry
		{
			FAssetThumbnailPackageFingerprint Fingerprint;
			FTextureRHIRef Texture;
			uint64 Serial = 1;
			uint64 LastUsedFrame = 0;
			bool bVisible = false;
			bool bUploading = false;
			bool bUploadFailed = false;
			uint32 Width = 0;
			uint32 Height = 0;
			std::string Diagnostic;
		};

		FAssetThumbnailBudgets Budgets;
		FAssetThumbnailProviderRegistry Registry;
		FAssetThumbnailScheduler Scheduler;
		FRenderedAssetThumbnailPipeline Pipeline;
		std::unique_ptr<FRenderedAssetThumbnailPreviewScenePool> ScenePool;
		std::unordered_map<FAssetPath, FEntry> Entries;
		std::shared_ptr<FRenderedThumbnailAsyncState> AsyncState =
			std::make_shared<FRenderedThumbnailAsyncState>();
		std::optional<FRenderedAssetThumbnailJob> ActiveJob;
		DMaterialInterface* ActiveMaterial = nullptr;
		DTextureCube* ActiveTextureCube = nullptr;
		DStaticMesh* ActiveStaticMesh = nullptr;
		uint64 FrameNumber = 0;
		uint64 PreviewSceneCreations = 0;
		uint64 PreviewSceneAssignments = 0;
		uint64 UploadsQueued = 0;
		uint64 UploadsCompleted = 0;
		uint64 UploadFailures = 0;
		uint64 GpuEvictions = 0;
		bool bProvidersRegistered = false;

		explicit FImpl(
			FAssetThumbnailBudgets InBudgets,
			FAssetThumbnailObjectStoreSettings StoreSettings)
			: Budgets(InBudgets)
			, Scheduler(Registry, Budgets)
			, Pipeline(Scheduler, std::move(StoreSettings), Budgets)
		{
			std::string Error;
			auto Material = std::make_shared<FMaterialAssetThumbnailProvider>(
				DMaterial::StaticClass()->GetQualifiedName().ToString());
			auto Instance = std::make_shared<FMaterialAssetThumbnailProvider>(
				DMaterialInstance::StaticClass()->GetQualifiedName().ToString());
			auto TextureCube = std::make_shared<FTextureCubeAssetThumbnailProvider>();
			auto StaticMesh = std::make_shared<FStaticMeshAssetThumbnailProvider>();
			bProvidersRegistered =
				Registry.Register(std::move(Material), Error)
				&& Registry.Register(std::move(Instance), Error)
				&& Registry.Register(std::move(TextureCube), Error)
				&& Registry.Register(std::move(StaticMesh), Error);
		}

		auto ClearActiveAssetReferences() -> void
		{
			ActiveMaterial = nullptr;
			ActiveTextureCube = nullptr;
			ActiveStaticMesh = nullptr;
		}

		auto ValidateActiveStaticMeshRevision(uint64 ExpectedRevision) const
			-> std::string
		{
			if (ActiveStaticMesh == nullptr) return {};
			bool bResourcesReady = false;
			std::string Error;
			const uint64 CurrentRevision = GetStaticMeshResourceRevision(
				ActiveStaticMesh, bResourcesReady, Error);
			if (!Error.empty()) return Error;
			if (!bResourcesReady || CurrentRevision != ExpectedRevision)
			{
				return std::format(
					"StaticMesh '{}' changed while its thumbnail was being generated.",
					ActiveJob
						? ActiveJob->ScheduledJob.GenerationRequest.KeyInput.Asset
							.VirtualPath.ToString()
						: ActiveStaticMesh->GetObjectPath());
			}
			return {};
		}

		auto QualifyStaticMeshDiagnostic(std::string_view Detail) const
			-> std::string
		{
			if (ActiveStaticMesh == nullptr) return std::string(Detail);
			return std::format(
				"StaticMesh '{}' thumbnail generation failed: {}",
				ActiveJob
					? ActiveJob->ScheduledJob.GenerationRequest.KeyInput.Asset
						.VirtualPath.ToString()
					: ActiveStaticMesh->GetObjectPath(),
				Detail.empty() ? "unknown preview error" : Detail);
		}

		auto EnsureScene() -> bool
		{
			if (ScenePool != nullptr) return ScenePool->IsAvailable();
			ScenePool = std::make_unique<FRenderedAssetThumbnailPreviewScenePool>(
				FRenderedAssetThumbnailVisualContract{}, Budgets);
			++PreviewSceneCreations;
			return ScenePool->IsAvailable();
		}

		auto UnregisterTexture(FEntry& Entry) -> void
		{
			if (Entry.Texture && Mona::GActiveUIBackend)
				Mona::GActiveUIBackend->UnregisterTexture(Entry.Texture);
			Entry.Texture = nullptr;
		}

		auto QueueUpload(
			const FAssetPath& AssetPath,
			uint64 Serial,
			std::vector<uint8> Pixels,
			uint32 Width,
			uint32 Height) -> void
		{
			auto It = Entries.find(AssetPath);
			if (It == Entries.end() || It->second.Serial != Serial) return;
			It->second.bUploading = true;
			It->second.bUploadFailed = false;
			++UploadsQueued;
			const std::weak_ptr<FRenderedThumbnailAsyncState> WeakState = AsyncState;
			auto SharedPixels = std::make_shared<std::vector<uint8>>(std::move(Pixels));
			ENQUEUE_RENDER_COMMAND(UploadRenderedAssetThumbnail)(
				[WeakState, AssetPath, Serial, SharedPixels, Width, Height](
					FRHICommandListImmediate& CommandList) {
					FRenderedThumbnailUploadResult Result{
						.AssetPath = AssetPath,
						.Serial = Serial,
						.Width = Width,
						.Height = Height};
					FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
						"RenderedAssetThumbnail", Width, Height, EPixelFormat::SRGBA8_UNORM);
					Desc.AddFlags(ETextureCreateFlags::ShaderResource);
					Result.Texture = GDynamicRHI != nullptr
						? GDynamicRHI->RHICreateTexture(CommandList, Desc)
						: nullptr;
					if (Result.Texture != nullptr)
					{
						const FUpdateTextureRegion2D Region(0, 0, 0, 0, Width, Height);
						GDynamicRHI->RHIUpdateTexture2D(
							CommandList,
							Result.Texture,
							0,
							0,
							Region,
							Width * 4,
							SharedPixels->data());
					}
					else
					{
						Result.Error = "Unable to create the rendered-asset thumbnail UI texture.";
					}
					if (const std::shared_ptr<FRenderedThumbnailAsyncState> State =
						WeakState.lock())
					{
						std::lock_guard Lock(State->Mutex);
						State->Uploads.push_back(std::move(Result));
					}
				});
		}

		auto DecodeAndQueueUpload(
			const FAssetPath& AssetPath,
			uint64 Serial,
			std::span<const uint8> EncodedBytes) -> bool
		{
			Asset::FDecodedImage Image;
			std::string Error;
			if (!Asset::DecodeImageFromMemory(EncodedBytes, Image, Error))
			{
				if (auto It = Entries.find(AssetPath); It != Entries.end())
				{
					It->second.Diagnostic = std::move(Error);
					It->second.bUploadFailed = true;
				}
				return false;
			}
			QueueUpload(
				AssetPath, Serial, std::move(Image.Pixels), Image.Width, Image.Height);
			return true;
		}

		auto DrainUploads() -> void
		{
			std::vector<FRenderedThumbnailUploadResult> Results;
			{
				std::lock_guard Lock(AsyncState->Mutex);
				Results.swap(AsyncState->Uploads);
			}
			for (FRenderedThumbnailUploadResult& Result : Results)
			{
				auto It = Entries.find(Result.AssetPath);
				if (It == Entries.end() || It->second.Serial != Result.Serial) continue;
				FEntry& Entry = It->second;
				Entry.bUploading = false;
				if (Result.Texture == nullptr || !Mona::GActiveUIBackend)
				{
					Entry.Diagnostic = Result.Error.empty()
						? "The UI backend is unavailable for rendered-asset thumbnails."
						: std::move(Result.Error);
					Entry.bUploadFailed = true;
					++UploadFailures;
					continue;
				}
				UnregisterTexture(Entry);
				Mona::GActiveUIBackend->RegisterTexture(Result.Texture);
				Entry.Texture = std::move(Result.Texture);
				Entry.Width = Result.Width;
				Entry.Height = Result.Height;
				Entry.Diagnostic.clear();
				Entry.bUploadFailed = false;
				++UploadsCompleted;
			}
		}

		auto FinishCapture() -> void
		{
			if (!ActiveJob || ScenePool == nullptr) return;
			std::vector<uint8> Pixels;
			std::string Error;
			const ERenderedAssetThumbnailCaptureState State =
				ScenePool->PollCapture(Pixels, Error);
			if (State == ERenderedAssetThumbnailCaptureState::Rendering
				|| State == ERenderedAssetThumbnailCaptureState::Idle)
				return;

			const FAssetPath AssetPath =
				ActiveJob->ScheduledJob.GenerationRequest.KeyInput.Asset.VirtualPath;
			const uint64 Serial =
				ActiveJob->ScheduledJob.GenerationRequest.RequestSerial;
			if (State == ERenderedAssetThumbnailCaptureState::Failed)
			{
				if (ActiveStaticMesh != nullptr)
					Error = QualifyStaticMeshDiagnostic(Error);
				Pipeline.CompleteRender(
					*ActiveJob, ActiveJob->AssetRevision, ActiveJob->ResourceRevision, Error);
			}
			else if (const std::string RevisionError =
					ValidateActiveStaticMeshRevision(ActiveJob->ResourceRevision);
				!RevisionError.empty())
			{
				Pipeline.CompleteRender(
					*ActiveJob,
					ActiveJob->AssetRevision,
					ActiveJob->ResourceRevision,
					RevisionError);
			}
			else if (Pipeline.CompleteRender(
						*ActiveJob,
						ActiveJob->AssetRevision,
						ActiveJob->ResourceRevision)
				&& Pipeline.CompleteReadback(
					*ActiveJob,
					ActiveJob->AssetRevision,
					ActiveJob->ResourceRevision)
				&& Pipeline.CompletePixels(
					*ActiveJob,
					ActiveJob->AssetRevision,
					ActiveJob->ResourceRevision,
					Pixels,
					FRenderedAssetThumbnailVisualContract{}.Output.Width,
					FRenderedAssetThumbnailVisualContract{}.Output.Height,
					{},
					[this, ExpectedRevision = ActiveJob->ResourceRevision]() {
						return ValidateActiveStaticMeshRevision(ExpectedRevision);
					}))
			{
				QueueUpload(
					AssetPath,
					Serial,
					std::move(Pixels),
					FRenderedAssetThumbnailVisualContract{}.Output.Width,
					FRenderedAssetThumbnailVisualContract{}.Output.Height);
			}
			ScenePool->Reset();
			ActiveJob.reset();
			ClearActiveAssetReferences();
		}

		auto TryBeginCapture() -> void
		{
			if (!ActiveJob || (ActiveMaterial == nullptr
				&& ActiveTextureCube == nullptr
				&& ActiveStaticMesh == nullptr))
				return;
			bool bResourcesReady = false;
			std::string Error;
			const uint64 ResourceRevision = ActiveStaticMesh != nullptr
				? GetStaticMeshResourceRevision(
					ActiveStaticMesh, bResourcesReady, Error)
				: ActiveTextureCube != nullptr
					? GetTextureCubeResourceRevision(
						ActiveTextureCube, bResourcesReady, Error)
					: GetMaterialResourceRevision(
						ActiveMaterial, bResourcesReady, Error);
			if (!Error.empty())
			{
				if (ActiveStaticMesh != nullptr)
					Error = QualifyStaticMeshDiagnostic(Error);
				Pipeline.BeginRender(
					*ActiveJob,
					false,
					ActiveJob->AssetRevision,
					ResourceRevision,
					Error);
				if (ActiveJob->ResourceRevision != 0)
				{
					Pipeline.CompleteRender(
						*ActiveJob,
						ActiveJob->AssetRevision,
						ActiveJob->ResourceRevision,
						Error);
				}
				if (ScenePool) ScenePool->Reset();
				ActiveJob.reset();
				ClearActiveAssetReferences();
				return;
			}
			if (ActiveStaticMesh != nullptr && bResourcesReady)
			{
				if (!Pipeline.BeginRender(
						*ActiveJob,
						true,
						ActiveJob->AssetRevision,
						ResourceRevision))
					return;
				if (!EnsureScene())
				{
					Error = QualifyStaticMeshDiagnostic(
						ScenePool != nullptr
							? ScenePool->GetDiagnostic()
							: "The rendered-thumbnail scene is unavailable.");
					Pipeline.CompleteRender(
						*ActiveJob,
						ActiveJob->AssetRevision,
						ActiveJob->ResourceRevision,
						Error);
					ActiveJob.reset();
					ClearActiveAssetReferences();
					return;
				}
				const auto Input = std::dynamic_pointer_cast<
					const FStaticMeshAssetThumbnailGenerationInput>(
					ActiveJob->ScheduledJob.GenerationRequest.Input);
				const std::optional<FBox> Bounds = ActiveStaticMesh->GetLOD0LocalBounds();
				FStaticMeshAssetThumbnailView ThumbnailView;
				if (Input == nullptr || !Bounds
					|| !CalculateStaticMeshAssetThumbnailView({
						.LocalBounds = Bounds.value_or(FBox()),
						.OutputAspectRatio = Input != nullptr
							? static_cast<double>(Input->VisualContract.Output.Width)
								/ static_cast<double>(Input->VisualContract.Output.Height)
							: 1.0,
						.VerticalFieldOfViewDegrees = Input != nullptr
							? Input->VisualContract.VerticalFieldOfViewDegrees
							: 42.0,
						.CameraDirection = Input != nullptr
							? FVector3(
								Input->VisualContract.CameraDirectionX,
								Input->VisualContract.CameraDirectionY,
								Input->VisualContract.CameraDirectionZ)
							: FVector3(2.6, -2.6, 1.8)},
						ThumbnailView,
						Error)
					|| !ScenePool->SetStaticMesh(ActiveStaticMesh, ThumbnailView, Error))
				{
					if (Error.empty()) Error = "StaticMesh thumbnail input is invalid.";
					Error = QualifyStaticMeshDiagnostic(Error);
					Pipeline.CompleteRender(
						*ActiveJob,
						ActiveJob->AssetRevision,
						ActiveJob->ResourceRevision,
						Error);
					ScenePool->Reset();
					ActiveJob.reset();
					ClearActiveAssetReferences();
					return;
				}
				++PreviewSceneAssignments;
				Error = ValidateActiveStaticMeshRevision(ActiveJob->ResourceRevision);
				if (!Error.empty()
					|| !ScenePool->BeginCapture(
						Error, FStaticMeshAssetThumbnailContract::bOutputOpaque))
				{
					if (Error.empty()) Error = "The preview capture could not start.";
					if (Error.find("StaticMesh '") != 0)
						Error = QualifyStaticMeshDiagnostic(Error);
					Pipeline.CompleteRender(
						*ActiveJob,
						ActiveJob->AssetRevision,
						ActiveJob->ResourceRevision,
						Error);
					ScenePool->Reset();
					ActiveJob.reset();
					ClearActiveAssetReferences();
				}
				return;
			}
			if (!Pipeline.BeginRender(
					*ActiveJob,
					bResourcesReady,
					ActiveJob->AssetRevision,
					ResourceRevision))
				return;
			if (!bResourcesReady) return;
			if (!EnsureScene())
			{
				Pipeline.CompleteRender(
					*ActiveJob,
					ActiveJob->AssetRevision,
					ActiveJob->ResourceRevision,
					ScenePool != nullptr
						? ScenePool->GetDiagnostic()
						: "The rendered-thumbnail scene is unavailable.");
				ActiveJob.reset();
				ClearActiveAssetReferences();
				return;
			}
			const bool bTextureCube = ActiveTextureCube != nullptr;
			FTransform PreviewTransform;
			if (!bTextureCube)
				PreviewTransform.Scale3D = FVector3(MaterialThumbnailSphereScale);
			const bool bSceneReady = bTextureCube
				? ScenePool->SetTextureCube(ActiveTextureCube, PreviewTransform, Error)
				: ScenePool->SetMaterial(
					ScenePool->GetSphereMesh(), ActiveMaterial, PreviewTransform, Error);
			if (bSceneReady) ++PreviewSceneAssignments;
			if (!bSceneReady
				|| !ScenePool->BeginCapture(Error, bTextureCube))
			{
				Pipeline.CompleteRender(
					*ActiveJob,
					ActiveJob->AssetRevision,
					ActiveJob->ResourceRevision,
					Error);
				ScenePool->Reset();
				ActiveJob.reset();
				ClearActiveAssetReferences();
			}
		}

		auto StartNext() -> void
		{
			FRenderedAssetThumbnailStartResult Start = Pipeline.StartNextDetailed();
			if (Start.WarmJob)
			{
				const FAssetThumbnailScheduledJob& WarmJob = *Start.WarmJob;
				const FAssetPath& Path =
					WarmJob.GenerationRequest.KeyInput.Asset.VirtualPath;
				if (!DecodeAndQueueUpload(
						Path, WarmJob.GenerationRequest.RequestSerial, Start.EncodedBytes))
				{
					Pipeline.InvalidatePersistentObject(WarmJob.CacheKey);
					Pipeline.RecordRetry();
					Scheduler.Cancel(Path);
					std::string Error;
					if (Scheduler.Request({
							.Asset = WarmJob.GenerationRequest.KeyInput.Asset,
							.Priority = WarmJob.Priority,
							.RequestSerial = WarmJob.GenerationRequest.RequestSerial}, Error))
					{
						if (auto It = Entries.find(Path); It != Entries.end())
						{
							It->second.Diagnostic.clear();
							It->second.bUploadFailed = false;
						}
					}
				}
				return;
			}
			if (!Start.ColdJob) return;
			ActiveJob = std::move(Start.ColdJob);
			if (const auto StaticMeshInput = std::dynamic_pointer_cast<
					const FStaticMeshAssetThumbnailGenerationInput>(
					ActiveJob->ScheduledJob.GenerationRequest.Input))
			{
				DObject* Loaded = nullptr;
				const Asset::FAssetResult Result =
					Asset::LoadAsset(StaticMeshInput->AssetPath, Loaded);
				ActiveStaticMesh = Result ? Cast<DStaticMesh>(Loaded) : nullptr;
				if (ActiveStaticMesh != nullptr
					&& ActiveStaticMesh->GetClass() != DStaticMesh::StaticClass())
				{
					ActiveStaticMesh = nullptr;
				}
				if (!Result || ActiveStaticMesh == nullptr)
				{
					Pipeline.CompleteLoad(
						*ActiveJob,
						0,
						Result.Message.empty()
							? std::format(
								"The requested asset '{}' is not an exact DStaticMesh.",
								StaticMeshInput->AssetPath.ToString())
							: Result.Message);
					ActiveJob.reset();
					ClearActiveAssetReferences();
					return;
				}
				FStaticMeshRenderResourceStatus Status =
					ActiveStaticMesh->GetRenderResourceStatus();
				if (!ActiveStaticMesh->GetLOD0LocalBounds())
				{
					Pipeline.CompleteLoad(
						*ActiveJob,
						Status.Revision,
						std::format(
							"StaticMesh '{}' has no valid non-degenerate LOD 0 bounds.",
							StaticMeshInput->AssetPath.ToString()));
					ActiveJob.reset();
					ClearActiveAssetReferences();
					return;
				}
				if (Status.Readiness
					== EStaticMeshRenderResourceReadiness::Unavailable)
				{
					ActiveStaticMesh->InitResources();
					Status = ActiveStaticMesh->GetRenderResourceStatus();
				}
				if (!Pipeline.CompleteLoad(*ActiveJob, Status.Revision))
				{
					ActiveJob.reset();
					ClearActiveAssetReferences();
					return;
				}
				TryBeginCapture();
				return;
			}
			if (const auto CubeInput =
					std::dynamic_pointer_cast<const FTextureCubeThumbnailGenerationInput>(
						ActiveJob->ScheduledJob.GenerationRequest.Input))
			{
				DObject* Loaded = nullptr;
				const Asset::FAssetResult Result =
					Asset::LoadAsset(CubeInput->AssetPath, Loaded);
				ActiveTextureCube = Result ? Cast<DTextureCube>(Loaded) : nullptr;
				if (!Result || ActiveTextureCube == nullptr)
				{
					Pipeline.CompleteLoad(
						*ActiveJob,
						0,
						Result.Message.empty()
							? "The requested asset is not a TextureCube."
							: Result.Message);
					ActiveJob.reset();
					ClearActiveAssetReferences();
					return;
				}
				if (!Pipeline.CompleteLoad(
						*ActiveJob, ActiveTextureCube->GetBuildRevision()))
				{
					ActiveJob.reset();
					ClearActiveAssetReferences();
					return;
				}
				TryBeginCapture();
				return;
			}
			const auto Input = std::dynamic_pointer_cast<const FMaterialThumbnailGenerationInput>(
				ActiveJob->ScheduledJob.GenerationRequest.Input);
			if (Input == nullptr)
			{
				Pipeline.CompleteLoad(
					*ActiveJob, 0, "Rendered thumbnail input is invalid.");
				ActiveJob.reset();
				ClearActiveAssetReferences();
				return;
			}
			DObject* Loaded = nullptr;
			const Asset::FAssetResult Result = Asset::LoadAsset(Input->AssetPath, Loaded);
			ActiveMaterial = Result ? Cast<DMaterialInterface>(Loaded) : nullptr;
			if (!Result || ActiveMaterial == nullptr)
			{
				Pipeline.CompleteLoad(
					*ActiveJob,
					0,
					Result.Message.empty()
						? "The requested asset is not a material."
						: Result.Message);
				ActiveJob.reset();
				ClearActiveAssetReferences();
				return;
			}
			const uint64 AssetRevision = ActiveMaterial->GetRenderStateVersion();
			if (!Pipeline.CompleteLoad(*ActiveJob, AssetRevision))
			{
				ActiveJob.reset();
				ClearActiveAssetReferences();
				return;
			}
			TryBeginCapture();
		}

		auto EvictToBudget() -> void
		{
			std::vector<FAssetThumbnailBudgetEntry> BudgetEntries;
			BudgetEntries.reserve(Entries.size());
			for (const auto& [Path, Entry] : Entries)
			{
				BudgetEntries.push_back({
					.Key = Path.ToString(),
					.Bytes = Entry.Texture
						? static_cast<uint64>(Entry.Width) * Entry.Height * 4
						: 0,
					.LastUsed = Entry.LastUsedFrame,
					.bPinned = Entry.bVisible});
			}
			for (const std::string& Key : SelectAssetThumbnailBudgetEvictions(
					BudgetEntries, Budgets.GpuTextureBudgetBytes))
			{
				FAssetPath Path;
				if (!FAssetPath::TryCreate(Key, Path)) continue;
				if (auto It = Entries.find(Path); It != Entries.end())
				{
					UnregisterTexture(It->second);
					Entries.erase(It);
					Scheduler.Cancel(Path);
					++GpuEvictions;
				}
			}
		}
	};

	FRenderedAssetThumbnailCache::FRenderedAssetThumbnailCache(
		FAssetThumbnailBudgets Budgets,
		FAssetThumbnailObjectStoreSettings StoreSettings)
		: Impl(std::make_unique<FImpl>(Budgets, std::move(StoreSettings)))
	{
	}

	FRenderedAssetThumbnailCache::~FRenderedAssetThumbnailCache()
	{
		Clear();
		Impl->ScenePool.reset();
		Impl->AsyncState.reset();
		Impl->Registry.Shutdown();
	}

	auto FRenderedAssetThumbnailCache::BeginFrame() -> void
	{
		++Impl->FrameNumber;
		for (auto& [Path, Entry] : Impl->Entries) Entry.bVisible = false;
		Impl->DrainUploads();
		Impl->FinishCapture();
		Impl->Pipeline.BeginFrame();
	}

	auto FRenderedAssetThumbnailCache::Request(
		const FAssetThumbnailPackageFingerprint& Asset,
		EAssetThumbnailPriority Priority) -> void
	{
		if (!Impl->bProvidersRegistered || !Asset.VirtualPath.IsValid()) return;
		auto [It, bInserted] = Impl->Entries.try_emplace(Asset.VirtualPath);
		FImpl::FEntry& Entry = It->second;
		if (bInserted)
			Entry.Fingerprint = Asset;
		else if (Entry.Fingerprint != Asset)
		{
			const uint64 NextSerial = Entry.Serial + 1;
			Impl->Scheduler.Cancel(Asset.VirtualPath);
			Impl->UnregisterTexture(Entry);
			Entry = {};
			Entry.Fingerprint = Asset;
			Entry.Serial = NextSerial;
		}
		Entry.bVisible |= Priority == EAssetThumbnailPriority::Visible;
		Entry.LastUsedFrame = Impl->FrameNumber;
		if (Entry.Texture)
		{
			Entry.Diagnostic.clear();
			return;
		}
		std::string Error;
		if (!Impl->Scheduler.Request({
				.Asset = Asset,
				.Priority = Priority,
				.RequestSerial = Entry.Serial}, Error))
		{
			Entry.Diagnostic = std::move(Error);
		}
		else if (!Entry.bUploadFailed)
		{
			Entry.Diagnostic.clear();
		}
	}

	auto FRenderedAssetThumbnailCache::Find(const FAssetPath& AssetPath) const
		-> FAssetThumbnailView
	{
		FAssetThumbnailView View = Impl->Scheduler.Find(AssetPath);
		const auto It = Impl->Entries.find(AssetPath);
		if (It == Impl->Entries.end()) return View;
		const FImpl::FEntry& Entry = It->second;
		View.Texture = Entry.Texture;
		View.Width = Entry.Width;
		View.Height = Entry.Height;
		View.bHasTransparency =
			Entry.Fingerprint.AssetClassName
				!= DTextureCube::StaticClass()->GetQualifiedName().ToString();
		View.bShowTransparencyGrid = false;
		if (Entry.Texture)
			View.State = EAssetThumbnailState::Ready;
		else if (View.State == EAssetThumbnailState::Ready
			&& Entry.bUploading)
			View.State = EAssetThumbnailState::Uploading;
		else if (View.State == EAssetThumbnailState::Ready && Entry.bUploadFailed)
			View.State = EAssetThumbnailState::Failed;
		if (!Entry.Diagnostic.empty()) View.Diagnostic = Entry.Diagnostic;
		return View;
	}

	auto FRenderedAssetThumbnailCache::EndFrame() -> void
	{
		if (Impl->ActiveJob)
			Impl->TryBeginCapture();
		else
			Impl->StartNext();
		Impl->EvictToBudget();
	}

	auto FRenderedAssetThumbnailCache::CancelPendingRequests() -> void
	{
		if (Impl->ActiveJob) Impl->Pipeline.Cancel(*Impl->ActiveJob);
		Impl->ActiveJob.reset();
		Impl->ClearActiveAssetReferences();
		if (Impl->ScenePool) Impl->ScenePool->Reset();
		Impl->Scheduler.CancelAll();
		for (auto& [Path, Entry] : Impl->Entries)
		{
			++Entry.Serial;
			Entry.bUploading = false;
			Entry.bUploadFailed = false;
			Entry.Diagnostic.clear();
		}
	}

	auto FRenderedAssetThumbnailCache::Clear() -> void
	{
		CancelPendingRequests();
		for (auto& [Path, Entry] : Impl->Entries) Impl->UnregisterTexture(Entry);
		Impl->Entries.clear();
	}

	auto FRenderedAssetThumbnailCache::GetStats() const
		-> FRenderedAssetThumbnailCacheStats
	{
		uint64 LiveGpuTextures = 0;
		for (const auto& [Path, Entry] : Impl->Entries)
			LiveGpuTextures += Entry.Texture != nullptr ? 1u : 0u;
		return {
			.Pipeline = Impl->Pipeline.GetStats(),
			.PreviewSceneCreations = Impl->PreviewSceneCreations,
			.PreviewSceneAssignments = Impl->PreviewSceneAssignments,
			.UploadsQueued = Impl->UploadsQueued,
			.UploadsCompleted = Impl->UploadsCompleted,
			.UploadFailures = Impl->UploadFailures,
			.GpuEvictions = Impl->GpuEvictions,
			.LiveGpuTextures = LiveGpuTextures,
			.bHasActiveJob = Impl->ActiveJob.has_value(),
			.bHasPreviewScene = Impl->ScenePool != nullptr};
	}
} // namespace Durin
