#pragma once

#include "MonaAPI.h"
#include "Rendering/ViewportDisplaySource.h"
#include "Widgets/MWidget.h"

namespace Durin::Mona
{
	class IMonaUIBackend;
}

namespace Durin
{
	// Presents a neutral display source inside the Mona widget tree without owning its lifetime.
	class MViewport : public MWidget
	{
	public:
		MViewport() = default;
		MONA_API ~MViewport() override;

		MONA_API auto Draw() -> void override;

		MONA_API auto SetDesiredSize(const FVector2f& InDesiredSize) -> void;

		MONA_API auto GetDesiredSize() const -> FVector2f;

		MONA_API auto SetDisplaySource(const std::shared_ptr<IViewportDisplaySource>& InDisplaySource) -> void;

		MONA_API auto GetDisplaySource() const -> std::shared_ptr<IViewportDisplaySource>;

		MONA_API auto WasTextureDrawn() const -> bool;

	private:
		// Logical widget size requested from the layout system.
		FVector2f DesiredSize = {640.0f, 360.0f};

		auto SynchronizeRegisteredTexture(const FTextureRHIRef& DisplayTexture) -> void;
		auto ReleaseRegisteredTexture() -> void;

		// The widget observes the source without extending its producer lifetime.
		std::weak_ptr<IViewportDisplaySource> DisplaySource;

		// Registration belongs to the consumer and changes only with texture or backend identity.
		FTextureRHIRef RegisteredTexture;
		Mona::IMonaUIBackend* RegisteredBackend = nullptr;

		// Records whether the latest draw submitted a valid viewport texture.
		bool bLastDrawSucceeded = false;
	};
}
