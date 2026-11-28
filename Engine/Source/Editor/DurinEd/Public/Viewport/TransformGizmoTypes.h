#pragma once

#include "CoreMinimal.h"

#include "Math/Transform.h"

namespace Durin
{
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

	// Adapts an arbitrary editable object to the transform gizmo contract.
	class ITransformGizmoTarget
	{
	public:
		virtual ~ITransformGizmoTarget() = default;
		// Indexed targets share the topology revision of their data owner.
		using FTopologyRevisionReader = uint64 (*)(const DObject&, uint64 HistoryId);
		virtual auto GetTopologyRevisionReader() const -> FTopologyRevisionReader { return nullptr; }
		virtual auto IsValid() const -> bool = 0;
		virtual auto GetIdentity() const -> FTransformGizmoTargetIdentity = 0;
		// All persistent effects of SetTransform must be restored by this owner's whole-object snapshot.
		// The proxy captures and verifies the owner before a transactional preview write.
		virtual auto GetDataOwner() const -> DObject* = 0;
		// Refresh any adapter-specific baselines needed to restore this gesture.
		virtual void BeginTransformEditSequence() {}
		virtual auto GetTransform() const -> FTransform = 0;
		// Apply a preview or restoration. The proxy calls Modify() before the owner's first preview write.
		virtual auto SetTransform(const FTransform& Transform) -> bool = 0;
		virtual auto GetParentRotation() const -> FQuat { return FQuatConstants::Identity; }
		virtual auto GetPackage() const -> DPackage* { return nullptr; }
		virtual auto GetLabel() const -> std::string = 0;
		virtual auto GetCapabilities() const -> ETransformGizmoCapability { return ETransformGizmoCapability::All; }
		virtual auto AddReferencedObjects(FReferenceCollector& Collector) -> void
		{
			(void)Collector;
		}
	};

	struct FTransformGizmoTargetSet
	{
		std::vector<std::shared_ptr<ITransformGizmoTarget>> Targets;
		std::string CollectionLabel = "Targets";
		auto IsEmpty() const -> bool { return Targets.empty(); }
	};

} // namespace Durin::LevelEditor
