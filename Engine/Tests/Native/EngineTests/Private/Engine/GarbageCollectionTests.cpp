#include "DObject/Class.h"
#include "Engine/GarbageCollectionScheduler.h"
#include "CoreGlobals.h"
#include "Engine/GarbageCollectionSettings.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/ObjectPtr.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "Engine/Engine.h"
#include "Misc/Time.h"
#include "NativeDObjectTestSupport.h"

#include <gtest/gtest.h>

TEST(FEngineGarbageCollectionTests, RequestsWaitForSafePointAndAreDeduplicatedAcrossHostsWithoutWorlds)
{
	using namespace Durin;
	Testing::InitializeDObjectSystemForTests();
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ConfigureAutomaticGarbageCollection({.bEnabled = false}, FTime::Seconds());
	struct FRestore { ~FRestore() { ConfigureAutomaticGarbageCollection({}, FTime::Seconds()); } } Restore;
	TStrongObjectPtr<DEngine> Host(NewObject<DEngine>(nullptr, "GCHost"));
	TStrongObjectPtr<DEngine> Replacement(NewObject<DEngine>(nullptr, "GCReplacement"));
	TWeakObjectPtr<DObject> First(NewObject<DObject>(nullptr, "GCFirst"));
	Host->ForceGarbageCollection();
	EXPECT_TRUE(First.IsValid());
	// Core records an actual collection independently of this host scheduling check.
	Host->ConditionalCollectGarbage();
	EXPECT_TRUE(IsGarbageCollectionRequested());
	++GFrameCounter;
	Replacement->ConditionalCollectGarbage();
	EXPECT_FALSE(First.IsValid());
	EXPECT_FALSE(IsGarbageCollectionRequested());
	TWeakObjectPtr<DObject> Second(NewObject<DObject>(nullptr, "GCSecond"));
	Host->ForceGarbageCollection();
	Replacement->ConditionalCollectGarbage();
	EXPECT_TRUE(Second.IsValid());
	EXPECT_TRUE(IsGarbageCollectionRequested());
	++GFrameCounter;
	Replacement->ConditionalCollectGarbage();
	EXPECT_FALSE(Second.IsValid());
	EXPECT_FALSE(IsGarbageCollectionRequested());
	Host->PreExit();
	Replacement->PreExit();
}

TEST(FEngineGarbageCollectionTests, ConditionalEntryExecutesAutomaticPressurePolicy)
{
	using namespace Durin;
	Testing::InitializeDObjectSystemForTests();
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ConfigureAutomaticGarbageCollection({.IntervalSeconds = 0.0,
		.PendingKillThreshold = 1, .ObjectGrowthThreshold = 0}, FTime::Seconds());
	struct FRestore { ~FRestore() { ConfigureAutomaticGarbageCollection({}, FTime::Seconds()); } } Restore;
	TStrongObjectPtr<DEngine> Host(NewObject<DEngine>(nullptr, "PressureGCHost"));
	TObjectPtr<DObject> Garbage = NewObject<DObject>(nullptr, "PressureGarbage");
	MarkAsGarbage(Garbage.Get());
	EXPECT_NE(Garbage.Get(), nullptr);
	EXPECT_FALSE(IsGarbageCollectionRequested());
	++GFrameCounter;
	Host->ConditionalCollectGarbage();
	EXPECT_EQ(Garbage.Get(), nullptr);
	TObjectPtr<DObject> Next = NewObject<DObject>(nullptr, "NextPressureGarbage");
	MarkAsGarbage(Next.Get());
	Host->ConditionalCollectGarbage();
	EXPECT_NE(Next.Get(), nullptr); // Pressure cannot consume the same frame twice.
	++GFrameCounter;
	Host->ConditionalCollectGarbage();
	EXPECT_EQ(Next.Get(), nullptr);
	Host->PreExit();
}

	TEST(FEngineGarbageCollectionTests, GarbageCollectionSchedulerUsesTimeAndPressure)
	{
		Durin::FGarbageCollectionSettings Settings;
		Settings.IntervalSeconds = 60.0;
		Settings.PendingKillThreshold = 128;
		Settings.ObjectGrowthThreshold = 1024;
		Durin::Private::FGarbageCollectionScheduler Scheduler(Settings);
		Scheduler.Reset(100.0, 20);

		EXPECT_EQ(Scheduler.ShouldCollect(159.0, 20, 0), Durin::Private::EGarbageCollectionTrigger::None);
		EXPECT_EQ(Scheduler.ShouldCollect(160.0, 20, 0), Durin::Private::EGarbageCollectionTrigger::Interval);
		EXPECT_EQ(Scheduler.ShouldCollect(101.0, 20, 128), Durin::Private::EGarbageCollectionTrigger::PendingKillPressure);
		EXPECT_EQ(Scheduler.ShouldCollect(101.0, 1044, 0), Durin::Private::EGarbageCollectionTrigger::ObjectGrowthPressure);

		Settings.bEnabled = false;
		Durin::Private::FGarbageCollectionScheduler DisabledScheduler(Settings);
		DisabledScheduler.Reset(100.0, 20);
		EXPECT_EQ(DisabledScheduler.ShouldCollect(1000.0, 10000, 10000), Durin::Private::EGarbageCollectionTrigger::None);
	}

	TEST(FEngineGarbageCollectionTests, GarbageCollectionSchedulerBacksOffEmptyCollectionsWithinBound)
	{
		Durin::FGarbageCollectionSettings Settings;
		Settings.IntervalSeconds = 60.0;
		Settings.MaxIntervalSeconds = 600.0;
		Settings.IntervalBackoffMultiplier = 2.0;
		Settings.PendingKillThreshold = 128;
		Settings.ObjectGrowthThreshold = 1024;
		Durin::Private::FGarbageCollectionScheduler Scheduler(Settings);
		Scheduler.Reset(0.0, 20);

		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 60.0);
		Scheduler.NotifyCollectionCompleted(60.0, 20, 0);
		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 120.0);
		EXPECT_EQ(Scheduler.ShouldCollect(179.0, 20, 0), Durin::Private::EGarbageCollectionTrigger::None);
		EXPECT_EQ(Scheduler.ShouldCollect(180.0, 20, 0), Durin::Private::EGarbageCollectionTrigger::Interval);

		Scheduler.NotifyCollectionCompleted(180.0, 20, 0);
		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 240.0);
		Scheduler.NotifyCollectionCompleted(420.0, 20, 0);
		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 480.0);
		Scheduler.NotifyCollectionCompleted(900.0, 20, 0);
		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 600.0);
		Scheduler.NotifyCollectionCompleted(1500.0, 20, 0);
		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 600.0);

		EXPECT_EQ(Scheduler.ShouldCollect(1501.0, 20, 128), Durin::Private::EGarbageCollectionTrigger::PendingKillPressure);
		EXPECT_EQ(Scheduler.ShouldCollect(1501.0, 1044, 0), Durin::Private::EGarbageCollectionTrigger::ObjectGrowthPressure);
		Scheduler.NotifyCollectionCompleted(1501.0, 20, 1);
		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 60.0);
	}

	TEST(FEngineGarbageCollectionTests, GarbageCollectionSchedulerNormalizesAdaptiveSettings)
	{
		Durin::FGarbageCollectionSettings Settings;
		Settings.IntervalSeconds = 60.0;
		Settings.MaxIntervalSeconds = 30.0;
		Settings.IntervalBackoffMultiplier = 0.5;
		Durin::Private::FGarbageCollectionScheduler Scheduler(Settings);
		Scheduler.Reset(0.0, 0);
		Scheduler.NotifyCollectionCompleted(60.0, 0, 0);

		EXPECT_DOUBLE_EQ(Scheduler.GetSettings().MaxIntervalSeconds, 60.0);
		EXPECT_DOUBLE_EQ(Scheduler.GetSettings().IntervalBackoffMultiplier, 1.0);
		EXPECT_DOUBLE_EQ(Scheduler.GetCurrentIntervalSeconds(), 60.0);

		Settings.IntervalSeconds = 0.0;
		Durin::Private::FGarbageCollectionScheduler NoIntervalScheduler(Settings);
		NoIntervalScheduler.Reset(0.0, 0);
		NoIntervalScheduler.NotifyCollectionCompleted(100.0, 0, 0);
		EXPECT_EQ(NoIntervalScheduler.ShouldCollect(1000.0, 0, 0), Durin::Private::EGarbageCollectionTrigger::None);
	}


TEST(FEngineGarbageCollectionTests, CheckingWithoutCollectionConsumesTheFrameOpportunity)
{
	using namespace Durin;
	Testing::InitializeDObjectSystemForTests();
	CollectGarbage(EObjectFlags::Standalone);
	ConfigureAutomaticGarbageCollection({.bEnabled = false}, FTime::Seconds());
	struct FRestore { ~FRestore() { ConfigureAutomaticGarbageCollection({}, FTime::Seconds()); } } Restore;
	TStrongObjectPtr<DEngine> Host(NewObject<DEngine>(nullptr, "CheckedGCHost"));
	++GFrameCounter;
	const auto Serial = GetLastGarbageCollectionStats().CollectionSerial;
	Host->ConditionalCollectGarbage();
	EXPECT_EQ(GetLastGarbageCollectionStats().CollectionSerial, Serial);
	Host->ForceGarbageCollection(true);
	Host->ConditionalCollectGarbage();
	EXPECT_TRUE(IsGarbageCollectionRequested());
	EXPECT_EQ(GetLastGarbageCollectionStats().CollectionSerial, Serial);
	++GFrameCounter;
	Host->ConditionalCollectGarbage();
	EXPECT_FALSE(IsGarbageCollectionRequested());
	EXPECT_EQ(GetLastGarbageCollectionStats().CollectionFrame, GFrameCounter);
	EXPECT_FALSE(IsIncrementalPurgePending());
	Host->PreExit();
}

namespace
{
	class DPendingPurgeTestObject : public Durin::DObject
	{
	public:
		bool bReady = true;
		explicit DPendingPurgeTestObject(const Durin::FObjectInitializer& Initializer)
			: DObject(Initializer) {}
		auto IsReadyForFinishDestroy() -> bool override { return bReady; }
		static void __DefaultConstructor(const Durin::FObjectInitializer& Initializer)
		{ new (Initializer.GetObj()) DPendingPurgeTestObject(Initializer); }
		static auto StaticClass() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(Durin::EC_StaticConstructor, "DPendingPurgeTestObject",
					sizeof(DPendingPurgeTestObject), alignof(DPendingPurgeTestObject),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None, Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)Durin::InternalConstructor<DPendingPurgeTestObject>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->Register(Durin::DClass::StaticClass, "", "DPendingPurgeTestObject");
				Durin::DObjectForceRegistration(Class);
			}
			return Class;
		}
	};
}

TEST(FEngineGarbageCollectionTests, DisabledPolicyStillAdvancesPendingDestructionWithoutScanning)
{
	using namespace Durin;
	Testing::InitializeDObjectSystemForTests();
	CollectGarbage(EObjectFlags::Standalone);
	ConfigureAutomaticGarbageCollection({.bEnabled = false}, FTime::Seconds());
	struct FRestore { ~FRestore() { ConfigureAutomaticGarbageCollection({}, FTime::Seconds()); } } Restore;
	TStrongObjectPtr<DEngine> Host(NewObject<DEngine>(nullptr, "PurgeGCHost"));
	auto* Pending = NewObject<DPendingPurgeTestObject>(nullptr, "PendingPurge");
	Pending->bReady = false;
	TObjectPtr<DObject> Handle = Pending;
	CollectGarbage(EObjectFlags::NoFlags, false);
	const auto Serial = GetLastGarbageCollectionStats().CollectionSerial;
	EXPECT_TRUE(IsIncrementalPurgePending());
	TObjectPtr<DObject> Fresh = NewObject<DObject>(nullptr, "FreshAfterMark");
	++GFrameCounter;
	Host->ConditionalCollectGarbage();
	EXPECT_TRUE(IsIncrementalPurgePending());
	Pending->bReady = true;
	Host->ForceGarbageCollection();
	++GFrameCounter;
	Host->ConditionalCollectGarbage();
	EXPECT_EQ(Handle.Get(), nullptr);
	EXPECT_FALSE(IsIncrementalPurgePending());
	EXPECT_NE(Fresh.Get(), nullptr);
	EXPECT_EQ(GetLastGarbageCollectionStats().CollectionSerial, Serial);
	EXPECT_TRUE(IsGarbageCollectionRequested());
	++GFrameCounter;
	Host->ConditionalCollectGarbage();
	EXPECT_EQ(Fresh.Get(), nullptr);
	EXPECT_FALSE(IsGarbageCollectionRequested());
	Host->PreExit();
}

TEST(FEngineGarbageCollectionTests, EngineSelectsEditorAndRuntimeKeepFlags)
{
	using namespace Durin;
	Testing::InitializeDObjectSystemForTests();
	CollectGarbage(EObjectFlags::Standalone);
	const bool WasEditor = GIsEditor;
	struct FRestore { bool Editor; ~FRestore() { GIsEditor = Editor; } } Restore{WasEditor};
	TStrongObjectPtr<DEngine> Host(NewObject<DEngine>(nullptr, "KeepFlagsGCHost"));
	TObjectPtr<DObject> Standalone = NewObject<DObject>(nullptr, "HostStandalone", EObjectFlags::Standalone);
	GIsEditor = true;
	Host->ForceGarbageCollection(true);
	++GFrameCounter;
	Host->ConditionalCollectGarbage();
	EXPECT_NE(Standalone.Get(), nullptr);
	GIsEditor = false;
	Host->ForceGarbageCollection(true);
	++GFrameCounter;
	Host->ConditionalCollectGarbage();
	EXPECT_EQ(Standalone.Get(), nullptr);
	Host->PreExit();
}

TEST(FEngineGarbageCollectionTests, FullRequestDrainsPendingAndCollectsNewGarbageAtTheSameSafePoint)
{
	using namespace Durin;
	Testing::InitializeDObjectSystemForTests();
	CollectGarbage(EObjectFlags::Standalone);
	TStrongObjectPtr<DEngine> Host(NewObject<DEngine>(nullptr, "FullPurgeGCHost"));
	auto* Pending = NewObject<DPendingPurgeTestObject>(nullptr, "OldPendingPurge");
	Pending->bReady = false;
	CollectGarbage(EObjectFlags::NoFlags, false);
	const auto Serial = GetLastGarbageCollectionStats().CollectionSerial;
	TObjectPtr<DObject> Fresh = NewObject<DObject>(nullptr, "NewGarbageForFullPurge");
	Pending->bReady = true;
	Host->ForceGarbageCollection(true);
	Host->ForceGarbageCollection(false);
	++GFrameCounter;
	Host->ConditionalCollectGarbage();
	EXPECT_EQ(Fresh.Get(), nullptr);
	EXPECT_EQ(GetLastGarbageCollectionStats().CollectionSerial, Serial + 1);
	EXPECT_FALSE(IsIncrementalPurgePending());
	EXPECT_FALSE(IsGarbageCollectionRequested());
	Host->PreExit();
}

TEST(FEngineGarbageCollectionTests, EnginePolicyObservesExplicitCollectionsAndRetainsIntervalFallback)
{
	using namespace Durin;
	Testing::InitializeDObjectSystemForTests();
	CollectGarbage(EObjectFlags::Standalone);
	ConfigureAutomaticGarbageCollection({.IntervalSeconds = 60.0, .MaxIntervalSeconds = 600.0,
		.PendingKillThreshold = 0, .ObjectGrowthThreshold = 0}, FTime::Seconds());
	struct FRestore { ~FRestore() { ConfigureAutomaticGarbageCollection({}, FTime::Seconds()); } } Restore;
	TStrongObjectPtr<DObject> Owner(NewObject<DObject>(nullptr, "IntervalFallbackOwner"));
	TObjectPtr<DObject> Handle = Owner.Get();
	for (int Index = 0; Index < 4; ++Index)
	{
		CollectGarbage(EObjectFlags::Standalone);
		EXPECT_EQ(GetLastGarbageCollectionStats().CandidateObjectCount, 0u);
		EXPECT_EQ(Private::EvaluateGarbageCollection(GetLastGarbageCollectionStats().CollectionTime),
			Private::EGarbageCollectionTrigger::None);
	}
	EXPECT_DOUBLE_EQ(Private::GetCurrentAutomaticGarbageCollectionIntervalSeconds(), 600.0);
	Owner.Reset();
	const double LastCollectionTime = GetLastGarbageCollectionStats().CollectionTime;
	EXPECT_EQ(Private::EvaluateGarbageCollection(LastCollectionTime + 599.0), Private::EGarbageCollectionTrigger::None);
	EXPECT_EQ(Private::EvaluateGarbageCollection(LastCollectionTime + 600.0), Private::EGarbageCollectionTrigger::Interval);
	EXPECT_NE(Handle.Get(), nullptr);
	CollectGarbage(EObjectFlags::Standalone);
	EXPECT_EQ(Handle.Get(), nullptr);
	EXPECT_DOUBLE_EQ(Private::GetCurrentAutomaticGarbageCollectionIntervalSeconds(), 60.0);
}
