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

#if DURIN_BUILD_DEBUG
		auto GetDebugOwnerString(const FRenderResource& Resource)
			-> std::string
		{
			const FName& Owner = Resource.GetDebugOwner();
			return Owner.IsNone()
				? "<unspecified>"
				: Owner.ToString();
		}
#endif

		template<typename CommandTag, typename LambdaType>
		auto EnqueueRequiredResourceCommand(
			FRenderResource* Resource, LambdaType&& Lambda) -> void
		{
			const bool bAccepted =
				FRenderThreadCommandPipe::TryEnqueue<CommandTag>(
					std::forward<LambdaType>(Lambda));
#if DURIN_BUILD_DEBUG
			checkf(bAccepted,
				"Required render-resource command '{}' was rejected: "
				"type='{}', owner='{}', queue='command_pipe'.",
				CommandTag::GetName(), Resource->GetFriendlyName(),
				GetDebugOwnerString(*Resource));
#else
			checkf(bAccepted,
				"Required render-resource command '{}' was rejected: "
				"type='{}', queue='command_pipe'.",
				CommandTag::GetName(), Resource->GetFriendlyName());
#endif
		}
	}

	FRenderResource::~FRenderResource()
	{
		if (IsInitialized())
		{
#if DURIN_BUILD_DEBUG
			DURIN_ERROR(
				"Render resource was not released before destruction: "
				"owner='{}', list_index={}, address={}.",
				GetDebugOwnerString(*this), ListIndex,
				static_cast<const void*>(this));
#else
			DURIN_ERROR("A FRenderResource was not released before destruction.");
#endif
		}
	}
#if DURIN_BUILD_DEBUG
	auto FRenderResource::SetDebugOwner(FName InOwner) -> void
	{
		check(!IsInitialized());
		DebugOwner = InOwner;
	}
#endif

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

		ReleaseRHI();
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
		EnqueueRequiredResourceCommand<FBeginInitResourceCommand>(
			this,
			[this](FRHICommandListImmediate& RHICmdList) {
				InitResource(RHICmdList);
			});
	}

	auto FRenderResource::BeginUpdateRHI_GameThread() -> void
	{
		check(IsInGameThread());
		EnqueueRequiredResourceCommand<FBeginUpdateResourceCommand>(
			this,
			[this](FRHICommandListImmediate& RHICmdList) {
				UpdateRHI(RHICmdList);
			});
	}

	auto FRenderResource::BeginRelease_GameThread() -> void
	{
		check(IsInGameThread());
		EnqueueRequiredResourceCommand<FBeginReleaseResourceCommand>(
			this,
			[this](FRHICommandListImmediate&) {
				ReleaseResource();
			});
	}

	void FRenderResource::UpdateRHI(FRHICommandListBase& RHICmdList)
	{
		check(IsInRenderingThread());
		if (!IsInitialized())
		{
			DURIN_ERROR("{} cannot update before initialization.", GetFriendlyName());
			return;
		}
		ReleaseRHI();
		InitRHI(RHICmdList);
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

	auto GetNumInitializedRenderResources() -> size_t
	{
		return RenderResources.size();
	}

	auto GetNumPendingRenderResourceCleanup() -> size_t
	{
		std::lock_guard Lock(PendingCleanupMutex);
		return PendingCleanup.size();
	}

	auto ValidateRenderResourceShutdown_RenderThread() -> bool
	{
		check(IsInRenderingThread());
		const size_t LiveResourceCount = RenderResources.size();
		std::lock_guard Lock(PendingCleanupMutex);
		const size_t PendingCleanupCount = PendingCleanup.size();
		if (LiveResourceCount == 0 && PendingCleanupCount == 0)
		{
			return true;
		}

		DURIN_ERROR(
			"Rendering thread shutdown found {} live render resource(s) and "
			"{} pending cleanup(s).",
			LiveResourceCount, PendingCleanupCount);
		for (const FRenderResource* Resource : RenderResources)
		{
#if DURIN_BUILD_DEBUG
			DURIN_ERROR(
				"Live render resource: type='{}', owner='{}'.",
				Resource->GetFriendlyName(),
				GetDebugOwnerString(*Resource));
#else
			DURIN_ERROR("Live render resource: type='{}'.",
				Resource->GetFriendlyName());
#endif
		}

		for (const FDeferredRenderResourceCleanup& Cleanup : PendingCleanup)
		{
			const FRenderResource* Resource =
				Cleanup.GetResourceForDiagnostics();
#if DURIN_BUILD_DEBUG
			DURIN_ERROR(
				"Pending render resource cleanup: type='{}', owner='{}', "
				"state={}.",
				Resource->GetFriendlyName(),
				GetDebugOwnerString(*Resource),
				Resource->IsInitialized()
					? "cleanup_release_pending"
					: "cleanup_released");
#else
			DURIN_ERROR(
				"Pending render resource cleanup: type='{}', state={}.",
				Resource->GetFriendlyName(),
				Resource->IsInitialized()
					? "cleanup_release_pending"
					: "cleanup_released");
#endif
		}
		return false;
	}

	FTextureReference::FTextureReference(FTextureRHIRef InFallbackTexture)
		: FallbackTextureRHI(std::move(InFallbackTexture))
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
		VertexBufferRHI = nullptr;
	}

	void FVertexBuffer::SetRHI(const FBufferRHIRef& BufferRHI)
	{
		check(IsInRenderingThread());
		VertexBufferRHI = BufferRHI;
	}

	FIndexBuffer::FIndexBuffer() = default;
	FIndexBuffer::~FIndexBuffer() = default;

	void FIndexBuffer::ReleaseRHI()
	{
		IndexBufferRHI = nullptr;
	}

	void FIndexBuffer::SetRHI(const FBufferRHIRef& BufferRHI)
	{
		check(IsInRenderingThread());
		IndexBufferRHI = BufferRHI;
	}
}
