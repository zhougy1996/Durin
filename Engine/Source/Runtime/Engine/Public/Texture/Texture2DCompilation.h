#pragma once

#include "Texture/Texture2DCompilationTypes.h"
#include "Texture/Texture2DBuildProvider.h"

namespace Durin
{
	// Identifies the terminal outcome of one accepted object-level compilation.
	enum class ETexture2DCompilationStatus : uint8
	{
		Succeeded,
		Failed,
		Canceled,
		Superseded
	};

	struct FTexture2DCompilationResult
	{
		ETexture2DCompilationStatus Status = ETexture2DCompilationStatus::Failed;
		std::string Diagnostic;

		auto Succeeded() const -> bool
		{
			return Status == ETexture2DCompilationStatus::Succeeded;
		}
	};

	// Accepted requests invoke their completion exactly once on the GameThread.
	using FTexture2DCompilationCompletion = std::function<void(FTexture2DCompilationResult)>;

	struct FTexture2DResultApplicationContext
	{
		bool bMarkPackageDirty = true;
		bool bReportLoadMutation = false;
		bool bSourceDecoderInvoked = true;
	};

	struct FTexture2DCompilationRequest
	{
		// Detached worker input and derived-data policy.
		FTexture2DBuildRequest Build;
		// Main-thread result application and source provenance.
		FTexture2DResultApplicationContext ResultApplication;
		ETexture2DCompilationPriority Priority = ETexture2DCompilationPriority::Background;
	};

	ENGINE_API auto SubmitTexture2DCompilation(
		DTexture2D& Texture,
		FTexture2DCompilationRequest Request,
		std::string& OutError,
		FTexture2DCompilationCompletion Completion = {}) -> bool;
	ENGINE_API auto GetTexture2DCompilationDiagnostic(const DTexture2D& Texture)
		-> FTexture2DCompilationDiagnostic;
	ENGINE_API auto GetTexture2DCompilationManagerDiagnostics()
		-> FTexture2DCompilationManagerDiagnostics;
	ENGINE_API auto HasPendingTexture2DCompilation(const DTexture2D& Texture) -> bool;
	ENGINE_API auto WaitForTexture2DCompilation(
		DTexture2D& Texture,
		double TimeoutSeconds = 300.0) -> bool;
	ENGINE_API auto BuildTexture2DSynchronously(
		DTexture2D& Texture,
		FTexture2DBuildRequest Request,
		const FTexture2DResultApplicationContext& Context,
		std::string& OutError) -> bool;
}

namespace Durin::AssetPrivate
{
	ENGINE_API auto SetTexture2DCompilationPhaseHookForTests(
		std::function<void(uint64, ETexture2DCompilationPhase)> Hook) -> void;
}
