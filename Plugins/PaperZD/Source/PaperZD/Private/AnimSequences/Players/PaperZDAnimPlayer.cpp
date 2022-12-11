// Copyright 2017 ~ 2022 Critical Failure Studio Ltd. All rights reserved.

#include "AnimSequences/Players/PaperZDAnimPlayer.h"
#include "AnimSequences/Players/PaperZDPlaybackHandle.h"
#include "AnimSequences/Sources/PaperZDAnimationSource.h"
#include "AnimSequences/PaperZDAnimSequence.h"
#include "Components/PrimitiveComponent.h"
#include "Notifies/PaperZDAnimNotify_Base.h"

#define MIN_RELEVANT_WEIGHT 0.35f

UPaperZDAnimPlayer::UPaperZDAnimPlayer() : Super()
{
	bPlaying = true;
	PlaybackMode = EAnimPlayerPlaybackMode::Forward;
	bPreviewPlayer = false;
	bFireSequenceChangedEvents = false;
}

float UPaperZDAnimPlayer::GetCurrentPlaybackTime() const
{
	return LastWeightedAnimation.PlaybackTime;
}

float UPaperZDAnimPlayer::GetPlaybackProgress() const
{	
	const UPaperZDAnimSequence* PrimaryAnimSequence = LastWeightedAnimation.AnimSequencePtr.Get();
	return PrimaryAnimSequence && PrimaryAnimSequence->GetTotalDuration() > 0.0f ? LastWeightedAnimation.PlaybackTime / PrimaryAnimSequence->GetTotalDuration() : 0.0f;
}

const UPaperZDAnimSequence* UPaperZDAnimPlayer::GetCurrentAnimSequence() const
{
	return LastWeightedAnimation.AnimSequencePtr.Get();
}

void UPaperZDAnimPlayer::ClearCachedAnimationData()
{
	LastPlaybackData = FPaperZDAnimationPlaybackData();
	LastWeightedAnimation = FPaperZDWeightedAnimation();
}

void UPaperZDAnimPlayer::ResumePlayback()
{
	bPlaying = true;
}

void UPaperZDAnimPlayer::PausePlayback()
{
	bPlaying = false;
}

void UPaperZDAnimPlayer::Init(const UPaperZDAnimationSource* AnimationSource)
{
	if (AnimationSource && AnimationSource->GetPlaybackHandleClass())
	{
		//Create the playback handle  and initialize it via the animation source.
		PlaybackHandle = NewObject<UPaperZDPlaybackHandle>(this, AnimationSource->GetPlaybackHandleClass());
		AnimationSource->InitPlaybackHandle(PlaybackHandle);

		//By default allow the sequence changed events to trigger only on animation sources that do not blend, as those are the only ones that make sense to support.
		bFireSequenceChangedEvents = !AnimationSource->SupportsBlending();

		//Optionally configure the render component if it was setup before initializing
		if (RegisteredRenderComponent.IsValid())
		{
			PlaybackHandle->ConfigureRenderComponent(RegisteredRenderComponent.Get(), bPreviewPlayer);
		}
	}
}

void UPaperZDAnimPlayer::SetIsPreviewPlayer(bool bInPreviewPlayer)
{
	bPreviewPlayer = bInPreviewPlayer;
}

bool UPaperZDAnimPlayer::IsRelevantWeight(float Weight) const
{
	return Weight > MIN_RELEVANT_WEIGHT;
}

//Playback controls
void UPaperZDAnimPlayer::TickPlayback(const UPaperZDAnimSequence* AnimSequence, float& PlaybackMarker, float DeltaTime, bool bLooping, UPaperZDAnimInstance* OwningInstance /* = nullptr */, float EffectiveWeight /* = 1.0f */, bool bSkipNotifies /* = false */)
{
	if (AnimSequence && AnimSequence->GetTotalDuration() > 0.0f && bPlaying && DeltaTime != 0.0f)
	{
		float PreviousTime = PlaybackMarker;
		bool bSequencePlaybackComplete = false;
		UPrimitiveComponent* RenderComponent = nullptr;

		//Find the render component for this class, if it exists
		if (RegisteredRenderComponent.IsValid())
		{
			RenderComponent = RegisteredRenderComponent.Get();
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Trying to tick AnimSequence '%s' without having registered a PrimitiveComponent as Renderer on AnimPlayer '%s'"), *AnimSequence->GetName(), *GetName());
		}
		
		//Adjust for forward/backwards
		if (PlaybackMode == EAnimPlayerPlaybackMode::Reversed)
		{
			DeltaTime *= -1.0f;
		}

		//Depending on the direction the animation is going, we loop differently
		if (DeltaTime > 0.0f)
		{
			PlaybackMarker += DeltaTime;
			bSequencePlaybackComplete = PlaybackMarker >= AnimSequence->GetTotalDuration();
			PlaybackMarker = bLooping ? FMath::Fmod(PlaybackMarker, AnimSequence->GetTotalDuration()) : FMath::Min(PlaybackMarker, AnimSequence->GetTotalDuration());
		}
		else
		{
			PlaybackMarker += DeltaTime;
			bSequencePlaybackComplete = PlaybackMarker <= 0.0f;
			const float Duration = AnimSequence->GetTotalDuration();
			PlaybackMarker = bLooping && bSequencePlaybackComplete ? Duration + PlaybackMarker : FMath::Max(PlaybackMarker, 0.0f);
		}
		
		//With the new playback marker set, we can go ahead and collect every AnimNotify object that should be triggered
		const bool bIsRelevant = IsRelevantWeight(EffectiveWeight);
		if (bIsRelevant && !bSkipNotifies && RenderComponent)
		{
			for (UPaperZDAnimNotify_Base* Notify : AnimSequence->GetAnimNotifies())
			{
#if WITH_EDITOR
				//Prevent from firing in editor if specifically requested
				if (OwningInstance != nullptr || Notify->bShouldFireInEditor)
#endif
				{
					Notify->TickNotify(DeltaTime, PlaybackMarker, PreviousTime, RenderComponent, OwningInstance);
				}
			}
		}

		//Notify anyone who needs to know about looping or sequence playback completion
		if (bSequencePlaybackComplete && bIsRelevant)
		{
			//We separate the delegates into two, one for looping, and one for playback completion
			if (bLooping)
			{
				OnPlaybackSequenceLooped.Broadcast(AnimSequence);
			}
			else if (PlaybackMarker != PreviousTime) //Make sure the animation actually just updated, instead of being stopped on the final frame of the animation due to a previous update
			{
				OnPlaybackSequenceComplete.Broadcast(AnimSequence);
				OnPlaybackSequenceComplete_Native.Broadcast(AnimSequence);
			}
		}
	}
}

void UPaperZDAnimPlayer::ProcessAnimSequenceNotifies(const UPaperZDAnimSequence* AnimSequence, float FromTime, float ToTime, float Weight /* = 1.0f */, UPaperZDAnimInstance* OwningInstance /* = nullptr */)
{
	const bool bIsRelevant = IsRelevantWeight(Weight);
	if (bIsRelevant && RegisteredRenderComponent.IsValid())
	{
		for (UPaperZDAnimNotify_Base* Notify : AnimSequence->GetAnimNotifies())
		{
			Notify->TickNotify(ToTime - FromTime, ToTime, FromTime, RegisteredRenderComponent.Get(), OwningInstance);
		}
	}
}

void UPaperZDAnimPlayer::PlaySingleAnimation(const UPaperZDAnimSequence* AnimSequence, float Playtime)
{
	if (AnimSequence && PlaybackHandle && RegisteredRenderComponent.IsValid())
	{
		//Because we're not given a playback data struct, we must create one from the given data
		const UPaperZDAnimSequence* PreviousAnimSequence = LastWeightedAnimation.AnimSequencePtr.Get();
		LastPlaybackData.SetAnimation(AnimSequence, Playtime);
		LastWeightedAnimation = LastPlaybackData.WeightedAnimations[0];

		//Potentially trigger the SequenceChanged events (backwards compatibility)
		if (bFireSequenceChangedEvents && OnPlaybackSequenceChanged.IsBound() && LastWeightedAnimation.AnimSequencePtr.Get() != PreviousAnimSequence)
		{
			OnPlaybackSequenceChanged.Broadcast(PreviousAnimSequence, LastWeightedAnimation.AnimSequencePtr.Get(), GetPlaybackProgress());
		}
		
		//Update the playback
		PlaybackHandle->UpdateRenderPlayback(RegisteredRenderComponent.Get(), LastPlaybackData, bPreviewPlayer);
	}
}

void UPaperZDAnimPlayer::Play(const FPaperZDAnimationPlaybackData& PlaybackData)
{
	if (PlaybackData.WeightedAnimations.Num() && PlaybackHandle && RegisteredRenderComponent.IsValid())
	{
		//Update playback
		PlaybackHandle->UpdateRenderPlayback(RegisteredRenderComponent.Get(), PlaybackData, bPreviewPlayer);

		//Store information for backwards support 
		const UPaperZDAnimSequence* PreviousAnimSequence = LastWeightedAnimation.AnimSequencePtr.Get();
		LastPlaybackData = PlaybackData;
		LastWeightedAnimation = LastPlaybackData.WeightedAnimations[0];

		//Potentially trigger the SequenceChanged events (backwards compatibility)
		if (bFireSequenceChangedEvents && OnPlaybackSequenceChanged.IsBound() && LastWeightedAnimation.AnimSequencePtr.Get() != PreviousAnimSequence)
		{
			OnPlaybackSequenceChanged.Broadcast(PreviousAnimSequence, LastWeightedAnimation.AnimSequencePtr.Get(), GetPlaybackProgress());
		}
	}
}

void UPaperZDAnimPlayer::RegisterRenderComponent(UPrimitiveComponent* RenderComponent)
{
	RegisteredRenderComponent = RenderComponent; 

	//Chance to configure the given render component
	if (PlaybackHandle)
	{
		PlaybackHandle->ConfigureRenderComponent(RenderComponent, bPreviewPlayer);
	}
}