#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/TransactionObjectEvent.h"
#include "Engine/Actor.h"
#include "Engine/ComponentInstanceDataCache.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineTestSupport.h"
#include <gtest/gtest.h>

namespace Durin::Tests::ObjectState
{
	template<typename T, typename Base>
	auto MakeTransactionEngineTestClass(const char* Name) -> Durin::DClass*
	{
		using namespace Durin;
		auto* Class = new DClass(EC_StaticConstructor, Name, sizeof(T), alignof(T), EObjectFlags::NoFlags,
			EClassFlags::NoClassDefaultObject, EClassCastFlags::DClass, (DClass::ClassConstructorType)InternalConstructor<T>);
		Class->SetSuperStructBase(Base::StaticClass()); Class->SetTypeNames(Name, "", "");
		Class->Register(DClass::StaticClass, "", Name); DObjectForceRegistration(Class); return Class;
	}

	struct FUnsupportedSceneInstanceData : Durin::FSceneComponentInstanceData
	{
		using FSceneComponentInstanceData::FSceneComponentInstanceData;
		auto GetAllocatedSize() const -> size_t override { return sizeof(*this); }
		int32 DomainValue = 17;
	};

	struct FTypedSceneInstanceData : Durin::FSceneComponentInstanceData
	{
		FTypedSceneInstanceData() = default;
		~FTypedSceneInstanceData() override { ++Destroyed; }
		inline static uint32 Destroyed = 0;
		explicit FTypedSceneInstanceData(const Durin::DActorComponent& Component) : FSceneComponentInstanceData(Component) {}
		auto GetDataType() const -> const Durin::FComponentInstanceDataType& override
		{ return RegisterType<FTypedSceneInstanceData>("Tests.TypedSceneInstanceData"); }
		auto Serialize(Durin::FArchive& Archive) -> void override
		{ FSceneComponentInstanceData::Serialize(Archive); Archive << DomainValue; }
		auto ApplyToComponent(Durin::DActorComponent& Component, Durin::ECacheApplyPhase Phase) const -> Durin::FCommandChangeResult override
		{
			if (auto Result = FSceneComponentInstanceData::ApplyToComponent(Component, Phase); !Result) return Result;
			AppliedValue = DomainValue;
			return {};
		}
		auto GetAllocatedSize() const -> size_t override { return sizeof(*this); }
		int32 DomainValue = 17;
		inline static int32 AppliedValue = 0;
	};

	class DTransactionLifecycleScene final : public Durin::DSceneComponent
	{
	public:
		explicit DTransactionLifecycleScene(const Durin::FObjectInitializer& Initializer) : DSceneComponent(Initializer) { SetRelativeLocation(NativeInitialLocation); }
		inline static Durin::FVector3 NativeInitialLocation{0.0};
		static void __DefaultConstructor(const Durin::FObjectInitializer& Initializer) { new (Initializer.GetObj()) DTransactionLifecycleScene(Initializer); }
		static auto StaticClass() -> Durin::DClass* { static auto* Class = MakeTransactionEngineTestClass<DTransactionLifecycleScene, Durin::DSceneComponent>("TransactionLifecycleScene"); return Class; }
		auto OnComponentCreated() -> void override { DSceneComponent::OnComponentCreated(); ++Created; CreatedLocation = GetWorldLocation(); }
		auto OnComponentDestroyed() -> void override { DSceneComponent::OnComponentDestroyed(); ++Destroyed; }
		auto OnRegister() -> void override
		{
			DSceneComponent::OnRegister(); ++Registered;
			if (RegistrationCallback) RegistrationCallback();
			if (bCollectOnRegister) Durin::CollectGarbage();
			if (bThrowOnRegister) throw std::runtime_error("Injected registration callback failure.");
			if (bDestroyOnRegister) DestroyComponent();
		}
		auto OnUnregister() -> void override { DSceneComponent::OnUnregister(); ++Unregistered; }
		auto BeginPlay() -> void override { DSceneComponent::BeginPlay(); ++Begun; }
		auto EndPlay() -> void override { DSceneComponent::EndPlay(); ++Ended; }
		auto InitializeComponent() -> void override { DSceneComponent::InitializeComponent(); ++Initialized; }
		auto UninitializeComponent() -> void override { DSceneComponent::UninitializeComponent(); ++Uninitialized; }
		auto GetComponentInstanceData() const -> std::shared_ptr<Durin::FActorComponentInstanceData> override
		{
			if (bTypedInstanceData) { auto Data = std::make_shared<FTypedSceneInstanceData>(*this); Data->DomainValue = 42; return Data; }
			if (bUnsupportedInstanceData) return std::make_shared<FUnsupportedSceneInstanceData>(*this);
			return DSceneComponent::GetComponentInstanceData();
		}
		uint32 Created = 0, Destroyed = 0, Registered = 0, Unregistered = 0, Initialized = 0, Uninitialized = 0, Begun = 0, Ended = 0;
		Durin::FVector3 CreatedLocation{0.0};
		bool bCollectOnRegister = false, bDestroyOnRegister = false, bThrowOnRegister = false;
		bool bUnsupportedInstanceData = false, bTypedInstanceData = false;
		std::function<void()> RegistrationCallback;
		auto PostTransacted(const Durin::FTransactionObjectEvent& Event) -> void override { if (TransactionObserver) TransactionObserver(*this, Event); }
		std::function<void(DTransactionLifecycleScene&, const Durin::FTransactionObjectEvent&)> TransactionObserver;
	};

	class DTransactionConstructionActor final : public Durin::AActor
	{
	public:
		explicit DTransactionConstructionActor(const Durin::FObjectInitializer& Initializer) : AActor(Initializer)
		{
			SetRootComponent(CreateDefaultComponent<DTransactionLifecycleScene>("NativeRoot"));
		}
		static void __DefaultConstructor(const Durin::FObjectInitializer& Initializer) { new (Initializer.GetObj()) DTransactionConstructionActor(Initializer); }
		static auto StaticClass() -> Durin::DClass* { static auto* Class = MakeTransactionEngineTestClass<DTransactionConstructionActor, Durin::AActor>("TransactionConstructionActor"); return Class; }
		static auto Key(uint32 Index) -> Durin::FActorGeneratedComponentKey { return {"TransactionGenerated", Durin::FGuid(0x5452414e, 0x53414354, 0, Index + 1)}; }
		auto Serialize(Durin::FArchive& Ar) -> void override
		{
			AActor::Serialize(Ar);
			auto Scope = Durin::EnterArchiveField(Ar, {StaticClass()->GetQualifiedName(), "GeneratedCount", Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32)});
			Ar << GeneratedCount;
		}
		int32 GeneratedCount = 1;
		uint32 ConstructionCalls = 0;
		bool bFailConstruction = false, bCollectDuringConstruction = false;
		std::optional<Durin::FVector3> ConstructionLocation;
		std::function<void(DTransactionLifecycleScene&, const Durin::FTransactionObjectEvent&)> GeneratedTransactionObserver;
	protected:
		auto OnConstruction(const Durin::FTransform&) -> void override
		{
			++ConstructionCalls;
			for (int32 Index = 0; Index < GeneratedCount; ++Index)
			{
				auto* Component = AddComponentByClass(DTransactionLifecycleScene::StaticClass(), false, Durin::FTransform(), true, Key(Index), "Generated");
				if (!Component) throw std::runtime_error("Component creation failed.");
				if (ConstructionLocation) static_cast<Durin::DSceneComponent*>(Component)->SetRelativeLocation(*ConstructionLocation);
				FinishAddComponent(Component, false, Durin::FTransform());
				static_cast<DTransactionLifecycleScene*>(Component)->TransactionObserver = GeneratedTransactionObserver;
			}
			if (bCollectDuringConstruction) Durin::CollectGarbage();
			if (bFailConstruction) throw std::runtime_error("Injected transaction reconstruction failure.");

		}
	};

	struct FTransactionEngineWorld
	{
		FTransactionEngineWorld(Durin::EWorldType Type = Durin::EWorldType::Editor)
		{
			InitializeDObjectSystem();
			World = Durin::TStrongObjectPtr<Durin::DWorld>(Durin::NewObject<Durin::DWorld>(nullptr, "TransactionAnnotationWorld"));
			World->SetWorldType(Type);
			EXPECT_TRUE(World->InitializeSubsystems());
			EXPECT_TRUE(World->SetCurrentLevel(Durin::NewObject<Durin::DLevel>(World.Get(), "Level")));
		}
		Durin::TStrongObjectPtr<Durin::DWorld> World;
	};

}
