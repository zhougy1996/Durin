#pragma once

#include "CoreMinimal.h"
#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "Texture/Texture2DCompilationTypes.h"

namespace Durin
{
	// Records wall-clock build timing and conservative versus observed retained bytes.
	struct FTexture2DCompilationMetrics
	{
		// Dispatch start to build completion, including asynchronous cache I/O.
		uint64 BuildElapsedNanoseconds = 0;
		uint64 CompletionNanoseconds = 0;
		uint64 EstimatedBytes = 0;
		uint64 ResultBytes = 0;
	};


	namespace DerivedData { class ICache; }
	class DTexture2D;

	namespace AssetPrivate
	{
		ENGINE_API auto SetTexture2DCompilationPhaseHookForTests(
			std::function<void(uint64, ETexture2DCompilationPhase)> Hook) -> void;
		// Shared Texture2D/Cube session. Replace only while quiescent; null restores production.
		ENGINE_API auto SetTextureAsyncCacheForTests(std::shared_ptr<DerivedData::ICache> Cache) -> void;
		// Bounded internal snapshot for qualification and regression tests.
		struct FTexture2DCompilationDiagnostic
		{
			uint64 RequestId = 0;
			std::string AssetIdentity;
			std::string Diagnostic;
			FTexture2DCompilationMetrics Metrics;
			uint64 QueuedNanoseconds = 0;
			ETexture2DCompilationPhase Phase = ETexture2DCompilationPhase::None;
		};

		ENGINE_API auto GetTexture2DCompilationDiagnosticForTests(const DTexture2D& Texture)
			-> FTexture2DCompilationDiagnostic;
	}
}

#endif
