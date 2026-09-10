#pragma once

#include "RDG.h"

namespace Durin
{
	// Test-only access to the production compiler; success seals the builder.
	class FRDGBuilderTestAccessor final
	{
	public:
		struct FEvidence
		{
			FRDGResult Result;
			auto IsSuccess() const -> bool { return Result.IsSuccess(); }
		};
		static auto HasDiagnostics(const FRDGBuilder& Builder) -> bool
		{ return Builder.Diagnostics != nullptr; }
		static auto Compile(FRDGBuilder& Builder) -> FEvidence
		{ return {Builder.CompileForTesting()}; }
		template<typename... Args>
		static auto AddPass(FRDGBuilder& Builder, Args&&... Arguments) -> decltype(auto)
		{
			if constexpr (requires { Builder.AddPass(std::forward<Args>(Arguments)...); })
				return Builder.AddPass(std::forward<Args>(Arguments)...);
			else
				return Builder.AddTestPass(std::forward<Args>(Arguments)...);
		}

		static auto UseTexture(FRDGBuilder& Builder, FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range, ERDGUse Use,
			ERHIAccess Access, bool bDiscard = false) -> void
		{ Builder.UseTexture(Pass, Texture, Range, Use, Access, bDiscard); }

		static auto UseBuffer(FRDGBuilder& Builder, FRDGPassHandle Pass,
			FRDGBufferHandle Buffer, uint64 Offset, uint64 Size,
			ERDGUse Use, ERHIAccess Access,
			bool bDiscard = false) -> void
		{ Builder.UseBuffer(Pass, Buffer, Offset, Size, Use, Access, bDiscard); }

		static auto UseColorAttachment(FRDGBuilder& Builder, FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction) -> void
		{ Builder.UseColorAttachment(Pass, Texture, Range, LoadAction, StoreAction); }

		static auto UseDepthStencilAttachment(FRDGBuilder& Builder, FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction) -> void
		{ Builder.UseDepthStencilAttachment(Pass, Texture, Range, LoadAction, StoreAction); }

		static auto UseManagedColorAttachment(FRDGBuilder& Builder, FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction,
			ERHIAccess ResultAccess) -> void
		{ Builder.UseManagedColorAttachment(Pass, Texture, Range, LoadAction, StoreAction, ResultAccess); }

		static auto UseManagedDepthStencilAttachment(FRDGBuilder& Builder, FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction,
			ERHIAccess ResultAccess) -> void
		{ Builder.UseManagedDepthStencilAttachment(Pass, Texture, Range, LoadAction, StoreAction, ResultAccess); }

		static auto UseManagedTexture(FRDGBuilder& Builder, FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range, ERDGUse Use,
			ERHIAccess EntryAccess, ERHIAccess ResultAccess,
			bool bDiscard = false) -> void
		{ Builder.UseManagedTexture(Pass, Texture, Range, Use, EntryAccess, ResultAccess, bDiscard); }

		static auto UseToken(FRDGBuilder& Builder, FRDGPassHandle Pass, FRDGTokenHandle Token,
			ERDGUse Use) -> void
		{ Builder.UseToken(Pass, Token, Use); }

		template<typename... Args>
		static auto UseValue(FRDGBuilder& Builder, Args&&... Arguments) -> decltype(auto)
		{ return Builder.UseValue(std::forward<Args>(Arguments)...); }

	};

}
