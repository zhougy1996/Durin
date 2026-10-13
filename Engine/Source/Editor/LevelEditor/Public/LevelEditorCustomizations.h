#pragma once

#include "CoreMinimal.h"

#include "LevelEditorAPI.h"
#include "DObject/WeakObjectPtr.h"
#include "PropertyEditor/PropertyView.h"
#include "SceneView.h"
#include "LevelEditorSelection.h"

namespace Durin
{
	class AActor;
	class DActorComponent;
	class DClass;
	class DLevel;
	class DObject;
}

namespace Durin::Editor::Level
{
	enum class EEditorVisualizationIcon : uint8
	{
		Camera,
		DirectionalLight,
		PlayerStart,
	};

	struct FLevelEditorContext;

	// Supplies scene and selection state to component visualization extensions.
	struct FEditorVisualizationContext
	{
		const FSceneView& View;
		DLevel* Level = nullptr;
		bool bSelected = false;
		bool bPrimarySelection = false;
		bool bComponentSelected = false;
		std::span<const FEditorSubElementSelection> SelectedSubElements;
	};

	// Describes a world-space line and its screen-space hit-test policy.
	struct FEditorVisualizationLine
	{
		FVector3 Start{0.0};
		FVector3 End{0.0};
		FVector4f Color{1.0f};
		float WidthPixels = 1.0f;
		float HitTolerancePixels = 6.0f;
		int32 HitPriority = 0;
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<DActorComponent> Component;
		ESimpleElementLinePattern Pattern = ESimpleElementLinePattern::Solid;
		float PatternPeriodPixels = 12.0f;
		// Applied only when this primitive's actor is hovered; absence preserves its base color.
		std::optional<FVector4f> HoverColor;
		FEditorSubElementSelection Element;
	};

	// Describes a world-space icon and its screen-space hit-test policy.
	struct FEditorVisualizationIcon
	{
		EEditorVisualizationIcon Icon = EEditorVisualizationIcon::Camera;
		FVector3 WorldPosition{0.0};
		FVector4f Color{1.0f};
		float SizePixels = 30.0f;
		float HitPaddingPixels = 3.0f;
		int32 HitPriority = 100;
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<DActorComponent> Component;
		bool bDepthIndependentHit = true;
		// Applied only when this primitive's actor is hovered; absence preserves its base color.
		std::optional<FVector4f> HoverColor;
		FEditorSubElementSelection Element;
	};

	// Describes a screen-sized solid box anchored in world space.
	struct FEditorVisualizationBox
	{
		FVector3 WorldPosition{0.0};
		FVector4f Color{1.0f};
		float SizePixels = 10.0f;
		float HitPaddingPixels = 4.0f;
		int32 HitPriority = 100;
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<DActorComponent> Component;
		bool bDepthIndependentHit = true;
		// Applied only when this primitive's actor is hovered; absence preserves its base color.
		std::optional<FVector4f> HoverColor;
		FEditorSubElementSelection Element;
	};

	// Describes a transformed procedural shape whose object identity controls lifetime and hover color.
	struct FEditorVisualizationPrimitive
	{
		EViewOverlayShape Shape = EViewOverlayShape::Box;
		FMatrix LocalToWorld{1.0};
		FVector4f Color{1.0f};
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<DActorComponent> Component;
		std::optional<FVector4f> HoverColor;
		FEditorSubElementSelection Element;
	};

	// Reports the highest-priority visualization hit at one viewport position.
	struct FEditorVisualizationHit
	{
		AActor* Actor = nullptr;
		DActorComponent* Component = nullptr;
		FEditorSubElementSelection Element;
		double Distance = std::numeric_limits<double>::max();
		int32 Priority = std::numeric_limits<int32>::min();
		bool bDepthIndependent = false;
	};

	// Collects component overlays and performs their shared viewport hit testing.
	class FEditorVisualizationCollector
	{
	public:
		LEVELEDITOR_API auto AddLine(const FEditorVisualizationLine& Line) -> void;
		LEVELEDITOR_API auto AddIcon(const FEditorVisualizationIcon& Icon) -> void;
		LEVELEDITOR_API auto AddBox(const FEditorVisualizationBox& Box) -> void;
		LEVELEDITOR_API auto AddPrimitive(const FEditorVisualizationPrimitive& Primitive) -> void;
		LEVELEDITOR_API auto AppendToView(FSceneView& View, const FEditorVisualizationHit* Hovered = nullptr) const -> void;
		LEVELEDITOR_API auto AppendToView(FSceneView& View, const AActor* HoveredActor) const -> void;
		LEVELEDITOR_API auto HitTest(const FSceneView& View, const FVector2f& ViewportPosition) const -> FEditorVisualizationHit;
		auto GetLines() const -> const std::vector<FEditorVisualizationLine>& { return Lines; }
		auto GetIcons() const -> const std::vector<FEditorVisualizationIcon>& { return Icons; }
		auto GetBoxes() const -> const std::vector<FEditorVisualizationBox>& { return Boxes; }
		auto GetPrimitives() const -> const std::vector<FEditorVisualizationPrimitive>& { return Primitives; }

	private:
		std::vector<FEditorVisualizationLine> Lines;
		std::vector<FEditorVisualizationIcon> Icons;
		std::vector<FEditorVisualizationBox> Boxes;
		std::vector<FEditorVisualizationPrimitive> Primitives;
	};

	// Defines a component-specific producer of editor viewport overlays.
	class IComponentEditorVisualizer
	{
	public:
		virtual ~IComponentEditorVisualizer() = default;
		virtual auto DrawVisualization(DActorComponent* Component, const FEditorVisualizationContext& Context, FEditorVisualizationCollector& Collector) const -> void = 0;
	};

	// Defines an actor-specific producer of editor viewport overlays.
	class IActorEditorVisualizer
	{
	public:
		virtual ~IActorEditorVisualizer() = default;
		virtual auto DrawVisualization(AActor* Actor, const FEditorVisualizationContext& Context,
			FEditorVisualizationCollector& Collector) const -> void = 0;
	};

	// Distinguishes viewport visualizers from object-details customizations.
	enum class ELevelEditorCustomizationKind : uint8
	{
		ActorVisualizer,
		ComponentVisualizer,
	};

	// Identifies one registered editor customization for later removal.
	struct FLevelEditorCustomizationHandle
	{
		uint64 Id = 0;
		ELevelEditorCustomizationKind Kind = ELevelEditorCustomizationKind::ComponentVisualizer;
		explicit operator bool() const { return Id != 0; }
	};

	// Resolves inherited component and details customizations by reflected class.
	class FLevelEditorCustomizationRegistry
	{
	public:
		LEVELEDITOR_API static auto Get() -> FLevelEditorCustomizationRegistry&;
		LEVELEDITOR_API auto RegisterActorVisualizer(DClass* Class, std::shared_ptr<IActorEditorVisualizer> Visualizer) -> FLevelEditorCustomizationHandle;
		LEVELEDITOR_API auto RegisterComponentVisualizer(DClass* Class, std::shared_ptr<IComponentEditorVisualizer> Visualizer) -> FLevelEditorCustomizationHandle;
		LEVELEDITOR_API auto Unregister(FLevelEditorCustomizationHandle Handle) -> bool;
		LEVELEDITOR_API auto FindActorVisualizer(const DClass* Class) const -> std::shared_ptr<IActorEditorVisualizer>;
		LEVELEDITOR_API auto FindComponentVisualizer(const DClass* Class) const -> std::shared_ptr<IComponentEditorVisualizer>;

	private:
		template<typename T>
		// Couples a registration identifier with its shared customization instance.
		struct TEntry
		{
			uint64 HandleId = 0;
			std::shared_ptr<T> Customization;
		};

		uint64 NextHandleId = 1;
		std::unordered_map<DClass*, TEntry<IActorEditorVisualizer>> ActorVisualizers;
		std::unordered_map<DClass*, TEntry<IComponentEditorVisualizer>> ComponentVisualizers;
	};
} // namespace Durin::Editor::Level
