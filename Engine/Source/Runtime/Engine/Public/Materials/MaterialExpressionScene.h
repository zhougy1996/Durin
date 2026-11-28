#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialProgramTypes.h"

#include "MaterialExpressionScene.gen.h"

namespace Durin
{
	DCLASS()
	class DMaterialExpressionWorldPosition : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionWorldPosition(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::WorldPosition, .Name = "World Position",
				.Description = "Surface position relative to the current view origin (Float3).",
				.Category = "Inputs", .FixedOutputType = EMaterialProgramValueType::Float3};
		}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionTime : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionTime(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::Time, .Name = "Time",
				.Description = "Elapsed real time in seconds (Float), updated every rendered view.",
				.Category = "Inputs", .FixedOutputType = EMaterialProgramValueType::Float};
		}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionCameraPosition : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionCameraPosition(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::CameraPosition, .Name = "Camera Position",
				.Description = "Active pass camera position in translated world space; zero at the view origin (Float3).",
				.Category = "Inputs", .FixedOutputType = EMaterialProgramValueType::Float3};
		}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionCameraVector : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionCameraVector(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::CameraVector, .Name = "Camera Vector",
				.Description = "Direction from the fragment to the active pass camera (Float3).",
				.Category = "Inputs", .FixedOutputType = EMaterialProgramValueType::Float3};
		}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionProceduralSkyRadiance : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionProceduralSkyRadiance(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::ProceduralSkyRadiance, .Name = "Procedural Sky Radiance",
				.Description = "Linear RGB radiance of the active analytic sky background. Returns black outside that background.",
				.Category = "Inputs", .FixedOutputType = EMaterialProgramValueType::Float3};
		}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionObjectPosition : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionObjectPosition(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::ObjectPosition, .Name = "Object Position",
				.Description = "Render primitive bounds center relative to the current view origin (Float3).",
				.Category = "Inputs", .FixedOutputType = EMaterialProgramValueType::Float3};
		}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionVertexNormal : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionVertexNormal(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::VertexNormal, .Name = "Vertex Normal",
				.Description = "Post-vertex-factory world normal for vertex offsets or explicit interpolation.",
				.Category = "Inputs", .FixedOutputType = EMaterialProgramValueType::Float3};
		}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionSceneTexture : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSceneTexture(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::SceneTexture, .Name = "Scene Texture",
				.Description = "PostProcess-only logical scene input. Coordinates are viewport UV.",
				.Category = "Scene", .FixedOutputType = EMaterialProgramValueType::Float4,
				.SearchKeywords = "scene postprocess"};
		}

		auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override
		{
			if (Index == 0) return {.Name = "Coordinates", .Type = EMaterialProgramValueType::Float2};
			return {};
		}
		DPROPERTY()
		FMaterialNumericInput Coordinates{2};
		DPROPERTY(Edit)
		EMaterialSceneTextureId SceneTextureId = EMaterialSceneTextureId::PostProcessInput0;
		DPROPERTY(Edit)
		bool bFiltered = true;
		auto GetAuthoredInputCount() const -> uint32 override { return 1; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	// Device depth, positive view depth, validity. No-geometry is (clear, 0, 0).
	DCLASS()
	class DMaterialExpressionSceneDepth : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSceneDepth(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::SceneDepth, .Name = "Scene Depth",
				.Description = "Point-sampled device depth, positive view depth and validity (Float3).",
				.Category = "Scene", .FixedOutputType = EMaterialProgramValueType::Float3,
				.SearchKeywords = "depth postprocess"};
		}

		auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override
		{
			if (Index == 0) return {.Name = "Coordinates", .Type = EMaterialProgramValueType::Float2};
			return {};
		}
		DPROPERTY()
		FMaterialNumericInput Coordinates{2};
		auto GetAuthoredInputCount() const -> uint32 override { return 1; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionScreenPosition : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionScreenPosition(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::ScreenPosition, .Name = "Screen Position",
				.Description = "Normalized position within the active pass viewport (Float2).",
				.Category = "Inputs", .FixedOutputType = EMaterialProgramValueType::Float2};
		}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionViewSize : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionViewSize(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::ViewSize, .Name = "View Size",
				.Description = "Active pass viewport size in pixels (Float2).",
				.Category = "Inputs", .FixedOutputType = EMaterialProgramValueType::Float2};
		}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};
}
