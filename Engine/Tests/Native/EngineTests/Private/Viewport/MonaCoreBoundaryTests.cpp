#include "gtest/gtest.h"

#include "ApplicationCoreGlobals.h"
#include "DynamicRHI.h"
#include "MonaCore.h"

namespace
{
	class FNullUIBackend final : public Durin::Mona::IMonaUIBackend
	{
	public:
		auto Initialize() -> void override {}
		auto Shutdown() -> void override {}
		auto NewFrame() -> void override {}
		auto Render() -> void override {}
		auto RegisterTexture(const Durin::FTextureRHIRef&) -> void override {}
		auto UnregisterTexture(const Durin::FTextureRHIRef&) -> void override {}
		auto IsTextureRegistered(const Durin::FRHITexture*) -> bool override
		{
			return false;
		}
		auto DrawImage(const Durin::FRHITexture*, const Durin::FVector2f&) -> bool override
		{
			return false;
		}
	};
}

TEST(FMonaCoreBoundaryTests, LoadsWithoutStartingRuntimeServices)
{
	EXPECT_EQ(Durin::GApp, nullptr);
	EXPECT_EQ(Durin::GDynamicRHI, nullptr);
	EXPECT_EQ(Durin::Mona::GetActiveUIBackend(), nullptr);
}

TEST(FMonaCoreBoundaryTests, RejectsDuplicateAndMismatchedBackendRegistration)
{
	FNullUIBackend FirstBackend;
	FNullUIBackend SecondBackend;

	ASSERT_TRUE(Durin::Mona::RegisterUIBackend(FirstBackend));
	EXPECT_EQ(Durin::Mona::GetActiveUIBackend(), &FirstBackend);
	EXPECT_FALSE(Durin::Mona::RegisterUIBackend(SecondBackend));
	EXPECT_FALSE(Durin::Mona::UnregisterUIBackend(SecondBackend));
	EXPECT_EQ(Durin::Mona::GetActiveUIBackend(), &FirstBackend);
	EXPECT_TRUE(Durin::Mona::UnregisterUIBackend(FirstBackend));
	EXPECT_EQ(Durin::Mona::GetActiveUIBackend(), nullptr);
	EXPECT_FALSE(Durin::Mona::UnregisterUIBackend(FirstBackend));
}
