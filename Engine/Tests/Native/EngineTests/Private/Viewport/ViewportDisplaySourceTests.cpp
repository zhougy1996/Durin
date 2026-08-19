#include "gtest/gtest.h"

#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "MonaTestFixtures.h"
#include "Rendering/MonaRHIRenderer.h"
#include "Rendering/ViewportDisplaySource.h"
#include "Widgets/MViewport.h"

namespace
{
	class FTestTexture final : public Durin::FRHITexture
	{
	public:
		~FTestTexture() override = default;
	};

	class FTestDisplaySource final : public Durin::IViewportDisplaySource
	{
	public:
		auto PrepareDisplay(const Durin::FVector2f& DesiredSize) -> void override
		{
			Events.emplace_back("prepare");
			LastDesiredSize = DesiredSize;
		}

		auto GetDisplayTexture() const -> const Durin::FTextureRHIRef& override
		{
			Events.emplace_back("get");
			return Texture;
		}

		Durin::FTextureRHIRef Texture;
		Durin::FVector2f LastDesiredSize = {};
		mutable std::vector<std::string> Events;
	};

	class FTestUIBackend final : public Durin::Mona::IMonaUIBackend
	{
	public:
		auto Initialize() -> void override {}
		auto Shutdown() -> void override {}
		auto NewFrame() -> void override {}
		auto Render() -> void override {}

		auto RegisterTexture(const Durin::FTextureRHIRef& Texture) -> void override
		{
			Registered.emplace_back(Texture.GetReference());
		}

		auto UnregisterTexture(const Durin::FTextureRHIRef& Texture) -> void override
		{
			Unregistered.emplace_back(Texture.GetReference());
		}

		auto IsTextureRegistered(const Durin::FRHITexture* Texture) -> bool override
		{
			return std::ranges::find(Registered, Texture) != Registered.end()
				&& std::ranges::find(Unregistered, Texture) == Unregistered.end();
		}

		auto DrawImage(const Durin::FRHITexture* Texture, const Durin::FVector2f& Size) -> bool override
		{
			Drawn.emplace_back(Texture);
			LastDrawSize = Size;
			return bDrawSucceeds;
		}

		std::vector<const Durin::FRHITexture*> Registered;
		std::vector<const Durin::FRHITexture*> Unregistered;
		std::vector<const Durin::FRHITexture*> Drawn;
		Durin::FVector2f LastDrawSize = {};
		bool bDrawSucceeds = true;
	};

}

TEST(FViewportDisplaySourceTests, CoalescesWindowResizeRequestsUntilPrepared)
{
	Durin::Mona::FMonaViewportInfo ViewportInfo;
	ViewportInfo.SubmittedExtent = {640, 480};

	ViewportInfo.QueueResize({640, 480});
	EXPECT_FALSE(ViewportInfo.PendingExtent.has_value());

	ViewportInfo.QueueResize({800, 600});
	ViewportInfo.QueueResize({1024, 768});
	ASSERT_TRUE(ViewportInfo.PendingExtent.has_value());
	EXPECT_EQ(*ViewportInfo.PendingExtent, Durin::FIntPoint(1024, 768));

	const std::optional<Durin::FIntPoint> PendingExtent =
		ViewportInfo.TakePendingResize();
	ASSERT_TRUE(PendingExtent.has_value());
	EXPECT_EQ(*PendingExtent, Durin::FIntPoint(1024, 768));
	EXPECT_FALSE(ViewportInfo.PendingExtent.has_value());
	EXPECT_EQ(ViewportInfo.SubmittedExtent, Durin::FIntPoint(640, 480));

	ViewportInfo.SubmittedExtent = *PendingExtent;
	ViewportInfo.QueueResize({1024, 768});
	EXPECT_FALSE(ViewportInfo.PendingExtent.has_value());
}

TEST(FViewportDisplaySourceTests, PublishesSizeBeforeReadingTextureAndDoesNotRetainSource)
{
	Durin::MViewport Widget;
	Widget.SetDesiredSize({123.25f, 456.5f});
	Widget.Draw();
	EXPECT_FALSE(Widget.WasTextureDrawn());

	auto Source = std::make_shared<FTestDisplaySource>();
	Widget.SetDisplaySource(Source);
	EXPECT_EQ(Widget.GetDisplaySource(), Source);
	Widget.Draw();
	EXPECT_EQ(Source->Events, (std::vector<std::string>{"prepare", "get"}));
	EXPECT_EQ(Source->LastDesiredSize, Durin::FVector2f(123.25f, 456.5f));
	EXPECT_FALSE(Widget.WasTextureDrawn());

	Source.reset();
	EXPECT_EQ(Widget.GetDisplaySource(), nullptr);
	Widget.Draw();
	EXPECT_FALSE(Widget.WasTextureDrawn());
}

TEST(FViewportDisplaySourceTests, RegistersStableTextureOnceAndReplacesItExactly)
{
	FTestUIBackend Backend;
	Durin::Tests::FScopedActiveUIBackend BackendScope(Backend);
	auto FirstSource = std::make_shared<FTestDisplaySource>();
	FirstSource->Texture = new FTestTexture();
	auto SecondSource = std::make_shared<FTestDisplaySource>();
	SecondSource->Texture = new FTestTexture();

	{
		Durin::MViewport Widget;
		Widget.SetDesiredSize({320.0f, 180.0f});
		Widget.SetDisplaySource(FirstSource);
		Widget.Draw();
		Widget.Draw();
		ASSERT_EQ(Backend.Registered.size(), 1u);
		EXPECT_EQ(Backend.Unregistered.size(), 0u);
		EXPECT_EQ(Backend.Drawn.size(), 2u);
		EXPECT_TRUE(Widget.WasTextureDrawn());

		const Durin::FTextureRHIRef FirstTexture = FirstSource->Texture;
		FirstSource->Texture = new FTestTexture();
		Widget.Draw();
		ASSERT_EQ(Backend.Registered.size(), 2u);
		ASSERT_EQ(Backend.Unregistered.size(), 1u);
		EXPECT_EQ(Backend.Unregistered[0], FirstTexture.GetReference());
		EXPECT_EQ(Backend.Registered[1], FirstSource->Texture.GetReference());

		Widget.SetDisplaySource(SecondSource);
		ASSERT_EQ(Backend.Unregistered.size(), 2u);
		EXPECT_EQ(Backend.Unregistered[1], FirstSource->Texture.GetReference());
		Widget.Draw();
		ASSERT_EQ(Backend.Registered.size(), 3u);
		EXPECT_EQ(Backend.Registered[2], SecondSource->Texture.GetReference());

		SecondSource->Texture = nullptr;
		Widget.Draw();
		ASSERT_EQ(Backend.Unregistered.size(), 3u);
	}
	EXPECT_EQ(Backend.Unregistered.size(), 3u);
}

TEST(FViewportDisplaySourceTests, HandlesExpirationDestructionAndUnavailableBackend)
{
	FTestUIBackend Backend;
	Durin::Tests::FScopedActiveUIBackend BackendScope(Backend);
	auto Source = std::make_shared<FTestDisplaySource>();
	Source->Texture = new FTestTexture();

	{
		Durin::MViewport Widget;
		Widget.SetDisplaySource(Source);
		Widget.Draw();
		Source.reset();
		Widget.SetDisplaySource(nullptr);
		EXPECT_EQ(Backend.Unregistered.size(), 1u);
		Widget.Draw();
	}

	auto ShutdownSource = std::make_shared<FTestDisplaySource>();
	ShutdownSource->Texture = new FTestTexture();
	{
		Durin::MViewport Widget;
		Widget.SetDisplaySource(ShutdownSource);
		Widget.Draw();
		Durin::Mona::GActiveUIBackend = nullptr;
		Widget.Draw();
		EXPECT_EQ(Backend.Unregistered.size(), 1u);
		Durin::Mona::GActiveUIBackend = &Backend;
		Widget.Draw();
		EXPECT_EQ(Backend.Registered.size(), 3u);
	}
	EXPECT_EQ(Backend.Unregistered.size(), 2u);
}
