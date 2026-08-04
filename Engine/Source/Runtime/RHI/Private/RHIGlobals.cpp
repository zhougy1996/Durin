#include "RHIGlobals.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RHIThread.h"

namespace Durin
{
	static std::unique_ptr<FRHIThread> RHIThreadOwner;

	namespace
	{
		auto UseThreadedRHIExecution() -> bool
		{
			return ResolveRHIExecutionMode(std::getenv("DURIN_RHI_EXECUTION"))
				== ERHIExecutionMode::Threaded;
		}

		auto CreateDynamicRHI() -> FDynamicRHI*
		{
			IDynamicRHIModule* DynamicRHIModule =
				FModuleManager::LoadModule<IDynamicRHIModule>("VulkanRHI");
			if (!DynamicRHIModule)
			{
				DURIN_ERROR("Failed to load VulkanRHI module");
				return nullptr;
			}
			return DynamicRHIModule->CreateRHI();
		}

		auto RollbackBackendInitialization() -> std::string
		{
			try
			{
				GDynamicRHI->Shutdown();
				return {};
			}
			catch (const std::exception& Exception)
			{
				return Exception.what();
			}
			catch (...)
			{
				return "unknown backend shutdown exception";
			}
		}

		auto InitializeBackendWithRollback() -> FRHIThreadWorkResult
		{
			try
			{
				GDynamicRHI->Init();
				return FRHIThreadWorkResult::Success();
			}
			catch (const std::exception& Exception)
			{
				std::string Diagnostic = Exception.what();
				const std::string RollbackDiagnostic =
					RollbackBackendInitialization();
				if (!RollbackDiagnostic.empty())
				{
					Diagnostic += "; backend rollback failed: ";
					Diagnostic += RollbackDiagnostic;
				}
				return FRHIThreadWorkResult::Failure(std::move(Diagnostic));
			}
			catch (...)
			{
				std::string Diagnostic =
					"Dynamic RHI initialization failed with an unknown exception.";
				const std::string RollbackDiagnostic =
					RollbackBackendInitialization();
				if (!RollbackDiagnostic.empty())
				{
					Diagnostic += " Backend rollback failed: ";
					Diagnostic += RollbackDiagnostic;
				}
				return FRHIThreadWorkResult::Failure(std::move(Diagnostic));
			}
		}

		auto ReleaseFailedInitialization() -> void
		{
			if (RHIThreadOwner)
			{
				RHIThreadOwner->Stop();
				RHIThreadOwner.reset();
			}
			delete GDynamicRHI;
			GDynamicRHI = nullptr;
		}

		auto InitializeRHI(
			FDynamicRHI* Backend,
			bool bThreaded,
			bool bForceThreadLaunchFailure) -> bool
		{
			if (!Backend)
			{
				DURIN_ERROR("Failed to create dynamic RHI");
				return false;
			}
			if (GDynamicRHI || RHIThreadOwner)
			{
				DURIN_ERROR("Cannot initialize RHI more than once.");
				delete Backend;
				return false;
			}

			GDynamicRHI = Backend;
			if (bThreaded)
			{
				RHIThreadOwner = std::make_unique<FRHIThread>();
				if (bForceThreadLaunchFailure || !RHIThreadOwner->Start())
				{
					DURIN_ERROR("Failed to start RHI thread");
					ReleaseFailedInitialization();
					return false;
				}
				FRHIThreadWork InitWork;
				InitWork.Execute = []() {
					return InitializeBackendWithRollback();
				};
				const FRHIThreadSynchronousResult InitResult =
					RHIThreadOwner->EnqueueSynchronous(InitWork);
				if (!InitResult.IsCompleted())
				{
					const std::string Diagnostic =
						RHIThreadOwner->GetStats().FailureDiagnostic;
					DURIN_ERROR(
						"Failed to initialize dynamic RHI on RHI thread: {}",
						Diagnostic);
					ReleaseFailedInitialization();
					return false;
				}
				GCommandListExecutor.SetThreadedMode(*RHIThreadOwner);
				DURIN_DEBUG("RHI execution mode: threaded");
			}
			else
			{
				const FRHIThreadWorkResult InitResult =
					InitializeBackendWithRollback();
				if (!InitResult.bSucceeded)
				{
					DURIN_ERROR(
						"Failed to initialize dynamic RHI inline: {}",
						InitResult.Diagnostic);
					ReleaseFailedInitialization();
					return false;
				}
				DURIN_DEBUG("RHI execution mode: inline");
			}

			// The command list exists before the backend; bind its default pipeline
			// only after the context is valid.
			FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::Graphics);
			DURIN_DEBUG("RHI initialized successfully");
			return true;
		}
	}

	auto ResolveRHIExecutionMode(const char* ConfiguredMode)
		-> ERHIExecutionMode
	{
		if (!ConfiguredMode)
		{
			return ERHIExecutionMode::Inline;
		}

		const std::string_view Mode(ConfiguredMode);
		if (Mode == "threaded")
		{
			return ERHIExecutionMode::Threaded;
		}
		if (Mode != "inline")
		{
			DURIN_ERROR(
				"Invalid DURIN_RHI_EXECUTION value '{}'; expected 'inline' or 'threaded'. Using inline mode.",
				Mode);
		}
		return ERHIExecutionMode::Inline;
	}

	auto RHIInit() -> bool
	{
		return InitializeRHI(
			CreateDynamicRHI(), UseThreadedRHIExecution(), false);
	}

	auto RHIInitWithBackendForTests(
		FDynamicRHI* Backend,
		bool bThreaded,
		bool bForceThreadLaunchFailure) -> bool
	{
		return InitializeRHI(
			Backend, bThreaded, bForceThreadLaunchFailure);
	}

	auto RHIExit() -> void
	{
		check(GDynamicRHI);
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		FRHICommandListImmediate::Get().ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
		if (RHIThreadOwner)
		{
			FRHIThreadWork ShutdownWork;
			ShutdownWork.Execute = []() {
				GDynamicRHI->Shutdown();
				return FRHIThreadWorkResult::Success();
			};
			const FRHIThreadSynchronousResult ShutdownResult =
				RHIThreadOwner->EnqueueSynchronous(ShutdownWork);
			checkf(ShutdownResult.IsCompleted(),
				"Dynamic RHI shutdown failed on RHI thread.");
			GCommandListExecutor.SetInlineMode();
			RHIThreadOwner->BeginDrain();
			RHIThreadOwner->Stop();
			RHIThreadOwner.reset();
		}
		else
		{
			GDynamicRHI->Shutdown();
		}
		delete GDynamicRHI;
		GDynamicRHI = nullptr;
	}
}
