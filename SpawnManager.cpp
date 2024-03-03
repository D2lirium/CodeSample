// Copyright 2024 Marchetti S. César A. All Rights Reserved.

#include "Spawners_Monsters/SheepsSpawnManager.h"

#include "AbilitySystemGlobals.h"
#include "EngineUtils.h"
#include "GameplayCueManager.h"
#include "AbilitySystem/GameplayData/AlwaysLoadedData.h"
#include "AbilitySystem/GameplayData/GameplayDataCharacter.h"
#include "AbilitySystem/GameplayData/GameplayDataRandomEffect.h"
#include "AbilitySystem/GameplayData/GameplayDataSubsystem.h"
#include "Units/BaseCharacter.h"
#include "AbilitySystem/AbilitySystemComponents/BaseAbilitySystemComponent.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "NavigationSystem.h"
#include "Engine/AssetManager.h"
#include "AbilitySystem/GlobalTags.h"
#include "Units/BaseAICharacter.h"

USpawnManager::USpawnManager() : Super()
{	
	PrimaryComponentTick.bCanEverTick = false;	
}

void USpawnManager::BeginPlay()
{
	Super::BeginPlay();

	GameplayDataSubsystem = GetOwner()->GetGameInstance()->GetSubsystem<UGameplayDataSubsystem>();
	if (GameplayDataSubsystem)
	{
		GameplayDataSubsystem->CallOrRegister_OnAllDataInitialized(FOnGameplayLibraryInitialized::FDelegate::CreateUObject(this, &USpawnManager::InitializeEnemyPool));
		navSys = UNavigationSystemV1::GetCurrent(GetWorld());
	}
}

void USpawnManager::CallOrRegister_OnInitialized(FOnSpawnerInitializedSignature::FDelegate&& Delegate)
{
	if (bInitialized)
	{
		Delegate.Execute();
	}
	else
	{
		OnSpawnerInitialized.Add(MoveTemp(Delegate));
	}
}

void USpawnManager::SpawnNextWave()
{		
	FEnvQueryRequest QueryRequest = FEnvQueryRequest(SpawnData->SpawnLocationQuery, GetOwner());
	QueryRequest.SetWorldOverride(GetWorld());
	const FQueryFinishedSignature OnLocationQueryEnded = FQueryFinishedSignature::CreateLambda([=](const TSharedPtr<FEnvQueryResult>& Result)
		{
			FVector Location = FVector::ZeroVector;
			if (Result.IsValid())
			{
				Location = Result.Get()->GetItemAsLocation(0);
			}
			else
			{
				FNavLocation NavResult;
				navSys->GetRandomReachablePointInRadius(Location, 2000.f, NavResult);
				Location = NavResult.Location;
			}

			SpawnWave(SpawnData->Score.GetRichCurveConst()->Eval(Wave), FMath::TruncToFloat(SpawnData->Variety.GetRichCurveConst()->Eval(Wave)), Location);
			Wave++;
			UpdateEnemyPool();
		});

	QueryRequest.Execute(EEnvQueryRunMode::RandomBest25Pct, OnLocationQueryEnded);
}


void USpawnManager::ClearEnemies()
{
	for (ABaseCharacter* Enemy : SpawnedEnemies)
	{
		if(Enemy)
		{
			if(UAbilitySystemComponent* ASC = 	UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Enemy))
			{
				ASC->bSuppressGameplayCues = true;
				ASC->RemoveAllGameplayCues();
			}
			Enemy->OnDeath.RemoveAll(this);
			Enemy->Destroy();
		}
	}
	SpawnedEnemies.Empty();
	OnEnemyCountChanged.Broadcast(0);
}

void USpawnManager::ResetState()
{
	GetWorld()->GetTimerManager().ClearTimer(SpawnTimerHandle);
	Wave = 1;
	LastRareWave = 0;
	EnemyPoolSequenceIndex = 0;

	SpawnQueue.Empty();
	
	ClearEnemies();		
	
	//destroy actors that are dying, but not in the Enemy array
	UGameplayCueManager* GameplayCueManager =  UAbilitySystemGlobals::Get().GetGameplayCueManager();
	for(TActorIterator<AActor> it(GetWorld(), ABaseAICharacter::StaticClass()); it; ++it)
	{
		if(IsValid(*it))
		{
			if(UAbilitySystemComponent* ASC = 	UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(*it))
			{				
				ASC->bSuppressGameplayCues = true;				
			}
			GameplayCueManager->EndGameplayCuesFor(*it);
			it->Destroy();
		}
	}
	
	DeinitializePool();	
	InitializeEnemyPool();
}

void USpawnManager::SpawnWave(float Score, float Variety, FVector Location)
{
	const float ScorePerEnemy = Score / Variety;

	//Randomize enemies from the pool.
	TArray<UGameplayDataAICharacter*> Enemies;
	TArray<UGameplayDataAICharacter*> Pool = EnemyPool;

	Pool.RemoveAll([=](const UGameplayDataAICharacter* PooledEnemy)
		{
			return PooledEnemy->Score > ScorePerEnemy;
		});

	for (int32 i = 0; i < Variety; i++)
	{
		const int32 Index = UKismetMathLibrary::RandomIntegerInRange(0, Pool.Num() - 1);
		Enemies.Add(Pool[Index]);
		Pool.RemoveAtSwap(Index);
		if (Pool.IsEmpty())
		{
			UE_LOG(LogTemp, Log, TEXT("USpawnManager::SpawnWave: not enought enemies in the pool to cover the variety at wave %i"), Wave);
			break;
		}
	}

	if (Enemies.Num() > 1)
	{				
		Algo::Sort(Enemies, [=](const UGameplayDataAICharacter* A, const UGameplayDataAICharacter* B) {
			return A->Score > B->Score;
			});
	}

	//Calculate amounts to fill the wave score per enemy. Assume each enemy uses an equal part of score, but use leftovers for next enemies.
	TArray<FSpawnQueueElement> EnemiesToSpawn;
	float RemainingScore = Score;

	for (const auto& Enemy : Enemies)
	{
		const float EnemyScore = RemainingScore / Variety;
		FSpawnQueueElement Element;		
		Element.Character = Enemy;
		Element.Location = Location;
		Element.Level = Wave;
		Element.Amount =  EnemyScore / Enemy->Score;
		ensure(Element.Amount >= 1);
		EnemiesToSpawn.Add(Element);
		const float UsedScore = Element.Amount * Enemy->Score;
		RemainingScore -= UsedScore;
		Variety--;
	}

	if (ShouldCreateRares())
	{
		CreateRares(EnemiesToSpawn);
	}

	UE_LOG(LogTemp, Log, TEXT("USpawnManager::SpawnWave: %i enemies were added to the queue at wave %i"), EnemiesToSpawn.Num(), Wave);

	//Start spawning the enemies over time.
	AddEnemiesToQueue(EnemiesToSpawn);
}

bool USpawnManager::ShouldCreateRares() const
{
	return Wave >= LastRareWave + SpawnData->RareWaveSpacing.GetRichCurveConst()->Eval(Wave);
}

void USpawnManager::CreateRares(TArray<FSpawnQueueElement>& Enemies)
{
	LastRareWave = Wave;	
	TArray<FSpawnQueueElement> RareElements;
	for (int32 i = 0; i < SpawnData->RareAmount.GetRichCurveConst()->Eval(Wave); i++)
	{
		//Pick one of the spawned enemies to convert to rare
		const int32 Index = UKismetMathLibrary::RandomIntegerInRange(0, Enemies.Num() - 1);

		FSpawnQueueElement RareElement= Enemies[Index];
		RareElement.Amount = 1;
		RareElement.Rarity = UGlobalTags::Unit_Rarity_Rare();
		RareElement.RandomEffectsAmount = SpawnData->RareEffectsAmount.GetRichCurveConst()->Eval(Wave);

		if (Enemies[Index].Amount == 1)
		{
			Enemies.RemoveAt(Index);
		}
		else
		{
			Enemies[Index].Amount--;
		}
		
		RareElements.Add(RareElement);

		if (Enemies.IsEmpty())
		{
			break;
		}
	}

	//all remaining enemies in the wave will have one effect applied to them.
	if (UGameplayDataRandomEffect* Effect = GameplayDataSubsystem->GetRandomEffect(UGlobalTags::Unit_Rarity_Magic().GetSingleTagContainer(), GetGlobalSpecificEffectsData()))
	{
		TMap<FGameplayTag, float> Magnitudes = Effect->RandomizeMagnitudes(Wave);
		for (auto& It : Enemies)
		{
			It.Rarity = UGlobalTags::Unit_Rarity_Magic();
			It.SpecificEffects.Add(Effect, FTagMagnitudesContainer(Magnitudes));
		}
	}
	
	UE_LOG(LogTemp, Log, TEXT("USpawnManager::CreateRares: %i rares were added to the queue at wave %i"), RareElements.Num(), Wave);

	Enemies.Append(RareElements);
}

void USpawnManager::AddEnemiesToQueue(TArray<FSpawnQueueElement>& Enemies)
{
	if (Enemies.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("USpawnManager::AddEnemiesToQueue No enemies added to Queue at wave %i"), Wave);
		return;
	}

	SpawnQueue.Append(Enemies);	

	for (auto& enemy : Enemies)
	{
		enemy.RandomEffectsAmount += GlobalRandomEffectsAmount;
		enemy.SpecificEffects.Append(GlobalSpecificEffects);
	}	

	if (!SpawnQueue.IsEmpty() && !GetWorld()->GetTimerManager().IsTimerActive(SpawnTimerHandle))
	{
		GetWorld()->GetTimerManager().SetTimer(SpawnTimerHandle, this, &USpawnManager::SpawnQueuedEnemy, SpawnData->TimeBetweenConcurrentSpawns, true);
	}
}

void USpawnManager::SpawnQueuedEnemy()
{
	FSpawnQueueElement& SpawnElement = SpawnQueue[0];	
	FTransform SpawnTransform;	
	FNavLocation Result;
	navSys->GetRandomReachablePointInRadius(SpawnElement.Location, SpawnData->SpawnRadius, Result);
	SpawnTransform.SetLocation(Result.Location);
	SpawnTransform.SetRotation(FQuat(FRotator(UKismetMathLibrary::FindLookAtRotation(SpawnElement.Location, FVector::ZeroVector))));
	ABaseCharacter* Enemy = GetWorld()->SpawnActorDeferred<ABaseCharacter>(SpawnElement.Character->CharacterClass.Get(), SpawnTransform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
	ensure(Enemy);
	UBaseAbilitySystemComponent* ASC = Enemy->GetBaseAbilitySystemComponent();
	ASC->AddLooseGameplayTag(SpawnElement.Rarity);
	Enemy->OnDeath.AddDynamic(this, &USpawnManager::OnEnemyKilled);
	Enemy->FinishSpawning(SpawnTransform);

	ASC->Server_SetLevel(SpawnElement.Level);

	if (SpawnElement.Rarity.IsValid() || !SpawnElement.SpecificEffects.IsEmpty() || SpawnElement.RandomEffectsAmount > 0)
	{
		ASC->CallOrRegister_OnNativeAbilitySystemInitialized(FOnAbilitySystemInitializedNativeSignature::FDelegate::CreateLambda([=]
		{
			if (SpawnElement.Rarity.IsValid())
			{
				//add rarity effect
				const FGameplayEffectSpecHandle RarityHandle = ASC->MakeOutgoingSpec(GameplayDataSubsystem->GetRarityEffectClass(SpawnElement.Rarity), 1, ASC->MakeEffectContext());
				ASC->ApplyGameplayEffectSpecToSelf(*RarityHandle.Data.Get());

				if (SpawnElement.Rarity == UGlobalTags::Unit_Rarity_Elite())
				{
					Enemy->SetActorScale3D(FVector(1.25f));
				}
				else if (SpawnElement.Rarity == UGlobalTags::Unit_Rarity_Rare())
				{
					Enemy->SetActorScale3D(FVector(1.15f));
				}
			}

			//apply effects
			{
				FGameplayTagContainer SourceTags = SpawnElement.Character->OwnedTags;
				SourceTags.AddTag(SpawnElement.Rarity);
				TArray<UGameplayDataRandomEffect*> AppliedEffects;

				//Specific effects
				for (auto& SpecifiedEffect : SpawnElement.SpecificEffects)
				{
					AppliedEffects.Add(SpecifiedEffect.Key);
					ASC->ApplyGameplayEffectSpecToSelf(*SpecifiedEffect.Key->CreateSpecHandle(ASC, SpecifiedEffect.Value.Magnitudes).Data.Get());
				}

				//randomized effects.
				if (SpawnElement.RandomEffectsAmount)
				{
					for (int32 j = 0; j < SpawnElement.RandomEffectsAmount; j++)
					{
						if (UGameplayDataRandomEffect* RandomEffect = GameplayDataSubsystem->GetRandomEffect(SourceTags, AppliedEffects))
						{
							AppliedEffects.Add(RandomEffect);
							ASC->ApplyGameplayEffectSpecToSelf(*RandomEffect->CreateSpecHandle(ASC, Wave).Data.Get());
						}
					}
				}
			}

			ASC->RestoreHealthAndResource();
		}));
	}

	SpawnedEnemies.Add(Enemy);
	OnEnemySpawned.Broadcast(Enemy);
	OnEnemyCountChanged.Broadcast(SpawnedEnemies.Num());
	
	SpawnElement.Amount--;
	
	if (SpawnElement.Amount == 0)
	{
		SpawnQueue.RemoveAtSwap(0);

		if (SpawnQueue.IsEmpty())
		{
			GetWorld()->GetTimerManager().ClearTimer(SpawnTimerHandle);
		}
	}
}

void USpawnManager::OnEnemyKilled(AActor* KilledActor)
{
	ABaseCharacter* Char = Cast<ABaseCharacter>(KilledActor);
	SpawnedEnemies.Remove(Char);
	OnEnemyCountChanged.Broadcast(SpawnedEnemies.Num());
}

void USpawnManager::InitializeEnemyPool()
{
	const float WaveScore = SpawnData->Score.GetRichCurveConst()->Eval(Wave);
	const float Variety = FMath::TruncToFloat(SpawnData->Variety.GetRichCurveConst()->Eval(Wave));
	const float MaxScore = WaveScore / Variety;

	const int32 EnemyPoolDesiredSize = FMath::Max(SpawnData->EnemyPoolAmount.GetRichCurveConst()->Eval(Wave), Variety);
	TArray<UGameplayDataAICharacter*> AddedData;
	if (EnemyPoolDesiredSize == -1)
	{
		GameplayDataSubsystem->GetAllEnemyDataForWave(Wave, MaxScore, FGameplayTagRequirements(), AddedData);
	}
	else
	{		
		GetEnemiesInSequence(EnemyPoolDesiredSize, MaxScore, AddedData);
	}

	EnemyPool = AddedData;
	for (auto& It : AddedData)
	{
		EnemyPoolDurationMap.Add(It, SpawnData->PooledEnemyDuration.GetRichCurveConst()->Eval(Wave) + FMath::RandRange(-5, 5));
		UE_LOG(LogTemp, Log, TEXT("USpawnManager::InitializeEnemyPool: %s was added to the pool at wave %i"), *It->GetName(), Wave);
	}

	check(!EnemyPool.IsEmpty());
	UE_LOG(LogTemp, Log, TEXT("USpawnManager::InitializeEnemyPool: %i enemies were added to the initial pool"), EnemyPool.Num());

	UAssetManager& AssetManager = UAssetManager::Get();
	TArray<FPrimaryAssetId> IDs;
	for (const auto& It : EnemyPool)
	{
		IDs.Add(AssetManager.GetPrimaryAssetIdForObject(It));	
	}

	const FStreamableDelegate Delegate = FStreamableDelegate::CreateLambda([&AssetManager, this]()
	{
		bInitialized = true;
		OnSpawnerInitialized.Broadcast();
		OnSpawnerInitialized.Clear();
	});

	const TArray<FName> RemovedBundles;
	TArray<FName> AddedBundles;
	AddedBundles.Add("Exec");
	AssetManager.ChangeBundleStateForPrimaryAssets(IDs, AddedBundles, RemovedBundles, false, Delegate);
}

void USpawnManager::DeinitializePool()
{
	UAssetManager& AssetManager = UAssetManager::Get();
	TArray<FPrimaryAssetId> RemovedIDs;
	for (const auto& It : EnemyPool)
	{
		RemovedIDs.Add(AssetManager.GetPrimaryAssetIdForObject(It));
	}
	EnemyPool.Empty();

	const TArray<FName> AddedBundles;
	TArray<FName> RemovedBundles;
	RemovedBundles.Add("Exec");
	AssetManager.ChangeBundleStateForPrimaryAssets(RemovedIDs, AddedBundles, RemovedBundles, false);
	bInitialized = false;
}

void USpawnManager::UpdateEnemyPool()
{	
	//Update enemy pool duration
	for (auto& it : EnemyPoolDurationMap)
	{
		if (it.Value > 0)
		{
			it.Value--;
		}
	}

	//Remove enemies that cannot be used anymore.
	TArray<FPrimaryAssetId> RemovedIDs;
	EnemyPool.RemoveAll([=, &RemovedIDs](const UGameplayDataAICharacter* Enemy) {
		const bool bRemove = (Enemy->RequiredMaxLevel != -1 && Wave > Enemy->RequiredMaxLevel) || EnemyPoolDurationMap[Enemy] == 0;
		if (bRemove)
		{
			UE_LOG(LogTemp, Log, TEXT("USpawnManager::UpdateEnemyPool: %s was removed from the pool at wave %i"), *Enemy->GetName(), Wave);
			RemovedIDs.Add(Enemy->GetPrimaryAssetId());
		}
		return bRemove;
		});
	
	UE_LOG(LogTemp, Log, TEXT("USpawnManager::UpdateEnemyPool: %i enemies were removed from the pool at wave %i"), RemovedIDs.Num(), Wave);

	UAssetManager& AssetManager = UAssetManager::Get();

	TArray<FName> AddedBundles, RemovedBundles;
	RemovedBundles.Add("Exec");
	AssetManager.ChangeBundleStateForPrimaryAssets(RemovedIDs, AddedBundles, RemovedBundles, false);

	const float Variety = FMath::TruncToFloat(SpawnData->Variety.GetRichCurveConst()->Eval(Wave));
	const int32 EnemyPoolDesiredSize = FMath::Max(SpawnData->EnemyPoolAmount.GetRichCurveConst()->Eval(Wave), Variety);

	const int32 EnemiesToAdd = EnemyPoolDesiredSize != -1 ? EnemyPoolDesiredSize - EnemyPool.Num() : -1;

	if (EnemiesToAdd > 0 || EnemiesToAdd == -1)
	{
		const float WaveScore = SpawnData->Score.GetRichCurveConst()->Eval(Wave);		
		const float MaxScore = WaveScore / Variety;

		TArray<UGameplayDataAICharacter*> AddedData;		
		if (EnemiesToAdd == -1)
		{
			GameplayDataSubsystem->GetAllEnemyDataForWave(Wave, MaxScore, FGameplayTagRequirements(), AddedData);			
		}
		else
		{		
			GetEnemiesInSequence(EnemiesToAdd, MaxScore, AddedData);
		}

		TArray<FPrimaryAssetId> AddedIDs;
		for (const auto& Data : AddedData)
		{
			AddedIDs.Add(Data->GetPrimaryAssetId());
			UE_LOG(LogTemp, Log, TEXT("USpawnManager::UpdateEnemyPool: %s was added to the pool at wave %i"), *Data->GetName(), Wave);
		}
	
		if (!AddedIDs.IsEmpty())
		{
			UE_LOG(LogTemp, Log, TEXT("USpawnManager::UpdateEnemyPool: %i enemies were added to the pool at wave %i"), AddedIDs.Num(), Wave);
			RemovedBundles.Empty();
			AddedBundles.Empty();
			AddedBundles.Add("Exec");

			const FStreamableDelegate Delegate = FStreamableDelegate::CreateLambda([=]()
				{
					EnemyPool.Append(AddedData);
					for (auto& it : AddedData)
					{
						EnemyPoolDurationMap.Add(it, SpawnData->PooledEnemyDuration.GetRichCurveConst()->Eval(Wave) + FMath::RandRange(-5, 5));
					}
				});
			AssetManager.ChangeBundleStateForPrimaryAssets(AddedIDs, AddedBundles, RemovedBundles, false, Delegate);
		}
	}
}

FGameplayTagRequirements USpawnManager::GetNextPoolTagRequirements()
{
	FGameplayTagRequirements Req;

	if (!SpawnData->EnemyPoolSequence.IsEmpty())
	{
		Req = SpawnData->EnemyPoolSequence[EnemyPoolSequenceIndex];
		EnemyPoolSequenceIndex++;

		if (!SpawnData->EnemyPoolSequence.IsValidIndex(EnemyPoolSequenceIndex))
		{
			EnemyPoolSequenceIndex = 0;
		}
	}

	return Req;
}

void USpawnManager::GetEnemiesInSequence(int32 Amount, float MaxScore, TArray<UGameplayDataAICharacter*>& AddedEnemyData)
{
	//Add enemies in sequence by tags			
	for (int32 i = 0; i < Amount; i++)
	{		
		bool bFoundEnemy = false;
		for (int32 j = 0; j < SpawnData->EnemyPoolSequence.Num(); j++)
		{
			TArray<UGameplayDataAICharacter*> IgnoredChars = EnemyPool;			
			IgnoredChars.Append(AddedEnemyData);
			if (UGameplayDataAICharacter* Data = GameplayDataSubsystem->GetRandomCharacterDataForWave(Wave, MaxScore, GetNextPoolTagRequirements(), IgnoredChars))
			{
				AddedEnemyData.Add(Data);				
				bFoundEnemy = true;
				break;
			}
		}

		if (!bFoundEnemy)
		{
			break;
		}
	}
}