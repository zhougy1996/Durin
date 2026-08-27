#include "Factories/Factory.h"

#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "HAL/PlatformLTS.h"

#include <gtest/gtest.h>

namespace
{
	using FFactoryCreateNewSignature = Durin::DObject* (Durin::DFactory::*)(
		Durin::DClass*,
		Durin::DObject*,
		Durin::FName,
		Durin::EObjectFlags,
		Durin::DObject*);
	using FFactoryCreateFromFileSignature = Durin::DObject* (Durin::DFactory::*)(
		Durin::DClass*,
		Durin::DObject*,
		Durin::FName,
		Durin::EObjectFlags,
		std::string_view,
		Durin::DObject*);

	static_assert(std::is_same_v<
		decltype(&Durin::DFactory::FactoryCreateNew),
		FFactoryCreateNewSignature>);
	static_assert(std::is_same_v<
		decltype(&Durin::DFactory::FactoryCreateFromFile),
		FFactoryCreateFromFileSignature>);

	auto InitializeFactoryTestGameThread() -> void
	{
		if (Durin::GIsGameThreadIdInitialized) return;
		Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
		Durin::GIsGameThreadIdInitialized = true;
	}
}

TEST(DFactoryTests, ExposesAbstractReflectedFactoryContract)
{
	Durin::DClass* FactoryClass = Durin::DFactory::StaticClass();
	ASSERT_NE(FactoryClass, nullptr);
	EXPECT_EQ(FactoryClass->GetSuperClass(), Durin::DObject::StaticClass());
	EXPECT_TRUE(FactoryClass->HasAnyClassFlags(Durin::EClassFlags::Abstract));
}

TEST(DFactoryTests, DiscoversOnlyConcreteFactoryDefaultObjects)
{
	InitializeFactoryTestGameThread();
	for (const Durin::DFactory* Factory :
		Durin::DFactory::GetAvailableFactories())
	{
		ASSERT_NE(Factory, nullptr);
		ASSERT_NE(Factory->GetClass(), nullptr);
		EXPECT_FALSE(Factory->GetClass()->HasAnyClassFlags(
			Durin::EClassFlags::Abstract));
		EXPECT_EQ(Factory->GetClass()->GetDefaultObject(), Factory);
	}
}

TEST(DFactoryTests, RejectsEmptyFactoryLookups)
{
	InitializeFactoryTestGameThread();
	Durin::DFactory::InvalidateFactoryCache();
	EXPECT_EQ(Durin::DFactory::FindFactory(nullptr), nullptr);
	EXPECT_EQ(Durin::DFactory::FindFactoryByExtension({}), nullptr);
	EXPECT_EQ(Durin::DFactory::FindFactoryByExtension("."), nullptr);
	Durin::DFactory::InvalidateFactoryCache();
	EXPECT_EQ(Durin::DFactory::FindFactoryByExtension({}), nullptr);
}
