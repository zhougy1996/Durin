#include <gtest/gtest.h>

namespace Durin
{
	namespace
	{
		enum class ETextureKind
		{
			Texture2D,
			TextureCube,
		};

		struct FResourceObservation
		{
			ETextureKind Kind = ETextureKind::Texture2D;
			bool bInitialized = false;
			bool bReleased = false;
			bool bDestroyed = false;
		};

		struct FStableTextureState
		{
			int Target = 0;
		};

		// Models one uniquely owned concrete texture resource without granting
		// ownership to commands or consumers.
		class FConcreteTextureResource
		{
		public:
			FConcreteTextureResource(int InIdentity,
				std::shared_ptr<FResourceObservation> InObservation)
				: Identity(InIdentity)
				, Observation(std::move(InObservation))
			{
			}

			~FConcreteTextureResource()
			{
				Observation->bDestroyed = true;
			}

			auto Init() -> void
			{
				EXPECT_FALSE(Observation->bInitialized);
				EXPECT_FALSE(Observation->bReleased);
				Observation->bInitialized = true;
			}

			auto Release() -> void
			{
				EXPECT_TRUE(Observation->bInitialized);
				EXPECT_FALSE(Observation->bReleased);
				Observation->bReleased = true;
			}

			auto GetIdentity() const -> int { return Identity; }

		private:
			int Identity = 0;
			std::shared_ptr<FResourceObservation> Observation;
		};

		// Executes accepted lifecycle commands one at a time and owns concrete
		// resources between producer release and render-thread retirement.
		class FDeterministicRenderScheduler
		{
		public:
			using FCommand = std::function<void()>;

			auto Enqueue(FCommand Command) -> void
			{
				Commands.push_back(std::move(Command));
			}

			auto AdoptForDeferredCleanup(
				std::unique_ptr<FConcreteTextureResource> Resource)
				-> FConcreteTextureResource*
			{
				FConcreteTextureResource* RawResource = Resource.get();
				DeferredResources.push_back(std::move(Resource));
				return RawResource;
			}

			auto Retire(FConcreteTextureResource* Resource) -> void
			{
				const auto It = std::ranges::find_if(
					DeferredResources,
					[Resource](const std::unique_ptr<FConcreteTextureResource>& Candidate) {
						return Candidate.get() == Resource;
					});
				ASSERT_NE(It, DeferredResources.end());
				DeferredResources.erase(It);
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

			auto NumDeferredResources() const -> size_t
			{
				return DeferredResources.size();
			}

		private:
			std::vector<FCommand> Commands;
			size_t NextCommand = 0;
			std::vector<std::unique_ptr<FConcreteTextureResource>> DeferredResources;
		};

		// Models the Stage 3/4 asset contract while keeping the test independent
		// of either texture's upload format and RHI backend.
		class FTextureAssetLifetimeModel
		{
		public:
			explicit FTextureAssetLifetimeModel(ETextureKind InKind)
				: Kind(InKind)
				, StableState(std::make_shared<FStableTextureState>())
			{
				CurrentObservation = std::make_shared<FResourceObservation>(
					FResourceObservation{.Kind = Kind});
				CurrentResource = std::make_unique<FConcreteTextureResource>(
					NextIdentity++, CurrentObservation);
				CurrentResource->Init();
				StableState->Target = CurrentResource->GetIdentity();
			}

			auto AcquireConsumerReference() const -> std::shared_ptr<FStableTextureState>
			{
				return StableState;
			}

			auto BeginReplacement(FDeterministicRenderScheduler& Scheduler)
				-> std::shared_ptr<FResourceObservation>
			{
				check(CurrentResource != nullptr);
				RetiredObservation = CurrentObservation;
				FConcreteTextureResource* Retired =
					Scheduler.AdoptForDeferredCleanup(std::move(CurrentResource));

				CurrentObservation = std::make_shared<FResourceObservation>(
					FResourceObservation{.Kind = Kind});
				CurrentResource = std::make_unique<FConcreteTextureResource>(
					NextIdentity++, CurrentObservation);
				FConcreteTextureResource* Candidate = CurrentResource.get();
				const std::shared_ptr<FStableTextureState> Reference = StableState;

				Scheduler.Enqueue([Candidate] {
					Candidate->Init();
				});
				Scheduler.Enqueue([Candidate, Reference] {
					EXPECT_TRUE(Candidate->GetIdentity() > 0);
					Reference->Target = Candidate->GetIdentity();
				});
				Scheduler.Enqueue([Retired] {
					Retired->Release();
				});
				Scheduler.Enqueue([Retired, &Scheduler] {
					Scheduler.Retire(Retired);
				});
				return RetiredObservation;
			}

			auto BeginUnload(FDeterministicRenderScheduler& Scheduler)
				-> std::shared_ptr<FResourceObservation>
			{
				check(CurrentResource != nullptr);
				RetiredObservation = CurrentObservation;
				FConcreteTextureResource* Retired =
					Scheduler.AdoptForDeferredCleanup(std::move(CurrentResource));
				const std::shared_ptr<FStableTextureState> Reference = StableState;

				Scheduler.Enqueue([Reference] {
					Reference->Target = 0;
				});
				Scheduler.Enqueue([Retired] {
					Retired->Release();
				});
				Scheduler.Enqueue([Retired, &Scheduler] {
					Scheduler.Retire(Retired);
				});
				StableState.reset();
				return RetiredObservation;
			}

			auto GetCurrentObservation() const
				-> const std::shared_ptr<FResourceObservation>&
			{
				return CurrentObservation;
			}

			auto GetCurrentIdentity() const -> int
			{
				check(CurrentResource != nullptr);
				return CurrentResource->GetIdentity();
			}

		private:
			ETextureKind Kind;
			int NextIdentity = 1;
			std::shared_ptr<FStableTextureState> StableState;
			std::unique_ptr<FConcreteTextureResource> CurrentResource;
			std::shared_ptr<FResourceObservation> CurrentObservation;
			std::shared_ptr<FResourceObservation> RetiredObservation;
		};

		class FTextureResourceLifetimeContractTests
			: public testing::TestWithParam<ETextureKind>
		{
		};

		auto TextureKindName(
			const testing::TestParamInfo<ETextureKind>& Parameter) -> std::string
		{
			return Parameter.param == ETextureKind::Texture2D
				? "Texture2D"
				: "TextureCube";
		}
	}

	TEST_P(FTextureResourceLifetimeContractTests,
		ReplacementRemainsOrderedAtInitPublicationReleaseAndRetirement)
	{
		FDeterministicRenderScheduler Scheduler;
		FTextureAssetLifetimeModel Asset(GetParam());
		const std::shared_ptr<FStableTextureState> Consumer =
			Asset.AcquireConsumerReference();
		const int PreviousIdentity = Consumer->Target;
		const std::shared_ptr<FResourceObservation> Retired =
			Asset.BeginReplacement(Scheduler);
		const std::shared_ptr<FResourceObservation> Candidate =
			Asset.GetCurrentObservation();
		EXPECT_EQ(Retired->Kind, GetParam());
		EXPECT_EQ(Candidate->Kind, GetParam());

		// Paused before initialization.
		EXPECT_EQ(Consumer->Target, PreviousIdentity);
		EXPECT_FALSE(Candidate->bInitialized);
		EXPECT_FALSE(Retired->bReleased);
		EXPECT_EQ(Scheduler.NumDeferredResources(), 1u);

		Scheduler.RunNext();

		// Paused before publication.
		EXPECT_EQ(Consumer->Target, PreviousIdentity);
		EXPECT_TRUE(Candidate->bInitialized);
		EXPECT_FALSE(Retired->bReleased);

		Scheduler.RunNext();

		// Paused before release.
		EXPECT_EQ(Consumer->Target, Asset.GetCurrentIdentity());
		EXPECT_FALSE(Retired->bReleased);
		EXPECT_FALSE(Retired->bDestroyed);

		Scheduler.RunNext();

		// Paused before retirement.
		EXPECT_TRUE(Retired->bReleased);
		EXPECT_FALSE(Retired->bDestroyed);
		EXPECT_EQ(Scheduler.NumDeferredResources(), 1u);

		Scheduler.Drain();
		EXPECT_TRUE(Retired->bDestroyed);
		EXPECT_EQ(Scheduler.NumDeferredResources(), 0u);
		EXPECT_EQ(Consumer->Target, Asset.GetCurrentIdentity());
	}

	TEST_P(FTextureResourceLifetimeContractTests,
		UnloadClearsConsumersBeforeReleaseAndRetirement)
	{
		FDeterministicRenderScheduler Scheduler;
		FTextureAssetLifetimeModel Asset(GetParam());
		const std::shared_ptr<FStableTextureState> Consumer =
			Asset.AcquireConsumerReference();
		const std::shared_ptr<FResourceObservation> Retired =
			Asset.BeginUnload(Scheduler);
		EXPECT_EQ(Retired->Kind, GetParam());

		// Asset finalization no longer owns either object, while accepted work
		// retains only the stable RHI state and deferred concrete storage.
		EXPECT_NE(Consumer->Target, 0);
		EXPECT_FALSE(Retired->bReleased);
		EXPECT_EQ(Scheduler.NumDeferredResources(), 1u);

		Scheduler.RunNext();
		EXPECT_EQ(Consumer->Target, 0);
		EXPECT_FALSE(Retired->bReleased);

		Scheduler.RunNext();
		EXPECT_TRUE(Retired->bReleased);
		EXPECT_FALSE(Retired->bDestroyed);

		Scheduler.Drain();
		EXPECT_TRUE(Retired->bDestroyed);
		EXPECT_EQ(Scheduler.NumDeferredResources(), 0u);
		EXPECT_EQ(Consumer->Target, 0);
	}

	INSTANTIATE_TEST_SUITE_P(
		TextureKinds,
		FTextureResourceLifetimeContractTests,
		testing::Values(ETextureKind::Texture2D, ETextureKind::TextureCube),
		TextureKindName);
}
