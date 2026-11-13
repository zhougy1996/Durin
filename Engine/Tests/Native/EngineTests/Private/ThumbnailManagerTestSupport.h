#pragma once

#include "CoreMinimal.h"
#include "NativeDObjectTestSupport.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailDrawContext.h"
#include "../../../../Source/Editor/DurinEd/Private/Thumbnail/ThumbnailGenerationRequest.h"
#include "../../../../Source/Editor/DurinEd/Private/Thumbnail/ThumbnailRenderJob.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/StrongObjectPtr.h"

namespace Durin
{
	// Synthetic catalog rows have no asset class. Inject only GC-managed renderers
	// for queue/cache tests; production registration always constructs by class.
	struct FThumbnailManagerTestAccess
	{
		static auto RegisterScoped(DThumbnailManager& Manager, DThumbnailRenderer* Renderer)
			-> FThumbnailRendererRegistrationHandle
		{
			auto Registration = Manager.RegisterRendererInstance(Renderer,
				Renderer ? Renderer->GetRegistration().AssetClassName : std::string{});
			if (Registration) Manager.SetLoadedObjectOverride(Renderer->GetRegistration().AssetClassName,
				NewObject<DObject>(nullptr, {}, EObjectFlags::Transient));
			return Registration;
		}
		static auto Register(DThumbnailManager& Manager, DThumbnailRenderer* Renderer)
			-> bool
		{
			auto Result = RegisterScoped(Manager, Renderer);
			if (!Result) return false;
			Result.RegistrationId = 0;
			Result.State.reset();
			return true;
		}
		static auto CreateJob(DThumbnailManager& Manager, const FAssetThumbnailGenerationRequest& Request)
			-> std::unique_ptr<FThumbnailRenderJob> { return Manager.CreateRenderJob(Request); }
		static auto Unregister(DThumbnailManager& Manager, std::string_view Name) -> bool
		{
			return Manager.UnregisterRendererName(Name);
		}
	};

	template<typename T, typename... TArgs>
	auto MakeThumbnailRenderer(TArgs&&... Args) -> TStrongObjectPtr<T>
	{
		Testing::InitializeDObjectSystemForTests();
		TStrongObjectPtr<T> Result(NewObject<T>(nullptr, {}, EObjectFlags::Transient));
		if constexpr (sizeof...(Args) != 0)
			Result->Configure(std::forward<TArgs>(Args)...);
		return Result;
	}
}

// Native tests register metadata explicitly, without adding test classes to DHT.
#define DURIN_TEST_THUMBNAIL_RENDERER(Type) \
	using DClass = Durin::DClass; \
	using FObjectInitializer = Durin::FObjectInitializer; \
	static auto GetPrivateStaticClass() -> Durin::DClass* { \
		static Durin::DClass* Class = nullptr; \
		if (!Class) Durin::GetPrivateStaticClassBody("", #Type, Class, nullptr, sizeof(Type), alignof(Type), \
			Durin::EClassFlags::None, Durin::InternalConstructor<Type>, &Durin::DThumbnailRenderer::StaticClass); \
		return Class; } \
	DECLARE_CLASS(Type, Durin::DThumbnailRenderer, Type::GetPrivateStaticClass) \
	DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(Type) \
public: \
	explicit Type(const Durin::FObjectInitializer& Initializer) : Super(Initializer) {}
