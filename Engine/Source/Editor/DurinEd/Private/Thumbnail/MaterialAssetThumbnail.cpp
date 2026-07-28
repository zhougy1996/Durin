#include "Thumbnail/MaterialAssetThumbnail.h"

#include "AssetSystem.h"
#include "DynamicRHI.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "ImageDecoder.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DRenderResource.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Thumbnail/RenderedAssetThumbnailPipeline.h"
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

		auto MakeMaterialUpdates(
			DStaticMesh* Mesh,
			DMaterialInterface* Material,
			uint64 ComponentRevision) -> std::vector<FMaterialRenderUpdate>
		{
			const FStaticMeshRenderData* RenderData = Mesh != nullptr ? Mesh->GetRenderData() : nullptr;
			const uint32 SlotCount =
				RenderData != nullptr ? static_cast<uint32>(RenderData->MaterialSlots.size()) : 0;
			std::vector<FMaterialRenderUpdate> Updates;
			Updates.reserve(SlotCount);
			for (uint32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
			{
				Updates.push_back({
					.SlotIndex = SlotIndex,
					.RenderData = Material->GetRenderData(),
					.MaterialVersion = Material->GetRenderStateVersion(),
					.ComponentRevision = ComponentRevision,
					.DirtyFlags = EMaterialRenderDirtyFlags::ParameterValues
						| EMaterialRenderDirtyFlags::ParentChain});
			}
			return Updates;
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

		struct FMaterialThumbnailUploadResult
		{
			FAssetPath AssetPath;
			uint64 Serial = 0;
			FTextureRHIRef Texture;
			uint32 Width = 0;
			uint32 Height = 0;
			std::string Error;
		};

		struct FMaterialThumbnailAsyncState
		{
			std::mutex Mutex;
			std::vector<FMaterialThumbnailUploadResult> Uploads;
		};
	} // namespace

	auto CreateMaterialPreviewPrimitive(
		DStaticMesh* Mesh,
		DMaterialInterface* Material,
		uint64 ComponentRevision,
		std::string& OutError) -> std::unique_ptr<PrimitiveSceneProxy>
	{
		OutError.clear();
		if (Mesh == nullptr || Mesh->GetRenderData() == nullptr
			|| Mesh->GetRenderData()->LODResources.empty())
		{
			OutError = "The material preview mesh has no render data.";
			return nullptr;
		}
		if (Material == nullptr)
		{
			OutError = "The material preview has no material.";
			return nullptr;
		}
		return std::make_unique<FStaticMeshSceneProxy>(
			Mesh->GetRenderData(),
			MakeMaterialUpdates(Mesh, Material, ComponentRevision));
	}

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
		const Asset::FAssetData* Root = Registry.FindAsset(Request.Asset.VirtualPath);
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

	struct FMaterialAssetThumbnailCache::FImpl
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
		std::shared_ptr<FMaterialThumbnailAsyncState> AsyncState =
			std::make_shared<FMaterialThumbnailAsyncState>();
		std::optional<FRenderedAssetThumbnailJob> ActiveJob;
		DMaterialInterface* ActiveMaterial = nullptr;
		DTextureCube* ActiveTextureCube = nullptr;
		uint64 FrameNumber = 0;
		bool bProvidersRegistered = false;

		explicit FImpl(FAssetThumbnailBudgets InBudgets)
			: Budgets(InBudgets)
			, Scheduler(Registry, Budgets)
			, Pipeline(Scheduler, {}, Budgets)
		{
			std::string Error;
			auto Material = std::make_shared<FMaterialAssetThumbnailProvider>(
				DMaterial::StaticClass()->GetQualifiedName().ToString());
			auto Instance = std::make_shared<FMaterialAssetThumbnailProvider>(
				DMaterialInstance::StaticClass()->GetQualifiedName().ToString());
			auto TextureCube = std::make_shared<FTextureCubeAssetThumbnailProvider>();
			bProvidersRegistered =
				Registry.Register(std::move(Material), Error)
				&& Registry.Register(std::move(Instance), Error)
				&& Registry.Register(std::move(TextureCube), Error);
		}

		auto EnsureScene() -> bool
		{
			if (ScenePool != nullptr) return ScenePool->IsAvailable();
			ScenePool = std::make_unique<FRenderedAssetThumbnailPreviewScenePool>(
				FRenderedAssetThumbnailVisualContract{}, Budgets);
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
			const std::weak_ptr<FMaterialThumbnailAsyncState> WeakState = AsyncState;
			auto SharedPixels = std::make_shared<std::vector<uint8>>(std::move(Pixels));
			ENQUEUE_RENDER_COMMAND(UploadMaterialAssetThumbnail)(
				[WeakState, AssetPath, Serial, SharedPixels, Width, Height](
					FRHICommandListImmediate& CommandList) {
					FMaterialThumbnailUploadResult Result{
						.AssetPath = AssetPath,
						.Serial = Serial,
						.Width = Width,
						.Height = Height};
					FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
						"MaterialAssetThumbnail", Width, Height, EPixelFormat::SRGBA8_UNORM);
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
						Result.Error = "Unable to create the material thumbnail UI texture.";
					}
					if (const std::shared_ptr<FMaterialThumbnailAsyncState> State =
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
			std::span<const uint8> EncodedBytes) -> void
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
				return;
			}
			QueueUpload(
				AssetPath, Serial, std::move(Image.Pixels), Image.Width, Image.Height);
		}

		auto DrainUploads() -> void
		{
			std::vector<FMaterialThumbnailUploadResult> Results;
			{
				std::lock_guard Lock(AsyncState->Mutex);
				Results.swap(AsyncState->Uploads);
			}
			for (FMaterialThumbnailUploadResult& Result : Results)
			{
				auto It = Entries.find(Result.AssetPath);
				if (It == Entries.end() || It->second.Serial != Result.Serial) continue;
				FEntry& Entry = It->second;
				Entry.bUploading = false;
				if (Result.Texture == nullptr || !Mona::GActiveUIBackend)
				{
					Entry.Diagnostic = Result.Error.empty()
						? "The UI backend is unavailable for material thumbnails."
						: std::move(Result.Error);
					Entry.bUploadFailed = true;
					continue;
				}
				UnregisterTexture(Entry);
				Mona::GActiveUIBackend->RegisterTexture(Result.Texture);
				Entry.Texture = std::move(Result.Texture);
				Entry.Width = Result.Width;
				Entry.Height = Result.Height;
				Entry.Diagnostic.clear();
				Entry.bUploadFailed = false;
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
				Pipeline.CompleteRender(
					*ActiveJob, ActiveJob->AssetRevision, ActiveJob->ResourceRevision, Error);
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
					FRenderedAssetThumbnailVisualContract{}.Output.Height))
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
			ActiveMaterial = nullptr;
			ActiveTextureCube = nullptr;
		}

		auto TryBeginCapture() -> void
		{
			if (!ActiveJob || (ActiveMaterial == nullptr && ActiveTextureCube == nullptr))
				return;
			bool bResourcesReady = false;
			std::string Error;
			const uint64 ResourceRevision = ActiveTextureCube != nullptr
				? GetTextureCubeResourceRevision(
					ActiveTextureCube, bResourcesReady, Error)
				: GetMaterialResourceRevision(
					ActiveMaterial, bResourcesReady, Error);
			if (!Error.empty())
			{
				Pipeline.BeginRender(
					*ActiveJob,
					false,
					ActiveJob->AssetRevision,
					ResourceRevision,
					Error);
				ActiveJob.reset();
				ActiveMaterial = nullptr;
				ActiveTextureCube = nullptr;
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
				ActiveMaterial = nullptr;
				ActiveTextureCube = nullptr;
				return;
			}
			std::unique_ptr<PrimitiveSceneProxy> Proxy =
				ActiveTextureCube != nullptr
				? CreateTextureCubePreviewPrimitive(
					ScenePool->GetSphereMesh(), ActiveTextureCube, Error)
				: CreateMaterialPreviewPrimitive(
					ScenePool->GetSphereMesh(),
					ActiveMaterial,
					ActiveJob->ResourceRevision,
					Error);
			const bool bTextureCube = ActiveTextureCube != nullptr;
			const FMatrix PreviewTransform = bTextureCube
				? FMatrix(1.0)
				: glm::scale(
					FMatrix(1.0),
					FVector3(MaterialThumbnailSphereScale));
			if (Proxy == nullptr
				|| !ScenePool->SetPrimitive(std::move(Proxy), PreviewTransform, Error)
				|| !ScenePool->BeginCapture(Error, bTextureCube))
			{
				Pipeline.CompleteRender(
					*ActiveJob,
					ActiveJob->AssetRevision,
					ActiveJob->ResourceRevision,
					Error);
				ScenePool->Reset();
				ActiveJob.reset();
				ActiveMaterial = nullptr;
				ActiveTextureCube = nullptr;
			}
		}

		auto StartNext() -> void
		{
			FRenderedAssetThumbnailStartResult Start = Pipeline.StartNextDetailed();
			if (Start.WarmJob)
			{
				const FAssetPath& Path =
					Start.WarmJob->GenerationRequest.KeyInput.Asset.VirtualPath;
				DecodeAndQueueUpload(
					Path, Start.WarmJob->GenerationRequest.RequestSerial, Start.EncodedBytes);
				return;
			}
			if (!Start.ColdJob) return;
			ActiveJob = std::move(Start.ColdJob);
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
					ActiveTextureCube = nullptr;
					return;
				}
				if (!Pipeline.CompleteLoad(
						*ActiveJob, ActiveTextureCube->GetBuildRevision()))
				{
					ActiveJob.reset();
					ActiveTextureCube = nullptr;
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
				return;
			}
			const uint64 AssetRevision = ActiveMaterial->GetRenderStateVersion();
			if (!Pipeline.CompleteLoad(*ActiveJob, AssetRevision))
			{
				ActiveJob.reset();
				ActiveMaterial = nullptr;
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
				}
			}
		}
	};

	FMaterialAssetThumbnailCache::FMaterialAssetThumbnailCache(
		FAssetThumbnailBudgets Budgets)
		: Impl(std::make_unique<FImpl>(Budgets))
	{
	}

	FMaterialAssetThumbnailCache::~FMaterialAssetThumbnailCache()
	{
		Clear();
		Impl->ScenePool.reset();
		Impl->AsyncState.reset();
		Impl->Registry.Shutdown();
	}

	auto FMaterialAssetThumbnailCache::BeginFrame() -> void
	{
		++Impl->FrameNumber;
		for (auto& [Path, Entry] : Impl->Entries) Entry.bVisible = false;
		Impl->DrainUploads();
		Impl->FinishCapture();
		Impl->Pipeline.BeginFrame();
	}

	auto FMaterialAssetThumbnailCache::Request(
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

	auto FMaterialAssetThumbnailCache::Find(const FAssetPath& AssetPath) const
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
		if (View.State == EAssetThumbnailState::Ready && Entry.Texture == nullptr
			&& Entry.bUploading)
			View.State = EAssetThumbnailState::Uploading;
		else if (View.State == EAssetThumbnailState::Ready && Entry.bUploadFailed)
			View.State = EAssetThumbnailState::Failed;
		if (!Entry.Diagnostic.empty()) View.Diagnostic = Entry.Diagnostic;
		return View;
	}

	auto FMaterialAssetThumbnailCache::EndFrame() -> void
	{
		if (Impl->ActiveJob)
			Impl->TryBeginCapture();
		else
			Impl->StartNext();
		Impl->EvictToBudget();
	}

	auto FMaterialAssetThumbnailCache::CancelPendingRequests() -> void
	{
		if (Impl->ActiveJob) Impl->Pipeline.Cancel(*Impl->ActiveJob);
		Impl->ActiveJob.reset();
		Impl->ActiveMaterial = nullptr;
		Impl->ActiveTextureCube = nullptr;
		if (Impl->ScenePool) Impl->ScenePool->Reset();
		Impl->Scheduler.CancelAll();
	}

	auto FMaterialAssetThumbnailCache::Clear() -> void
	{
		CancelPendingRequests();
		for (auto& [Path, Entry] : Impl->Entries) Impl->UnregisterTexture(Entry);
		Impl->Entries.clear();
	}
} // namespace Durin
