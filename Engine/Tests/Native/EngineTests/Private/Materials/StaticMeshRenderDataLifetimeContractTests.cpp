#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace Durin
{
	namespace
	{
		enum class EStaticMeshConsumerKind
		{
			Component,
			MaterialThumbnail,
			TextureCubePreview,
			AuxiliaryScene
		};

		enum class EStaticMeshLifetimeEvent
		{
			Draw,
			CandidateInit,
			Detach,
			Release,
			Fence,
			Destroy,
			Attach
		};

		struct FStaticMeshAggregateObservation
		{
			std::uint64_t Revision = 0;
			bool bResourcesInitialized = false;
			bool bResourcesReleased = false;
			bool bFenceComplete = false;
			bool bDestroyed = false;
			bool bUseObservedAfterDestroy = false;
			std::vector<EStaticMeshLifetimeEvent> Events;
		};

		class FObservedStaticMeshAggregate
		{
		public:
			explicit FObservedStaticMeshAggregate(
				std::shared_ptr<FStaticMeshAggregateObservation> InObservation)
				: Observation(std::move(InObservation))
			{
			}

			~FObservedStaticMeshAggregate()
			{
				Observation->bDestroyed = true;
				Observation->Events.push_back(EStaticMeshLifetimeEvent::Destroy);
			}

			auto InitResources(bool bHasRHI) -> void
			{
				EXPECT_FALSE(Observation->bDestroyed);
				Observation->bResourcesInitialized = bHasRHI;
				Observation->Events.push_back(EStaticMeshLifetimeEvent::CandidateInit);
			}

			auto ReleaseResources() -> void
			{
				EXPECT_FALSE(Observation->bDestroyed);
				Observation->bResourcesReleased = true;
				Observation->Events.push_back(EStaticMeshLifetimeEvent::Release);
			}

			auto CompleteFence() -> void
			{
				EXPECT_TRUE(Observation->bResourcesReleased);
				Observation->bFenceComplete = true;
				Observation->Events.push_back(EStaticMeshLifetimeEvent::Fence);
			}

			auto GetObservation() const
				-> const std::shared_ptr<FStaticMeshAggregateObservation>&
			{
				return Observation;
			}

		private:
			std::shared_ptr<FStaticMeshAggregateObservation> Observation;
		};

		struct FRegisteredStaticMeshConsumer
		{
			EStaticMeshConsumerKind Kind = EStaticMeshConsumerKind::Component;
			std::uint64_t Revision = 0;
			bool bAttached = true;
			bool bRegistered = true;
			bool bGarbageMarked = false;
			bool bReassigned = false;
		};

		class FDeterministicStaticMeshScheduler
		{
		public:
			using FCommand = std::function<void()>;

			auto Enqueue(FCommand Command) -> void
			{
				Commands.push_back(std::move(Command));
			}

			auto RunNext() -> void
			{
				ASSERT_LT(NextCommand, Commands.size());
				FCommand Command = std::move(Commands[NextCommand++]);
				Command();
			}

			auto Drain() -> void
			{
				while (NextCommand < Commands.size()) RunNext();
			}

			auto NumPendingCommands() const -> size_t
			{
				return Commands.size() - NextCommand;
			}

		private:
			std::vector<FCommand> Commands;
			size_t NextCommand = 0;
		};

		class FStaticMeshLifetimeModel
		{
		public:
			explicit FStaticMeshLifetimeModel(bool bInHasRHI = true)
				: bHasRHI(bInHasRHI)
			{
				Consumers.reserve(8);
				Current = MakeAggregate();
				Current->InitResources(bHasRHI);
				CurrentObservation = Current->GetObservation();
				CurrentObservation->Events.clear();
			}

			auto RegisterConsumer(EStaticMeshConsumerKind Kind)
				-> FRegisteredStaticMeshConsumer&
			{
				return Consumers.emplace_back(FRegisteredStaticMeshConsumer{
					.Kind = Kind,
					.Revision = CurrentObservation->Revision});
			}

			auto QueueDraw(FDeterministicStaticMeshScheduler& Scheduler) -> void
			{
				const std::shared_ptr Observation = CurrentObservation;
				Scheduler.Enqueue([Observation] {
					Observation->Events.push_back(EStaticMeshLifetimeEvent::Draw);
					if (Observation->bDestroyed)
						Observation->bUseObservedAfterDestroy = true;
				});
			}

			auto ReplaceLikeBaseline(FDeterministicStaticMeshScheduler& Scheduler)
				-> std::shared_ptr<FStaticMeshAggregateObservation>
			{
				const std::shared_ptr Retired = CurrentObservation;
				Current = MakeAggregate();
				CurrentObservation = Current->GetObservation();
				Scheduler.Enqueue([this, Retired] {
					for (FRegisteredStaticMeshConsumer& Consumer : Consumers)
					{
						if (Consumer.Revision != Retired->Revision) continue;
						Consumer.bAttached = false;
						Retired->Events.push_back(EStaticMeshLifetimeEvent::Detach);
					}
				});
				return Retired;
			}

			auto BeginReplacement(FDeterministicStaticMeshScheduler& Scheduler)
				-> std::shared_ptr<FStaticMeshAggregateObservation>
			{
				auto Candidate = MakeAggregate();
				FObservedStaticMeshAggregate* CandidatePointer = Candidate.get();
				const std::shared_ptr CandidateObservation =
					Candidate->GetObservation();
				FObservedStaticMeshAggregate* RetiredPointer = Current.get();
				const std::shared_ptr RetiredObservation = CurrentObservation;
				Retirements.push_back(std::move(Current));
				Current = std::move(Candidate);
				CurrentObservation = CandidateObservation;

				Scheduler.Enqueue([CandidatePointer, this] {
					CandidatePointer->InitResources(bHasRHI);
				});
				Scheduler.Enqueue([this, RetiredObservation] {
					for (FRegisteredStaticMeshConsumer& Consumer : Consumers)
					{
						if (!Consumer.bAttached
							|| Consumer.Revision != RetiredObservation->Revision
							|| Consumer.bReassigned)
						{
							continue;
						}
						Consumer.bAttached = false;
						RetiredObservation->Events.push_back(
							EStaticMeshLifetimeEvent::Detach);
					}
				});
				Scheduler.Enqueue([RetiredPointer] {
					RetiredPointer->ReleaseResources();
				});
				Scheduler.Enqueue([RetiredPointer] {
					RetiredPointer->CompleteFence();
				});
				Scheduler.Enqueue([this, RetiredPointer] {
					const auto It = std::ranges::find_if(
						Retirements,
						[RetiredPointer](
							const std::unique_ptr<FObservedStaticMeshAggregate>&
								Retirement) {
							return Retirement.get() == RetiredPointer;
						});
					ASSERT_NE(It, Retirements.end());
					Retirements.erase(It);
				});
				Scheduler.Enqueue([this, CandidateObservation] {
					for (FRegisteredStaticMeshConsumer& Consumer : Consumers)
					{
						if (!Consumer.bRegistered || Consumer.bGarbageMarked
							|| Consumer.bReassigned)
						{
							continue;
						}
						Consumer.Revision = CandidateObservation->Revision;
						Consumer.bAttached = true;
						CandidateObservation->Events.push_back(
							EStaticMeshLifetimeEvent::Attach);
					}
				});
				return RetiredObservation;
			}

			auto BeginDestroy(FDeterministicStaticMeshScheduler& Scheduler)
				-> std::shared_ptr<FStaticMeshAggregateObservation>
			{
				bAdmissionOpen = false;
				FObservedStaticMeshAggregate* RetiredPointer = Current.get();
				const std::shared_ptr RetiredObservation = CurrentObservation;
				Retirements.push_back(std::move(Current));
				CurrentObservation.reset();

				Scheduler.Enqueue([this, RetiredObservation] {
					for (FRegisteredStaticMeshConsumer& Consumer : Consumers)
					{
						if (!Consumer.bAttached
							|| Consumer.Revision != RetiredObservation->Revision)
						{
							continue;
						}
						Consumer.bAttached = false;
						RetiredObservation->Events.push_back(
							EStaticMeshLifetimeEvent::Detach);
					}
				});
				Scheduler.Enqueue([RetiredPointer] {
					RetiredPointer->ReleaseResources();
				});
				Scheduler.Enqueue([RetiredPointer] {
					RetiredPointer->CompleteFence();
				});
				Scheduler.Enqueue([this, RetiredPointer] {
					const auto It = std::ranges::find_if(
						Retirements,
						[RetiredPointer](
							const std::unique_ptr<FObservedStaticMeshAggregate>&
								Retirement) {
							return Retirement.get() == RetiredPointer;
						});
					ASSERT_NE(It, Retirements.end());
					Retirements.erase(It);
				});
				return RetiredObservation;
			}

			auto IsReadyForFinishDestroy() const -> bool
			{
				return !bAdmissionOpen && Current == nullptr
					&& Retirements.empty();
			}

			auto NumRetirements() const -> size_t
			{
				return Retirements.size();
			}

			auto GetCurrentObservation() const
				-> const std::shared_ptr<FStaticMeshAggregateObservation>&
			{
				return CurrentObservation;
			}

		private:
			auto MakeAggregate() -> std::unique_ptr<FObservedStaticMeshAggregate>
			{
				auto Observation =
					std::make_shared<FStaticMeshAggregateObservation>();
				Observation->Revision = NextRevision++;
				return std::make_unique<FObservedStaticMeshAggregate>(
					std::move(Observation));
			}

			bool bHasRHI = true;
			bool bAdmissionOpen = true;
			std::uint64_t NextRevision = 1;
			std::unique_ptr<FObservedStaticMeshAggregate> Current;
			std::shared_ptr<FStaticMeshAggregateObservation> CurrentObservation;
			std::vector<std::unique_ptr<FObservedStaticMeshAggregate>>
				Retirements;
			std::vector<FRegisteredStaticMeshConsumer> Consumers;
		};
	}

	TEST(FStaticMeshRenderDataLifetimeContractTests,
		BaselineReplacementDetectsAcceptedDrawAfterImmediateDestroy)
	{
		FDeterministicStaticMeshScheduler Scheduler;
		FStaticMeshLifetimeModel Asset;
		Asset.RegisterConsumer(EStaticMeshConsumerKind::Component);
		Asset.QueueDraw(Scheduler);

		const std::shared_ptr Retired = Asset.ReplaceLikeBaseline(Scheduler);
		EXPECT_TRUE(Retired->bDestroyed);

		Scheduler.RunNext();
		EXPECT_TRUE(Retired->bUseObservedAfterDestroy);
		EXPECT_EQ(Retired->Events, (std::vector{
			EStaticMeshLifetimeEvent::Destroy,
			EStaticMeshLifetimeEvent::Draw}));
	}

	TEST(FStaticMeshRenderDataLifetimeContractTests,
		ReplacementPausesAtDrawDetachReleaseFenceAndDestroyBoundaries)
	{
		FDeterministicStaticMeshScheduler Scheduler;
		FStaticMeshLifetimeModel Asset;
		for (const EStaticMeshConsumerKind Kind : {
			EStaticMeshConsumerKind::Component,
			EStaticMeshConsumerKind::MaterialThumbnail,
			EStaticMeshConsumerKind::TextureCubePreview,
			EStaticMeshConsumerKind::AuxiliaryScene})
		{
			Asset.RegisterConsumer(Kind);
		}
		Asset.QueueDraw(Scheduler);
		const std::shared_ptr Retired = Asset.BeginReplacement(Scheduler);
		const std::shared_ptr Candidate = Asset.GetCurrentObservation();
		EXPECT_EQ(Asset.NumRetirements(), 1u);

		Scheduler.RunNext();
		EXPECT_EQ(Retired->Events, (std::vector{
			EStaticMeshLifetimeEvent::Draw}));
		EXPECT_FALSE(Retired->bDestroyed);

		Scheduler.RunNext();
		EXPECT_TRUE(Candidate->bResourcesInitialized);
		EXPECT_FALSE(Retired->bResourcesReleased);

		Scheduler.RunNext();
		EXPECT_EQ(std::ranges::count(
			Retired->Events, EStaticMeshLifetimeEvent::Detach), 4);
		EXPECT_FALSE(Retired->bResourcesReleased);

		Scheduler.RunNext();
		EXPECT_TRUE(Retired->bResourcesReleased);
		EXPECT_FALSE(Retired->bFenceComplete);
		EXPECT_FALSE(Retired->bDestroyed);

		Scheduler.RunNext();
		EXPECT_TRUE(Retired->bFenceComplete);
		EXPECT_FALSE(Retired->bDestroyed);

		Scheduler.RunNext();
		EXPECT_TRUE(Retired->bDestroyed);
		EXPECT_EQ(Asset.NumRetirements(), 0u);

		Scheduler.RunNext();
		EXPECT_EQ(std::ranges::count(
			Candidate->Events, EStaticMeshLifetimeEvent::Attach), 4);
	}

	TEST(FStaticMeshRenderDataLifetimeContractTests,
		DestroyClosesAdmissionAndDefersReadinessThroughFence)
	{
		FDeterministicStaticMeshScheduler Scheduler;
		FStaticMeshLifetimeModel Asset;
		Asset.RegisterConsumer(EStaticMeshConsumerKind::Component);
		Asset.RegisterConsumer(EStaticMeshConsumerKind::MaterialThumbnail);

		const std::shared_ptr Retired = Asset.BeginDestroy(Scheduler);
		EXPECT_FALSE(Asset.IsReadyForFinishDestroy());
		EXPECT_EQ(Asset.NumRetirements(), 1u);

		Scheduler.RunNext();
		EXPECT_EQ(std::ranges::count(
			Retired->Events, EStaticMeshLifetimeEvent::Detach), 2);
		EXPECT_FALSE(Asset.IsReadyForFinishDestroy());
		Scheduler.RunNext();
		EXPECT_TRUE(Retired->bResourcesReleased);
		EXPECT_FALSE(Asset.IsReadyForFinishDestroy());
		Scheduler.RunNext();
		EXPECT_TRUE(Retired->bFenceComplete);
		EXPECT_FALSE(Asset.IsReadyForFinishDestroy());
		Scheduler.RunNext();
		EXPECT_TRUE(Retired->bDestroyed);
		EXPECT_TRUE(Asset.IsReadyForFinishDestroy());
	}

	TEST(FStaticMeshRenderDataLifetimeContractTests,
		RapidReplacementKeepsEveryAggregateUntilItsOwnFence)
	{
		FDeterministicStaticMeshScheduler Scheduler;
		FStaticMeshLifetimeModel Asset;
		Asset.RegisterConsumer(EStaticMeshConsumerKind::Component);

		const std::shared_ptr FirstRetired = Asset.BeginReplacement(Scheduler);
		const std::shared_ptr SecondRetired = Asset.BeginReplacement(Scheduler);
		const std::shared_ptr Current = Asset.GetCurrentObservation();
		EXPECT_NE(FirstRetired->Revision, SecondRetired->Revision);
		EXPECT_NE(SecondRetired->Revision, Current->Revision);
		EXPECT_EQ(Asset.NumRetirements(), 2u);

		Scheduler.Drain();
		EXPECT_TRUE(FirstRetired->bFenceComplete);
		EXPECT_TRUE(FirstRetired->bDestroyed);
		EXPECT_TRUE(SecondRetired->bFenceComplete);
		EXPECT_TRUE(SecondRetired->bDestroyed);
		EXPECT_EQ(Asset.NumRetirements(), 0u);
		EXPECT_EQ(std::ranges::count(
			Current->Events, EStaticMeshLifetimeEvent::Attach), 1);
	}

	TEST(FStaticMeshRenderDataLifetimeContractTests,
		RecreateSkipsUnregisteredGarbageAndReassignedConsumers)
	{
		FDeterministicStaticMeshScheduler Scheduler;
		FStaticMeshLifetimeModel Asset;
		auto& Registered =
			Asset.RegisterConsumer(EStaticMeshConsumerKind::Component);
		auto& Unregistered =
			Asset.RegisterConsumer(EStaticMeshConsumerKind::Component);
		auto& Garbage =
			Asset.RegisterConsumer(EStaticMeshConsumerKind::MaterialThumbnail);
		auto& Reassigned =
			Asset.RegisterConsumer(EStaticMeshConsumerKind::AuxiliaryScene);
		Unregistered.bRegistered = false;
		Unregistered.bAttached = false;
		Garbage.bGarbageMarked = true;
		Reassigned.bReassigned = true;
		Reassigned.Revision = 999;

		const std::shared_ptr Retired = Asset.BeginReplacement(Scheduler);
		const std::shared_ptr Candidate = Asset.GetCurrentObservation();
		Scheduler.Drain();

		EXPECT_TRUE(Registered.bAttached);
		EXPECT_FALSE(Unregistered.bAttached);
		EXPECT_FALSE(Garbage.bAttached);
		EXPECT_TRUE(Reassigned.bAttached);
		EXPECT_EQ(std::ranges::count(
			Retired->Events, EStaticMeshLifetimeEvent::Detach), 2);
		EXPECT_EQ(std::ranges::count(
			Candidate->Events, EStaticMeshLifetimeEvent::Attach), 1);
	}

	TEST(FStaticMeshRenderDataLifetimeContractTests,
		NoRHIDestroyStillDetachesFencesAndRetires)
	{
		FDeterministicStaticMeshScheduler Scheduler;
		FStaticMeshLifetimeModel Asset(false);
		Asset.RegisterConsumer(EStaticMeshConsumerKind::TextureCubePreview);
		const std::shared_ptr Retired = Asset.BeginDestroy(Scheduler);
		EXPECT_FALSE(Retired->bResourcesInitialized);

		Scheduler.Drain();
		EXPECT_TRUE(Retired->bResourcesReleased);
		EXPECT_TRUE(Retired->bFenceComplete);
		EXPECT_TRUE(Retired->bDestroyed);
		EXPECT_TRUE(Asset.IsReadyForFinishDestroy());
		EXPECT_EQ(Scheduler.NumPendingCommands(), 0u);
	}
}
