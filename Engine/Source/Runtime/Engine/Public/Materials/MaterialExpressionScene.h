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
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionTime : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionTime(const FObjectInitializer& Initializer) : Super(Initializer) {}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionCameraPosition : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionCameraPosition(const FObjectInitializer& Initializer) : Super(Initializer) {}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionCameraVector : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionCameraVector(const FObjectInitializer& Initializer) : Super(Initializer) {}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionProceduralSkyRadiance : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionProceduralSkyRadiance(const FObjectInitializer& Initializer) : Super(Initializer) {}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionObjectPosition : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionObjectPosition(const FObjectInitializer& Initializer) : Super(Initializer) {}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionVertexNormal : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionVertexNormal(const FObjectInitializer& Initializer) : Super(Initializer) {}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionSceneTexture : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionSceneTexture(const FObjectInitializer& Initializer) : Super(Initializer) {}
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
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};

	DCLASS()
	class DMaterialExpressionViewSize : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionViewSize(const FObjectInitializer& Initializer) : Super(Initializer) {}
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};
}
