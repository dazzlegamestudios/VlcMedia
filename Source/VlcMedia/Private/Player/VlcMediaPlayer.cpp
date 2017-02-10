// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

#include "VlcMediaPlayer.h"
#include "VlcMediaPrivate.h"

#include "ArrayReader.h"
#include "FileHelper.h"
#include "IMediaOptions.h"
#include "Vlc.h"
#include "VlcMediaUtils.h"


/* FVlcMediaPlayer structors
 *****************************************************************************/


FVlcMediaPlayer::FVlcMediaPlayer(FLibvlcInstance* InVlcInstance)
	: CurrentRate(0.0f)
	, CurrentTime(FTimespan::Zero())
	, CurrentTimeDrift(FTimespan::Zero())
	, MediaSource(InVlcInstance)
	, Player(nullptr)
	, ShouldLoop(false)
{ }


FVlcMediaPlayer::~FVlcMediaPlayer()
{
	Close();
}


/* IMediaControls interface
 *****************************************************************************/

FTimespan FVlcMediaPlayer::GetDuration() const
{
	if (Player == nullptr)
	{
		return FTimespan::Zero();
	}

	int64 Length = FVlc::MediaPlayerGetLength(Player);

	if (Length <= 0)
	{
		return GetTime();
	}

	return FTimespan::FromMilliseconds(Length);
}


float FVlcMediaPlayer::GetRate() const
{
	return CurrentRate;
}


EMediaState FVlcMediaPlayer::GetState() const
{
	if (Player == nullptr)
	{
		return EMediaState::Closed;
	}

	ELibvlcState State = FVlc::MediaPlayerGetState(Player);

	switch (State)
	{
	case ELibvlcState::Error:
		return EMediaState::Error;

	case ELibvlcState::Buffering:
	case ELibvlcState::Opening:
		return EMediaState::Preparing;

	case ELibvlcState::Paused:
		return EMediaState::Paused;

	case ELibvlcState::Playing:
		return EMediaState::Playing;

	case ELibvlcState::Ended:
	case ELibvlcState::NothingSpecial:
	case ELibvlcState::Stopped:
		return EMediaState::Stopped;
	}

	return EMediaState::Error; // should never get here
}


TRangeSet<float> FVlcMediaPlayer::GetSupportedRates(EMediaRateThinning Thinning) const
{
	TRangeSet<float> Result;

	if (Thinning == EMediaRateThinning::Thinned)
	{
		Result.Add(TRange<float>(0.0f, 10.0f));
	}
	else
	{
		Result.Add(TRange<float>(0.0f, 1.0f));
	}

	return Result;
}


FTimespan FVlcMediaPlayer::GetTime() const
{
	return CurrentTime;
}


bool FVlcMediaPlayer::IsLooping() const
{
	return ShouldLoop;
}


bool FVlcMediaPlayer::Seek(const FTimespan& Time)
{
	ELibvlcState State = FVlc::MediaPlayerGetState(Player);

	if ((State == ELibvlcState::Opening) ||
		(State == ELibvlcState::Buffering) ||
		(State == ELibvlcState::Error))
	{
		return false;
	}

	if (Time != CurrentTime)
	{
		CurrentTimeDrift = FTimespan::Zero();
		FVlc::MediaPlayerSetTime(Player, Time.GetTotalMilliseconds());
	}

	return true;
}


bool FVlcMediaPlayer::SetLooping(bool Looping)
{
	ShouldLoop = Looping;
	return true;
}


bool FVlcMediaPlayer::SetRate(float Rate)
{
	if (Player == nullptr)
	{
		return false;
	}

	if ((FVlc::MediaPlayerSetRate(Player, Rate) == -1))
	{
		return false;
	}

	if (FMath::IsNearlyZero(Rate))
	{
		if (FVlc::MediaPlayerGetState(Player) == ELibvlcState::Playing)
		{
			if (FVlc::MediaPlayerCanPause(Player) == 0)
			{
				return false;
			}

			FVlc::MediaPlayerPause(Player);
		}
	}
	else if (FVlc::MediaPlayerGetState(Player) != ELibvlcState::Playing)
	{
		if (FVlc::MediaPlayerPlay(Player) == -1)
		{
			return false;
		}
	}

	return true;
}


bool FVlcMediaPlayer::SupportsFeature(EMediaFeature Feature) const
{
	if ((Feature == EMediaFeature::Scrubbing) || (Feature == EMediaFeature::Seeking))
	{
		return ((Player != nullptr) && (FVlc::MediaPlayerIsSeekable(Player) != 0));
	}

	return ((Feature == EMediaFeature::AudioSink) ||
			(Feature == EMediaFeature::VideoSink));
}


bool FVlcMediaPlayer::SupportsRate(float Rate, EMediaRateThinning Thinning) const
{
	return (Rate >= 0.0f) && (Rate <= 10.0f);
}


/* IMediaPlayer interface
 *****************************************************************************/

void FVlcMediaPlayer::Close()
{
	if (Player == nullptr)
	{
		return;
	}

	// detach callback handlers
	Output.Shutdown();
	Tracks.Shutdown();

	// release player
	FVlc::MediaPlayerStop(Player);
	FVlc::MediaPlayerRelease(Player);
	Player = nullptr;

	// reset fields
	CurrentRate = 0.0f;
	CurrentTime = FTimespan::Zero();
	MediaSource.Close();
	Info.Empty();

	// notify listeners
	MediaEvent.Broadcast(EMediaEvent::TracksChanged);
	MediaEvent.Broadcast(EMediaEvent::MediaClosed);
}


IMediaControls& FVlcMediaPlayer::GetControls() 
{
	return *this;
}


FString FVlcMediaPlayer::GetInfo() const
{
	return Info;
}


FName FVlcMediaPlayer::GetName() const
{
	static FName PlayerName(TEXT("VlcMedia"));
	return PlayerName;
}


IMediaOutput& FVlcMediaPlayer::GetOutput()
{
	return Output;
}


FString FVlcMediaPlayer::GetStats() const
{
	FLibvlcMedia* Media = MediaSource.GetMedia();

	if (Media == nullptr)
	{
		return TEXT("No media opened.");
	}

	FLibvlcMediaStats Stats;
	
	if (!FVlc::MediaGetStats(Media, &Stats))
	{
		return TEXT("Stats currently not available.");
	}

	FString StatsString;
	{
		StatsString += TEXT("General\n");
		StatsString += FString::Printf(TEXT("    Decoded Video: %i\n"), Stats.DecodedVideo);
		StatsString += FString::Printf(TEXT("    Decoded Audio: %i\n"), Stats.DecodedAudio);
		StatsString += FString::Printf(TEXT("    Displayed Pictures: %i\n"), Stats.DisplayedPictures);
		StatsString += FString::Printf(TEXT("    Lost Pictures: %i\n"), Stats.LostPictures);
		StatsString += FString::Printf(TEXT("    Played A-Buffers: %i\n"), Stats.PlayedAbuffers);
		StatsString += FString::Printf(TEXT("    Lost Lost A-Buffers: %i\n"), Stats.LostAbuffers);
		StatsString += TEXT("\n");

		StatsString += TEXT("Input\n");
		StatsString += FString::Printf(TEXT("    Bit Rate: %i\n"), Stats.InputBitrate);
		StatsString += FString::Printf(TEXT("    Bytes Read: %i\n"), Stats.ReadBytes);
		StatsString += TEXT("\n");

		StatsString += TEXT("Demux\n");
		StatsString += FString::Printf(TEXT("    Bit Rate: %f\n"), Stats.DemuxBitrate);
		StatsString += FString::Printf(TEXT("    Bytes Read: %i\n"), Stats.DemuxReadBytes);
		StatsString += FString::Printf(TEXT("    Corrupted: %i\n"), Stats.DemuxCorrupted);
		StatsString += FString::Printf(TEXT("    Discontinuity: %i\n"), Stats.DemuxDiscontinuity);
		StatsString += TEXT("\n");

		StatsString += TEXT("Network\n");
		StatsString += FString::Printf(TEXT("    Bitrate: %f\n"), Stats.SendBitrate);
		StatsString += FString::Printf(TEXT("    Sent Bytes: %i\n"), Stats.SentBytes);
		StatsString += FString::Printf(TEXT("    Sent Packets: %i\n"), Stats.SentPackets);
		StatsString += TEXT("\n");
	}

	return StatsString;
}


IMediaTracks& FVlcMediaPlayer::GetTracks()
{
	return Tracks;
}


FString FVlcMediaPlayer::GetUrl() const
{
	return MediaSource.GetCurrentUrl();
}


bool FVlcMediaPlayer::Open(const FString& Url, const IMediaOptions& Options)
{
	Close();

	if (Url.IsEmpty())
	{
		return false;
	}

	if (Url.StartsWith(TEXT("file://")))
	{
		// open local files via platform file system
		TSharedPtr<FArchive, ESPMode::ThreadSafe> Archive;
		const TCHAR* FilePath = &Url[7];

		if (Options.GetMediaOption("PrecacheFile", false))
		{
			FArrayReader* Reader = new FArrayReader;

			if (FFileHelper::LoadFileToArray(*Reader, FilePath))
			{
				Archive = MakeShareable(Reader);
			}
			else
			{
				delete Reader;
			}
		}
		else
		{
			Archive = MakeShareable(IFileManager::Get().CreateFileReader(FilePath));
		}

		if (!Archive.IsValid())
		{
			UE_LOG(LogVlcMedia, Warning, TEXT("Failed to open media file: %s"), FilePath);

			return false;
		}

		if (!MediaSource.OpenArchive(Archive.ToSharedRef(), Url))
		{
			return false;
		}
	}
	else if (!MediaSource.OpenUrl(Url))
	{
		return false;
	}

	return InitializePlayer();
}


bool FVlcMediaPlayer::Open(const TSharedRef<FArchive, ESPMode::ThreadSafe>& Archive, const FString& OriginalUrl, const IMediaOptions& Options)
{
	Close();

	if (OriginalUrl.IsEmpty() || !MediaSource.OpenArchive(Archive, OriginalUrl))
	{
		return false;
	}
	
	return InitializePlayer();
}


/* IMediaTickable interface
 *****************************************************************************/

void FVlcMediaPlayer::TickInput(FTimespan Timecode, FTimespan DeltaTime, bool /*Locked*/)
{
	if (Player == nullptr)
	{
		return;
	}

	// process events
	ELibvlcEventType Event;

	while (Events.Dequeue(Event))
	{
		switch (Event)
		{
		case ELibvlcEventType::MediaParsedChanged:
			MediaEvent.Broadcast(EMediaEvent::TracksChanged);
			break;

		case ELibvlcEventType::MediaPlayerEndReached:
			// begin hack: this causes a short delay, but there seems to be no
			// other way. looping via VLC Media List players is also broken :(
			FVlc::MediaPlayerStop(Player);
			// end hack

			MediaEvent.Broadcast(EMediaEvent::PlaybackEndReached);

			if (ShouldLoop && (CurrentRate != 0.0f))
			{
				SetRate(CurrentRate);
			}
			else
			{
				MediaEvent.Broadcast(EMediaEvent::PlaybackSuspended);
			}
			break;

		case ELibvlcEventType::MediaPlayerPaused:
			MediaEvent.Broadcast(EMediaEvent::PlaybackSuspended);
			break;

		case ELibvlcEventType::MediaPlayerPlaying:
			CurrentTimeDrift = FTimespan::Zero();
			MediaEvent.Broadcast(EMediaEvent::PlaybackResumed);
			break;

		case ELibvlcEventType::MediaPlayerPositionChanged:
			CurrentTime = FTimespan::FromMilliseconds(FMath::Max<int64>(0, FVlc::MediaPlayerGetTime(Player)));
			CurrentTimeDrift = FTimespan::Zero();
			break;

		default:
			continue;
		}
	}

	const ELibvlcState State = FVlc::MediaPlayerGetState(Player);

	// update current time & rate
	if (State == ELibvlcState::Playing)
	{
		CurrentRate = FVlc::MediaPlayerGetRate(Player);

		// interpolate time (FVlc::MediaPlayerGetTime is too inacurate)
		const FTimespan TimeCorrection = DeltaTime * CurrentRate;

		CurrentTime += TimeCorrection;
		CurrentTimeDrift += TimeCorrection;
	}
	else
	{
		CurrentRate = 0.0f;

		// poll time when paused (VLC doesn't send events when scrubbing)
		if (State == ELibvlcState::Paused)
		{
			CurrentTime = FTimespan::FromMilliseconds(FMath::Max<int64>(0, FVlc::MediaPlayerGetTime(Player))) + CurrentTimeDrift;
		}
	}

	Output.Update(Timecode, CurrentTime, (State == ELibvlcState::Playing) ? CurrentRate : 0.0f);
}


/* FVlcMediaPlayer implementation
 *****************************************************************************/

bool FVlcMediaPlayer::InitializePlayer()
{
	// create player for media source
	Player = FVlc::MediaPlayerNewFromMedia(MediaSource.GetMedia());

	if (Player == nullptr)
	{
		UE_LOG(LogVlcMedia, Warning, TEXT("Failed to initialize media player: %s"), ANSI_TO_TCHAR(FVlc::Errmsg()));

		return false;
	}

	// attach to event managers
	FLibvlcEventManager* MediaEventManager = FVlc::MediaEventManager(MediaSource.GetMedia());
	FLibvlcEventManager* PlayerEventManager = FVlc::MediaPlayerEventManager(Player);

	if ((MediaEventManager == nullptr) || (PlayerEventManager == nullptr))
	{
		FVlc::MediaPlayerRelease(Player);
		Player = nullptr;

		return false;
	}

	FVlc::EventAttach(MediaEventManager, ELibvlcEventType::MediaParsedChanged, &FVlcMediaPlayer::StaticEventCallback, this);
	FVlc::EventAttach(PlayerEventManager, ELibvlcEventType::MediaPlayerEndReached, &FVlcMediaPlayer::StaticEventCallback, this);
	FVlc::EventAttach(PlayerEventManager, ELibvlcEventType::MediaPlayerPlaying, &FVlcMediaPlayer::StaticEventCallback, this);
	FVlc::EventAttach(PlayerEventManager, ELibvlcEventType::MediaPlayerPositionChanged, &FVlcMediaPlayer::StaticEventCallback, this);
	FVlc::EventAttach(PlayerEventManager, ELibvlcEventType::MediaPlayerStopped, &FVlcMediaPlayer::StaticEventCallback, this);

	// initialize player
	CurrentRate = 0.0f;
	CurrentTime = FTimespan::Zero();
	CurrentTimeDrift = FTimespan::Zero();

	MediaEvent.Broadcast(EMediaEvent::MediaOpened);

	return true;
}


/* FVlcMediaPlayer static functions
 *****************************************************************************/

void FVlcMediaPlayer::StaticEventCallback(FLibvlcEvent* Event, void* UserData)
{
	UE_LOG(LogVlcMedia, Verbose, TEXT("LibVLC event: %s"), *VlcMedia::EventToString(Event));

	auto MediaPlayer = (FVlcMediaPlayer*)UserData;

	if (MediaPlayer == nullptr)
	{
		return;
	}

	if (Event->Type == ELibvlcEventType::MediaParsedChanged)
	{
		MediaPlayer->Tracks.Initialize(*MediaPlayer->Player, MediaPlayer->Info);
		MediaPlayer->Output.Initialize(*MediaPlayer->Player);
	}

	MediaPlayer->Events.Enqueue(Event->Type);
}
