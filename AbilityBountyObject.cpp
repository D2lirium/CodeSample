// Copyright 2024 Marchetti S. César A. All Rights Reserved.

#include "Bounty/Objects/AbilityBountyObject.h"
#include "Bounty/BaseBountyComponent.h"
#include "Bounty/BountyObjectData.h"
#include "AbilitySystem/AbilitySystemComponents/BaseAbilitySystemComponent.h"
#include "AbilitySystem/GameplayData/GameplayDataSubsystem.h"
#include "AbilitySystem/GameplayData/GameplayDataAbility.h"
#include "AbilitySystem/GameplayData/GameplayDataAbilityModifier.h"
#include "AbilitySystem/GameplayData/GameplayDataDisplay.h"
#include "Engine/AssetManager.h"

UAbilityBountyObject::UAbilityBountyObject()
{
}

void UAbilityBountyObject::InitializeBountyObject(UBountyObjectData* InBountyObjectData, UBaseBountyComponent* InBountyComponent)
{
	GenerateAbilityClass();
	GenerateAbilityLevel();
	GenerateModifiers();

	TArray<FGameplayTag> ModifierTags;
	for (auto& Mod : Modifiers)
	{
		ModifierTags.Add(Mod.Tag);	
	}

	TArray<FPrimaryAssetId> IDs;
	IDs.Add(GetGameplayDataSubsystem()->GetAbilityDisplayDataByClass(AbilityClass)->GetPrimaryAssetId());
	GetGameplayDataSubsystem()->GetDisplaysPrimaryAssets(ModifierTags, IDs);

	TArray<FName> AddedBundles;
	AddedBundles.Add(FName("Display"));
	UAssetManager& AssetManager = UAssetManager::Get();

	const FStreamableDelegate Delegate = FStreamableDelegate::CreateLambda([this]()
		{
			bIsBountyInitialized = true;
			OnBountyInitialized.Broadcast();
		});

	AssetManager.LoadPrimaryAssets(IDs, AddedBundles, Delegate);	
}

void UAbilityBountyObject::OnBountySelected()
{		
	int32 InputID = -1;
	for (int32 i = 0; i <= 3; i++)
	{
		const FGameplayAbilitySpec* Spec = GetAbilitySystemComponent()->FindAbilitySpecFromInputID(i);
		if (!Spec)
		{
			InputID = i;
			break;
		}
	}
	
	if (InputID == -1)
	{
		const FGameplayAbilitySpec* Spec = GetAbilitySystemComponent()->FindAbilitySpecFromInputID(0);
		GetAbilitySystemComponent()->ClearAbility(Spec->Handle);
		InputID = 0;
	}

	GetAbilitySystemComponent()->GiveAsyncModifiedAbility(AbilityClass,	Modifiers, InputID,	AbilityLevel);
}

void UAbilityBountyObject::GenerateAbilityClass()
{
	TArray<TSubclassOf<UGameplayAbility>> Abilities;
	GetGameplayDataSubsystem()->GetAllPlayerAbilityClasses(Abilities);
		
	for (int32 i = 0; i <= 3; i++)
	{
		const FGameplayAbilitySpec* Spec = GetAbilitySystemComponent()->FindAbilitySpecFromInputID(i);
		if (Spec && Spec->Ability)
		{
			Abilities.RemoveSwap(Spec->Ability->GetClass());
		}
	}	

	//avoid duplicated ones.
	Abilities.RemoveAllSwap([=](TSubclassOf<UGameplayAbility> Ability){
		for (const auto& Bounty : BountyComponent->GetBountyObjects())
		{
			if (Bounty == this)
			{
				continue;
			}

			if (Bounty->GetClass() != GetClass())
			{
				continue;
			}

			if (const UAbilityBountyObject* OtherAbilityBounty = Cast<UAbilityBountyObject>(Bounty))
			{
				if (OtherAbilityBounty->AbilityClass == Ability)
				{
					return true;
				}
			}
		}
		return false;
		});

	AbilityClass = Abilities[GetGameplayDataSubsystem()->GetRandomFromStream(0, Abilities.Num() - 1)];
}

void UAbilityBountyObject::GenerateAbilityLevel()
{
	AbilityLevel = BountyObjectData->GetMagnitudeByName(FName("AbilityLevel"), GetBountyLevel());
}

void UAbilityBountyObject::GenerateModifiers()
{
	if (const int32 ModifierAmount = BountyObjectData->GetMagnitudeByName(FName("ModifierAmount"), GetBountyLevel()); ModifierAmount > 0)
	{
		FModifiedAbility NewModifiedAbility = GetModifiedAbilityForGeneration();		
		TArray<FGameplayTag> IgnoredModifiers;
		GetIgnoredModifiers(IgnoredModifiers);

		for (int32 Mod = 0; Mod < ModifierAmount; Mod++)
		{
			UGameplayDataAbilityModifier* SelectedModifier = GetGameplayDataSubsystem()->GetRandomValidModifierForAbility(NewModifiedAbility, IgnoredModifiers)
			if(!SelectedModifier)
			{
				break;
			}

			NewModifiedAbility.ApplyModifier(SelectedModifier);
			const int32 ModifierLevel = FMath::Max(
				GetAbilitySystemComponent()->GetAbilityModifierLevel(SelectedModifier->ModifierTag),
				BountyObjectData->GetMagnitudeByName(FName("ModifierLevel"),
				GetBountyLevel()));
			Modifiers.Add(FModifierWithLevel(SelectedModifier->ModifierTag, ModifierLevel));			
		}
	}
}

FModifiedAbility UAbilityBountyObject::GetModifiedAbilityForGeneration() const
{
	return FModifiedAbility(AbilityClass, AbilityLevel);
}

void UAbilityBountyObject::GetIgnoredModifiers(TArray<FGameplayTag>& IgnoredMods) const
{
}

float UAbilityBountyObject::GetCostMultiplier() const
{
	float ModifierCostMultiplier = 0.f;
	for (auto& Mod : Modifiers)
	{
		ModifierCostMultiplier += .5f + (Mod.Level - 1) * .35f;
	}
	return 1 + (AbilityLevel - 1) * .35 + ModifierCostMultiplier;
}

int32 UAbilityBountyObject::GetDescriptionLength() const
{
	int32 Size = GetGameplayDataSubsystem()->GetAbilityDescriptionByClass(AbilityClass, 1, 1, GetAbilitySystemComponent()).ToString().Len();
	
	for (auto& Mod : Modifiers)
	{
		Size += GetGameplayDataSubsystem()->GetModifierDescriptionByTag(Mod.Tag, 1, 1, GetAbilitySystemComponent()).ToString().Len();
	}
	
	return Size;
}
