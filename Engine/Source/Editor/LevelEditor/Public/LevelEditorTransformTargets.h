#pragma once

#include "LevelEditorAPI.h"
#include "Math/Transform.h"

namespace Durin
{
	class DPackage;
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

	// Adapts an arbitrary editable object to the transform gizmo contract.
	class ITransformGizmoTarget
	{
	public:
		virtual ~ITransformGizmoTarget() = default;
		virtual auto IsValid() const -> bool = 0;
		virtual auto GetIdentity() const -> const void* = 0;
		virtual auto GetTransform() const -> FTransform = 0;
		virtual auto SetTransform(const FTransform& Transform) -> bool = 0;
		virtual auto GetParentRotation() const -> FQuat { return FQuatConstants::Identity; }
		virtual auto GetPackage() const -> DPackage* { return nullptr; }
		virtual auto GetLabel() const -> std::string = 0;
		virtual auto GetCapabilities() const -> ETransformGizmoCapability { return ETransformGizmoCapability::All; }
	};

	struct FTransformGizmoTargetSet
	{
		std::vector<std::shared_ptr<ITransformGizmoTarget>> Targets;
		std::string CollectionLabel = "Targets";
		auto IsEmpty() const -> bool { return Targets.empty(); }
	};

	LEVELEDITOR_API auto MakeActorTransformGizmoTargets(const FLevelEditorContext& Context) -> FTransformGizmoTargetSet;
} // namespace Durin
