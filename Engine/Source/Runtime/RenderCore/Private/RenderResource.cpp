#include "RenderResource.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		std::vector<FRenderResource*> RenderResources;
		std::mutex PendingCleanupMutex;
		std::vector<FDeferredRenderResourceCleanup> PendingCleanup;

		struct FBeginInitResourceCommand
		{
			static constexpr auto GetName() -> const char*
			{
				return "BeginInitResource";
			}
		};

		struct FBeginUpdateResourceCommand
		{
			static constexpr auto GetName() -> const char*
			{
				return "BeginUpdateResource";
			}
		};

		struct FBeginReleaseResourceCommand
		{
			static constexpr auto GetName() -> const char*
			{
				return "BeginReleaseResource";
			}
		};

		struct FBeginCleanupRenderResourceCommand
		{
			static constexpr auto GetName() -> const char*
			{
				return "BeginCleanupRenderResource";
			}
		};

		template<typename CommandTag, typename LambdaType>
		auto EnqueueRequiredResourceCommand(
			FRenderResource* Resource, LambdaType&& Lambda) -> void
		{
			const bool bAccepted =
				FRenderThreadCommandPipe::TryEnqueue<CommandTag>(
					std::forward<LambdaType>(Lambda));
			checkf(bAccepted,
				"Required render-resource command '{}' was rejected: "
				"type='{}', owner='{}', revision={}, queue='command_pipe'.",
				CommandTag::GetName(), Resource->GetFriendlyName(),
				Resource->GetLifetimeOwner(),
				Resource->GetLifetimeRevision());
		}
	}

	void FRenderResource::ReleaseRHIForAllResources()
	{
		check(IsInRenderingThread());
		for (FRenderResource* Resource : RenderResources)
		{
			if (!Resource->bRHIInitialized) continue;
			Resource->ReleaseRHI();
			Resource->bRHIInitialized = false;
		}
	}

	void FRenderResource::InitRHIForAllResources(FRHICommandListBase& RHICmdList)
	{
		check(IsInRenderingThread());
		for (FRenderResource* Resource : RenderResources)
		{
			if (Resource->bRHIInitialized) continue;
			Resource->InitRHI(RHICmdList);
			Resource->bRHIInitialized = true;
		}
	}

	void FRenderResource::InitPreRHIResources()
	{
		check(IsInRenderingThread());
		FRHICommandListBase& RHICmdList = GetImmediateCommandList_ForRenderCommand();
		for (FRenderResource* Resource : RenderResources)
		{
			if (Resource->InitPhase != EInitPhase::Pre
				|| Resource->bRHIInitialized)
			{
				continue;
			}
			Resource->InitRHI(RHICmdList);
			Resource->bRHIInitialized = true;
		}
	}

	FRenderResource::FRenderResource()
	{
	}

	FRenderResource::FRenderResource(EInitPhase InInitPhase)
		: InitPhase(InInitPhase)
	{
	}

	FRenderResource::FRenderResource(ERHIFeatureLevel InFeatureLevel)
		: FeatureLevel(InFeatureLevel)
	{
	}

	FRenderResource::~FRenderResource()
	{
		if (IsInitialized())
		{
			DURIN_ERROR("A FRenderResource was not released before destruction.");
		}
	}
	void FRenderResource::InitRHI(FRHICommandListBase& RHICmdList) {}

	auto FRenderResource::SetLifetimeDiagnostic(
		std::string InOwner, uint64 InRevision) -> void
	{
		check(!IsInitialized());
		LifetimeOwner = std::move(InOwner);
		LifetimeRevision = InRevision;
	}

	void FRenderResource::InitResource(FRHICommandListBase& RHICmdList)
	{
		check(IsInRenderingThread());
		if (IsInitialized())
		{
			DURIN_ERROR("{} was initialized more than once.", GetFriendlyName());
			return;
		}

		ListIndex = static_cast<uint32>(RenderResources.size());
		RenderResources.push_back(this);
		InitRHI(RHICmdList);
		bRHIInitialized = true;
	}

	void FRenderResource::ReleaseResource()
	{
		check(IsInRenderingThread());
		if (!IsInitialized())
		{
			DURIN_ERROR("{} was released before initialization or more than once.",
				GetFriendlyName());
			return;
		}

		if (bRHIInitialized)
		{
			ReleaseRHI();
			bRHIInitialized = false;
		}
		const uint32 RemovedIndex = ListIndex;
		check(RemovedIndex < RenderResources.size());
		FRenderResource* MovedResource = RenderResources.back();
		RenderResources[RemovedIndex] = MovedResource;
		RenderResources.pop_back();
		if (MovedResource != this) MovedResource->ListIndex = RemovedIndex;
		ListIndex = INDEX_NONE_U32;
	}

	auto FRenderResource::BeginInit_GameThread() -> void
	{
		check(IsInGameThread());
		BeginInitResource(this);
	}

	auto FRenderResource::BeginUpdateRHI_GameThread() -> void
	{
		check(IsInGameThread());
		BeginUpdateResourceRHI(this);
	}

	auto FRenderResource::BeginRelease_GameThread() -> void
	{
		check(IsInGameThread());
		BeginReleaseResource(this);
	}

	void FRenderResource::UpdateRHI(FRHICommandListBase& RHICmdList)
	{
		check(IsInRenderingThread());
		if (!IsInitialized())
		{
			DURIN_ERROR("{} cannot update before initialization.", GetFriendlyName());
			return;
		}
		if (bRHIInitialized) ReleaseRHI();
		InitRHI(RHICmdList);
		bRHIInitialized = true;
	}

	FDeferredRenderResourceCleanup::FDeferredRenderResourceCleanup(
		std::unique_ptr<FRenderResource> InResource)
		: Resource(std::move(InResource))
	{
		check(Resource != nullptr);
	}

	FDeferredRenderResourceCleanup::~FDeferredRenderResourceCleanup() = default;
	FDeferredRenderResourceCleanup::FDeferredRenderResourceCleanup(
		FDeferredRenderResourceCleanup&& Other) noexcept = default;
	auto FDeferredRenderResourceCleanup::operator=(
		FDeferredRenderResourceCleanup&& Other) noexcept
		-> FDeferredRenderResourceCleanup& = default;

	auto BeginInitResource(FRenderResource* Resource) -> void
	{
		check(Resource != nullptr);
		EnqueueRequiredResourceCommand<FBeginInitResourceCommand>(
			Resource,
			[Resource](FRHICommandListImmediate& RHICmdList) {
				Resource->InitResource(RHICmdList);
			});
	}

	auto BeginUpdateResourceRHI(FRenderResource* Resource) -> void
	{
		check(Resource != nullptr);
		EnqueueRequiredResourceCommand<FBeginUpdateResourceCommand>(
			Resource,
			[Resource](FRHICommandListImmediate& RHICmdList) {
				Resource->UpdateRHI(RHICmdList);
			});
	}

	auto BeginReleaseResource(FRenderResource* Resource) -> void
	{
		check(Resource != nullptr);
		EnqueueRequiredResourceCommand<FBeginReleaseResourceCommand>(
			Resource,
			[Resource](FRHICommandListImmediate&) {
				Resource->ReleaseResource();
			});
	}

	auto BeginCleanupRenderResource(
		FDeferredRenderResourceCleanup&& Cleanup) -> void
	{
		FRenderResource* Resource = Cleanup.Resource.get();
		{
			std::lock_guard Lock(PendingCleanupMutex);
			PendingCleanup.push_back(std::move(Cleanup));
		}
		EnqueueRequiredResourceCommand<FBeginCleanupRenderResourceCommand>(
			Resource,
			[Resource](FRHICommandListImmediate&) {
				check(IsInRenderingThread());
				std::lock_guard Lock(PendingCleanupMutex);
				const auto It = std::ranges::find_if(PendingCleanup,
					[Resource](const FDeferredRenderResourceCleanup& Candidate) {
						return Candidate.Resource.get() == Resource;
					});
				check(It != PendingCleanup.end());
				PendingCleanup.erase(It);
			});
	}

	auto FlushPendingRenderResourceCleanup_RenderThread() -> void
	{
		check(IsInRenderingThread());
		std::lock_guard Lock(PendingCleanupMutex);
		PendingCleanup.clear();
	}

	auto GetNumInitializedRenderResources() -> size_t
	{
		return RenderResources.size();
	}

	auto GetNumPendingRenderResourceCleanup() -> size_t
	{
		std::lock_guard Lock(PendingCleanupMutex);
		return PendingCleanup.size();
	}

	auto ValidateRenderResourceShutdown_RenderThread(
		const char* Phase) -> bool
	{
		check(IsInRenderingThread());
		bool bValid = RenderResources.empty();
		for (const FRenderResource* Resource : RenderResources)
		{
			DURIN_ERROR(
				"Render resource remained live during '{}': type='{}', "
				"owner='{}', revision={}, lifecycle_phase={}, "
				"init_phase={}, queue='registry'.",
				Phase, Resource->GetFriendlyName(),
				Resource->GetLifetimeOwner(),
				Resource->GetLifetimeRevision(),
				Resource->IsRHIInitialized()
					? "rhi_initialized"
					: "registered_rhi_released",
				Resource->GetInitPhase() == FRenderResource::EInitPhase::Pre
					? "pre" : "default");
		}

		std::lock_guard Lock(PendingCleanupMutex);
		bValid &= PendingCleanup.empty();
		for (const FDeferredRenderResourceCleanup& Cleanup : PendingCleanup)
		{
			const FRenderResource* Resource =
				Cleanup.GetResourceForDiagnostics();
			DURIN_ERROR(
				"Render resource remained live during '{}': type='{}', "
				"owner='{}', revision={}, lifecycle_phase={}, init_phase={}, "
				"queue='deferred_cpp_cleanup'.",
				Phase, Resource->GetFriendlyName(),
				Resource->GetLifetimeOwner(),
				Resource->GetLifetimeRevision(),
				Resource->IsInitialized()
					? "cleanup_release_pending"
					: "cleanup_released",
				Resource->GetInitPhase() == FRenderResource::EInitPhase::Pre
					? "pre" : "default");
		}
		return bValid;
	}

	FTextureReference::FTextureReference(FTextureRHIRef InFallbackTexture)
		: FRenderResource(EInitPhase::Default)
		, FallbackTextureRHI(std::move(InFallbackTexture))
		, TextureReferenceRHI(new FRHITextureReference(FallbackTextureRHI))
	{
	}

	FTextureReference::~FTextureReference() = default;

	void FTextureReference::InitRHI(FRHICommandListBase&)
	{
		check(IsInRenderingThread());
		check(TextureReferenceRHI);
		ResetToFallback_RenderThread();
	}

	void FTextureReference::ReleaseRHI()
	{
		check(IsInRenderingThread());
		if (!TextureReferenceRHI) return;
		ResetToFallback_RenderThread();
		TextureReferenceRHI = nullptr;
	}

	auto FTextureReference::GetReferencedTexture_RenderThread() const
		-> FRHITexture*
	{
		check(IsInRenderingThread());
		return TextureReferenceRHI
			? TextureReferenceRHI->GetReferencedTexture_RenderThread()
			: nullptr;
	}

	void FTextureReference::SetReferencedTexture_RenderThread(
		FTextureRHIRef InTexture)
	{
		check(IsInRenderingThread());
		check(TextureReferenceRHI);
		if (GDynamicRHI != nullptr)
		{
			GDynamicRHI->RHIUpdateTextureReference(
				TextureReferenceRHI.GetReference(), InTexture.GetReference());
			return;
		}

		// RenderCore contract tests exercise resource ordering without starting a
		// platform RHI. Runtime updates always pass through FDynamicRHI.
		TextureReferenceRHI->SetReferencedTexture_RenderThread(
			std::move(InTexture));
	}

	void FTextureReference::ResetToFallback_RenderThread()
	{
		check(IsInRenderingThread());
		if (TextureReferenceRHI)
		{
			SetReferencedTexture_RenderThread(FallbackTextureRHI);
		}
	}

	void FTextureReference::Clear_RenderThread()
	{
		check(IsInRenderingThread());
		if (TextureReferenceRHI)
		{
			SetReferencedTexture_RenderThread({});
		}
	}

	void FTextureReference::ResetToFallbackIfMatches_RenderThread(
		FRHITexture* ExpectedTexture)
	{
		check(IsInRenderingThread());
		if (TextureReferenceRHI
			&& TextureReferenceRHI->GetReferencedTexture_RenderThread()
				== ExpectedTexture)
		{
			ResetToFallback_RenderThread();
		}
	}

	FTextureResource::FTextureResource(FTextureReference* InTextureReference)
		: TextureReference(InTextureReference)
	{
		check(TextureReference != nullptr);
	}

	FTextureResource::~FTextureResource() = default;

	void FTextureResource::ReleaseRHI()
	{
		check(IsInRenderingThread());
		TextureReference->ResetToFallbackIfMatches_RenderThread(
			TextureRHI.GetReference());
		TextureRHI = nullptr;
	}

	auto FTextureResource::GetTextureRHI_RenderThread() const
		-> const FTextureRHIRef&
	{
		check(IsInRenderingThread());
		return TextureRHI;
	}

	void FTextureResource::PublishTexture_RenderThread()
	{
		check(IsInRenderingThread());
		check(TextureRHI);
		TextureReference->SetReferencedTexture_RenderThread(TextureRHI);
	}

	void FTextureResource::SetTextureRHI_RenderThread(
		FTextureRHIRef InTexture)
	{
		check(IsInRenderingThread());
		TextureRHI = std::move(InTexture);
	}

	FVertexBuffer::FVertexBuffer() = default;
	FVertexBuffer::~FVertexBuffer() = default;

	void FVertexBuffer::ReleaseRHI()
	{
		VertexBufferRHI.reset();
	}

	void FVertexBuffer::SetRHI(const std::shared_ptr<FRHIBuffer>& BufferRHI)
	{
		check(IsInRenderingThread());
		VertexBufferRHI = BufferRHI;
	}
}
