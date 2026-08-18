#include "RHIGlobals.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RHIThread.h"

namespace Durin
{
	static std::unique_ptr<FRHIThread> RHIThreadOwner;
	static std::string LastRHIInitializationDiagnostic;
	static bool GOwnsBackendModule = false;

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

		auto InitializeBackendWithRollback(
			const FRHIInitializationContext& Context) -> FRHIThreadWorkResult
		{
			try
			{
				GDynamicRHI->Init(Context);
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

		auto ReleaseFailedInitialization(bool bUnloadBackendModule) -> void
		{
			if (RHIThreadOwner)
			{
				RHIThreadOwner->Stop();
				RHIThreadOwner.reset();
			}
			delete GDynamicRHI;
			GDynamicRHI = nullptr;
			if (bUnloadBackendModule)
			{
				const auto Result = FModuleManager::Get().UnloadModule("VulkanRHI");
				if (!Result.Succeeded()) DURIN_ERROR(STR("Failed to unload VulkanRHI after initialization failure: {}"), Result.Message);
			}
			GOwnsBackendModule = false;
		}

		auto InitializeRHI(
			FDynamicRHI* Backend,
			bool bThreaded,
			bool bForceThreadLaunchFailure,
			bool bOwnsBackendModule,
			FRHIInitializationContext Context) -> bool
		{
			LastRHIInitializationDiagnostic.clear();
			if (!Backend)
			{
				LastRHIInitializationDiagnostic =
					"Failed to create dynamic RHI.";
				DURIN_ERROR("Failed to create dynamic RHI");
				if (bOwnsBackendModule)
				{
					const auto Result = FModuleManager::Get().UnloadModule("VulkanRHI");
					if (!Result.Succeeded()) DURIN_ERROR(STR("Failed to unload VulkanRHI after backend creation failure: {}"), Result.Message);
				}
				return false;
			}
			if (GDynamicRHI || RHIThreadOwner)
			{
				DURIN_ERROR("Cannot initialize RHI more than once.");
				delete Backend;
				return false;
			}

			GDynamicRHI = Backend;
			GOwnsBackendModule = bOwnsBackendModule;
			if (bThreaded)
			{
				RHIThreadOwner = std::make_unique<FRHIThread>();
				if (bForceThreadLaunchFailure || !RHIThreadOwner->Start())
				{
					LastRHIInitializationDiagnostic =
						"Failed to start RHI thread.";
					DURIN_ERROR("Failed to start RHI thread");
					ReleaseFailedInitialization(bOwnsBackendModule);
					return false;
				}
				FRHIThreadWork InitWork;
				InitWork.Execute = [Context]() {
					return InitializeBackendWithRollback(Context);
				};
				const FRHIThreadSynchronousResult InitResult =
					RHIThreadOwner->EnqueueSynchronous(InitWork);
				if (!InitResult.IsCompleted())
				{
					const std::string Diagnostic =
						RHIThreadOwner->GetStats().FailureDiagnostic;
					LastRHIInitializationDiagnostic = Diagnostic;
					DURIN_ERROR(
						"Failed to initialize dynamic RHI on RHI thread: {}",
						Diagnostic);
					ReleaseFailedInitialization(bOwnsBackendModule);
					return false;
				}
				GCommandListExecutor.SetThreadedMode(*RHIThreadOwner);
				DURIN_DEBUG("RHI execution mode: threaded");
			}
			else
			{
				const FRHIThreadWorkResult InitResult =
					InitializeBackendWithRollback(Context);
				if (!InitResult.bSucceeded)
				{
					LastRHIInitializationDiagnostic = InitResult.Diagnostic;
					DURIN_ERROR(
						"Failed to initialize dynamic RHI inline: {}",
						InitResult.Diagnostic);
					ReleaseFailedInitialization(bOwnsBackendModule);
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
			return ERHIExecutionMode::Threaded;
		}

		const std::string_view Mode(ConfiguredMode);
		if (Mode == "inline")
		{
			return ERHIExecutionMode::Inline;
		}
		if (Mode != "threaded")
		{
			DURIN_ERROR(
				"Invalid DURIN_RHI_EXECUTION value '{}'; expected 'inline' or 'threaded'. Using threaded mode.",
				Mode);
		}
		return ERHIExecutionMode::Threaded;
	}

	auto RHIInit(FRHIInitializationContext Context) -> bool
	{
		return InitializeRHI(
			CreateDynamicRHI(), UseThreadedRHIExecution(), false, true,
			std::move(Context));
	}

	auto GetLastRHIInitializationDiagnostic() -> std::string_view
	{
		return LastRHIInitializationDiagnostic;
	}

	auto RHIInitWithBackendForTests(
		FDynamicRHI* Backend,
		bool bThreaded,
		bool bForceThreadLaunchFailure,
		FRHIInitializationContext Context) -> bool
	{
		return InitializeRHI(
			Backend, bThreaded, bForceThreadLaunchFailure, false,
			std::move(Context));
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
				RHIFlushDeferredResources();
				checkf(FRHIResource::GetNumPendingDeletes() == 0,
					"RHI shutdown marker found pending RHI resource deletions.");
				GDynamicRHI->Shutdown();
				return FRHIThreadWorkResult::Success();
			};
			const FRHIThreadSubmission ShutdownSubmission =
				RHIThreadOwner->EnqueueTerminal(ShutdownWork);
			if (!ShutdownSubmission.IsAccepted())
			{
				DURIN_FATAL(
					"Failed to atomically install the dynamic RHI shutdown marker ({}).",
					static_cast<uint32>(ShutdownSubmission.Result));
				std::terminate();
			}
			const ERHIThreadWaitResult ShutdownWaitResult =
				RHIThreadOwner->WaitForSerial(ShutdownSubmission.Serial);
			if (ShutdownWaitResult != ERHIThreadWaitResult::Completed)
			{
				DURIN_FATAL(
					"Dynamic RHI shutdown marker failed on RHI thread ({}).",
					static_cast<uint32>(ShutdownWaitResult));
				std::terminate();
			}
			GCommandListExecutor.SetInlineMode();
			RHIThreadOwner->Stop();
			const FRHIThreadStats ShutdownStats = RHIThreadOwner->GetStats();
			check(ShutdownStats.AdmissionState
				== ERHIThreadAdmissionState::Stopped);
			check(ShutdownStats.OutstandingEntryCount == 0);
			check(ShutdownStats.OutstandingBatchCount == 0);
			check(ShutdownStats.OutstandingPayloadBytes == 0);
			RHIThreadOwner.reset();
		}
		else
		{
			GDynamicRHI->Shutdown();
		}
		delete GDynamicRHI;
		GDynamicRHI = nullptr;
		if (std::exchange(GOwnsBackendModule, false))
		{
			const auto Result = FModuleManager::Get().UnloadModule("VulkanRHI");
			checkf(Result.Succeeded(),
				"VulkanRHI must unload after its backend and RHI thread are destroyed: {}",
				Result.Message);
		}
	}
}
