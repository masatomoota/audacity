/**********************************************************************

   Audacity - A Digital Audio Editor
   Copyright 1999-2024 Audacity Team
   License: wxwidgets

******************************************************************//**

\file GetAudioStatsCommand.cpp
\brief Contains definitions for GetAudioStatsCommand.

\class GetAudioStatsCommand
\brief Reports per-channel audio measurement statistics (peak, RMS, DC offset,
       clipping count) for every selected WaveTrack over the current time
       selection (or the full track when UseSelection=false).

Output is a JSON array emitted through the CommandContext structured API so
that scripting/MCP callers receive parseable results. Peak, RMS, DC offset and
clip count are all accumulated in a single block-by-block sample read loop
(mirroring CompareAudioCommand) so the command depends only on the stable
WaveTrack sample-access API.

*//*******************************************************************/

#include "GetAudioStatsCommand.h"

#include "CommandDispatch.h"
#include "MenuRegistry.h"
#include "../CommonCommandFlags.h"
#include "LoadCommands.h"
#include "ViewInfo.h"
#include "WaveTrack.h"
#include "MemoryX.h"        // MAX_AUDIO, LINEAR_TO_DB
#include "SampleCount.h"    // limitSampleBufferSize, sampleCount

#include <algorithm>
#include <cmath>

#include "SettingsVisitor.h"
#include "ShuttleGui.h"
#include "CommandContext.h"

// ─── Symbol ───────────────────────────────────────────────────────────────────
const ComponentInterfaceSymbol GetAudioStatsCommand::Symbol
{ XO("Get Audio Stats") };

// ─── Self-registration ────────────────────────────────────────────────────────
// A single anonymous-namespace static object registers the command with the
// BuiltinCommandsModule at static-init time.  No central list entry is needed.
namespace{ BuiltinCommandsModule::Registration< GetAudioStatsCommand > reg; }

// ─── Parameter declaration ────────────────────────────────────────────────────
// UseSelection (bool, optional, default present=true):
//   true  -> measure [t0, t1] from the project's current time selection.
//   false -> measure the full extent [GetStartTime(), GetEndTime()] of each track.
template<bool Const>
bool GetAudioStatsCommand::VisitSettings( SettingsVisitorBase<Const> &S )
{
   S.OptionalY( bHasUseSelection ).Define( mUseSelection, wxT("UseSelection"), true );
   return true;
}

bool GetAudioStatsCommand::VisitSettings( SettingsVisitor &S )
   { return VisitSettings<false>(S); }

bool GetAudioStatsCommand::VisitSettings( ConstSettingsVisitor &S )
   { return VisitSettings<true>(S); }

void GetAudioStatsCommand::PopulateOrExchange( ShuttleGui &S )
{
   S.AddSpace(0, 5);
   S.StartMultiColumn(2, wxALIGN_CENTER);
   {
      S.TieCheckBox( XXO("Use current selection:"), mUseSelection );
   }
   S.EndMultiColumn();
}

namespace {
// Emit one JSON struct of statistics for a single channel.
void EmitStats( const CommandContext &context, const wxString &name,
   int trackIndex, int channelIndex, int nChannels, double sampleRate,
   double t0, double t1, double nSamples, double peakLinear, double peakDBFS,
   double rmsLinear, double rmsDBFS, double dcOffset, double clipCount )
{
   context.StartStruct();
   context.AddItem( name,                  wxT("name") );
   context.AddItem( (double)trackIndex,    wxT("track_index") );
   context.AddItem( (double)channelIndex,  wxT("channel_index") );
   context.AddItem( (double)nChannels,     wxT("n_channels") );
   context.AddItem( sampleRate,            wxT("sample_rate") );
   context.AddItem( t0,                    wxT("start") );
   context.AddItem( t1,                    wxT("end") );
   context.AddItem( nSamples,              wxT("n_samples") );
   context.AddItem( peakLinear,            wxT("peak_linear") );
   context.AddItem( peakDBFS,              wxT("peak_dbfs") );
   context.AddItem( rmsLinear,             wxT("rms_linear") );
   context.AddItem( rmsDBFS,               wxT("rms_dbfs") );
   context.AddItem( dcOffset,              wxT("dc_offset") );
   context.AddItem( clipCount,             wxT("clip_count") );
   context.EndStruct();
}
}

// ─── Apply ────────────────────────────────────────────────────────────────────
bool GetAudioStatsCommand::Apply( const CommandContext &context )
{
   AudacityProject &proj = context.project;
   const double kSilenceFloor = -120.0;   // dBFS reported for digital silence

   double selT0 = 0.0, selT1 = 0.0;
   bool haveSelection = false;
   if ( mUseSelection )
   {
      auto &selectedRegion = ViewInfo::Get( proj ).selectedRegion;
      selT0 = selectedRegion.t0();
      selT1 = selectedRegion.t1();
      haveSelection = ( selT1 > selT0 );
   }

   auto trackRange = TrackList::Get( proj ).Selected<const WaveTrack>();
   if ( !*trackRange.first )
   {
      context.Error( wxT("No wave tracks are selected.") );
      return false;
   }

   context.StartArray();

   int trackIndex = 0;
   for ( const auto *wt : trackRange )
   {
      double t0, t1;
      if ( mUseSelection && haveSelection )
         { t0 = selT0; t1 = selT1; }
      else
         { t0 = wt->GetStartTime(); t1 = wt->GetEndTime(); }

      const int nChannels = (int)wt->NChannels();
      const double sampleRate = wt->GetRate();

      if ( t1 <= t0 )
      {
         // No audio in the window; emit zero-filled structs (one per channel).
         int channelIndex = 0;
         for ( const auto &channel : wt->Channels() )
         {
            (void)channel;
            EmitStats( context, wt->GetName(), trackIndex, channelIndex,
               nChannels, sampleRate, t0, t1, 0.0,
               0.0, kSilenceFloor, 0.0, kSilenceFloor, 0.0, 0.0 );
            ++channelIndex;
         }
         ++trackIndex;
         continue;
      }

      const sampleCount s0 = wt->TimeToLongSamples( t0 );
      const sampleCount s1 = wt->TimeToLongSamples( t1 );
      const sampleCount windowLen = s1 - s0;

      const size_t buffSize = wt->GetMaxBlockSize();
      Floats buf{ buffSize };

      int channelIndex = 0;
      for ( const auto &channel : wt->Channels() )
      {
         float  peakLinear = 0.0f;
         double sumSq      = 0.0;   // for RMS
         double sampleSum  = 0.0;   // for DC offset
         long   clipCount  = 0;
         long   nSamples   = 0;

         auto position = s0;
         while ( position < s1 )
         {
            auto block = limitSampleBufferSize(
               channel->GetBestBlockSize( position ), s1 - position );
            channel->GetFloats( buf.get(), position, block );

            for ( decltype(block) i = 0; i < block; ++i )
            {
               const float v = buf[i];
               const float a = std::fabs( v );
               if ( a > peakLinear ) peakLinear = a;
               if ( a >= MAX_AUDIO ) ++clipCount;
               sumSq     += (double)v * (double)v;
               sampleSum += v;
            }
            nSamples += (long)block;
            position += block;

            if ( windowLen.as_double() > 0 )
               context.Progress(
                  ( position - s0 ).as_double() / windowLen.as_double() );
         }

         const double rmsLinear = ( nSamples > 0 ) ? std::sqrt( sumSq / nSamples ) : 0.0;
         const double dcOffset  = ( nSamples > 0 ) ? ( sampleSum / nSamples ) : 0.0;
         const double peakDBFS  = ( peakLinear > 0.0f )
            ? LINEAR_TO_DB( (double)peakLinear ) : kSilenceFloor;
         const double rmsDBFS   = ( rmsLinear > 0.0 )
            ? LINEAR_TO_DB( rmsLinear ) : kSilenceFloor;

         EmitStats( context, wt->GetName(), trackIndex, channelIndex,
            nChannels, sampleRate, t0, t1, (double)nSamples,
            (double)peakLinear, peakDBFS, rmsLinear, rmsDBFS,
            dcOffset, (double)clipCount );

         ++channelIndex;
      }
      ++trackIndex;
   }

   context.EndArray();
   return true;
}

// ─── Menu registration (Extra > Scriptables II) ───────────────────────────────
namespace {
using namespace MenuRegistry;

AttachedItem sAttachment{
   Command( wxT("GetAudioStats"), XXO("Get Audio Stats..."),
      CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag() ),
   wxT("Optional/Extra/Part2/Scriptables2")
};
} // namespace
