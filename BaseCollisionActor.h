//Copyright 2022 Marchetti S. César A. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AbilitySystem/AbilityTypes.h"
#include "AbilitySystem/Targeting/TargetTypes.h"
#include "AbilitySystem/Targeting/TargetFilter.h"
#include "AbilitySystem/CollisionActors/CollisionActorTypes.h"
#include "AbilitySystem/ActorPool/PooledActorInterface.h"
#include "BaseCollisionActor.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCollisionActorSignature, ABaseCollisionActor*, CollisionActorReference);

class UGameplayCueManager;
class UBaseAbilitySystemComponent;
class UShapeComponent;
class USceneComponent;

/** Collision actors are used to apply effects in the world by abilities.*/
UCLASS(Abstract)
class CAMERAPLAY_API ABaseCollisionActor : public AActor, public IPooledActorInterface
{
	GENERATED_BODY()

public:

	//------------------------------------------------------------------------------
	//	Overrides and general purpose functions
	//------------------------------------------------------------------------------

	ABaseCollisionActor(const FObjectInitializer& ObjectInitializer);
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float Delta) override;	
	virtual bool RequiresTick() const;	
	virtual bool IsNetRelevantFor(const AActor* RealViewer, const AActor* ViewTarget, const FVector& SrcLocation) const override;
	virtual float ScaleValueWithAttribute(UAbilitySystemComponent* InASC, const FGameplayTagContainer& InAbilityTags, float Value, FGameplayAttribute Attribute) const;

	/** Returns location based on actor with optional bone name. @TODO: Move to a library.*/
	FVector GetActorBoneSocketLocation(AActor* InActor, FName Bone) const;

	/** Direct set to bReplicates, this should be called only for pre-init actors. Needed when pooling.*/
	FORCEINLINE void SetReplicatesDirectly(bool bNewReplicate)
	{
		bReplicates = bNewReplicate;
	}

	//------------------------------------------------------------------------------
	//	Delegates
	//------------------------------------------------------------------------------

	UPROPERTY(BlueprintAssignable)
	FCollisionActorSignature OnCollisionActorActivate;

	UPROPERTY(BlueprintAssignable)
	FCollisionActorSignature OnCollisionActorDeactivate;

	UPROPERTY(BlueprintAssignable)
	FCollisionActorSignature OnCollisionActorExpired;

	UPROPERTY(BlueprintAssignable)
	FCollisionActorSignature OnCollisionActorRotationCompleted;

	//------------------------------------------------------------------------------
	//	Properties
	//------------------------------------------------------------------------------

	/** How to interpolate the scale if at all.*/
	UPROPERTY(BlueprintReadOnly, EditDefaultsOnly, Category = "Collision Actor")
	FScaleInterp ScaleInterpolation;

	/** Changes rotation over time.*/
	UPROPERTY(EditDefaultsOnly, Category = "Collision Actor")
	FCollisionActorRotationInterp RotationInterpolation;

	/** How much time the collision actor lasts.*/
	UPROPERTY(BlueprintReadOnly, EditDefaultsOnly, Category = "Collision Actor")
	FCollisionActorDuration Duration;

	/** How the targetting is done.*/
	UPROPERTY(BlueprintReadOnly, EditDefaultsOnly, Category = "Targeting")
	FCollisionActorTargetting Targeting;

	/** Filter to determine wheter or not an actor is a valid target.*/
	UPROPERTY(EditDefaultsOnly, Category = "Targeting")
	FAbilityTargetFilter Filter;

	/** Wheter we want a full attachment, with rotation included or only copy the location but keep rotation. This requires the attach actor to be valid.*/
	UPROPERTY(EditDefaultsOnly, Category = Targeting)
	ECollisionActorAttachmentType AttachmentType = ECollisionActorAttachmentType::LocationAndRotation;

	/**
	*	Preactivation gameplay cue. Its active while the activation delay is running until we activate the actor gameplay cue.
	*	WhileActive event is called on interpolation scale changes.
	*/
	UPROPERTY(EditDefaultsOnly, meta = (Categories = "GameplayCue"), Category = "Gameplay Cue")
	FGameplayTag PreactivationGameplayCue;

	/**
	*	Primary gameplay cue. Its activated and removed based on the collision actor lifetime. Its executed at key moments depending on the collision actor.
	*	WhileActive event is called on interpolation scale changes.
	*/
	UPROPERTY(EditDefaultsOnly, meta = (Categories = "GameplayCue"), Category = "Gameplay Cue")
	FGameplayTag ActorGameplayCue;

	/**	Burst Area effect.*/
	UPROPERTY(EditDefaultsOnly, meta = (Categories = "GameplayCue"), Category = "Gameplay Cue")
	FGameplayTag BurstGameplayCue;

	/** Gameplay Cues to play on the target when we hit.*/
	UPROPERTY(EditDefaultsOnly, meta = (Categories = "GameplayCue"), Category = "Gameplay Cue")
	FGameplayTagContainer HitTargetGameplayCues;

	/** Gameplay Cue to play when the actor deactivates.*/
	UPROPERTY(EditDefaultsOnly, meta = (Categories = "GameplayCue"), Category = "Gameplay Cue")
	FGameplayTag DeactivationGameplayCue;

	/** Area of effect preview.	*/
	UPROPERTY(EditDefaultsOnly, meta = (Categories = "GameplayCue.Preview"), Category = "Gameplay Cue")
	FGameplayTag PreviewGameplayCue;

	//------------------------------------------------------------------------------
	//	Activation / Deactivation
	//------------------------------------------------------------------------------

	UFUNCTION(BlueprintPure)
	virtual TSubclassOf<UGameplayAbility> GetOwningAbilityClass();

	UFUNCTION(BlueprintPure)
	virtual int32 GetSharedDataID();

	UFUNCTION(BlueprintPure)
	virtual int32 GetActivationKey();

	/** Returns a shared data for replication.*/
	virtual void InitializeSharedData(UAbilitySystemComponent* InASC, UGameplayAbility* InAbility, FCollisionActorSharedData& OutData) const;

	/** Wheter or not this actor was preactivated.*/
	virtual bool IsCollisionActorPreactivated() const;

	/** Pre activates the collision actor, setting important variables prior to it activating.*/
	virtual void PreActivateCollisionActor(const FCollisionActorIndividualData& InIndividualData);

protected:

	/** Waits for shared data to update so it can finish activation.*/
	UFUNCTION()
	virtual void OnSharedDataReplicatedBack(int32 SharedDataID);
	
	/** Initialization done after the shared data is replicated back.*/
	virtual void InitializeVariablesFromSharedData();
		
	/** Initilizes variables when receiving the shared data.*/	
	virtual void SetSharedData(const FCollisionActorSharedData& InSharedData);
	
	/** Initilizes variables when receiving the individual data.*/
	virtual void SetIndividualData(const FCollisionActorIndividualData& InIndividualData);

	/** Calls BeginActivate at the right time.*/
	virtual void CallBeginActivate();

	/** Starts the activation process, calls FinishActivate or sets the timer for delayed activations. Prepares the initial conditions. Must be called after the transform is set.*/
	UFUNCTION(BlueprintCallable, Category = Activation)
	virtual void BeginActivate();

	/**	Finish the activation. This applies effects for aoes, start projectiles, etc. Can be called with a delay.*/
	UFUNCTION(BlueprintCallable)
	virtual void FinishActivate();

	UFUNCTION(BlueprintCallable, Category = "Activation")
	virtual void Deactivate(float PoolingDelay = 0.f);

	UFUNCTION(BlueprintPure, Category = "Activation")
	bool IsCollisionActorActive() const;

	/** Should we send a multihit event at the end. This allows us to gather targets overtime and send a unique multihit with all the targets adquired over the lifetime.*/
	virtual bool ShouldSendMultihitEventOnDeactivation() const;

	/** Called when the actor expires on time. Calls deactivate. */
	virtual void Expire();
	
	/** Initializes expiration timer.*/
	virtual void InitExpirationTimer();

	/** Clears expiration timer. Useful for when we need to override the timer to allow the actor to complete certain behavior like return.*/
	virtual void ClearExpirationTimer();

	UPROPERTY()
	bool bActive;

	UPROPERTY()
	bool bPreactivated;

	UPROPERTY()
	float PreActivationTime = 0.f;

	UPROPERTY()
	float StartTime = 0.f;

	UPROPERTY()
	FTimerHandle DurationTimerHandle;

	UPROPERTY()
	FTimerHandle DeactivationDelayTimerHandle;

	UPROPERTY()
	FTimerHandle ActivationDelayTimerHandle;

	UPROPERTY()
	bool bSkipVariableInitialization;

	UPROPERTY()
	FGameplayTagContainer OwningAbilityTags;

	UPROPERTY()
	FCollisionActorIndividualData IndividualData;

	UPROPERTY()
	FCollisionActorSharedData SharedData;

public:

	//------------------------------------------------------------------------------
	//	Interpolation
	//------------------------------------------------------------------------------
	
	/** Called on tick to interpolate the size of the collision. Subclassess can override this to perform other types of interpolation logic.*/
	virtual void Interpolate(float Delta);

	/** Returns a normalized time, representing how much of the total life time has elapsed. */
	float GetNormalizedElapsedTime() const;

	/** Adjust initial transform, this is to account for changes that may happen between preactivation and activation.*/
	virtual void AdjustTransform();

	virtual void SetStartLocation();

	/** Called on tick to interpolate the size of the collision.*/
	virtual void InitializeScale();

	/** Actor additive Scale based on the ability level.*/
	UFUNCTION(BlueprintNativeEvent, BlueprintPure, Category = "Scale")
	FVector GetBaseAdditiveScale(int32 AbilityLevel) const;
		
	FVector CalculateActorScale(float RelativeElapsedTime) const;
	float CalculateScaledRadius(float RelativeElapsedTime) const;
	FVector GetCollisionActorScaleByLifetime(float InTime, int32 InLevel) const;
	virtual void SetCollisionActorScale(FVector NewScale);

	//Collision actor rotation interpolation.
	virtual void InitializeRotationInterpolation();
	virtual void OnRotationCompleted();
	virtual void OnRotationSynced();
	virtual void InterpolateRotation(float DeltaSeconds);

	/** Changes Actor Location.Z to maintain a desired distance to floor. Returns delta height.*/
	virtual void InterpolateHeightToMatchFloor(float DeltaSeconds, float DesiredHeight, float InterpSpeed = 15.f);

	/** Removes interp data from previous frames.*/
	void ClearHeightInterpolationData();

protected:

	UPROPERTY()
	FVector StartLocation;

	UPROPERTY()
	bool bInterpolatingScale;

	UPROPERTY()
	FVector CachedAdditiveScale;

	UPROPERTY()
	bool bInterpolatingRotation;

	UPROPERTY()
	FTimerHandle RotationCompleteTimer;

	UPROPERTY()
	float PredictionRotationRateMultiplier;

	UPROPERTY()
	FTimerHandle RotationSyncTimerHandle;

	//Data used to smooth height interpolation, by comparing with previous frames.
	UPROPERTY()
	TArray<float> PreviousInterpZValues;

	//------------------------------------------------------------------------------
	//	Attachment
	//------------------------------------------------------------------------------

public:

	/** Attach Actor, by default we attach to target actor if there is one.*/
	UFUNCTION(BlueprintNativeEvent, Category = "Targeting")
	AActor* GetAttachTarget() const;

	/** Initialize Attachent*/
	virtual void InitializeAttachToActor();

	/** Remove attached actor*/
	virtual void UninitializeAttachToActor();

	/** Updates attachment on tick when mirroring location.*/
	virtual void UpdateAttachment(float DeltaSeconds);

private:

	UPROPERTY()
	bool bUpdateAttachmentOnTick = false;

	UPROPERTY()
	bool bAttached;

public:

	//------------------------------------------------------------------------------
	//	Collision and effect aplication.
	//------------------------------------------------------------------------------

	/** Initialize things are needed only for duration / persistent collision actors, that instant execution actors don't care about.*/
	virtual void InitializePersistentElements();

	/** Bind to shape callbacks, this is needed only for persistent collision actors.*/
	virtual void BindShapeCallbacks();

	/** Remove previously bound shape callbacks.*/	
	virtual void UnbindShapeCallbacks();

	/** Initialize any target related elements. Can do things like attach or setup homing projectile elements.*/
	virtual void InitializeTarget();

	/** Undoes anything done in InitializeTarget().*/
	virtual void UninitializeTarget();

	/** Apply Area of Effect periodically.*/
	virtual void OnAreaOfEffectPeriod();

	UFUNCTION()
	virtual void OnBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	UFUNCTION()
	virtual void OnEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex);

	/**	Applys the effect container to an actor. Updates hit context with ContextHitResult. Returns true if succeds.*/
	virtual bool ApplyEffectToActor(AActor* A, const FHitResult& ContextHitResult, FGameplayTagContainer* ContextTags = nullptr);

	/** Applies the container to all the valid actors and returns the number of succesful aplications. Updates hit result on the effect context.*/
	int32 ApplyEffectToActorArray(const TArray<AActor*>& A, FGameplayTagContainer* ContextTags = nullptr, bool bSendMultiHitEvent = false);

	/**	Handles interactions with actors that don't have an ASC, like destructibles.*/
	virtual bool ApplyActorInteraction(AActor* A, UPrimitiveComponent* OverlappedComponent, const FHitResult& Hit);
	
	/** Remove infinite effects applied by this actor. We consider that persistent effects applied by this actor should be tied to the overlap duration of the actor.*/
	virtual int32 RemoveAppliedPersistentEffects(AActor* Actor);

	virtual bool TransferPersistentEffects(AActor* Target);

	FGameplayEffectContextHandle GetEffectContext() const;

	UFUNCTION(BlueprintPure, Category = "Effect")
	FGameplayEffectContainerSpec& GetEffectContainerSpec();

	void SetEffectContainerSpec(const FGameplayEffectContainerSpec& NewSpec);	
	
	UPROPERTY()
	FGameplayEffectContainerSpec EffectContainerSpec;

protected:

	UPROPERTY()
	FTimerHandle AreaPeriodTimerHandle;

	UPROPERTY()
	int32 MaximumPeriodsToExecute;

	UPROPERTY()
	int32 ExecutedPeriods;

	UPROPERTY()
	bool bDiscreteCollisionChecks;

	UPROPERTY()
	bool bAppliesPersistentEffects;

	//-----------------------------------------------
	// Targeting
	//-----------------------------------------------

public:

	/** Returns the targeting visualization for the collision actor.*/
	UFUNCTION(BlueprintNativeEvent, Category = Targeting)
	FTargetVisualization GetTargetingVisualRepresentation(FGameplayTagContainer AbilityTags) const;

	/** Determines if an actor is valid to apply the effect container.*/
	virtual bool IsValidTargetActor(AActor* Actor);
	
	/** Determines if an actor can respond to interaction. This is for destructible, physic objects, etc.*/
	virtual bool IsValidInteractableActor(AActor* Actor, FVector ImpactPoint);

	/** Had we already applied effects to this target.*/
	virtual bool IsAlreadyTargeted(AActor* Target);

	virtual bool IsInteractableActorAlreadyTargeted(AActor* Actor) const;

	/** Wheter this actor can target an actor based on their priority (spawn index).*/
	virtual bool HasTargetPriority(AActor* Target) const;

	/** Wheter or not we have vision of the target.*/
	bool HasLineOfSightToTarget(AActor* Target) const;

	/** Wheter or not we have vision of the location.*/
	bool HasLineOfSightToLocation(FVector Location) const;

	/** Wheter or not this target actor is at more or equal distance to the minimum distance allowed*/
	bool IsTargetInMinimalDistance(AActor* Target) const;

	/** Wheter or not this location is at more or equal distance to the minimum distance allowed*/
	bool IsLocationInMinimalDistance(FVector Location) const;

	/** Wheter or not this target is inside of the cone defined by maximum angle deviation relative to this actor rotation.*/
	bool IsTargetBetweenAngleDeviation(AActor* Target) const;

	/** Wheter or not this location is inside of the cone defined by maximum angle deviation relative to this actor rotation.*/
	bool IsLocationBetweenAngleDeviation(FVector Location) const;

	/** Used to filter target using the distance to the collision actor. It takes capsule size into account.*/
	virtual float GetMinimumDistanceRequired() const;

	/** Version that calculates at any lifetime.*/
	virtual float GetMinimumDistanceRequiredByLifetime(float InTime, int32 InLevel) const;

	/** Used to filter target based on the direction difference (angle span) from the actor direction to the actor-target direction.*/
	virtual float GetMaximumDirectionDeviation() const;

	/** Version that calculates at any lifetime.*/
	virtual float GetMaximumDirectionDeviationByLifetime(float InTime, int32 InLevel) const;

	/** Returns a list of allready targeted actors.*/
	virtual TArray<TWeakObjectPtr<AActor>>* GetPreviousTargets();
	virtual TArray<TWeakObjectPtr<AActor>>* GetLocalPreviousTargets();
	virtual TArray<AActor*> GetPreviousTargetsHardReference();

protected:
	
	virtual void RegisterSharedTargetInstance();
	virtual void UnregisterSharedTargetInstance();	
	virtual void SoftUnregisterSharedTargetInstance();
	virtual int32 GetSharedTargetRegisteredAmount() const;
	virtual int32 GetSharedTargetSoftRegisteredAmount() const;

	//Internal functions to keep track of targeted actors by this particular collision actor.
	virtual void AddPreviousTarget(AActor* TargetToAdd);
	virtual void AddPreviousTargets(TArray<TWeakObjectPtr<AActor>>& TargetsToAdd);
	virtual void RemovePreviousTarget(AActor* TargetToRemove);
	virtual void ClearPreviousTargets();
	virtual void AddPreviousInteractableTarget(AActor* TargetToAdd);

private:

	UPROPERTY()
	FTimerHandle ClearTargetsTimerHandle;

	UPROPERTY()
	TArray<TWeakObjectPtr<AActor>> PreviousTargetedActors;

	UPROPERTY()
	TArray<TWeakObjectPtr<AActor>> PreviousInteractableActors;

	UPROPERTY()
	bool bRegisteredTargetInstance;	
	
	UPROPERTY()
	bool bSoftRegisteredTargetInstance;

	/**
	*	Periodic Areas, that apply instant effects, need to be able to reapply those effects to allready targetted actors. Because of this, we want to allow retargeting of local previous targets in this cases.
	*	When using shared targets, for periodic effects.
	*/
	UPROPERTY()
	bool bAllowRetargetting;

	//-----------------------------------------------
	// Pooling
	//-----------------------------------------------

protected:
	
	virtual	void SetInRecycleQueue_Implementation(bool NewValue);		
	virtual bool IsInRecycleQueue_Implementation() const;
	virtual	bool Recycle_Implementation() override;
	virtual	void ReuseAfterRecycle_Implementation();
	virtual void PoolCollisionActor();

	/** How many instances of this actor to preallocate.*/
	UPROPERTY(EditDefaultsOnly, Category = "Pooling")
	int32 NumPreallocatedInstances;

	UPROPERTY()
	bool bInRecycleQueue;

	//-----------------------------------------------
	// Gameplay Cue
	//-----------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Gameplay Cue")
	bool CanExecuteGameplayCue() const;

	UFUNCTION(BlueprintCallable, Category = "Gameplay Cue")
	virtual void HandleGameplayCueEvent(FGameplayTag CueTag, EGameplayCueEvent::Type EventType);

	UGameplayCueManager* GetGameplayCueManager();
	virtual void GetDefaultGameplayCueParams(FGameplayCueParameters& Params);	
	virtual void GetPreviewGameplayCueParams(FGameplayCueParameters& Params) const;
	virtual bool GetImpactLocationForGameplayCues(AActor* HitActor, FVector& Location, FVector& Normal) const;
	virtual FGameplayTag GetPreactivationGameplayCue() const;
	virtual void ExecuteGameplayCues();
	virtual void InitializeActorGameplayCue();		
	virtual void InitializePreactivationGameplayCue();
	virtual void InitializePreviewGameplayCue();
	virtual void RemovePreactivationGameplayCue();
	virtual void RemovePreviewGameplayCue();
	virtual void RemoveGameplayCues();	
	virtual void ResetParticleSystems() const;

	UPROPERTY()
	UGameplayCueManager* GameplayCueManager;

	UPROPERTY()
	bool bActorGameplayCueInitialized;

	UPROPERTY()
	bool bPreviewGameplayCueInitialized;

	UPROPERTY()
	bool bPreactivationGameplayCueInitialized;

	UPROPERTY()
	bool bExecuteDeactivationCue;
	
	UPROPERTY()
	bool bSkipGameplayCues;

	//-----------------------------------------------
	// Components
	//-----------------------------------------------

public:

	UShapeComponent* GetShapeComponent() const;

protected:

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Scene Component", meta = (AllowPrivateAccess = "true"))
	USceneComponent* SceneComp = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Shape", meta = (AllowPrivateAccess = "true"))
	UShapeComponent* ShapeComp = nullptr;

	static FName ShapeComponentName;

	UAbilitySystemComponent* GetInstigatorAbilitySystemComponent() const;
	UBaseAbilitySystemComponent* GetInstigatorBaseAbilitySystemComponent() const;
	void SetSourceAbilitySystemComponent();
	void SendGameplayEvent(UAbilitySystemComponent* InASC, FGameplayTag EventTag, const FGameplayEventData& Payload);

private:

	UPROPERTY()
	UAbilitySystemComponent* InstigatorASC = nullptr;

	UPROPERTY()
	UBaseAbilitySystemComponent* InstigatorBaseASC = nullptr;

	//-----------------------------------------------
	// Prediction. WIP, simple logic test.
	//-----------------------------------------------
	
protected:

	/** Should we execute predicting logic for this collision actor.*/
	virtual bool ShouldPredict();

	/** Diference in time that the server prediction is ahead of the replicated actor. Should be bassed off of ping.*/
	virtual float GetPredictionDeltaTime() const;

	/** Time related functions needed to predict.*/
	bool IsServerWorldTimeAvailable() const;
	float GetServerWorldTime() const;
	float GetWorldTime() const;

	UPROPERTY(Replicated)
	bool bAbilityFromListenServer = false;

	/** Fake and master collision actors are in sync and dont need more prediction.*/
	UPROPERTY()
	bool bSynched = false;

	UPROPERTY()
	float CompensationActivationDelay = 0.f;

};
