// Copyright 2024 Marchetti S. César A. All Rights Reserved.

#include "AbilitySystem/Abilities/BaseOverlapAbility.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystem/Targeting/TargetFunctionLibrary.h"
#include "AbilitySystem/AttributeSets/AbilityAttributeSet.h"
#include "AbilitySystem/AbilitySystemComponents/BaseAbilitySystemComponent.h"
#include "AbilitySystem/GlobalTags.h"
#include "Kismet/KismetSystemLibrary.h"
#include "AbilitySystemGlobals.h"
#include "AbilitySystem/BPL_AbilitySystem.h"
#include "AbilitySystem/Targeting/TargetTypes.h"

int32 ShowOverlapDebug = 0;
static FAutoConsoleVariableRef CVarEnableOverlapDebug(TEXT("AbilitySystem.ShowOverlapDebug"), ShowOverlapDebug, TEXT("Draw debug lines to show the overlap events. Values are 0 or 1."), ECVF_Default);

UBaseOverlapAbility::UBaseOverlapAbility() : Super()
{
	bRetriggerInstancedAbility = true;
}

void UBaseOverlapAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	//Make sure to restart queues for retriggereable abilities. The new activation might have stopped the queues from the previous one.
	if (InstancingPolicy == EGameplayAbilityInstancingPolicy::InstancedPerActor && bRetriggerInstancedAbility)
	{
		RestartQueues();
	}
}

bool UBaseOverlapAbility::CanActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, OUT FGameplayTagContainer* OptionalRelevantTags) const
{
	const bool CanActivate = Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags);

	if (!CanActivate)
	{
		return false;
	}

	if (InstancingPolicy == EGameplayAbilityInstancingPolicy::InstancedPerActor && bRetriggerInstancedAbility)
	{
		if (IsActive())
		{
			if (const bool bCannotRetrigger = HasActiveTask(NAME_None))
			{
				return false;
			}
		}		
	}

	return true;
}

FGameplayAbilityTargetDataHandle UBaseOverlapAbility::GetMoveToTargetData(const FGameplayAbilityTargetDataHandle& InTargetData) const
{
	FGameplayAbilityTargetDataHandle Handle;

	FTargetInformation TargetInformation;
	GetTargetInformationFromTargetData(TargetInformation, InTargetData);

	if (TargetInformation.TargetActor)
	{
		Handle = InTargetData;
	}
	else
	{
		FVector SourceLocation = TargetInformation.GetStartLocation(AbilityTags, GetAvatarActorFromActorInfo());
		FVector TargetLocation = TargetInformation.GetEndLocation(AbilityTags, GetAvatarActorFromActorInfo());


		if (bClampTargetToRange && GetAbilityRange() > 0.f)
		{	
			FVector SourceTargetDir = SourceLocation - TargetLocation;			
			SourceTargetDir = SourceTargetDir.GetClampedToSize(0.f, GetAbilityRange());		
			SourceLocation = TargetLocation + SourceTargetDir;
		}

		if (!bMoveIntoLineOfSight)
		{
			FHitResult TraceHit;

			GetWorld()->LineTraceSingleByChannel(TraceHit, SourceLocation, TargetLocation, ECollisionChannel::ECC_Visibility);

			if (TraceHit.bBlockingHit)
			{
				TargetLocation = TraceHit.ImpactPoint;
			}
		}

		FGameplayAbilityTargetingLocationInfo TargetLocationInfo = FGameplayAbilityTargetingLocationInfo();
		TargetLocationInfo.LocationType = EGameplayAbilityTargetingLocationType::LiteralTransform;
		TargetLocationInfo.LiteralTransform = FTransform(SourceLocation);

		Handle = UAbilitySystemBlueprintLibrary::AbilityTargetDataFromLocations(TargetLocationInfo, TargetLocationInfo);
	}

	return Handle;
}

float UBaseOverlapAbility::GetAnimMontageLengthExtension() const
{
	const float InternalSpawnDelay = GetSpawnDelay(GetAbilityLevel());
	if (!AbilityTags.HasTag(UGlobalTags::Ability_SpawnBatch()))
	{
		if (InternalSpawnDelay > 0.f)
		{
			const int32 InternalOverlapAmount = GetOverlapAmount(GetAbilityLevel());
			if (InternalOverlapAmount > 1)
			{
				return (InternalOverlapAmount - 1) * InternalSpawnDelay;
			}
		}
	}
	return 0.0f;
}

void UBaseOverlapAbility::GetTargetContext(const FTargetInformation& TargetInfo, FTargetContext& Context) const
{
	Super::GetTargetContext(TargetInfo, Context);

	Context.TagMagnitudes.Add(UGlobalTags::Ability_Targeting_Context_MinAngleSpan(), GetMinAngleSpan(GetAbilityLevel()));
	Context.TagMagnitudes.Add(UGlobalTags::Ability_Targeting_Context_MaxAngleSpan(), GetMaxAngleSpan(GetAbilityLevel()));
	Context.TagMagnitudes.Add(UGlobalTags::Ability_Targeting_Context_TargetAmount(), GetOverlapAmount(GetAbilityLevel()));
	Context.TagMagnitudes.Add(UGlobalTags::Ability_Targeting_Context_DesiredFloorDistance(), UTargetFunctionLibrary::GetDistanceToFloor(GetAvatarActorFromActorInfo(), GetAvatarActorFromActorInfo()->GetActorLocation()));
}

void UBaseOverlapAbility::GetVisualizationParams_Implementation(TMap<FString, float>& Params) const
{
	const FVector InitialExtent = GetAreaBoundsByLifeTime(0, true);
	const FVector FinalExtent = GetAreaBoundsByLifeTime(1, true);
	const FVector Extent = InitialExtent.Size() > FinalExtent.Size() ? InitialExtent : FinalExtent;
	Params.Add("OuterRadius", Extent.X);
	Params.Add("InnerRadius", GetMinimumTargetDistanceToCenterRequired(1));
	Params.Add("HalfAngle", GetMaximumAngleDeviationBetweenTargetAndOverlap(1));
	Params.Add("Width", Extent.X);
	Params.Add("Length", Extent.Y);		
	Params.Add("TargetAmount", GetOverlapAmount(GetAbilityLevel()));

	if (GetTargetDistribution())
	{
		GetTargetDistribution().GetDefaultObject()->GetVisualizationParams(Params);
	}
}

FTargetVisualization UBaseOverlapAbility::GetVisualRepresentation_Implementation() const
{	
	FTargetVisualization Visualization = FTargetVisualization();
	switch (Shape)
	{
	case EOverlapAbilityShape::Sphere:
		Visualization.DecalMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/Decals/M_Decal_Circle_Gradient.M_Decal_Circle_Gradient"));
		break;
	case EOverlapAbilityShape::Box:
		Visualization.DecalMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/Decals/M_Decal_Square_Gradient.M_Decal_Square_Gradient"));
		break;
	default:
		break;
	}

	ensureMsgf(Visualization.DecalMaterial, TEXT("UBaseOverlapAbility::GetVisualRepresentation_Implementation: Could not find decal material for visualization"));
	Visualization.DecalLocation = EVisualizationPlacementLocation::Source;
	return Visualization;
}

float UBaseOverlapAbility::GetBaseRadius_Implementation() const
{
	return ShapeExtent.X;
}

float UBaseOverlapAbility::GetAbilityRadius() const
{
	float InternalRadius = Super::GetAbilityRadius();
	if (HasScaleInterp())
	{ 
		InternalRadius *= ScaleInterpolation.Evaluate(1).X;
	}

	return InternalRadius;
}

FVector UBaseOverlapAbility::GetAreaBoundsByLifeTime(float NormalizedLifeTime,  bool bScaleWithAttributes) const
{
	FVector OutExtent = ShapeExtent;

	if (bScaleWithAttributes)
	{
		const float InternalAreaMultiplier = bScaleWithAttributes ? ScaleValueWithAttribute(1.f, UAbilityAttributeSet::GetAreaOfEffectAttribute()) : 1.f;
		OutExtent *= InternalAreaMultiplier;
	}

	if (HasScaleInterp())
	{
		const FVector Scale = ScaleInterpolation.Evaluate(NormalizedLifeTime);
		OutExtent.X *= Scale.X;
		OutExtent.Y *= Scale.Y;
		OutExtent.Z *= Scale.Z;
	}

	return OutExtent;
}

bool UBaseOverlapAbility::ShouldEndAbilityWhenQueueIsEmpty_Implementation() const
{		
	return !HasActiveTask(NAME_None);
}

bool UBaseOverlapAbility::HasScaleInterp() const
{
	return ScaleInterpolation.IsValid() && Duration.LifeSpan > 0.f;
}

FVector UBaseOverlapAbility::GetScaleByTime(float NormalizedTime) const
{
	return HasScaleInterp() ? ScaleInterpolation.Evaluate(FMath::Clamp(NormalizedTime, 0.f, 1.f)) : FVector(1);
}

int32 UBaseOverlapAbility::GetInterpSteps() const
{
	return FMath::Max(5, Duration.LifeSpan/.15);
}

void UBaseOverlapAbility::ExpandOverlapEvent(FOverlapEventSnapshot& Event, TArray<FOverlapEventSnapshot>& ExpandedEvent) const
{
	const float InternalActivationTime = Event.ActivationTime;
	const int32 Steps = GetInterpSteps();
	const float StepLength = Duration.LifeSpan / Steps;
	for (int32 i = 0; i <= Steps; i++)
	{
		Event.ActivationTime = InternalActivationTime + i * StepLength;
		ExpandedEvent.Add(Event);
	}
}

void UBaseOverlapAbility::GeneratePeriodicOverlapEvents(const FGameplayEventData& Payload, int32 EventID, TArray<FOverlapEventSnapshot>& GeneratedEvents) const
{
	if (Duration.LifeSpan <= 0 || Duration.Period <= 0.f || Duration.FirstPeriodDelay > Duration.LifeSpan)
	{
		return;
	}

	FEventSnapshottedPeriodicAttributes Attributes;
	ProcessEventPeriodicAttributes(Payload, EventID, Attributes);	
	const int32 NumPeriods = FMath::TruncToInt((Attributes.LifeSpan - Attributes.FirstPeriodDelay) / Attributes.Period) + 1;

	TArray<FOverlapEventSnapshot> InitialEvents = GeneratedEvents;
	GeneratedEvents.Empty();
	GeneratedEvents.Reserve(InitialEvents.Num() * NumPeriods);
		
	for (auto& it : InitialEvents)
	{
		it.ActivationTime += Attributes.FirstPeriodDelay;
	}
	GeneratedEvents.Append(InitialEvents);
	float AddedTime = Attributes.FirstPeriodDelay;

	while (AddedTime < Attributes.LifeSpan - Attributes.FirstPeriodDelay)
	{
		for (auto& it : InitialEvents)
		{
			it.ActivationTime += Attributes.Period;
		}

		GeneratedEvents.Append(InitialEvents);
		AddedTime += Attributes.Period;
	}
}

int32 UBaseOverlapAbility::GetBaseOverlapAmount_Implementation(int32 AbilityLevel) const
{
	return 1;
}

int32 UBaseOverlapAbility::GetOverlapAmount(int32 AbilityLevel) const
{
	if (AbilityTags.HasTag(UGlobalTags::Ability_DisableMultipleSpawn()))
	{
		return 1;
	}

	return ScaleValueWithAttribute(GetBaseOverlapAmount(AbilityLevel), UAbilityAttributeSet::GetTargetAmountAttribute());
}

float UBaseOverlapAbility::GetBaseSpawnDelay_Implementation(int32 AbilityLevel) const
{
	return 0.0f;
}

float UBaseOverlapAbility::GetSpawnDelay(int32 AbilityLevel) const
{
	return ScaleValueWithAttribute(GetBaseSpawnDelay(AbilityLevel), UAbilityAttributeSet::GetSpawnDelayAttribute());
}

float UBaseOverlapAbility::GetBaseMinAngleSpan_Implementation(int32 AbilityLevel) const
{
	return FMath::Clamp(8.f * GetOverlapAmount(AbilityLevel), 0.f, 45.f);
}

float UBaseOverlapAbility::GetBaseMaxAngleSpan_Implementation(int32 AbilityLevel) const
{
	return FMath::Clamp(45.f * GetOverlapAmount(AbilityLevel), 90.f, 180.f);
}

float UBaseOverlapAbility::GetMinAngleSpan(int32 AbilityLevel) const
{
	if (AbilityTags.HasTag(UGlobalTags::Ability_DisableAngleSpanModifiers()))
	{
		return GetBaseMinAngleSpan(AbilityLevel);
	}

	return ScaleValueWithAttribute(GetBaseMinAngleSpan(AbilityLevel), UAbilityAttributeSet::GetMinimumTargetAngleSpanAttribute());
}

float UBaseOverlapAbility::GetMaxAngleSpan(int32 AbilityLevel) const
{
	if (AbilityTags.HasTag(UGlobalTags::Ability_DisableAngleSpanModifiers()))
	{
		return GetBaseMaxAngleSpan(AbilityLevel);
	}
	
	return ScaleValueWithAttribute(GetBaseMaxAngleSpan(AbilityLevel), UAbilityAttributeSet::GetMaximumTargetAngleSpanAttribute());
}

float UBaseOverlapAbility::GetBaseMinimumTargetDistanceToCenterRequired(float InLifetime) const
{
	float MinDist = MinimumDistanceFromCenterToTarget;

	if (HasScaleInterp())
	{
		MinDist *= ScaleInterpolation.Evaluate(InLifetime).X;
	}

	return MinDist;
}

float UBaseOverlapAbility::GetMinimumTargetDistanceToCenterRequired(float InLifetime) const
{
	const float MinDist = GetBaseMinimumTargetDistanceToCenterRequired(InLifetime);
	return ScaleValueWithAttribute(MinDist, UAbilityAttributeSet::GetAreaOfEffectAttribute());	
}

float UBaseOverlapAbility::GetBaseMaximumAngleDeviationBetweenTargetAndOverlap(float InLifetime) const
{
	float Deviation = MaximumAngleDeviationFromCenterToTarget;
	
	if (bScaleMaximumDirectionDeviationWithOverlapScale && HasScaleInterp())
	{
		Deviation *= ScaleInterpolation.Evaluate(InLifetime).X;
	}

	return FMath::Clamp(Deviation, 0.f, 180.f);
}

float UBaseOverlapAbility::GetMaximumAngleDeviationBetweenTargetAndOverlap(float InLifetime) const
{
	float Deviation = GetBaseMaximumAngleDeviationBetweenTargetAndOverlap(InLifetime);
	if (bScaleMaximumDirectionDeviationWithAreaAttributeModifiers)
	{
		Deviation = ScaleValueWithAttribute(Deviation, UAbilityAttributeSet::GetAreaOfEffectAttribute());
	}
	return FMath::Clamp(Deviation, 0.f, 180.f);
}

void UBaseOverlapAbility::OnQueueEmptied_Implementation()
{
	//meant for override in child classes if needed.
}

void UBaseOverlapAbility::OnOverlapEvent_Implementation(const FOverlapEventSnapshot& OverlapEventData)
{
	const FOverlapEventID ID = FOverlapEventID(OverlapEventData.EventID, OverlapEventData.OverlapID);
	const bool bPeriodic = Duration.Period > 0.f;
	if (bPeriodic)
	{
		RemoveTargets(OverlapEventData.EventID, /*OverlapEventData.OverlapID**/-1); //clear all targets from previous overlaps. This is needed for cases like miasma. If this requires to filter by overlapID, we can put an option for this so we can choose per ability.

		if (bExecuteGameplayCueOnEveryPeriod)
		{
			ExecutedGameplayCues.Remove(ID);
		}
	}

	const TArray<AActor*, FDefaultAllocator> IgnoreActors = GetIgnoredActors(OverlapEventData.EventID, AbilityTags.HasTag(UGlobalTags::Ability_Targeting_IndividualTargeting()) ? OverlapEventData.OverlapID : -1);
	TArray<AActor*, FDefaultAllocator> FilteredActors;
	const TArray<TEnumAsByte<EObjectTypeQuery>> Query{ EObjectTypeQuery::ObjectTypeQuery3 };
	
	const float ElapsedTime = OverlapEventData.ActivationTime - OverlapEventData.InitialEventTime;
	const float NormalizedElapsedTime = Duration.LifeSpan != 0 ? ElapsedTime / (Duration.LifeSpan * OverlapEventData.DurationMultiplier) : 1.f;
	FVector CurrentExtent = GetAreaBoundsByLifeTime(NormalizedElapsedTime, false);
	CurrentExtent *= OverlapEventData.AreaMultiplier;
	
	FRotator CurrentRotator = FRotator(0, OverlapEventData.YawRotation + RotationRate * ElapsedTime, 0);
	CurrentRotator.Normalize();	
	float CurrentYaw = CurrentRotator.Yaw;

	switch (Shape)
	{
	case EOverlapAbilityShape::Sphere:
		UKismetSystemLibrary::SphereOverlapActors(this, OverlapEventData.Location, CurrentExtent.X, Query, APawn::StaticClass(), IgnoreActors, FilteredActors);
		break;
	case EOverlapAbilityShape::Box:
		UBPL_AbilitySystem::RotatedBoxOverlapActors(this, OverlapEventData.Location, FRotator(0.f,CurrentYaw, 0.f), CurrentExtent, Query, APawn::StaticClass(), IgnoreActors, FilteredActors);
		break;
	default:
		break;
	}

	FGameplayTargetDataFilterHandle Filter = GetOverlapFilter();
	FilteredActors.RemoveAll([&Filter](AActor* it)
	{
		return !Filter.FilterPassesForActor(it);
	});

	if (bTargetRequiresLineOfSightToCenterLocation)
	{
		FilteredActors.RemoveAll([this, &OverlapEventData](AActor* it)
		{
			return !UTargetFunctionLibrary::HasLineOfSightToTarget(this, OverlapEventData.Location, it, this->GetAvatarActorFromActorInfo());
		});
	}

	const float MinDistance = GetBaseMinimumTargetDistanceToCenterRequired(NormalizedElapsedTime) * OverlapEventData.AreaMultiplier;
	if (MinDistance > 0.f)
	{
		FilteredActors.RemoveAll([MinDistance, &OverlapEventData](AActor* it)
		{
			return !UTargetFunctionLibrary::IsTargetInMinimalDistance(OverlapEventData.Location, it, MinDistance);
		});
	}

	const float AngleDeviation = GetBaseMaximumAngleDeviationBetweenTargetAndOverlap(NormalizedElapsedTime) * OverlapEventData.AreaMultiplier;
	if (AngleDeviation < 180.f)
	{
		FilteredActors.RemoveAll([AngleDeviation, CurrentYaw, &OverlapEventData](AActor* it)
		{
			return !UTargetFunctionLibrary::IsTargetBetweenAngleDeviation(OverlapEventData.Location, it, CurrentYaw, AngleDeviation);
		});
	}

	if (!FilteredActors.IsEmpty())
	{
		FGameplayEffectContainerSpec Spec = FGameplayEffectContainerSpec();
		GetContainerSpecCacheForEvent(OverlapEventData.EventID, Spec);		
		UBPL_AbilitySystem::AddOriginPointToContainerEffectContext(Spec, OverlapEventData.Location);
		
		FGameplayAbilityTargetData_ActorArray* NewData = new FGameplayAbilityTargetData_ActorArray();
		NewData->SourceLocation.LocationType = EGameplayAbilityTargetingLocationType::LiteralTransform;
		NewData->SourceLocation.LiteralTransform = FTransform(OverlapEventData.Location);
		NewData->TargetActorArray.Reset();
		NewData->TargetActorArray.Append(FilteredActors);
		FGameplayAbilityTargetDataHandle TargetData(NewData);	

		ApplyEffectContainerSpecTarget(Spec, TargetData);
		
		AddTargets(OverlapEventData.EventID, OverlapEventData.OverlapID, FilteredActors);

		FGameplayEventData Payload = FGameplayEventData();
		Payload.EventMagnitude = 1;
		Payload.ContextHandle = Spec.GetEffectContext();
		Payload.Instigator = GetAvatarActorFromActorInfo();
		Payload.InstigatorTags = AbilityTags;
		GetAbilitySystemComponentFromActorInfo()->GetOwnedGameplayTags(Payload.InstigatorTags);

		for (auto& Target : FilteredActors)
		{
			Payload.Target = Target;
			
			if (UAbilitySystemComponent* TargetASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Target))
			{
				TargetASC->GetOwnedGameplayTags(Payload.TargetTags);
				FScopedPredictionWindow NewScopedWindow(TargetASC, true);
				TargetASC->HandleGameplayEvent(UGlobalTags::Event_Hit(), &Payload);
			}

			SendGameplayEvent(UGlobalTags::Event_Hit(), Payload);
		}
	}

	if (bPeriodic)
	{
		SendMultihitEvent(OverlapEventData.EventID, FilteredActors.Num());
	}
	
	if (GameplayCueTag.IsValid() && !ExecutedGameplayCues.Contains(ID))
	{		
		FGameplayCueParameters Params = FGameplayCueParameters();
		Params.AggregatedSourceTags = AbilityTags;
		if ( !GetEventData(ID.EventID).InstigatorTags.IsEmpty())
		{
			Params.AggregatedSourceTags.AppendTags(GetEventData(ID.EventID).InstigatorTags);
		}		
		Params.RawMagnitude = OverlapEventData.DurationMultiplier;
		Params.AbilityLevel = FMath::TruncToInt32(AngleDeviation);
		Params.GameplayEffectLevel = FMath::TruncToInt32(MinDistance);
		Params.Location = OverlapEventData.Location;		
		Params.NormalizedMagnitude = CurrentYaw;
		Params.Normal = GetAreaBoundsByLifeTime(1, false) * OverlapEventData.AreaMultiplier;
		Params.Normal.Z = GetWorld()->GetTimeSeconds(); 
		Params.SourceObject = this;
		ModifyGameplayCueParams(ID,Params);

		UAbilitySystemComponent* const AbilitySystemComponent = GetAbilitySystemComponentFromActorInfo_Checked();
		AbilitySystemComponent->ExecuteGameplayCue(GameplayCueTag, Params);
		ExecutedGameplayCues.Add(ID);		
	}

#if !UE_BUILD_SHIPPING
		
	if (ShowOverlapDebug)
	{			
		switch (Shape)
		{
		case EOverlapAbilityShape::Sphere:
		{
			if (AngleDeviation < 180.f)
			{
				const FRotator Rot = FRotator(0.f,CurrentYaw, 0.f);
				const FVector Dir = Rot.RotateVector(FVector(1, 0, 0));
				UKismetSystemLibrary::DrawDebugCone(this, OverlapEventData.Location, Dir, CurrentExtent.X, FMath::DegreesToRadians(AngleDeviation), FMath::DegreesToRadians(AngleDeviation), 12, FLinearColor::Green, 0.5, 1.f);
			}
			else
			{
				UKismetSystemLibrary::DrawDebugCircle(this, OverlapEventData.Location, CurrentExtent.X, 30, FLinearColor::Green, 0.5f, 1.f, FVector(0, 1, 0), FVector(1, 0, 0));
			}
			break;
		}
		case EOverlapAbilityShape::Box:
		{
			UKismetSystemLibrary::DrawDebugBox(this, OverlapEventData.Location, CurrentExtent, FLinearColor::Green, FRotator(0.f,CurrentYaw, 0.f), .5f, 1.f);			
			break;
		}			
		default:
			break;
		}

		FRotator Rotator = FRotator(0.f,CurrentYaw, 0.f); 
		FVector RotationVector = FVector(150.f, 0.f, 0.f);
		RotationVector = Rotator.RotateVector(RotationVector);
		UKismetSystemLibrary::DrawDebugArrow(this, OverlapEventData.Location, OverlapEventData.Location + RotationVector, 3, FLinearColor::White, 1.f, 3.f);		
	}

#endif //UE_BUILD_SHIPPING
}

FGameplayAbilityTargetDataHandle UBaseOverlapAbility::GetTargetData_Implementation(const FGameplayEventData& EventData) const
{
	return EventData.TargetData;
}

void UBaseOverlapAbility::ProcessOverlapEvent(const FGameplayEventData& Payload, int32 EventID)
{	
	CreateContainerSpec(Payload, EventID);
	FGameplayAbilityTargetDataHandle OutHandle = ProcessTargetDataForEvent(Payload);

	EventDataMap.Add(EventID, Payload);	

	FEventSnapshottedAttributes SnapshotAttributes = FEventSnapshottedAttributes();
	ProcessEventAttributes(Payload, EventID, SnapshotAttributes);
	const float CurrentTime = GetWorld()->GetTimeSeconds();	

	TArray<FOverlapEventSnapshot> EventSnapshots;
	EventSnapshots.Reserve(OutHandle.Num() * (GetInterpSteps() + 1));

	const float SpawnDelayInternal = AbilityTags.HasTag(UGlobalTags::Ability_SpawnBatch()) ? 0.f : SnapshotAttributes.SpawnDelay;

	for (int32 i = 0; i < OutHandle.Num(); i++)
	{		
		FOverlapEventSnapshot SnapshotEvent;
		SnapshotEvent.EventID = EventID;
		SnapshotEvent.OverlapID = i;
		SnapshotEvent.InitialAvatarLocation = SnapshotAttributes.InitialAvatarLocation;
		SnapshotEvent.AreaMultiplier = SnapshotAttributes.AreaMultiplier;
		SnapshotEvent.InitialEventTime = CurrentTime + i * SpawnDelayInternal;
		SnapshotEvent.ActivationTime = CurrentTime + i * SpawnDelayInternal;
		SnapshotEvent.DurationMultiplier = SnapshotAttributes.DurationMultiplier;
		const FGameplayAbilityTargetData* TargetData = OutHandle.Get(i);
		if (TargetData->HasOrigin())
		{			
			SnapshotEvent.Location = TargetData->GetOrigin().GetLocation();
		/*	FGameplayAbilityTargetData_SourceData* SourceData = (FGameplayAbilityTargetData_SourceData*)(TargetData);
			ensure(SourceData);*/
			SnapshotEvent.YawRotation = TargetData->GetOrigin().Rotator().Yaw;
		}

		if (HasScaleInterp() && Duration.Period <= 0.f)
		{
			TArray<FOverlapEventSnapshot> ExpandedEvents;
			ExpandedEvents.Reserve(GetInterpSteps() + 1);
			ExpandOverlapEvent(SnapshotEvent, ExpandedEvents);
			EventSnapshots.Append(ExpandedEvents);
			if (AbilityTags.HasTag(UGlobalTags::Ability_SpawnBatch()))
			{
				for (auto& Event : ExpandedEvents)
				{
					Event.ActivationTime += SnapshotAttributes.SpawnDelay;
					Event.OverlapID += 10000;
					EventSnapshots.Add(Event);
				}
			}
		}
		else
		{
			EventSnapshots.Add(SnapshotEvent);
			if (AbilityTags.HasTag(UGlobalTags::Ability_SpawnBatch()))
			{
				SnapshotEvent.ActivationTime += SnapshotAttributes.SpawnDelay;	
				SnapshotEvent.OverlapID += 10000;
				EventSnapshots.Add(SnapshotEvent);
			}
		}
	}

	if (Duration.ActivationDelay)
	{
		const float ActivationDelay = ScaleValueWithAttribute(Duration.ActivationDelay, UAbilitySystemComponent::GetOutgoingDurationProperty());

		FGameplayCueParameters Params = FGameplayCueParameters();
		Params.AggregatedSourceTags.AppendTags(AbilityTags);
		Params.AggregatedSourceTags.AppendTags(Payload.InstigatorTags);
		Params.RawMagnitude = SnapshotAttributes.DurationMultiplier;
		Params.AbilityLevel = FMath::TruncToInt32(GetBaseMaximumAngleDeviationBetweenTargetAndOverlap(0) * SnapshotAttributes.AreaMultiplier);
		Params.GameplayEffectLevel = FMath::TruncToInt32(GetBaseMinimumTargetDistanceToCenterRequired(0) * SnapshotAttributes.AreaMultiplier);
		Params.Normal = GetAreaBoundsByLifeTime(1, false) * SnapshotAttributes.AreaMultiplier;
		Params.SourceObject = this;
		ModifyGameplayCueParams(FOverlapEventID(EventID, 0), Params);
		UAbilitySystemComponent* const AbilitySystemComponent = GetAbilitySystemComponentFromActorInfo_Checked();

		for (auto& it : EventSnapshots)
		{
			it.InitialEventTime += ActivationDelay;
			it.ActivationTime += ActivationDelay;
			Params.Normal.Z = it.ActivationTime;
			FOverlapEventID ID = FOverlapEventID(it.EventID, it.OverlapID);
			Params.Location = it.Location;
			Params.NormalizedMagnitude = it.YawRotation;			
			AbilitySystemComponent->ExecuteGameplayCue(GameplayCueTag, Params);
			ExecutedGameplayCues.Add(ID);
		}
	}

	GeneratePeriodicOverlapEvents(Payload, EventID, EventSnapshots);

	if (HasScaleInterp() || SnapshotAttributes.SpawnDelay || Duration.Period || Duration.ActivationDelay)
	{
		bool bUpdateTimer = false;

		for (auto& Event : EventSnapshots)
		{
			bUpdateTimer = AddOverlapEventToQueue(Event) || bUpdateTimer;
		}

		if (bUpdateTimer)
		{
			UpdateQueueTimer();
		}		
	}
	else
	{
		AppendOverlapEventsToInstantQueue(EventSnapshots);
	}		
}

FGameplayAbilityTargetDataHandle UBaseOverlapAbility::ProcessTargetDataForEvent(const FGameplayEventData& Payload)
{
	FGameplayAbilityTargetDataHandle OutHandle = FGameplayAbilityTargetDataHandle();
	const FGameplayAbilityTargetDataHandle InitialHandle = GetTargetData(Payload);
	FTargetInformation TargetInfo = FTargetInformation();
	ApplyTargetDistribution(InitialHandle, OutHandle, TargetInfo);
	return OutHandle;
}

void UBaseOverlapAbility::ProcessEventAttributes(const FGameplayEventData& Payload, int32 EventID, FEventSnapshottedAttributes& Attributes) const
{	
	Attributes.DurationMultiplier = ScaleValueWithAttribute(1, UAbilitySystemComponent::GetOutgoingDurationProperty());
	Attributes.AreaMultiplier = ScaleValueWithAttribute(1, UAbilityAttributeSet::GetAreaOfEffectAttribute());
	Attributes.InitialRadius = GetAreaBoundsByLifeTime(0).X;
	Attributes.SpawnDelay = GetSpawnDelay(GetAbilityLevel());
	Attributes.InitialAvatarLocation = GetAvatarActorFromActorInfo()->GetActorLocation();
}

void UBaseOverlapAbility::ProcessEventPeriodicAttributes(const FGameplayEventData& Payload, int32 EventID, FEventSnapshottedPeriodicAttributes& Attributes) const
{
	Attributes.FirstPeriodDelay = ScaleValueWithAttribute(Duration.FirstPeriodDelay, UAbilityAttributeSet::GetOutgoingTickDurationAttribute());
	Attributes.Period = ScaleValueWithAttribute(Duration.Period, UAbilityAttributeSet::GetOutgoingTickDurationAttribute());
	Attributes.LifeSpan = ScaleValueWithAttribute(Duration.LifeSpan, UAbilitySystemComponent::GetOutgoingDurationProperty());
}

void UBaseOverlapAbility::ExecuteOverlapAtLocation(const FGameplayAbilityTargetDataHandle& TargetData)
{
	FGameplayEventData Payload = FGameplayEventData();
	Payload.TargetData = TargetData;
	ProcessOverlapEvent(Payload, GetCurrentActivationInfo().GetActivationPredictionKey().Current);
}

bool UBaseOverlapAbility::IsOverlapQueueEmpty() const
{
	return InstantQueue.IsEmpty() && Queue.IsEmpty();
}

bool UBaseOverlapAbility::AddOverlapEventToQueue(const FOverlapEventSnapshot& EventData)
{
	if (Queue.IsEmpty() || EventData.ActivationTime < Queue.Last().ActivationTime)
	{
		Queue.Add(EventData);
		return true;
	}

	for (int32 i = 0; i < Queue.Num(); i++)
	{
		if (EventData.ActivationTime >= Queue[i].ActivationTime)
		{
			Queue.Insert(EventData, i);
			break;
		}
	}

	return false;
}

void UBaseOverlapAbility::AppendOverlapEventsToInstantQueue(const TArray<FOverlapEventSnapshot>& EventsData)
{
	if (EventsData.IsEmpty())
	{
		return;
	}

	const bool bSetTimer = InstantQueue.IsEmpty();
	InstantQueue.Append(EventsData);

	if (bSetTimer)
	{
		UpdateInstantQueueTimer();
	}
}

void UBaseOverlapAbility::AddOverlapEventToInstantQueue(const FOverlapEventSnapshot& EventData)
{
	const bool bSetTimer = InstantQueue.IsEmpty();
	InstantQueue.Add(EventData);

	if (bSetTimer)
	{
		UpdateInstantQueueTimer();
	}
}

void UBaseOverlapAbility::UpdateQueueTimer()
{
	if (GetWorld() && Queue.Num())
	{
		const float TimerDuration = Queue.Last().ActivationTime - GetWorld()->GetTimeSeconds();
		if (TimerDuration <= 0)
		{
			AddOverlapEventToInstantQueue(Queue.Pop());
			UpdateQueueTimer();
		}
		else
		{
			GetWorld()->GetTimerManager().SetTimer(QueueTimerHandle, this, &UBaseOverlapAbility::OnQueueTimerFinished, TimerDuration, false, TimerDuration);
		}
	}
}

void UBaseOverlapAbility::UpdateInstantQueueTimer()
{
	if (GetWorld() && InstantQueue.Num())
	{
		GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(this, &UBaseOverlapAbility::OnIsntantQueueTimerFinished));
	}
}

void UBaseOverlapAbility::OnQueueTimerFinished()
{
	if (Queue.Num())
	{
		UE_LOG(LogTemp, Log, TEXT("UBaseOverlapAbility::OnQueueTimerFinished: Triggered Overlap Event n�: %i"));
		FOverlapEventSnapshot Snapshot = Queue.Pop();
		InitAbilityModifiedTags(&GetEventData(Snapshot.EventID));
		CompensateOverlapLocation(Snapshot);
		OnOverlapEvent(Snapshot);
		UpdateQueueTimer();
		if (ShouldCleanUpEvent(Snapshot.EventID))
		{	
			CleanUpEvent(Snapshot.EventID);		
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("UBaseOverlapAbility::OnQueueTimerFinished: Timer called without data on the Queue"))
	}
}

void UBaseOverlapAbility::OnIsntantQueueTimerFinished()
{
	if (InstantQueue.Num())
	{
		FOverlapEventSnapshot Snapshot = InstantQueue.Pop();
		InitAbilityModifiedTags(&GetEventData(Snapshot.EventID));		
		CompensateOverlapLocation(Snapshot);
		OnOverlapEvent(Snapshot);
		UpdateInstantQueueTimer();
		if (ShouldCleanUpEvent(Snapshot.EventID))
		{
			CleanUpEvent(Snapshot.EventID);			
		}
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("UBaseOverlapAbility::OnIsntantQueueTimerFinished: Timer called without data on the Queue"))
	}
}

void UBaseOverlapAbility::RestartQueues()
{
	UpdateInstantQueueTimer();
	UpdateQueueTimer();
}

bool UBaseOverlapAbility::ShouldCleanUpEvent(int32 EventID)
{
	for (const auto& it : InstantQueue)
	{
		if (it.EventID == EventID)
		{
			return false;
		}
	}

	for (const auto& it : Queue)
	{
		if (it.EventID == EventID)
		{
			return false;
		}
	}

	return true;
}

void UBaseOverlapAbility::CleanUpEvent(int32 EventID)
{
	RemoveConsumableEffect();

	const int32 TargetCount = RemoveTargets(EventID);

	if (ShouldSendMultihitEventOnCleanUp())
	{
		SendMultihitEvent(EventID, TargetCount);
	}

	EventDataMap.Remove(EventID);
	EventEffectsMap.Remove(EventID);

	ExecutedGameplayCues.RemoveAll([&EventID](FOverlapEventID ID)
	{
		return ID.EventID == EventID;
	});
	
	if (Queue.IsEmpty() && InstantQueue.IsEmpty())
	{
		OnQueueEmptied();
		
		if (ShouldEndAbilityWhenQueueIsEmpty())
		{
			EndAbility(GetCurrentAbilitySpecHandle(), GetCurrentActorInfo(), GetCurrentActivationInfo(), true, false);
		}
	}
}

void UBaseOverlapAbility::CreateContainerSpec(const FGameplayEventData& Payload, int32 EventID)
{
	FGameplayEffectContainerSpec Spec = FGameplayEffectContainerSpec();
	BuildContainerSpec(Payload, Spec);	
	EventEffectsMap.Add(EventID, Spec);
}

void UBaseOverlapAbility::AddTarget(int32 EventID, int32 OverlapID, AActor* TargetToAdd)
{
	const FOverlapEventID ID = FOverlapEventID(EventID, OverlapID);
	if (TargetsMap.Contains(ID))
	{
		FTargetWrapper Wrapper = TargetsMap[ID];
		Wrapper.Targets.AddUnique(TargetToAdd);
		TargetsMap.Add(ID, Wrapper);
	}
	else
	{
		FTargetWrapper Wrapper = FTargetWrapper();
		Wrapper.Targets.Add(TargetToAdd);
		TargetsMap.Add(ID, Wrapper);
	}	
}

void UBaseOverlapAbility::AddTargets(int32 EventID, int32 OverlapID, UPARAM(ref) TArray<AActor*>& TargetsToAdd)
{
	for (auto& it : TargetsToAdd)
	{
		AddTarget(EventID, OverlapID, it);
	}
}

FGameplayTargetDataFilterHandle UBaseOverlapAbility::GetOverlapFilter_Implementation() const
{
	return MakeAbilityFilterHandleFromAbility();
}

void UBaseOverlapAbility::GetContainerSpecCacheForEvent(int32 EventID, FGameplayEffectContainerSpec& Spec) const
{
	Spec = *EventEffectsMap.Find(EventID);
}

TArray<AActor*> UBaseOverlapAbility::GetPreviousTargetsByOverlap(int32 EventID, int32 OverlapID) const
{
	TArray<AActor*> PreviousActors;

	const FOverlapEventID ID = FOverlapEventID(EventID, OverlapID);
	if (TargetsMap.Contains(ID))
	{
		PreviousActors.Append(TargetsMap[ID].Targets);
	}

	return PreviousActors;
}

TArray<AActor*> UBaseOverlapAbility::GetPreviousTargetsByEvent(int32 EventID) const
{
	TArray<AActor*> PreviousActors;

	for (auto& Pair : TargetsMap)
	{
		if (Pair.Key.EventID == EventID)
		{
			PreviousActors.Append(Pair.Value.Targets);
		}
	}

	return PreviousActors;
}

TArray<AActor*> UBaseOverlapAbility::GetIgnoredActors_Implementation(int32 EventID, int32 OverlapID) const
{
	TArray<AActor*> IgnoredActors;
	IgnoredActors.Empty();
	
	if (OverlapID <= 0)
	{
		for (auto& Pair : TargetsMap)
		{
			if (Pair.Key.EventID == EventID)
			{
				IgnoredActors.Append(Pair.Value.Targets);
			}
		}
	}
	else
	{
		FOverlapEventID ID = FOverlapEventID(EventID, OverlapID);

		if (TargetsMap.Contains(ID))
		{
			IgnoredActors.Append(TargetsMap[ID].Targets);
		}
	}
	
	if (bIgnoreInitialTarget && EventDataMap.Contains(EventID))
	{
		AActor* Target = const_cast<AActor*>(GetEventData(EventID).Target.Get());
		IgnoredActors.Add(Target);
	}
	
	if(bIgnorePayloadActors && EventDataMap.Contains(EventID))
	{
		for (auto& it : GetEventData(EventID).TargetData.Get(0)->GetActors())
		{
			if (!it.IsValid())
			{
				continue;
			}
			IgnoredActors.Add(it.Get());
		}
	}

	return IgnoredActors;
}

const FGameplayEventData& UBaseOverlapAbility::GetEventData(int32 EventID) const
{
	const FGameplayEventData* Data = EventDataMap.Find(EventID);
	return *Data;
}

int32 UBaseOverlapAbility::RemoveTargets(int32 EventID, int32 OverlapID)
{
	int32 Count = 0;
	TArray<FOverlapEventID> MustRemoveIDs;
	
	for (auto& it : TargetsMap)
	{
		if (it.Key.EventID == EventID && (OverlapID == -1 || it.Key.OverlapID == OverlapID))
		{
			MustRemoveIDs.Add(it.Key);
		}
	}

	for (auto& it : MustRemoveIDs)
	{			
		Count += TargetsMap[it].Targets.Num();
		TargetsMap.Remove(it);
	}

	return Count;
}

bool UBaseOverlapAbility::ShouldSendMultihitEventOnCleanUp() const
{	
	return Duration.Period <= 0.f;
}

void UBaseOverlapAbility::SendMultihitEvent(int32 EventID, int32 Amount)
{
	if (Amount > 0)
	{
		FGameplayEventData Payload;
		Payload.EventMagnitude = Amount;
		Payload.Instigator = GetAvatarActorFromActorInfo();
		Payload.Target = nullptr;
		Payload.InstigatorTags = GetAbilityTags();
		SendGameplayEvent(UGlobalTags::Event_MultiHit(), Payload);
	}
}

void UBaseOverlapAbility::CompensateOverlapLocation(FOverlapEventSnapshot& OverlapEvent)
{	
	if(AbilityTags.HasTag(UGlobalTags::Ability_Targeting_Start_Actor_Avatar()))
	{
		OverlapEvent.Location += GetAvatarActorFromActorInfo()->GetActorLocation() - OverlapEvent.InitialAvatarLocation;
	}
}

void UBaseOverlapAbility::ModifyGameplayCueParams(const FOverlapEventID& ID, FGameplayCueParameters& Params) const
{
	//meant for override in child classes
}
