// Fill out your copyright notice in the Description page of Project Settings.

#include "AbilitySystem/CollisionActors/BaseCollisionActor.h"

#include "Runtime/Engine/Public/TimerManager.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "GameplayCueManager.h"
#include "Net/UnrealNetwork.h"
#include "Components/TimelineComponent.h"
#include "Components/SceneComponent.h"
#include "Components/ShapeComponent.h"
#include "Components/CapsuleComponent.h"
#include "Particles/ParticleSystemComponent.h"
#include "NiagaraComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/GameNetworkManager.h"
#include "GameFramework/Character.h"
#include "DrawDebugHelpers.h"
#include "Abilities/GameplayAbility.h"

#include "AbilitySystem/BPL_AbilitySystem.h"
#include "AbilitySystem/AbilitySystemComponents/BaseAbilitySystemComponent.h"
#include "AbilitySystem/MyAbilitySystemGlobals.h"
#include "AbilitySystem/GlobalTags.h"
#include "AbilitySystem/ActorPool/ActorPoolManager.h"
#include "AbilitySystem/AttributeSets/AbilityAttributeSet.h"
#include "AbilitySystem/Targeting/TargetFunctionLibrary.h"
#include "cameraplay/cameraplay.h"

#include "SplineManager/SplineManagerInterface.h" //destructible actors

FName ABaseCollisionActor::ShapeComponentName(TEXT("Shape Component"));

ABaseCollisionActor::ABaseCollisionActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{			
	//We need a scene comp so we can offset the shape comp relative to this.
	SceneComp = CreateDefaultSubobject<USceneComponent>(TEXT("SceneComponent"));
	SetRootComponent(SceneComp);

	ShapeComp = CreateDefaultSubobject<UShapeComponent>(ShapeComponentName);
	if (ShapeComp)
	{		
		ShapeComp->SetCollisionProfileName(FName(TEXT("CollisionActor")));		
		ShapeComp->SetGenerateOverlapEvents(true);
		ShapeComp->SetCanEverAffectNavigation(false);		
		ShapeComp->SetupAttachment(SceneComp);
	}

	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bHighPriority = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	//Actor must be replicated to share their reference to clients through FFAS.
	bReplicates = true;		
	bNetUseOwnerRelevancy = true;

	//This is needed even for non moving actors, so when we change the transform on server when reusing, it updates also in the clients.
	SetReplicateMovement(true);
	
	//Start with collision disabled, this way, when we turn on the collision, it will check for overlaps and trigger the right events.
	SetActorEnableCollision(false);

	GameplayCueManager = nullptr;
	bActive = false;
	bPreactivated = false;
	bRegisteredTargetInstance = false;
	bSoftRegisteredTargetInstance = false;
	NumPreallocatedInstances = 0;
	bInRecycleQueue = false;		
	bActorGameplayCueInitialized = false;
	bPreactivationGameplayCueInitialized = false;
	bPreviewGameplayCueInitialized = false;
	bExecuteDeactivationCue = true;
	bInterpolatingScale = false;
	bSkipVariableInitialization = false;
	bSynched = false;
	bSkipGameplayCues = false;
	bAttached = false;
	bAllowRetargetting = false;
	bAppliesPersistentEffects = false;
	bDiscreteCollisionChecks = false;

	PredictionRotationRateMultiplier = 1.25f;

	HitTargetGameplayCues.AddTag(UGlobalTags::GameplayCue_HitTaken());
}

void ABaseCollisionActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ABaseCollisionActor, bAbilityFromListenServer);
}

void ABaseCollisionActor::BeginPlay()
{
	Super::BeginPlay();
	
	//SetActorHiddenInGame(true);
}

void ABaseCollisionActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);

	Deactivate();
}

void ABaseCollisionActor::Tick(float Delta)
{
	Super::Tick(Delta);

	Interpolate(Delta);

	InterpolateRotation(Delta);

	UpdateAttachment(Delta);

	if (!RequiresTick())
	{
		SetActorTickEnabled(false);
	}
}

bool ABaseCollisionActor::RequiresTick() const
{
	return bInterpolatingScale || bUpdateAttachmentOnTick || bInterpolatingRotation;
}

bool ABaseCollisionActor::IsNetRelevantFor(const AActor* RealViewer, const AActor* ViewTarget, const FVector& SrcLocation) const
{
	//Should be irrelevant when predicting for the owning client.
	if (GetInstigatorBaseAbilitySystemComponent() && GetInstigatorBaseAbilitySystemComponent()->IsCollisionActorPredictionEnabled() && RealViewer->GetRemoteRole() != ENetRole::ROLE_Authority && (IsOwnedBy(ViewTarget) || IsOwnedBy(RealViewer)))
	{
		return false;
	}

	return Super::IsNetRelevantFor(RealViewer, ViewTarget, SrcLocation);
}

float ABaseCollisionActor::ScaleValueWithAttribute(UAbilitySystemComponent* InASC, const FGameplayTagContainer& InAbilityTags, float Value, FGameplayAttribute Attribute) const
{
	//-1 is used in many cases as "infinite" and we dont want to apply attribute modifications to this type of attributes.
	if (Value != -1 && InASC)
	{
		bool bSucc = false;
		float OutValue = UAbilitySystemBlueprintLibrary::EvaluateAttributeValueWithTagsAndBase(InASC, Attribute, InAbilityTags, InAbilityTags, Value, bSucc);

		if (bSucc)
		{
			return OutValue;
		}
	}

	return Value;
}

FVector ABaseCollisionActor::GetActorBoneSocketLocation(AActor* InActor, FName Bone) const
{
	if (InActor)
	{
		if (Bone != NAME_None)
		{
			TArray<UMeshComponent*> MeshComponents;
			InActor->GetComponents<UMeshComponent>(MeshComponents);

			for (auto& it : MeshComponents)
			{
				if (it->DoesSocketExist(Bone))
				{
					return it->GetSocketLocation(Bone);
				}
			}
		}

		return InActor->GetActorLocation();
	}

	return  FVector::ZeroVector;
}

TSubclassOf<UGameplayAbility> ABaseCollisionActor::GetOwningAbilityClass()
{
	return IndividualData.AbilityClass;
}

int32 ABaseCollisionActor::GetSharedDataID()
{
	return SharedData.ID;
}

int32 ABaseCollisionActor::GetActivationKey()
{
	return IndividualData.ActivationKey;
}

void ABaseCollisionActor::InitializeSharedData(UAbilitySystemComponent* InASC, UGameplayAbility* InAbility, FCollisionActorSharedData& OutData) const
{
	if (InASC && InAbility)
	{
		OutData.DurationMultiplier = ScaleValueWithAttribute(InASC, InAbility->AbilityTags, 1.f, UAbilitySystemComponent::GetOutgoingDurationProperty());
		OutData.PeriodMultiplier = ScaleValueWithAttribute(InASC, InAbility->AbilityTags, 1.f, UAbilityAttributeSet::GetOutgoingTickDurationAttribute());
		OutData.AreaMultiplier = ScaleValueWithAttribute(InASC, InAbility->AbilityTags, 1.f, UAbilityAttributeSet::GetAreaOfEffectAttribute());
	}
}

bool ABaseCollisionActor::IsCollisionActorPreactivated() const
{
	return bPreactivated;
}

void ABaseCollisionActor::PreActivateCollisionActor(const FCollisionActorIndividualData& InIndividualData)
{
	bPreactivated = true;

	SetNetDormancy(ENetDormancy::DORM_Awake);

	SetIndividualData(InIndividualData);

	FCollisionActorSharedData* SharedDataPtr = GetInstigatorBaseAbilitySystemComponent()->CollisionActorSharedData.FindSharedDataByID(InIndividualData.SharedDataID);

	if (SharedDataPtr)
	{
		UE_LOG(CollisionActorLog, Log, TEXT("ABaseCollisionActor::PreActivateCollisionActor : Shared data was valid for %s."), *GetName());

		SetSharedData(*SharedDataPtr);

		CallBeginActivate();
	}
	else
	{
		UE_LOG(CollisionActorLog, Log, TEXT("ABaseCollisionActor::PreActivateCollisionActor : Waiting for Shared data for %s."), *GetName());

		//Bind to delegate to wait for shared data.
		if (!GetInstigatorBaseAbilitySystemComponent()->CollisionActorSharedData.OnSharedDataAdded.IsAlreadyBound(this, &ABaseCollisionActor::OnSharedDataReplicatedBack))
		{
			GetInstigatorBaseAbilitySystemComponent()->CollisionActorSharedData.OnSharedDataAdded.AddDynamic(this, &ABaseCollisionActor::OnSharedDataReplicatedBack);
		}
	}
}

void ABaseCollisionActor::OnSharedDataReplicatedBack(int32 SharedDataID)
{
	if (IndividualData.SharedDataID == SharedDataID)
	{
		UE_LOG(CollisionActorLog, Log, TEXT("ABaseCollisionActor::OnSharedDataReplicatedBack : Collision Actor %s received shared data."), *GetName());

		if (GetInstigatorBaseAbilitySystemComponent()->CollisionActorSharedData.OnSharedDataAdded.IsAlreadyBound(this, &ABaseCollisionActor::OnSharedDataReplicatedBack))
		{
			GetInstigatorBaseAbilitySystemComponent()->CollisionActorSharedData.OnSharedDataAdded.RemoveDynamic(this, &ABaseCollisionActor::OnSharedDataReplicatedBack);
		}

		SetSharedData(*GetInstigatorBaseAbilitySystemComponent()->CollisionActorSharedData.FindSharedDataByID(SharedDataID));

		CallBeginActivate();
	}
}

void ABaseCollisionActor::InitializeVariablesFromSharedData()
{
	//Restore default values in case this is not a fresh spawn.
	const ABaseCollisionActor* CDO = Cast<ABaseCollisionActor>(GetClass()->ClassDefaultObject);

	Duration = CDO->Duration;
	ScaleInterpolation = CDO->ScaleInterpolation;
	RotationInterpolation = CDO->RotationInterpolation;
	if (RotationInterpolation.bAlternateRotationDirection)
	{
		RotationInterpolation.RotationRate *= FMath::Pow(-1.f, IndividualData.SpawnIndex);
	}

	Duration.LifeSpan *= SharedData.DurationMultiplier;
	Duration.FirstPeriodDelay *= SharedData.PeriodMultiplier;
	Duration.Period *= SharedData.PeriodMultiplier;

	//Scales area with avatar scale
	if (OwningAbilityTags.HasTag(UGlobalTags::Ability_Device_Trap_Enviroment()))
	{
		float AvatarScale = GetInstigator() != nullptr ? GetInstigator()->GetActorScale().X : GetOwner()->GetActorScale().X;
		SharedData.AreaMultiplier *= AvatarScale;
	}
}

void ABaseCollisionActor::SetSharedData(const FCollisionActorSharedData& InSharedData)
{
	SharedData = InSharedData;

	if (IndividualData.AbilityClass)
	{
		//Init owning ability tags.
		FModifiedAbility ModifiedAbility = FModifiedAbility(IndividualData.MainModifierAbilityClass ? IndividualData.MainModifierAbilityClass : IndividualData.AbilityClass, SharedData.AbilityLevel);
		TArray<FGameplayTag> ModifierTags;
		SharedData.ModifierTags.GetGameplayTagArray(ModifierTags);
		for (const FGameplayTag& ModifierTag : ModifierTags)
		{
			ModifiedAbility.ApplyModifier(UBPL_AbilitySystem::FindAbilityModifier(ModifierTag));
		}

		if (ModifiedAbility.AffectedAbilitiesModifiedTags.Contains(IndividualData.AbilityClass))
		{
			OwningAbilityTags = *ModifiedAbility.AffectedAbilitiesModifiedTags.Find(IndividualData.AbilityClass);
		}
		else
		{
			OwningAbilityTags = IndividualData.AbilityClass.GetDefaultObject()->AbilityTags;
			UE_LOG(CollisionActorLog, Warning, TEXT("ABaseCollisionActor::SetSharedData: Could not generate tags for %s, falling back to CDO tags."), *IndividualData.AbilityClass.Get()->GetFName().ToString());
		}
	}

	//We can now use tags to calculate attributes
	InitializeVariablesFromSharedData();
}

void ABaseCollisionActor::SetIndividualData(const FCollisionActorIndividualData& InIndividualData)
{
	IndividualData = InIndividualData;

	SetSourceAbilitySystemComponent();

	Filter.InitializeFilterContext(GetInstigator() != nullptr ? GetInstigator() : GetOwner());

	if (HasAuthority())
	{
		//Is the ability a listen server + client ability. this abilities should not predict.
		if (GetIsReplicated())
		{
			bAbilityFromListenServer = GetInstigatorAbilitySystemComponent()->AbilityActorInfo.Get()->IsLocallyControlled() && GetInstigatorAbilitySystemComponent()->AbilityActorInfo.Get()->IsNetAuthority();
		}

		//Update context. Effect causer and instigator.
		UBPL_AbilitySystem::SetInstigatorAndEffectCauserToContainerEffectContext(EffectContainerSpec, GetInstigator() != nullptr ? GetInstigator() : GetOwner(), this);
	}

	//Init targeting
	bRegisteredTargetInstance = false;
	RegisterSharedTargetInstance();
}

void ABaseCollisionActor::CallBeginActivate()
{
	//How long this collision actor has been going in the server.
	float DeltaServerTime = IndividualData.ServerActivationTime - GetServerWorldTime();

	//Set a timer to the moment the actor should activate.
	if (DeltaServerTime <= 0.f)
	{
		CompensationActivationDelay = DeltaServerTime;
		BeginActivate();
	}
	else if (GetWorld())
	{
		CompensationActivationDelay = 0.f;
		FTimerHandle PreactivationTimer;
		GetWorld()->GetTimerManager().SetTimer(PreactivationTimer, this, &ABaseCollisionActor::BeginActivate, DeltaServerTime, false, DeltaServerTime);
	}
	else
	{
		UE_LOG(CollisionActorLog, Log, TEXT("ABaseCollisionActor::CallBeginActivate: World was invalidated for %s."), *IndividualData.AbilityClass.Get()->GetFName().ToString());
		Deactivate();
	}
}

void ABaseCollisionActor::BeginActivate()
{
	//Predicted actors apply their cues locally and skip the server cues.
	bSkipGameplayCues = GetIsReplicated() && GetOwner()->IsOwnedBy(UGameplayStatics::GetPlayerController(this, 0)) && GetNetMode() != ENetMode::NM_ListenServer && GetNetMode() != ENetMode::NM_Standalone;
	
	//Cache to know if we apply persistent effects.
	bAppliesPersistentEffects = false;
	
	for (auto& it : EffectContainerSpec.TargetGameplayEffectSpecs)
	{
		if (it.Data.Get()->Def->DurationPolicy == EGameplayEffectDurationType::Infinite)
		{
			bAppliesPersistentEffects = true;
			break;
		}
	}
	
	//debug
#if WITH_EDITOR
	if (bSkipGameplayCues)
	{
		UE_LOG(CollisionActorLog, Log, TEXT("Skip GameplayCues for %s."), *GetName());
	}
#endif

	//TEST
//	bool bOwnerPanw = GetInstigator() == UGameplayStatics::GetPlayerPawn(this, 0);
//	bool bClientOwner = IsOwnedBy(UGameplayStatics::GetPlayerController(this, 0));
//	bool Autonomous = GetInstigator()->GetRemoteRole() == ENetRole::ROLE_AutonomousProxy; 

	InitializeScale();

	//If we have a predicting actor, we hide the server one.	
	SetActorHiddenInGame(GetIsReplicated() && GetOwner()->IsOwnedBy(UGameplayStatics::GetPlayerController(this, 0)));
	
	bSynched = false;

	//Activation delay spawn. To make telegraphed area of effect that apply effects after a delay.
	if (Duration.ActivationDelay > 0.f)
	{
		//Adjust transform on preactivation.
		AdjustTransform();
		InitializePreactivationGameplayCue();

		//substract the prediction time for server actors.
		if (ShouldPredict() && !FMath::IsNearlyZero(GetPredictionDeltaTime(), 0.05f))
		{
			Duration.ActivationDelay -= GetPredictionDeltaTime();
			Duration.ActivationDelay += CompensationActivationDelay;
			bSynched = true;
		}

		if (GetWorld())
		{
			GetWorld()->GetTimerManager().SetTimer(ActivationDelayTimerHandle, this, &ABaseCollisionActor::FinishActivate, Duration.ActivationDelay, false, Duration.ActivationDelay);
		}
		else
		{
			UE_LOG(CollisionActorLog, Log, TEXT("ABaseCollisionActor::BeginActivate: World was invalidated for %s."), *IndividualData.AbilityClass.Get()->GetFName().ToString());
			Deactivate();
		}
	}
	else if (Duration.ActivationDelay == 0.f)
	{
		FinishActivate();
	}
}

void ABaseCollisionActor::FinishActivate()
{
	UE_LOG(CollisionActorLog, Log, TEXT("Activated %s"), *GetFName().ToString());

	bActive = true;
	StartTime = UKismetSystemLibrary::GetGameTimeInSeconds(this);

	//Adjust transform for non delayed activations.
	if (Duration.ActivationDelay == 0.f)
	{
		AdjustTransform();
	}

	//Initialize
	SetStartLocation();
	InitializeRotationInterpolation();
	RemovePreactivationGameplayCue();
	InitializeActorGameplayCue();
	ExecuteGameplayCues();

	OnCollisionActorActivate.Broadcast(this);

	//Send Collision Actor Activated Event. Predicted client side versions dont send events.
	if (GetIsReplicated())
	{
		//Send hit events	
		FGameplayEventData Payload;
		Payload.EventMagnitude = 1;
		Payload.Instigator = GetInstigator() != nullptr ? GetInstigator() : GetOwner();
		Payload.InstigatorTags = OwningAbilityTags;
		Payload.OptionalObject = this;
		Payload.ContextHandle = GetEffectContext();
		SendGameplayEvent(GetInstigatorAbilitySystemComponent(), UGlobalTags::Event_CollisionActorActivate(), Payload);
	}

	if (Duration.LifeSpan == 0.f)
	{
		//Enable Collisions
		SetActorEnableCollision(true);

		//Apply effect to overlapping actors	
		TArray<AActor*> OverlappingActors;
		ShapeComp->GetOverlappingActors(OverlappingActors);
		ApplyEffectToActorArray(OverlappingActors, nullptr, false);

		Deactivate(0.5f);
	}
	else
	{
		InitializePersistentElements();
		InitializePreviewGameplayCue();

		//Set expiration timer if we have an specific duration
		if (Duration.LifeSpan > 0.f)
		{
			//substract the prediction time for server actors.
			if (ShouldPredict())
			{
				Duration.LifeSpan -= GetPredictionDeltaTime();
			}

			InitExpirationTimer();
		}

		//Activates tick if neccesary. The tick starts along the duration timer basically, because most tick things are interpolations that are related to collision actor duration.
		if (RequiresTick())
		{
			SetActorTickEnabled(true);
		}
	}

	//Invalid world, we must deactivate.
	if (!GetWorld())
	{
		UE_LOG(CollisionActorLog, Log, TEXT("ABaseCollisionActor::FinishActivate: World was invalidated for %s."), *IndividualData.AbilityClass.Get()->GetFName().ToString());
		Deactivate();
	}
}

void ABaseCollisionActor::Deactivate(float PoolingDelay)
{
	if (bActive)
	{
		UE_LOG(CollisionActorLog, Log, TEXT("Deactivated %s"), *GetFName().ToString());
		OnCollisionActorDeactivate.Broadcast(this);

		if (ShouldSendMultihitEventOnDeactivation() && GetInstigatorBaseAbilitySystemComponent())
		{
			TArray<TWeakObjectPtr<AActor>>* TotalTargets = GetInstigatorBaseAbilitySystemComponent()->GetSharedTargets(IndividualData.ActivationKey, GetIsReplicated());

			if (TotalTargets && TotalTargets->Num())
			{
				int32 NumTargets = TotalTargets->Num();

				if (NumTargets > 0)
				{
					FGameplayEventData Payload;
					Payload.EventMagnitude = NumTargets;
					Payload.Instigator = GetInstigator() != nullptr ? GetInstigator() : GetOwner();
					Payload.Target = nullptr;
					Payload.InstigatorTags = OwningAbilityTags;
					Payload.ContextHandle = GetEffectContext();
					Payload.OptionalObject = this;
					SendGameplayEvent(GetInstigatorAbilitySystemComponent(), UGlobalTags::Event_MultiHit(), Payload);
				}
			}
		}

		//Send Collision Actor Deactivated Event
		if (GetIsReplicated())
		{
			//Send hit events	
			FGameplayEventData Payload;
			Payload.EventMagnitude = 1;
			Payload.Instigator = GetInstigator() != nullptr ? GetInstigator() : GetOwner();
			Payload.InstigatorTags = OwningAbilityTags;
			Payload.OptionalObject = this;
			Payload.ContextHandle = GetEffectContext();
			SendGameplayEvent(GetInstigatorAbilitySystemComponent(), UGlobalTags::Event_CollisionActorDeactivate(), Payload);
		}

		SoftUnregisterSharedTargetInstance();//Soft unregister. Hard unregister after the pooling. But this allows us to know when to send multihit event.
		SetActorEnableCollision(false);
		UnbindShapeCallbacks();
		UninitializeTarget();
		UninitializeAttachToActor();
		SetActorTickEnabled(false);
		RemoveGameplayCues();

		//Clear local target references
		PreviousTargetedActors.Empty();
		PreviousInteractableActors.Empty();

		//Clear height interpolation values.
		ClearHeightInterpolationData();
				
		bInterpolatingScale = false;
		bSkipVariableInitialization = false;
		bPreactivated = false;
		bSynched = false;
		bSkipGameplayCues = false;

		//Remove bind if needed.
		if (GetInstigatorBaseAbilitySystemComponent())
		{
			if (GetInstigatorBaseAbilitySystemComponent()->CollisionActorSharedData.OnSharedDataAdded.IsAlreadyBound(this, &ABaseCollisionActor::OnSharedDataReplicatedBack))
			{
				GetInstigatorBaseAbilitySystemComponent()->CollisionActorSharedData.OnSharedDataAdded.RemoveDynamic(this, &ABaseCollisionActor::OnSharedDataReplicatedBack);
			}
		}
		else
		{
			UE_LOG(CollisionActorLog, Warning, TEXT("ABaseCollisionActor::Deactivate: Invalid Base ASC for %s"), *GetFName().ToString())
		}
		

		// End timeline components
		TInlineComponentArray<UTimelineComponent*> TimelineComponents(this);
		for (UTimelineComponent* Timeline : TimelineComponents)
		{
			if (Timeline)
			{
				// May be too spammy, but want to call visibility to this. Maybe make this editor only?
				if (Timeline->IsPlaying())
				{
					UE_LOG(CollisionActorLog, Warning, TEXT("Collision Actor %s had active timelines when it was recycled."), *GetName());
				}

				Timeline->SetPlaybackPosition(0.f, false, false);
				Timeline->Stop();
			}
		}

		//Clear latent actions
		UWorld* MyWorld = GetWorld();
		if (MyWorld)
		{
			GetWorld()->GetTimerManager().ClearAllTimersForObject(this);

			if (MyWorld->GetLatentActionManager().GetNumActionsForObject(this))
			{
				//May be too spammy, but want ot call visibility to this. Maybe make this editor only?
				UE_LOG(CollisionActorLog, Warning, TEXT("Collision Actor %s has active latent actions (Delays, etc) when it was recycled."), *GetName());
			}

			//End latent actions
			MyWorld->GetLatentActionManager().RemoveActionsForObject(this);

			if (PoolingDelay == 0.f)
			{
				MyWorld->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &ABaseCollisionActor::PoolCollisionActor));
			}
			else
			{
				FTimerHandle PoolTimerHandle;
				MyWorld->GetTimerManager().SetTimer(PoolTimerHandle, this, &ABaseCollisionActor::PoolCollisionActor, PoolingDelay, false, PoolingDelay);
			}
		}
		else
		{
			Destroy();
		}

		bActive = false;
	}
}

bool ABaseCollisionActor::IsCollisionActorActive() const
{
	return bActive;
}

bool ABaseCollisionActor::ShouldSendMultihitEventOnDeactivation() const
{
	return (Duration.Period <= 0 || bDiscreteCollisionChecks) && GetIsReplicated() && GetSharedTargetSoftRegisteredAmount() <= 1;
}

void ABaseCollisionActor::Expire()
{
	UE_LOG(CollisionActorLog, Log, TEXT("%s has expired."), *GetName());

	OnCollisionActorExpired.Broadcast(this);

	//Deactivate if period timer is invalid, otherwise it will deactivate on last period.
	if (!AreaPeriodTimerHandle.IsValid() || (AreaPeriodTimerHandle.IsValid() && GetWorld() && !GetWorld()->GetTimerManager().IsTimerActive(AreaPeriodTimerHandle)))
	{
		Deactivate(.25f);
	}
}

void ABaseCollisionActor::InitExpirationTimer()
{
	if (OwningAbilityTags.HasTag(UGlobalTags::Ability_DisableExpiration()))
	{
		return;
	}

	ClearExpirationTimer();

	if (GetWorld())
	{
		GetWorld()->GetTimerManager().SetTimer(DurationTimerHandle, this, &ABaseCollisionActor::Expire, Duration.LifeSpan, false, Duration.LifeSpan);
	}
}

void ABaseCollisionActor::ClearExpirationTimer()
{
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(DurationTimerHandle);
	}
}

void ABaseCollisionActor::Interpolate(float Delta)
{
	if (bInterpolatingScale)
	{		
		float RelativeElapsedTime = GetNormalizedElapsedTime();

		if (RelativeElapsedTime <= 1.f)
		{
			SetCollisionActorScale(CalculateActorScale(RelativeElapsedTime));
		}
		else
		{
			bInterpolatingScale = false;
		}
	}	
}

float ABaseCollisionActor::GetNormalizedElapsedTime() const
{
	if (Duration.LifeSpan == 0.f)
	{
		return 0;
	}

	return abs(UKismetSystemLibrary::GetGameTimeInSeconds(this) - StartTime) / Duration.LifeSpan;
}

void ABaseCollisionActor::AdjustTransform()
{
	//Since we spawn all actors at once, for cases with activation time, that uses actor locations, the spawn location can change if the actor is moving, this corrects that.
	switch (IndividualData.LocationType)
	{
	case ECollisionActorSpawnLocationType::LiteralLocation:
		SetActorLocation(GetActorLocation() + FVector(0, 0, 1));
		break;
	case ECollisionActorSpawnLocationType::UseInstigator:
		SetActorLocation(GetActorBoneSocketLocation(GetInstigator() != nullptr ? GetInstigator() : GetOwner(), Targeting.SpawnBoneName), false, nullptr, ETeleportType::ResetPhysics);
		break;
	case ECollisionActorSpawnLocationType::UseTarget:
		if (IndividualData.TargetActor)
		{
			SetActorLocation(GetActorBoneSocketLocation(IndividualData.TargetActor, Targeting.SpawnBoneName), false, nullptr, ETeleportType::ResetPhysics);
		}/*
		else
		{
			SetActorLocation(IndividualData.TargetLocation, false, nullptr, ETeleportType::ResetPhysics);
		}		*/
		break;
	default:
		break;
	}
}

void ABaseCollisionActor::SetStartLocation()
{
	StartLocation = GetActorLocation();

	if (HasAuthority())
	{
		//Set spawn location as the Context Origin. This allow us to make calculations using distance to origin, distance travelled, etc.
		UBPL_AbilitySystem::AddOriginPointToContainerEffectContext(EffectContainerSpec, StartLocation);
	}
}

void ABaseCollisionActor::InitializeScale()
{
	bInterpolatingScale = ScaleInterpolation.ScaleCurve != nullptr;	
	CachedAdditiveScale = GetBaseAdditiveScale(SharedData.AbilityLevel);
	FVector NewScale = bInterpolatingScale ? FVector(ScaleInterpolation.ScaleCurve->GetVectorValue(0.f) * ScaleInterpolation.ScaleCurveMultiplier + CachedAdditiveScale) : FVector(1);
	NewScale += CachedAdditiveScale; //Apply additive scale
	NewScale *= SharedData.AreaMultiplier; //Apply AOE multiplier to scale.
	SetCollisionActorScale(NewScale);
}

FVector ABaseCollisionActor::GetBaseAdditiveScale_Implementation(int32 AbilityLevel) const
{
	return FVector::ZeroVector;
}

FVector ABaseCollisionActor::CalculateActorScale(float RelativeElapsedTime) const
{
	FVector CalculatedScale = ScaleInterpolation.ScaleCurve != nullptr ? ScaleInterpolation.ScaleCurve->GetVectorValue(RelativeElapsedTime) * ScaleInterpolation.ScaleCurveMultiplier : FVector(1);
	CalculatedScale += CachedAdditiveScale;
	CalculatedScale *= SharedData.AreaMultiplier;
	return  CalculatedScale;
}

float ABaseCollisionActor::CalculateScaledRadius(float RelativeElapsedTime) const
{
	return CalculateActorScale(RelativeElapsedTime).X * ShapeComp->Bounds.SphereRadius / GetActorScale().X;
}

FVector ABaseCollisionActor::GetCollisionActorScaleByLifetime(float InTime, int32 InLevel) const
{
	FVector Scale = FVector(1);

	if (ScaleInterpolation.IsValid())
	{
		Scale = ScaleInterpolation.Evaluate(InTime);
	}
	else
	{
		Scale = GetActorScale3D();
	}

	Scale += GetBaseAdditiveScale(InLevel);
	return Scale;
}

void ABaseCollisionActor::SetCollisionActorScale(FVector NewScale)
{ 
	SetActorScale3D(NewScale);
		
	//Update gameplay cue aswell, since they could be using scale
	if (bActorGameplayCueInitialized)
	{
		FGameplayCueParameters CueParams;
		GetDefaultGameplayCueParams(CueParams);
		
		//Context is only valid on the server, so we cannot pass it along for GCs
		GetGameplayCueManager()->HandleGameplayCue(this, ActorGameplayCue, EGameplayCueEvent::WhileActive, CueParams);
	}
}

void ABaseCollisionActor::InitializeRotationInterpolation()
{	
	if (GetWorld())
	{
		//Setup rotation complete timer, based on the rotation speed.
		float RotationTime = 360.f / (RotationInterpolation.RotationRate >= 0 ? RotationInterpolation.RotationRate : RotationInterpolation.RotationRate * -1);
			GetWorld()->GetTimerManager().SetTimer(ClearTargetsTimerHandle, this, &ABaseCollisionActor::OnRotationCompleted, RotationTime, true, RotationTime);

			//Rotation Prediction. Server catch up setup.
			float DeltaT = GetPredictionDeltaTime();
			if (ShouldPredict() && !FMath::IsNearlyZero(DeltaT, 0.05f) && Duration.LifeSpan >= 3 * DeltaT)
			{				
				float DegreeDiff = DeltaT * RotationInterpolation.RotationRate;
				float SyncTime = DegreeDiff / ((PredictionRotationRateMultiplier - 1) * RotationInterpolation.RotationRate);
				RotationInterpolation.RotationRate *= PredictionRotationRateMultiplier;

				//Set Sync timer
				GetWorld()->GetTimerManager().SetTimer(RotationSyncTimerHandle, this, &ABaseCollisionActor::OnRotationSynced, SyncTime, false, SyncTime);
			}
	}
}

void ABaseCollisionActor::OnRotationCompleted()
{
	UE_LOG(CollisionActorLog, Log, TEXT("ABaseCollisionActor::OnCollisionActorRotationCompleted: %s completed a full rotation."), *GetFName().ToString());
	OnCollisionActorRotationCompleted.Broadcast(this);
}

void ABaseCollisionActor::OnRotationSynced()
{
	//Restore rotation speed to normal.
	RotationInterpolation.RotationRate *= 1 / PredictionRotationRateMultiplier;
}

void ABaseCollisionActor::InterpolateRotation(float DeltaSeconds)
{
	AddActorWorldRotation(FRotator(0, DeltaSeconds * RotationInterpolation.RotationRate, 0));
}

void ABaseCollisionActor::InterpolateHeightToMatchFloor(float DeltaSeconds, float DesiredHeight, float InterpSpeed)
{
	//Adjust location Z value to match terrain shape.
	FVector CurrentLocation = GetShapeComponent()->GetComponentLocation();
	FHitResult WorldStaticHit = FHitResult();
	if (GetWorld() && GetWorld()->LineTraceSingleByObjectType(WorldStaticHit, CurrentLocation + FVector(0, 0, 1000), CurrentLocation + FVector(0, 0, -1000), ECollisionChannel::ECC_WorldStatic))
	{
		//Use landscape hit, if the diference is too big, this means it's a very tall asset, most likely a tree and we want to skip it.	
		FHitResult LandscapeHit = FHitResult();
		GetWorld()->LineTraceSingleByChannel(LandscapeHit, CurrentLocation + FVector(0, 0, 1000), CurrentLocation + FVector(0, 0, -1000), ECollisionChannel::ECC_GameTraceChannel1);
		if (!LandscapeHit.IsValidBlockingHit())
		{
			return;
		}

		float ImpactPointZ = abs(WorldStaticHit.ImpactPoint.Z - LandscapeHit.ImpactPoint.Z) > 200.f ? LandscapeHit.ImpactPoint.Z : WorldStaticHit.ImpactPoint.Z;
		
		//Calculate distance using the hit result
		float NewZ = FMath::FInterpTo(CurrentLocation.Z, ImpactPointZ + DesiredHeight, DeltaSeconds, InterpSpeed);
	
		PreviousInterpZValues.Add(NewZ);

		//keep 7 values. This amount produces a good enough interpolation.
		if (PreviousInterpZValues.Num() > 7)
		{
			PreviousInterpZValues.RemoveAt(0);
		}
		
		float Sum = 0.f;
		for (auto& it : PreviousInterpZValues)
		{
			Sum += it;
		}

		float AverageZ = Sum / PreviousInterpZValues.Num();

		SetActorLocation(FVector(GetActorLocation().X, GetActorLocation().Y, AverageZ));
		//DrawDebugPoint(GetWorld(), GetShapeComponent()->GetComponentLocation(), 5.f, FColor::Red, false, 1.f, 0);
	}
}

void ABaseCollisionActor::ClearHeightInterpolationData()
{
	PreviousInterpZValues.Empty();
}

AActor* ABaseCollisionActor::GetAttachTarget_Implementation() const
{
	return IndividualData.TargetActor;
}

void ABaseCollisionActor::InitializeAttachToActor()
{
	bAttached = GetAttachTarget() != nullptr;
	bUpdateAttachmentOnTick = false;

	if (bAttached)
	{
		if (AttachmentType == ECollisionActorAttachmentType::LocationAndRotation)
		{
			AttachToComponent(GetAttachTarget()->GetRootComponent(), FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		}
		else
		{
			bUpdateAttachmentOnTick = true;
		}
	}
}

void ABaseCollisionActor::UninitializeAttachToActor()
{
	if (bAttached)
	{
		if (AttachmentType == ECollisionActorAttachmentType::LocationAndRotation)
		{
			DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		}

		bAttached = false;
		bUpdateAttachmentOnTick = false;
	}
}

void ABaseCollisionActor::UpdateAttachment(float DeltaSeconds)
{
	if (bAttached && GetAttachTarget() && AttachmentType == ECollisionActorAttachmentType::Location)
	{
		SetActorLocation(GetAttachTarget()->GetActorLocation(), true);
	}
	else
	{
		bUpdateAttachmentOnTick = false;
	}
}

void ABaseCollisionActor::InitializePersistentElements()
{	
	InitializeAttachToActor();
	InitializeTarget();

	//For interpolating scale, we want to force a period and threat it like periodic, to avoid constant collision checks for performance reasons.
	bDiscreteCollisionChecks = false;
	if (bInterpolatingScale && Duration.Period <= 0.f)
	{
		Duration.Period = Duration.LifeSpan / 5.f; //Make sure to at least do 5 periods.
		Duration.Period = FMath::Min(Duration.Period, 0.15f); //Make sure the period is at least 0.15f so the collision still feels continuous.
		bDiscreteCollisionChecks = true;
	}	

	if (Duration.Period > 0.f )
	{
		bAllowRetargetting = !bDiscreteCollisionChecks;
		ExecutedPeriods = 0;
		SetActorEnableCollision(true);
		MaximumPeriodsToExecute = FMath::TruncToInt((Duration.LifeSpan - Duration.FirstPeriodDelay) / Duration.Period) + 1;
		
		if (GetWorld())
		{
			GetWorld()->GetTimerManager().SetTimer(AreaPeriodTimerHandle, this, &ABaseCollisionActor::OnAreaOfEffectPeriod, Duration.Period, true, Duration.FirstPeriodDelay);
		}		
		else
		{
			UE_LOG(CollisionActorLog, Log, TEXT("ABaseCollisionActor::InitializePersistentElements: World was invalidated for %s."), *IndividualData.AbilityClass.Get()->GetFName().ToString());
			Deactivate();
		}
	}
	else
	{
		//Bind before enabling collision so we overlap something and can react through overlap events.
		BindShapeCallbacks();
		SetActorEnableCollision(true);
	}
}

void ABaseCollisionActor::BindShapeCallbacks()
{
	if (!ShapeComp->OnComponentBeginOverlap.IsAlreadyBound(this, &ABaseCollisionActor::OnBeginOverlap))
	{
		ShapeComp->OnComponentBeginOverlap.AddDynamic(this, &ABaseCollisionActor::OnBeginOverlap);
	}

	if (!ShapeComp->OnComponentEndOverlap.IsAlreadyBound(this, &ABaseCollisionActor::OnEndOverlap))
	{
		ShapeComp->OnComponentEndOverlap.AddDynamic(this, &ABaseCollisionActor::OnEndOverlap);
	}
}

void ABaseCollisionActor::UnbindShapeCallbacks()
{
	if (ShapeComp->OnComponentBeginOverlap.IsAlreadyBound(this, &ABaseCollisionActor::OnBeginOverlap))
	{
		ShapeComp->OnComponentBeginOverlap.RemoveDynamic(this, &ABaseCollisionActor::OnBeginOverlap);
	}

	if (ShapeComp->OnComponentEndOverlap.IsAlreadyBound(this, &ABaseCollisionActor::OnEndOverlap))
	{
		ShapeComp->OnComponentEndOverlap.RemoveDynamic(this, &ABaseCollisionActor::OnEndOverlap);
	}
}

void ABaseCollisionActor::InitializeTarget()
{	
}

void ABaseCollisionActor::UninitializeTarget()
{	
	IndividualData.TargetActor = nullptr;
}

void ABaseCollisionActor::OnAreaOfEffectPeriod()
{
	//DrawDebugSphere(GetWorld(), GetActorLocation(), GetShapeComponent()->Bounds.SphereRadius, 12, FColor::Green, false, 3.f, 0.f, 3.f);
	
	//We dont clear targets here, periodic AOEs can retarget local previous targets.
	TArray<AActor*> OverlappingActors;
	ShapeComp->GetOverlappingActors(OverlappingActors);

	//Apply effects and send multihit event.
	ApplyEffectToActorArray(OverlappingActors, nullptr, !bDiscreteCollisionChecks);
	
	//GameplayCues are already executed on finish activate, we want to skip the first tick if it happens on start.
	if (!bDiscreteCollisionChecks && (!(Duration.FirstPeriodDelay == 0.f && ExecutedPeriods == 0)))
	{		
		ExecuteGameplayCues();		
	}	
	
	ExecutedPeriods++;

	//Either interrupt here or wait for expiration to make sure we completed all periods.
	if (GetWorld() && ExecutedPeriods >= MaximumPeriodsToExecute)
	{
		GetWorld()->GetTimerManager().ClearTimer(AreaPeriodTimerHandle);
		
		if (!DurationTimerHandle.IsValid() || (DurationTimerHandle.IsValid() && !GetWorld()->GetTimerManager().IsTimerActive(DurationTimerHandle)))
		{
			Deactivate(.25f);
		}
	}
}

void ABaseCollisionActor::OnBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	if (IsValidInteractableActor(OtherActor, bFromSweep ? SweepResult.ImpactPoint : OtherComp->GetComponentLocation()))
	{
		ApplyActorInteraction(OtherActor, OtherComp, SweepResult);
		return;
	}

	if (IsValidTargetActor(OtherActor))
	{
		UE_LOG(CollisionActorLog, Log, TEXT("%s Overlapped %s"), *GetFName().ToString(), *OtherActor->GetFName().ToString());

		//We calculate the hit instead of using the sweep one, because that one is over the capsule surface, and we want to have a hit result on the mesh.
		FHitResult OutHit = FHitResult();	
		TArray<FHitResult> HitArray;

		if (GetWorld()) 
		{
			GetWorld()->LineTraceMultiByObjectType(HitArray, GetActorLocation(), OtherActor->GetActorLocation(), OtherComp->GetCollisionObjectType());
			if (HitArray.Num() > 0)
			{
				OutHit = HitArray[HitArray.Num() - 1];
			}
			else
			{
				UE_LOG(CollisionActorLog, Warning, TEXT("ABaseCollisionActor::OnBeginOverlap: %s Failed to get a valid hit result"), *GetNameSafe(this));
			}
		}

		ApplyEffectToActor(OtherActor, OutHit);
	}
}

void ABaseCollisionActor::OnEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
	UE_LOG(CollisionActorLog, Log, TEXT("End Overlap %s"), *OtherActor->GetFName().ToString());

	if (bAppliesPersistentEffects)
	{
		if (GetPreviousTargets() && GetPreviousTargets()->Contains(OtherActor))
		{
			RemoveAppliedPersistentEffects(OtherActor);
			RemovePreviousTarget(OtherActor);
		}
	}
}

bool ABaseCollisionActor::ApplyEffectToActor(AActor * A, const FHitResult& ContextHitResult, FGameplayTagContainer* ContextTags)
{
	if (!GetWorld())
	{
		return false;
	}

	//Apply effect on the authority
	if (HasAuthority() && EffectContainerSpec.HasValidEffects() && GetInstigatorAbilitySystemComponent())
	{
		UBPL_AbilitySystem::AddHitToContainerEffectContext(EffectContainerSpec, ContextHitResult);

		for (auto& it : EffectContainerSpec.TargetGameplayEffectSpecs)
		{
			GetInstigatorAbilitySystemComponent()->ApplyGameplayEffectSpecToTarget(*it.Data.Get(), UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(A));
		}			
	}

	//Avoid 2x event on predicting client.
	if (GetIsReplicated())
	{
		FGameplayEventData Payload;
		Payload.EventMagnitude = 1;
		Payload.Instigator = GetInstigator() != nullptr ? GetInstigator() : GetOwner();
		Payload.InstigatorTags = OwningAbilityTags;
		if (ContextTags && ContextTags->Num())
		{
			Payload.InstigatorTags.AppendTags(*ContextTags);
		}

		Payload.Target = A;
		UAbilitySystemComponent* TargetASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(A);
		if (TargetASC)
		{
			TargetASC->GetOwnedGameplayTags(Payload.TargetTags);
		}
		
		Payload.ContextHandle = GetEffectContext();
		Payload.OptionalObject = this;
			
		SendGameplayEvent(GetInstigatorAbilitySystemComponent(), UGlobalTags::Event_Hit(), Payload);
		SendGameplayEvent(UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(A), UGlobalTags::Event_Hit(), Payload);
	}

	//Send Gameplay Cues if possible
	if (CanExecuteGameplayCue() && !HitTargetGameplayCues.IsEmpty())
	{
		//Context is only available in the server, send hit result instead.
		FGameplayCueParameters CueParams;
		GetDefaultGameplayCueParams(CueParams);
		GetImpactLocationForGameplayCues(A, CueParams.Location, CueParams.Normal);
		CueParams.PhysicalMaterial = ContextHitResult.PhysMaterial;
		CueParams.EffectCauser = A;
		if (ContextTags && ContextTags->Num())
		{
			CueParams.AggregatedSourceTags.AppendTags(*ContextTags);
		}

		for (const FGameplayTag& Tag : HitTargetGameplayCues)
		{
			GetGameplayCueManager()->HandleGameplayCue(A, Tag, EGameplayCueEvent::Executed, CueParams);
		}
	}

	//Save as already targeted.
	AddPreviousTarget(A);

	return true;
}

int32 ABaseCollisionActor::ApplyEffectToActorArray(const TArray<AActor*>& A, FGameplayTagContainer* ContextTags,bool bSendMultiHitEvent)
{
	int32 Amount = 0;

	if (A.Num() > 0 && GetWorld())
	{
		if (HasAuthority())
		{
			TArray<FHitResult> TraceHits;
			TraceHits.Empty();

			for (auto& CurrentActor : A)
			{
				GetWorld()->LineTraceMultiByObjectType(TraceHits, GetActorLocation(), CurrentActor->GetActorLocation(), ECollisionChannel::ECC_Pawn);

				FHitResult ActorHitResult = TraceHits.Num() > 0 ? TraceHits.Last() : FHitResult();

				if (IsValidTargetActor(CurrentActor))
				{
					ApplyEffectToActor(CurrentActor, ActorHitResult, ContextTags);
					Amount++;
				}
			}

			//Send multihit event to instigator.
			if (bSendMultiHitEvent && GetIsReplicated() && Amount > 0)
			{	
				FGameplayEventData Payload;
				Payload.EventMagnitude = Amount;
				Payload.Instigator = GetInstigator() != nullptr ? GetInstigator() : GetOwner();
				Payload.Target = nullptr;
				Payload.InstigatorTags = OwningAbilityTags;
				if (ContextTags && ContextTags->Num())
				{
					Payload.InstigatorTags.AppendTags(*ContextTags);
				}
				Payload.ContextHandle = GetEffectContext();
				Payload.OptionalObject = this;
				SendGameplayEvent(GetInstigatorAbilitySystemComponent(), UGlobalTags::Event_MultiHit(), Payload);
			}
		}

		//Handle destructibles and physic objects
		if (GetNetMode() != ENetMode::NM_DedicatedServer)
		{		
			for (auto& Actor : A)
			{
				if (IsValidInteractableActor(Actor, Actor->GetActorLocation()))
				{
					ApplyActorInteraction(Actor, nullptr, FHitResult());					
				}
			}
		}
	}

	return Amount;
}

bool ABaseCollisionActor::ApplyActorInteraction(AActor* A, UPrimitiveComponent* OverlappedComponent, const FHitResult& Hit)
{
	if (A && GetNetMode() != ENetMode::NM_DedicatedServer && bActive)
	{	
		ISplineManagerInterface::Execute_ApplyDamageToDestructibleSplineComponent(A, this, OverlappedComponent, Hit);		
		AddPreviousInteractableTarget(A);
		return true;		
	}

	return false;
}

int32 ABaseCollisionActor::RemoveAppliedPersistentEffects(AActor* Actor)
{
	int32 Amount = 0;

	if (bAppliesPersistentEffects)
	{
		UAbilitySystemComponent* InternalTargetASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Actor);

		if (InternalTargetASC)
		{
			TArray<FActiveGameplayEffectHandle> ActiveHandles = InternalTargetASC->GetActiveEffects(FGameplayEffectQuery());

			for (auto& it : ActiveHandles)
			{
				if (it.IsValid())
				{
					const FActiveGameplayEffect* AGE = InternalTargetASC->GetActiveGameplayEffect(it);
					if (AGE && AGE->Spec.Def->DurationPolicy == EGameplayEffectDurationType::Infinite)
					{
						FGameplayEffectContextHandle ContextHandle = InternalTargetASC->GetEffectContextFromActiveGEHandle(it);
						if (ContextHandle.GetEffectCauser() == this)
						{
							InternalTargetASC->RemoveActiveGameplayEffect(it);
							Amount++;
						}
					}
				}
			}
		}
	}

	return Amount;
}

bool ABaseCollisionActor::TransferPersistentEffects(AActor* Target)
{
	//if we are not the only collision actor overlapping this target, other one should take care of applying the effect.
	if (bAppliesPersistentEffects && !OwningAbilityTags.HasTag(UGlobalTags::Ability_Targeting_IndividualTargeting()))
	{
		//Get overlapping actors of the same class as this one, that overlaps target to remove and have the same key and apply the effect from them.
		TArray<AActor*> OverlappingActors;
		Target->GetOverlappingActors(OverlappingActors, GetClass());

		for (auto& it : OverlappingActors)
		{
			if (it && it != this)
			{
				ABaseCollisionActor* CA = Cast<ABaseCollisionActor>(it);
				if (CA && CA->IndividualData.ActivationKey == IndividualData.ActivationKey)
				{
					//Transfer the effect. Does not check for valid target, it should be valid.
					CA->ApplyEffectToActor(Target, FHitResult());
					return true;
				}
			}
		}
	}

	return false;
}

FGameplayEffectContextHandle ABaseCollisionActor::GetEffectContext() const
{
	if (EffectContainerSpec.HasValidEffects())
	{
		return EffectContainerSpec.GetEffectContext();
	}

	if (GetInstigatorAbilitySystemComponent())
	{
		FGameplayEffectContextHandle Context = GetInstigatorAbilitySystemComponent()->MakeEffectContext();
		Context.SetAbility(IndividualData.MainModifierAbilityClass ? IndividualData.MainModifierAbilityClass.GetDefaultObject() : IndividualData.AbilityClass.GetDefaultObject());
		return Context;
	}

	return FGameplayEffectContextHandle();
}

FGameplayEffectContainerSpec& ABaseCollisionActor::GetEffectContainerSpec()
{
	return EffectContainerSpec;
}

void ABaseCollisionActor::SetEffectContainerSpec(const FGameplayEffectContainerSpec& NewSpec)
{
	if (GetIsReplicated() && HasAuthority())
	{
		EffectContainerSpec = NewSpec;
		UBPL_AbilitySystem::SetInstigatorAndEffectCauserToContainerEffectContext(EffectContainerSpec, GetInstigator() != nullptr ? GetInstigator() : GetOwner(), this);
	}
}

FTargetVisualization ABaseCollisionActor::GetTargetingVisualRepresentation_Implementation(FGameplayTagContainer AbilityTags) const
{
	return FTargetVisualization();
}

bool ABaseCollisionActor::IsValidTargetActor(AActor* Actor)
{
	bool bValid = false;

	if (Actor)
	{
		//Is the target being targetted by this actor or others that share the target?
		if (!bAllowRetargetting && IsAlreadyTargeted(Actor))
		{
			UE_LOG(CollisionActorLog, Verbose, TEXT("ABaseCollisionActor::IsValidTargetActor: Already targeted %s by"), *Actor->GetFName().ToString(), *GetFName().ToString());
			return false;
		}

		//Has target priority. Multiple AOE with shared targeting need to decide if they can target the actor or not.
		if (!HasTargetPriority(Actor))
		{
			UE_LOG(CollisionActorLog, Verbose, TEXT("ABaseCollisionActor::IsValidTargetActor: %s has no target priority over %s."), *GetFName().ToString(), *Actor->GetFName().ToString());
			return false;
		}

		//Requires LOS.
		if (Targeting.bValidTargetRequiresCollisionActorLineOfSight && !HasLineOfSightToTarget(Actor))
		{
			UE_LOG(CollisionActorLog, Verbose, TEXT("ABaseCollisionActor::IsValidTargetActor: No LOS to %s from %s"), *Actor->GetFName().ToString(), *GetFName().ToString());
			return false;
		}

		//Distance check (inner radius)
		if (!IsTargetInMinimalDistance(Actor))
		{
			UE_LOG(CollisionActorLog, Verbose, TEXT("ABaseCollisionActor::IsValidTargetActor: %s failed to pass inner radius check for %s"), *Actor->GetFName().ToString(), *GetFName().ToString());
			return false;
		}

		//Direction check (half angle span for cone shapes).
		if (!IsTargetBetweenAngleDeviation(Actor))
		{
			UE_LOG(CollisionActorLog, Verbose, TEXT("ABaseCollisionActor::IsValidTargetActor: %s failed to pass angle span check for %s"), *Actor->GetFName().ToString(), *GetFName().ToString());
			return false;
		}

		bValid = Filter.FilterPassesForActor(Actor);			
	}

	UE_LOG(CollisionActorLog, Log, TEXT("ABaseCollisionActor::IsValidTargetActor: %s is %s target"), Actor ? *Actor->GetFName().ToString() : TEXT("Invalid Actor"), bValid ? TEXT("valid") : TEXT("not valid"));

	return bValid;
}

bool ABaseCollisionActor::IsValidInteractableActor(AActor* Actor, FVector ImpactPoint)
{
	bool bValid = false;

	if (Actor)
	{
		//Requires Destructible interface.
		if (!Actor->GetClass()->ImplementsInterface(USplineManagerInterface::StaticClass()))
		{
			UE_LOG(CollisionActorLog, Verbose, TEXT("ABaseCollisionActor::IsValidInteractableActor: %s does not implement destructible interface"), *Actor->GetFName().ToString());
			return false;
		}

		//Is the target being targetted by this actor or others that share the target?
		if (!bAllowRetargetting && IsInteractableActorAlreadyTargeted(Actor))
		{
			UE_LOG(CollisionActorLog, Verbose, TEXT("ABaseCollisionActor::IsValidInteractableActor: Already targeted %s by"), *Actor->GetFName().ToString(), *GetFName().ToString());
			return false;
		}

		//Distance check (inner radius)
		if (!IsLocationInMinimalDistance(ImpactPoint))
		{
			UE_LOG(CollisionActorLog, Verbose, TEXT("ABaseCollisionActor::IsValidInteractableActor: %s failed to pass inner radius check for %s"), *Actor->GetFName().ToString(), *GetFName().ToString());
			return false;
		}

		//Direction check (half angle span for cone shapes).
		if (!IsLocationBetweenAngleDeviation(ImpactPoint))
		{
			UE_LOG(CollisionActorLog, Verbose, TEXT("ABaseCollisionActor::IsValidInteractableActor: %s failed to pass angle deviation check for %s"), *Actor->GetFName().ToString(), *GetFName().ToString());
			return false;
		}

		bValid = true;
	}

	UE_LOG(CollisionActorLog, Log, TEXT("ABaseCollisionActor::IsValidTargetActor: %s is %s target"), Actor ? *Actor->GetFName().ToString() : TEXT("Invalid Actor"), bValid ? TEXT("valid") : TEXT("not valid"));
	return bValid;
}

bool ABaseCollisionActor::IsAlreadyTargeted(AActor* Target)
{
	return GetPreviousTargets() && GetPreviousTargets()->Contains(Target);
}

bool ABaseCollisionActor::IsInteractableActorAlreadyTargeted(AActor* Actor) const
{
	return PreviousInteractableActors.Contains(Actor);
}

bool ABaseCollisionActor::HasTargetPriority(AActor* Target) const
{
	//We only care for priority when we apply persistent effects. This is mostly designed for AOEs with persistent effects that overlap.
	if (!bAppliesPersistentEffects)
	{
		return true;
	}

	//Individual targeting always has priority
	if (OwningAbilityTags.HasTag(UGlobalTags::Ability_Targeting_IndividualTargeting()))
	{
		return true;
	}

	//Get overlapping actors of the same class as this one, that overlaps target to remove and have the same key and apply the effect from them.
	TArray<AActor*> OverlappingActors;
	Target->GetOverlappingActors(OverlappingActors, GetClass());

	AActor* LowestIndexActor = nullptr;
	uint8 LowestIndex = MAX_uint8;

	for (auto& it : OverlappingActors)
	{
		if (it)
		{
			ABaseCollisionActor* CA = Cast<ABaseCollisionActor>(it);
			
			if (CA && CA->IndividualData.SpawnIndex < LowestIndex)
			{
				LowestIndexActor = it;
			}
		}
	}

	//Lowest spawn index overlapping actor will have priority.
	return LowestIndexActor == this;
}

bool ABaseCollisionActor::HasLineOfSightToTarget(AActor* Target) const
{	
	if (!GetWorld())
	{
		return false;
	}

	//Add an offset to avoid the trace to immediately hit the terrain for actors that are in the same height as the floor.
	FVector Offset = FVector(0, 0, FMath::Max(UTargetFunctionLibrary::GetDistanceToFloor(this, GetActorLocation()), 45.f));

	FHitResult TraceHit;
	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(Target);

	GetWorld()->LineTraceSingleByChannel(TraceHit, GetActorLocation() + Offset, Target->GetActorLocation(), ECollisionChannel::ECC_Visibility, QueryParams, FCollisionResponseParams::DefaultResponseParam);

	return !TraceHit.bBlockingHit;
}

bool ABaseCollisionActor::HasLineOfSightToLocation(FVector Location) const
{
	if (!GetWorld())
	{
		return false;
	}

	//Add an offset to avoid the trace to immediately hit the terrain for actors that are in the same height as the floor.
	FVector Offset = FVector(0, 0, FMath::Max(UTargetFunctionLibrary::GetDistanceToFloor(this, GetActorLocation()), 45.f));

	FHitResult TraceHit;
	FCollisionQueryParams QueryParams;

	GetWorld()->LineTraceSingleByChannel(TraceHit, GetActorLocation() + Offset, Location, ECollisionChannel::ECC_Visibility, QueryParams, FCollisionResponseParams::DefaultResponseParam);

	return !TraceHit.bBlockingHit;
}

bool ABaseCollisionActor::IsTargetInMinimalDistance(AActor* Target) const
{
	float MinimumDistance = GetMinimumDistanceRequired();
	if (MinimumDistance > 0.f)
	{
		//Subtract capsule size to the minimum distance required.
		ACharacter* Char = Cast<ACharacter>(Target);
		if (Char)
		{
			MinimumDistance -= Char->GetCapsuleComponent()->GetScaledCapsuleRadius();
		}

		//If inner radius is greater than capsule, it means it could be possible for the actor to be in the inner ring without overlapping. Otherwise is always overlapping.
		if (MinimumDistance > 0.f)
		{
			if (FVector::Dist2D(GetActorLocation(), Target->GetActorLocation()) < MinimumDistance)
			{			
				return false;
			}
		}
	}

	return true;
}

bool ABaseCollisionActor::IsLocationInMinimalDistance(FVector Location) const
{
	float MinimumDistance = GetMinimumDistanceRequired();
	if (MinimumDistance > 0.f)
	{
		return FVector::Dist2D(GetActorLocation(), Location) < MinimumDistance;
	}

	return true;
}

bool ABaseCollisionActor::IsTargetBetweenAngleDeviation(AActor* Target) const
{
	float MaximumDeviation = GetMaximumDirectionDeviation();
	if (MaximumDeviation < 180.f)
	{
		//Compensation for capsule size
		float Compensation = 0.f;
		ACharacter* Char = Cast<ACharacter>(Target);
		if (Char)
		{				
			float DistanceToTarget = FVector::Dist2D(GetActorLocation(), Target->GetActorLocation());
			if (DistanceToTarget != 0.f)
			{
				float CapsuleRadius = Char->GetCapsuleComponent()->GetScaledCapsuleRadius();
				Compensation = UKismetMathLibrary::DegAtan((CapsuleRadius / DistanceToTarget));
			}		
		}

		FRotator DirectionRotator = UKismetMathLibrary::FindLookAtRotation(GetActorLocation(), Target->GetActorLocation());
		if (abs(DirectionRotator.Yaw - GetActorRotation().Yaw) > MaximumDeviation + Compensation)
		{			
			return false;
		}
	}

	return true;
}

bool ABaseCollisionActor::IsLocationBetweenAngleDeviation(FVector Location) const
{
	float MaximumDeviation = GetMaximumDirectionDeviation();
	if (MaximumDeviation < 180.f)
	{
		//Compensation for capsule size
		FRotator DirectionRotator = UKismetMathLibrary::FindLookAtRotation(GetActorLocation(), Location);
		if (abs(DirectionRotator.Yaw - GetActorRotation().Yaw) > MaximumDeviation)
		{
			return false;
		}
	}

	return true;
}

float ABaseCollisionActor::GetMinimumDistanceRequired() const
{	
	return Targeting.MinimumDistanceRequired * GetActorScale3D().X;
}

float ABaseCollisionActor::GetMinimumDistanceRequiredByLifetime(float InTime, int32 InLevel) const
{
	return Targeting.MinimumDistanceRequired * GetCollisionActorScaleByLifetime(InTime, InLevel).X;
}

float ABaseCollisionActor::GetMaximumDirectionDeviation() const
{
	return FMath::Clamp(Targeting.bScaleMaximumDirectionDeviation ? Targeting.MaximumDirectionDeviation * GetActorScale3D().X : Targeting.MaximumDirectionDeviation, 0.f ,180.f);
}

float ABaseCollisionActor::GetMaximumDirectionDeviationByLifetime(float InTime, int32 InLevel) const
{
	return FMath::Clamp(Targeting.bScaleMaximumDirectionDeviation ? Targeting.MaximumDirectionDeviation * GetCollisionActorScaleByLifetime(InTime, InLevel).X : Targeting.MaximumDirectionDeviation, 0.f, 180.f);
}

TArray<TWeakObjectPtr<AActor>>* ABaseCollisionActor::GetPreviousTargets()
{
	//Shared Targetting goes through the ASC.
	if (!OwningAbilityTags.HasTag(UGlobalTags::Ability_Targeting_IndividualTargeting()) && GetInstigatorBaseAbilitySystemComponent())
	{	
		return GetInstigatorBaseAbilitySystemComponent()->GetSharedTargets(IndividualData.ActivationKey, GetIsReplicated());				
	}
	
	return GetLocalPreviousTargets();	
}

TArray<TWeakObjectPtr<AActor>>* ABaseCollisionActor::GetLocalPreviousTargets()
{
	return &PreviousTargetedActors;
}

TArray<AActor*> ABaseCollisionActor::GetPreviousTargetsHardReference()
{
	TArray<AActor*> OutValue;

	if (GetPreviousTargets())
	{
		for (auto iter(GetPreviousTargets()->CreateIterator()); iter; iter++)
		{
			if (iter->IsValid())
			{
				OutValue.AddUnique(iter->Get());
			}
		}
	}	

	return OutValue;
}

void ABaseCollisionActor::RegisterSharedTargetInstance()
{	
	if (!bRegisteredTargetInstance && !bSoftRegisteredTargetInstance )// && !OwningAbilityTags.HasTag(UGlobalTags::Ability_Targeting_IndividualTargeting()))
	{
		if (GetInstigatorBaseAbilitySystemComponent())
		{
			GetInstigatorBaseAbilitySystemComponent()->RegisterCollisionActorForSharedTargeting(IndividualData.ActivationKey, 1, GetIsReplicated());
			bRegisteredTargetInstance = true;
			bSoftRegisteredTargetInstance = true;
		}
		else
		{
			UE_LOG(CollisionActorLog, Warning, TEXT("ABaseCollisionActor::RegisterSharedTargetInstance : Invalid Base ASC for %s."), *GetName());
		}
	}
}

void ABaseCollisionActor::UnregisterSharedTargetInstance()
{
	if (bRegisteredTargetInstance)
	{
		if (GetInstigatorBaseAbilitySystemComponent())
		{
			GetInstigatorBaseAbilitySystemComponent()->UnregisterCollisionActorForSharedTargeting(IndividualData.ActivationKey, 1, GetIsReplicated());
			bRegisteredTargetInstance = false;
		}
		else
		{
			UE_LOG(CollisionActorLog, Warning, TEXT("ABaseCollisionActor::UnregisterSharedTargetInstance : Invalid Base ASC for %s."), *GetName());
		}
	}
}

void ABaseCollisionActor::SoftUnregisterSharedTargetInstance()
{
	if (bSoftRegisteredTargetInstance)
	{
		if (GetInstigatorBaseAbilitySystemComponent())
		{
			GetInstigatorBaseAbilitySystemComponent()->SoftUnregisterCollisionActorForSharedTargeting(IndividualData.ActivationKey, 1, GetIsReplicated());
			bSoftRegisteredTargetInstance = false;
		}
		else
		{
			UE_LOG(CollisionActorLog, Warning, TEXT("ABaseCollisionActor::SoftUnregisterSharedTargetInstance : Invalid Base ASC for %s."), *GetName());
		}
	}
}

int32 ABaseCollisionActor::GetSharedTargetRegisteredAmount() const
{
	if (bRegisteredTargetInstance)
	{
		if (GetInstigatorBaseAbilitySystemComponent())
		{
			return GetInstigatorBaseAbilitySystemComponent()->GetRegisteredCollisionActorsAmount(IndividualData.ActivationKey, GetIsReplicated());
		}
	}

	return 0;
}

int32 ABaseCollisionActor::GetSharedTargetSoftRegisteredAmount() const
{
	if (bSoftRegisteredTargetInstance)
	{
		if (GetInstigatorBaseAbilitySystemComponent())
		{
			return GetInstigatorBaseAbilitySystemComponent()->GetSoftRegisteredCollisionActorsAmount(IndividualData.ActivationKey, GetIsReplicated());
		}
	}

	return 0;
}

void ABaseCollisionActor::AddPreviousTarget(AActor* TargetToAdd)
{
	if (TargetToAdd)
	{				
		if (GetInstigatorBaseAbilitySystemComponent())
		{
			GetInstigatorBaseAbilitySystemComponent()->AddSharedTarget(IndividualData.ActivationKey, TargetToAdd, GetIsReplicated());
		}	

		//This is done in both cases, because it allows to track what targets were targeted by this actor.
		PreviousTargetedActors.AddUnique(TargetToAdd);		
	}	
}

void ABaseCollisionActor::AddPreviousTargets(TArray<TWeakObjectPtr<AActor>>& TargetsToAdd)
{
	if (TargetsToAdd.Num() > 0)
	{				
		if (GetInstigatorBaseAbilitySystemComponent())
		{
			GetInstigatorBaseAbilitySystemComponent()->AddSharedTargets(IndividualData.ActivationKey, TargetsToAdd, GetIsReplicated());
		}

		PreviousTargetedActors.Append(TargetsToAdd);
	}	
}

void ABaseCollisionActor::RemovePreviousTarget(AActor* TargetToRemove)
{	
	GetInstigatorBaseAbilitySystemComponent()->RemoveSharedTarget(IndividualData.ActivationKey, TargetToRemove, GetIsReplicated());
	if (!TransferPersistentEffects(TargetToRemove))
	{
		//We remove the target if we cannot find a new CA that continues the effect.
		PreviousTargetedActors.Remove(TargetToRemove);
	}
	else
	{
		PreviousTargetedActors.Remove(TargetToRemove);
	}
}

void ABaseCollisionActor::ClearPreviousTargets()
{
	if (PreviousTargetedActors.Num() > 0)
	{	
		//Local copy to avoid changing array elements while looping.
		TArray<TWeakObjectPtr<AActor>> LocalTargets = PreviousTargetedActors;
		
		for (auto iter(LocalTargets.CreateIterator()); iter; iter++)
		{
			RemovePreviousTarget(iter->Get());
		}
	}
}

void ABaseCollisionActor::AddPreviousInteractableTarget(AActor* TargetToAdd)
{
	PreviousInteractableActors.Add(TargetToAdd);
}

void ABaseCollisionActor::SetInRecycleQueue_Implementation(bool NewValue)
{
	bInRecycleQueue = NewValue;
}

bool ABaseCollisionActor::IsInRecycleQueue_Implementation() const
{
	return bInRecycleQueue;
}

bool ABaseCollisionActor::Recycle_Implementation()
{
	return true;
}

void ABaseCollisionActor::ReuseAfterRecycle_Implementation()
{	
	//This is so IsNetRelevantFor() start to consider this actor for replication.
	//bPreactivated = true;
}

void ABaseCollisionActor::PoolCollisionActor()
{
	/**
	*	Wait one frame, so it is still valid for other actors of the same activation to check for shared targets.
	*	This is important for instant AOES because they activate, and destroy before others activate, losing the allready targeted actors.
	*	If one frame is not enough we may need to keep the actor alive for a longer period of time before pooling and unregistering it from targets.	
	*/
	UnregisterSharedTargetInstance();

	//Remove data from replicated array.
	if (HasAuthority() && GetInstigatorBaseAbilitySystemComponent() != nullptr)
	{
		GetInstigatorBaseAbilitySystemComponent()->CollisionActorIndividualData.Items.Remove(IndividualData);
		GetInstigatorBaseAbilitySystemComponent()->CollisionActorIndividualData.MarkArrayDirty();
		GetInstigatorBaseAbilitySystemComponent()->CollisionActorSharedData.DecreaseSharedDataCounter(SharedData.ID, GetWorldTime());
	}

	InstigatorASC = nullptr;
	InstigatorBaseASC = nullptr;

	IndividualData = FCollisionActorIndividualData();
	SharedData = FCollisionActorSharedData();

	//Destroy if we could not use the pool. Give time for VFX to finish.
	if (GetWorld())
	{
		if (GetIsReplicated())
		{
			SetNetDormancy(ENetDormancy::DORM_DormantAll);

			if (HasAuthority() && GetNetMode() != ENetMode::NM_Client)
			{
				UMyAbilitySystemGlobals* MyASG = Cast<UMyAbilitySystemGlobals>(IGameplayAbilitiesModule::Get().GetAbilitySystemGlobals());
				if (MyASG)
				{
					MyASG->GetActorPoolManager()->NotifyPooledActorFinished(this);
					return;
				}
			}
		}
	
		SetLifeSpan(5.f);	
	}
	else
	{
 		Destroy();
	}
}

bool ABaseCollisionActor::CanExecuteGameplayCue() const
{
	return GetNetMode() != ENetMode::NM_DedicatedServer && !bSkipGameplayCues;
}

void ABaseCollisionActor::HandleGameplayCueEvent(FGameplayTag CueTag, EGameplayCueEvent::Type EventType)
{
	FGameplayCueParameters CueParams;
	GetDefaultGameplayCueParams(CueParams);
	GetGameplayCueManager()->HandleGameplayCue(this, CueTag, EventType, CueParams);
}

UGameplayCueManager* ABaseCollisionActor::GetGameplayCueManager()
{
	if (!GameplayCueManager)
	{
		GameplayCueManager = UAbilitySystemGlobals::Get().GetGameplayCueManager();
	}

	return GameplayCueManager;
}

void ABaseCollisionActor::GetDefaultGameplayCueParams(FGameplayCueParameters& Params)
{
	Params = FGameplayCueParameters();
	Params.GameplayEffectLevel = GetMinimumDistanceRequired();
	Params.RawMagnitude = GetMaximumDirectionDeviation();
	Params.EffectCauser = this;
	Params.Location = GetActorLocation();
	Params.TargetAttachComponent = GetShapeComponent();
	Params.Instigator = GetInstigator() != nullptr ? GetInstigator() : GetOwner();
	Params.AggregatedSourceTags.AppendTags(OwningAbilityTags);
	Params.SourceObject = IndividualData.TargetActor;
}

void ABaseCollisionActor::GetPreviewGameplayCueParams(FGameplayCueParameters& Params) const
{
	//InnerRadius
	Params.GameplayEffectLevel = FMath::Max(GetMinimumDistanceRequiredByLifetime(0, SharedData.AbilityLevel), GetMinimumDistanceRequiredByLifetime(1, SharedData.AbilityLevel));
	
	//Half Angle
	Params.RawMagnitude = FMath::Max(GetMaximumDirectionDeviationByLifetime(0, SharedData.AbilityLevel), GetMaximumDirectionDeviationByLifetime(1, SharedData.AbilityLevel));
	if (Targeting.bScaleMaximumDirectionDeviation)
	{
		Params.RawMagnitude *= SharedData.AreaMultiplier;
	}

	//Scaled extent
	FBoxSphereBounds Bounds = GetShapeComponent()->CalcLocalBounds();
	FVector Scale = GetCollisionActorScaleByLifetime(0, SharedData.AbilityLevel);
	Params.Normal.X = Bounds.BoxExtent.X * Scale.X * SharedData.AreaMultiplier;
	Params.Normal.Y = Bounds.BoxExtent.Y * Scale.Y * SharedData.AreaMultiplier;
	Params.Normal.Z = Bounds.BoxExtent.Z * Scale.Z * SharedData.AreaMultiplier;
	Params.AbilityLevel = GetActorRotation().Yaw;

	//General data
	Params.Location = GetActorLocation();
	Params.Instigator = GetInstigator() != nullptr ? GetInstigator() : GetOwner();	
}

bool ABaseCollisionActor::GetImpactLocationForGameplayCues(AActor* HitActor, FVector& Location, FVector& Normal) const
{
	//try to find a point close to the mesh for gameplay cues.
	ACharacter* OtherChar = Cast<ACharacter>(HitActor);
	if (OtherChar)
	{
		FClosestPointOnPhysicsAsset PointFound = FClosestPointOnPhysicsAsset();
		if (OtherChar->GetMesh()->GetClosestPointOnPhysicsAsset(GetShapeComponent()->GetComponentLocation(), PointFound, true))
		{
			Location = PointFound.ClosestWorldPosition;
			Normal = PointFound.Normal;
			return true;
		}
	}

	return false;
}

FGameplayTag ABaseCollisionActor::GetPreactivationGameplayCue() const
{
	if (!PreactivationGameplayCue.IsValid())
	{
		//Override GC for trap and mines if needed.
		if (OwningAbilityTags.HasTag(UGlobalTags::Ability_Device_Mine()))
		{
			return UGlobalTags::GameplayCue_Mine();
		}

		if (OwningAbilityTags.HasTag(UGlobalTags::Ability_Device_Trap()))
		{
			return UGlobalTags::GameplayCue_Trap();
		}
	}	

	return PreactivationGameplayCue;
}

void ABaseCollisionActor::ExecuteGameplayCues()
{
	if (CanExecuteGameplayCue())
	{
		FGameplayCueParameters CueParams;
		GetDefaultGameplayCueParams(CueParams);

		if (BurstGameplayCue.IsValid())
		{
			GetGameplayCueManager()->HandleGameplayCue(this, BurstGameplayCue, EGameplayCueEvent::Executed, CueParams);
		}
		else
		{
			UE_LOG(CollisionActorLog, Log, TEXT("No Burst GameplayCue Tag set for %s"), *GetName());
		}

		if (bActorGameplayCueInitialized)
		{
			GetGameplayCueManager()->HandleGameplayCue(this, ActorGameplayCue, EGameplayCueEvent::Executed, CueParams);
		}
	}
}

void ABaseCollisionActor::InitializeActorGameplayCue()
{
	if (CanExecuteGameplayCue() && !bActorGameplayCueInitialized && ActorGameplayCue.IsValid() && Duration.LifeSpan != 0)
	{
		if (CanExecuteGameplayCue())
		{
			FGameplayCueParameters CueParams;
			GetDefaultGameplayCueParams(CueParams);
			GetGameplayCueManager()->HandleGameplayCue(this, ActorGameplayCue, EGameplayCueEvent::OnActive, CueParams);
			bActorGameplayCueInitialized = true;
		}

		bExecuteDeactivationCue = true;
	}
}

void ABaseCollisionActor::InitializePreactivationGameplayCue()
{
	if (CanExecuteGameplayCue() && !bPreactivationGameplayCueInitialized && PreactivationGameplayCue.IsValid())
	{
		FGameplayCueParameters CueParams;
		GetDefaultGameplayCueParams(CueParams);
		GetGameplayCueManager()->HandleGameplayCue(this, GetPreactivationGameplayCue(), EGameplayCueEvent::OnActive, CueParams);
		bPreactivationGameplayCueInitialized = true;				
	}
}

void ABaseCollisionActor::InitializePreviewGameplayCue()
{
	if (CanExecuteGameplayCue() && !bPreviewGameplayCueInitialized && PreviewGameplayCue.IsValid() && Duration.LifeSpan != 0)
	{
		if (CanExecuteGameplayCue())
		{
			FGameplayCueParameters CueParams;
			GetPreviewGameplayCueParams(CueParams);
			GetGameplayCueManager()->HandleGameplayCue(this, PreviewGameplayCue, EGameplayCueEvent::OnActive, CueParams);
			bPreviewGameplayCueInitialized = true;
		}

		bPreviewGameplayCueInitialized = true;
	}
}

void ABaseCollisionActor::RemovePreactivationGameplayCue()
{
	if (bPreactivationGameplayCueInitialized)
	{
		FGameplayCueParameters CueParams;
		GetDefaultGameplayCueParams(CueParams);
		GetGameplayCueManager()->HandleGameplayCue(this, GetPreactivationGameplayCue(), EGameplayCueEvent::Removed, CueParams);
		bPreactivationGameplayCueInitialized = false;
	}
}

void ABaseCollisionActor::RemovePreviewGameplayCue()
{
	if (bPreviewGameplayCueInitialized)
	{
		GetGameplayCueManager()->HandleGameplayCue(this, PreviewGameplayCue, EGameplayCueEvent::Removed, FGameplayCueParameters());
		bPreviewGameplayCueInitialized = false;
	}
}

void ABaseCollisionActor::RemoveGameplayCues()
{
	FGameplayCueParameters CueParams;
	GetDefaultGameplayCueParams(CueParams);

	if (bExecuteDeactivationCue && CanExecuteGameplayCue())
	{
		if (DeactivationGameplayCue.IsValid())
		{				
			GetGameplayCueManager()->HandleGameplayCue(this, DeactivationGameplayCue, EGameplayCueEvent::Executed, CueParams);			
		}
		else
		{
			UE_LOG(CollisionActorLog, Log, TEXT("No DeactivationGameplayCue Tag set for %s"), *GetName());
		}
	}	

	if (bActorGameplayCueInitialized)
	{
		GetGameplayCueManager()->HandleGameplayCue(this, ActorGameplayCue, EGameplayCueEvent::Removed, CueParams);
		bActorGameplayCueInitialized = false;
	}

	RemovePreviewGameplayCue();

	bExecuteDeactivationCue = true;
}

void ABaseCollisionActor::ResetParticleSystems() const
{
	//Reset particle systems. This avoids the trails from last used location to new location. Must be done after setting the actor to be seen again.
	TArray<UParticleSystemComponent*> ParticleComponents;
	GetComponents(ParticleComponents);
	for (UParticleSystemComponent*& it : ParticleComponents)
	{
		it->ForceReset();
	}

	//Reset Niagara systems aswell.
	TArray<UNiagaraComponent*> NiagaraComponents;
	GetComponents(NiagaraComponents);
	for (UNiagaraComponent*& it : NiagaraComponents)
	{
		it->ResetSystem();
	}
}


UShapeComponent* ABaseCollisionActor::GetShapeComponent() const
{
	return ShapeComp;
}

UAbilitySystemComponent* ABaseCollisionActor::GetInstigatorAbilitySystemComponent() const
{
	return InstigatorASC;
}

UBaseAbilitySystemComponent* ABaseCollisionActor::GetInstigatorBaseAbilitySystemComponent() const
{
	return InstigatorBaseASC;
}

void ABaseCollisionActor::SetSourceAbilitySystemComponent()
{
	InstigatorASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(GetInstigator() != nullptr ? GetInstigator() : GetOwner());
	InstigatorBaseASC = Cast<UBaseAbilitySystemComponent>(InstigatorASC);
}

void ABaseCollisionActor::SendGameplayEvent(UAbilitySystemComponent* InASC, FGameplayTag EventTag, const FGameplayEventData& Payload)
{
	if (InASC)
	{
		FScopedPredictionWindow NewScopedWindow(InASC, true);
		InASC->HandleGameplayEvent(EventTag, &Payload);
	}
}

bool ABaseCollisionActor::ShouldPredict()
{	 
	return GetInstigatorBaseAbilitySystemComponent()->IsCollisionActorPredictionEnabled() && !bAbilityFromListenServer && GetIsReplicated() && !bSynched;
}

float ABaseCollisionActor::GetPredictionDeltaTime() const
{	
	if (GetInstigator())
	{
		APlayerController* PC = Cast<APlayerController>(GetInstigator()->GetController());
		if (PC)
		{
			return PC->PlayerState->ExactPing * 0.001f;
		}
	}	

	return 0.f;	
}

bool ABaseCollisionActor::IsServerWorldTimeAvailable() const
{
	UWorld* World = GetWorld();
	check(World);

	AGameStateBase* GameState = World->GetGameState();
	return (GameState != nullptr);
}

float ABaseCollisionActor::GetServerWorldTime() const
{
	UWorld* World = GetWorld();
	if (World)
	{
		AGameStateBase* GameState = World->GetGameState();
		if (GameState)
		{
			return GameState->GetServerWorldTimeSeconds();
		}

		return World->GetTimeSeconds();
	}
	
	return 0.f;
}

float ABaseCollisionActor::GetWorldTime() const
{
	UWorld* World = GetWorld();
	if (World)
	{
		return World->GetTimeSeconds();
	}

	return 0.f;
}
