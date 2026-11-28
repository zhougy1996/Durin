#pragma once

#include "CoreMinimal.h"

#include "Math/Transform.h"
#include "DurinEdAPI.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	class DSceneComponent;
	class AActor;
	class DPackage;
	class DObject;
	class FReferenceCollector;
}

namespace Durin::LevelEditor
{
	struct FLevelEditorContext;

	enum class ETransformGizmoCapability : uint8
	{
		None = 0,
		Translate = 1 << 0,
		Rotate = 1 << 1,
		Scale = 1 << 2,
		All = Translate | Rotate | Scale,
	};
	constexpr auto operator|(ETransformGizmoCapability A, ETransformGizmoCapability B) -> ETransformGizmoCapability
	{
		return static_cast<ETransformGizmoCapability>(static_cast<uint8>(A) | static_cast<uint8>(B));
	}
	constexpr auto HasCapability(ETransformGizmoCapability Mask, ETransformGizmoCapability Value) -> bool
	{
		return (static_cast<uint8>(Mask) & static_cast<uint8>(Value)) != 0;
	}

	struct FTransformGizmoTargetIdentity
	{
		const void* Object;
		int32 ElementIndex = INDEX_NONE;
		uint8 ElementKind = 0;
		uint64 TopologyRevision = 0;
		auto operator==(const FTransformGizmoTargetIdentity&) const -> bool = default;
	};

	struct FTransformGizmoEditState
	{
		virtual ~FTransformGizmoEditState() = default;
	};

	struct FTransformGizmoWriteContext
	{
		const FTransformGizmoEditState* InitialState = nullptr;
		uint64 ReplayHistoryId = 0;
	};

	using FTopologyRevisionReader = uint64 (*)(const DObject&, uint64 HistoryId);
	using FComponentTransformGetter = std::function<FTransform()>;
	using FComponentTransformSetter = std::function<bool(const FTransform&, FTransformGizmoWriteContext)>;

	struct FTransformComponentOptions
	{
		FTransformGizmoTargetIdentity Identity{};
		std::string Label;
		ETransformGizmoCapability Capabilities = ETransformGizmoCapability::All;
		std::function<bool(uint64)> Validate;
		std::function<std::shared_ptr<const FTransformGizmoEditState>()> CaptureEditState;
		FTopologyRevisionReader ReadRevision = nullptr;
	};

	struct FTransformComponentBinding
	{
		TWeakObjectPtr<DSceneComponent> Component;
		TWeakObjectPtr<AActor> RootOwner;
		FComponentTransformGetter Getter;
		FComponentTransformSetter Setter;
		FTransformComponentOptions Options;

		DURINED_API bool IsValid(uint64 ReplayHistoryId = 0) const;
		DURINED_API DSceneComponent* GetDataOwner() const;
		auto GetIdentity() const { return Options.Identity; }
		DURINED_API FTransform GetTransform() const;
		DURINED_API bool SetTransform(const FTransform& Value, FTransformGizmoWriteContext Context = {}) const;
		DURINED_API FQuat GetParentRotation() const;
		DURINED_API DPackage* GetPackage() const;
		DURINED_API void AddReferencedObjects(FReferenceCollector& Collector) const;
		auto GetLabel() const -> const std::string& { return Options.Label; }
		auto GetCapabilities() const { return Options.Capabilities; }
		auto GetTopologyRevisionReader() const { return Options.ReadRevision; }
		auto CaptureEditState() const { return Options.CaptureEditState ? Options.CaptureEditState() : nullptr; }
	};

	struct FTransformGizmoTargetSet
	{
		std::vector<std::shared_ptr<const FTransformComponentBinding>> Targets;
		std::string CollectionLabel = "Targets";
		auto IsEmpty() const -> bool { return Targets.empty(); }
		DURINED_API void AddComponent(DSceneComponent* Component);
		DURINED_API void AddComponentCustom(DSceneComponent* Component, FComponentTransformGetter Getter,
			FComponentTransformSetter Setter, FTransformComponentOptions Options = {});
	};
}
