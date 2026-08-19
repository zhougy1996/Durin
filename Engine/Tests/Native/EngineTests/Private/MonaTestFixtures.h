#pragma once

#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"

#include <cstddef>
#include <unordered_set>

namespace Durin::Tests
{
	class FScopedActiveUIBackend
	{
	public:
		explicit FScopedActiveUIBackend(Mona::IMonaUIBackend& Backend)
			: PreviousBackend(Mona::GActiveUIBackend)
		{
			Mona::GActiveUIBackend = &Backend;
		}

		FScopedActiveUIBackend(const FScopedActiveUIBackend&) = delete;
		auto operator=(const FScopedActiveUIBackend&) -> FScopedActiveUIBackend& = delete;

		~FScopedActiveUIBackend()
		{
			Mona::GActiveUIBackend = PreviousBackend;
		}

	private:
		Mona::IMonaUIBackend* PreviousBackend;
	};

	class FThumbnailTestUIBackend final : public Mona::IMonaUIBackend
	{
	public:
		auto Initialize() -> void override {}
		auto Shutdown() -> void override { Registered.clear(); }
		auto NewFrame() -> void override {}
		auto Render() -> void override {}

		auto RegisterTexture(const FTextureRHIRef& Texture) -> void override
		{
			if (Texture) Registered.insert(Texture.GetReference());
		}

		auto UnregisterTexture(const FTextureRHIRef& Texture) -> void override
		{
			if (Texture) Registered.erase(Texture.GetReference());
		}

		auto IsTextureRegistered(const FRHITexture* Texture) -> bool override
		{
			return Registered.contains(Texture);
		}

		auto DrawImage(const FRHITexture* Texture, const FVector2f&) -> bool override
		{
			return IsTextureRegistered(Texture);
		}

		auto NumRegistered() const -> size_t { return Registered.size(); }

	private:
		std::unordered_set<const FRHITexture*> Registered;
	};
}
