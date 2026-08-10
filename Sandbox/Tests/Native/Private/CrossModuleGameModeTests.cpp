#include "Actors/GameMode.h"
#include "DObject/Class.h"
#include "Engine/ProjectGameSettings.h"
#include "NativeDObjectTestSupport.h"

#include <gtest/gtest.h>

TEST(FSandboxGameplayReflectionTests, ResolvesExactDerivedGameModeAfterLoadingProjectModule)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	Durin::FProjectGameSettings Settings;
	Settings.NativeModule = "Sandbox";
	Settings.GameModeClass = "Durin::Sandbox::Testing::ACrossModuleGameModeFixture";
	const Durin::FNativeGameModeResolution Resolution = Durin::ResolveNativeGameMode(Settings);
	ASSERT_TRUE(Resolution.Result) << Resolution.Result.Message;
	ASSERT_NE(Resolution.GameModeClass, nullptr);
	EXPECT_EQ(Resolution.GameModeClass->GetQualifiedName().ToString(), Settings.GameModeClass);
	EXPECT_TRUE(Resolution.GameModeClass->IsChildOf(Durin::AGameMode::StaticClass()));
}
