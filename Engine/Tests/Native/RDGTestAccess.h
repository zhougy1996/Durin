#pragma once

#include "RDG.h"

namespace Durin
{
	// Inspect typed phase errors without adding a universal production error view.
	template<typename T, typename E>
	auto FindRDGTestDetail(const E& Error) -> const T*
	{
		if constexpr (std::same_as<T, E>) return &Error;
		else if constexpr (requires { std::variant_size<E>::value; })
			return std::visit([](const auto& Value) { return FindRDGTestDetail<T>(Value); }, Error);
		else if constexpr (requires { Error.Detail; }) return FindRDGTestDetail<T>(Error.Detail);
		else
		{
			if constexpr (requires { Error.Context; })
				if (const auto* Value = FindRDGTestDetail<T>(Error.Context)) return Value;
			if constexpr (requires { Error.Cause; }) return FindRDGTestDetail<T>(Error.Cause);
			return nullptr;
		}
	}

	// Compare only a reason from the requested domain; unrelated alternatives fail.
	template<typename E, typename R>
	auto HasRDGTestReason(const E& Error, R Reason) -> bool
	{
		if constexpr (std::same_as<E, R>) return Error == Reason;
		else if constexpr (requires { std::variant_size<E>::value; })
			return std::visit([&](const auto& Value) { return HasRDGTestReason(Value, Reason); }, Error);
		else if constexpr (requires { Error.Detail; }) return HasRDGTestReason(Error.Detail, Reason);
		else if constexpr (requires { Error.Reason == Reason; }) return Error.Reason == Reason;
		else return false;
	}

	// Test-only access to the production compiler; success seals the builder.
	class FRDGBuilderTestAccessor final
	{
	public:
		static auto GetSubmissionSyncPoints(const FRDGBuilder& Builder)
			-> std::span<const FRHIGPUSyncPointRef>
		{ return Builder.GetSubmissionSyncPoints(); }
		static auto HasDiagnostics(const FRDGBuilder& Builder) -> bool
		{ return Builder.Diagnostics != nullptr; }
		static auto Compile(FRDGBuilder& Builder) -> FRDGCompileResult
		{ return Builder.CompileForTesting(); }
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
