#pragma once

#include "CoreMinimal.h"
#include "MaterialGraphOperations.h"

namespace Durin::MaterialEditor
{
	struct FMaterialGraphCanvasMetrics
	{
		float NodeWidth = 224.0f;
		float HeaderHeight = 30.0f;
		float SecondaryHeight = 20.0f;
		float PinRowHeight = 24.0f;
		float BodyPadding = 10.0f;
		float ColumnGap = 96.0f;
		float RowGap = 28.0f;
		float SurfaceWidth = 268.0f;
		float SurfaceLabelWidth = 144.0f;
		float SurfaceValueGap = 12.0f;
		float SurfaceValueWidth = 92.0f;
		float MinimumHitDiameter = 16.0f;
	};

	enum class EMaterialGraphDetailLevel : uint8
	{
		Overview,
		Readable,
		Editing,
	};

	// Provides deterministic presentation calculations without mutable ImGui state.
	class FMaterialGraphGeometry
	{
	public:
		MATERIALEDITOR_API static auto GetMetrics()
			-> const FMaterialGraphCanvasMetrics&;
		MATERIALEDITOR_API static auto GetNodeHeight(uint32 InputCount) -> float;
		MATERIALEDITOR_API static auto SelectDetailLevel(
			float Zoom,
			EMaterialGraphDetailLevel Previous) -> EMaterialGraphDetailLevel;
	};

	// Keeps detached position drafts and commits one position delta on pointer release.
	class FMaterialGraphMoveSession
	{
	public:
		MATERIALEDITOR_API FMaterialGraphMoveSession();
		MATERIALEDITOR_API ~FMaterialGraphMoveSession();
		FMaterialGraphMoveSession(const FMaterialGraphMoveSession&) = delete;
		auto operator=(const FMaterialGraphMoveSession&)
			-> FMaterialGraphMoveSession& = delete;

		MATERIALEDITOR_API auto Begin(
			DObject& Owner,
			std::span<const FGuid> NodeIds,
			DTransactor* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Apply(
			std::span<const FMaterialGraphNodePresentation> Positions)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Commit() -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Cancel() -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto IsActive() const -> bool;

		MATERIALEDITOR_API auto IsCurrent(const DObject& Owner) const -> bool;
		MATERIALEDITOR_API auto GetPositions() const -> std::span<const FMaterialGraphNodePresentation>;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	// Publishes parameter previews during one pointer gesture and records one undo step.
	class FMaterialGraphParameterEditSession
	{
	public:
		MATERIALEDITOR_API FMaterialGraphParameterEditSession();
		MATERIALEDITOR_API ~FMaterialGraphParameterEditSession();
		FMaterialGraphParameterEditSession(const FMaterialGraphParameterEditSession&) = delete;
		auto operator=(const FMaterialGraphParameterEditSession&)
			-> FMaterialGraphParameterEditSession& = delete;

		MATERIALEDITOR_API auto Begin(
			DMaterial& Material,
			const FGuid& ParameterId,
			DTransactor* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Apply(FMaterialParameterValue Value)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Commit() -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Cancel() -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto IsActive() const -> bool;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
