#pragma once

#include "CoreMinimal.h"
#include "NativeDObjectTestSupport.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailDrawContext.h"
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
		static auto IsWaiting(const FThumbnailDrawContext& Context) -> bool { return Context.Status == FThumbnailDrawContext::EState::Waiting; }
		static auto IsReady(const FThumbnailDrawContext& Context) -> bool { return Context.Status == FThumbnailDrawContext::EState::Ready; }
		static auto HasFailed(const FThumbnailDrawContext& Context) -> bool { return Context.Status == FThumbnailDrawContext::EState::Failed; }
		static auto GetDiagnostic(const FThumbnailDrawContext& Context) -> const std::string& { return Context.Diagnostic; }
		static auto Validate(const FThumbnailDrawContext& Context) -> std::expected<void, std::string> { return Context.Validate(); }
		static auto BindCapture(FThumbnailDrawContext& Context, FThumbnailCapture& Capture) -> std::expected<void, std::string> { return Context.BindCapture(Capture); }
		static auto RegisterScoped(DThumbnailManager& Manager, DThumbnailRenderer* Renderer, std::string AssetClassName)
			-> FThumbnailRendererRegistrationHandle
		{
			auto Registration = Manager.RegisterRendererInstance(Renderer,
				AssetClassName);
			if (Registration) Manager.SetLoadedObjectOverride(AssetClassName,
				NewObject<DObject>(nullptr, {}, EObjectFlags::Transient));
			return Registration;
		}
		static auto Register(DThumbnailManager& Manager, DThumbnailRenderer* Renderer, std::string AssetClassName)
			-> bool
		{
			auto Result = RegisterScoped(Manager, Renderer, std::move(AssetClassName));
			if (!Result) return false;
			Result.AssetClassName.clear();
			Result.Renderer = nullptr;
			Result.State.reset();
			return true;
		}
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
