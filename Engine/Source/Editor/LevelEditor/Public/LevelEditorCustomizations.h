#pragma once

#include "LevelEditorAPI.h"
#include "IRendererModule.h"

namespace Durin
{
	class AActor;
	class DActorComponent;
	class DClass;
	class DLevel;
	class DObject;
	class FReflectedPropertyView;
	struct FReflectedPropertyViewContext;
	struct FLevelEditorContext;

	struct FEditorVisualizationContext
	{
		const FSceneView& View;
		DLevel* Level = nullptr;
		bool bSelected = false;
		bool bHovered = false;
		bool bPrimarySelection = false;
	};

	struct FEditorVisualizationLine
	{
		FVector3 Start{0.0};
		FVector3 End{0.0};
		FVector4f Color{1.0f};
		float WidthPixels = 1.0f;
		float HitTolerancePixels = 6.0f;
		int32 HitPriority = 0;
		AActor* Actor = nullptr;
		DActorComponent* Component = nullptr;
		EViewOverlayLinePattern Pattern = EViewOverlayLinePattern::Solid;
		float PatternPeriodPixels = 12.0f;
	};

	struct FEditorVisualizationIcon
	{
		EViewOverlayIcon Icon = EViewOverlayIcon::Camera;
		FVector3 WorldPosition{0.0};
		FVector4f Color{1.0f};
		float SizePixels = 30.0f;
		float HitPaddingPixels = 3.0f;
		int32 HitPriority = 100;
		AActor* Actor = nullptr;
		DActorComponent* Component = nullptr;
		bool bDepthIndependentHit = true;
	};

	struct FEditorVisualizationHit
	{
		AActor* Actor = nullptr;
		DActorComponent* Component = nullptr;
		double Distance = std::numeric_limits<double>::max();
		int32 Priority = std::numeric_limits<int32>::min();
		bool bDepthIndependent = false;
	};

	class FEditorVisualizationCollector
	{
	public:
		LEVELEDITOR_API auto AddLine(const FEditorVisualizationLine& Line) -> void;
		LEVELEDITOR_API auto AddIcon(const FEditorVisualizationIcon& Icon) -> void;
		LEVELEDITOR_API auto AppendToView(FSceneView& View) const -> void;
		LEVELEDITOR_API auto HitTest(const FSceneView& View, const FVector2f& ViewportPosition) const -> FEditorVisualizationHit;
		auto GetLines() const -> const std::vector<FEditorVisualizationLine>& { return Lines; }
		auto GetIcons() const -> const std::vector<FEditorVisualizationIcon>& { return Icons; }

	private:
		std::vector<FEditorVisualizationLine> Lines;
		std::vector<FEditorVisualizationIcon> Icons;
	};

	class IComponentEditorVisualizer
	{
	public:
		virtual ~IComponentEditorVisualizer() = default;
		virtual auto DrawVisualization(DActorComponent* Component, const FEditorVisualizationContext& Context, FEditorVisualizationCollector& Collector) const -> void = 0;
	};

	class IObjectDetailsCustomization
	{
	public:
		virtual ~IObjectDetailsCustomization() = default;
		// Return true when the customization replaces the object's reflected property rows.
		virtual auto DrawDetails(FLevelEditorContext& Context, DObject* Object, FReflectedPropertyView& PropertyView,
			const FReflectedPropertyViewContext& ViewContext) -> bool = 0;
	};

	enum class ELevelEditorCustomizationKind : uint8
	{
		ComponentVisualizer,
		ObjectDetails
	};

	struct FLevelEditorCustomizationHandle
	{
		uint64 Id = 0;
		ELevelEditorCustomizationKind Kind = ELevelEditorCustomizationKind::ComponentVisualizer;
		explicit operator bool() const { return Id != 0; }
	};

	class FLevelEditorCustomizationRegistry
	{
	public:
		LEVELEDITOR_API static auto Get() -> FLevelEditorCustomizationRegistry&;
		LEVELEDITOR_API auto RegisterComponentVisualizer(DClass* Class, std::shared_ptr<IComponentEditorVisualizer> Visualizer) -> FLevelEditorCustomizationHandle;
		LEVELEDITOR_API auto RegisterObjectDetails(DClass* Class, std::shared_ptr<IObjectDetailsCustomization> Customization) -> FLevelEditorCustomizationHandle;
		LEVELEDITOR_API auto Unregister(FLevelEditorCustomizationHandle Handle) -> bool;
		LEVELEDITOR_API auto FindComponentVisualizer(const DClass* Class) const -> std::shared_ptr<IComponentEditorVisualizer>;
		LEVELEDITOR_API auto FindObjectDetails(const DClass* Class) const -> std::shared_ptr<IObjectDetailsCustomization>;

	private:
		template<typename T>
		struct TEntry
		{
			uint64 HandleId = 0;
			std::shared_ptr<T> Customization;
		};

		uint64 NextHandleId = 1;
		std::unordered_map<DClass*, TEntry<IComponentEditorVisualizer>> ComponentVisualizers;
		std::unordered_map<DClass*, TEntry<IObjectDetailsCustomization>> ObjectDetails;
	};
} // namespace Durin
