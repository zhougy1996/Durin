#pragma once

#include "TextureBuildAPI.h"
#include "Texture/Texture2DCompilationTypes.h"
#include "Texture/TextureBuildOperations.h"

namespace Durin::Asset
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

	struct FTexture2DCompilationRequest
	{
		// Detached worker input and derived-data policy.
		FTexture2DBuildRequest Build;
		// Main-thread asset publication and source provenance.
		FTexture2DPublicationContext Publication;
		ETexture2DCompilationPriority Priority = ETexture2DCompilationPriority::Background;
	};

	TEXTUREBUILD_API auto SubmitTexture2DCompilation(
		DTexture2D& Texture,
		FTexture2DCompilationRequest Request,
		std::string& OutError,
		FTexture2DCompilationCompletion Completion = {}) -> bool;
	TEXTUREBUILD_API auto GetTexture2DCompilationDiagnostic(const DTexture2D& Texture)
		-> FTexture2DCompilationDiagnostic;
	TEXTUREBUILD_API auto HasPendingTexture2DCompilation(const DTexture2D& Texture) -> bool;
	TEXTUREBUILD_API auto WaitForTexture2DCompilation(
		DTexture2D& Texture,
		double TimeoutSeconds = 300.0) -> bool;
}
