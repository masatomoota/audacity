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
// truePeakLinear / truePeakDBFS are the 4x-oversampled inter-sample true-peak
// (Catmull-Rom cubic interpolation approximation of ITU BS.1770 true-peak).
void EmitStats( const CommandContext &context, const wxString &name,
   int trackIndex, int channelIndex, int nChannels, double sampleRate,
   double t0, double t1, double nSamples, double peakLinear, double peakDBFS,
   double rmsLinear, double rmsDBFS, double dcOffset, double clipCount,
   double truePeakLinear, double truePeakDBFS )
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
   context.AddItem( truePeakLinear,        wxT("truepeak_linear") );
   context.AddItem( truePeakDBFS,          wxT("truepeak_dbfs") );
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
               0.0, kSilenceFloor, 0.0, kSilenceFloor, 0.0, 0.0,
               0.0, kSilenceFloor );
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
         float  peakLinear    = 0.0f;
         float  truePeakLin   = 0.0f; // 4x-oversampled inter-sample true-peak (Catmull-Rom)
         double sumSq         = 0.0;  // for RMS
         double sampleSum     = 0.0;  // for DC offset
         long   clipCount     = 0;
         long   nSamples      = 0;

         // Catmull-Rom true-peak: we maintain a 4-sample sliding history
         // [p0, p1, p2, p3] where p1-p2 is the current pair being interpolated.
         // Between each consecutive pair (p1, p2) we compute 3 equally-spaced
         // interior points at t=0.25, 0.5, 0.75, making this a 4x oversampling
         // approximation of the ITU BS.1770 inter-sample true-peak estimator.
         float hist[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
         int   histFill = 0; // how many real samples have been loaded into history

         // Catmull-Rom: P(t) = 0.5 * ((2*p1) + (-p0+p2)*t
         //                           + (2*p0-5*p1+4*p2-p3)*t^2
         //                           + (-p0+3*p1-3*p2+p3)*t^3)
         // Compute 3 interior points at t=0.25, 0.5, 0.75 (4x oversampling).
         auto updateTruePeak = [&]( float p0, float p1, float p2, float p3 ) {
            static const float ts[3] = { 0.25f, 0.5f, 0.75f };
            for ( int ti = 0; ti < 3; ++ti )
            {
               const float t  = ts[ti];
               const float t2 = t * t;
               const float t3 = t2 * t;
               const float v  = 0.5f * (
                     (2.0f * p1)
                  + (-p0 + p2) * t
                  + (2.0f*p0 - 5.0f*p1 + 4.0f*p2 - p3) * t2
                  + (-p0 + 3.0f*p1 - 3.0f*p2 + p3) * t3 );
               const float av = std::fabs(v);
               if ( av > truePeakLin ) truePeakLin = av;
            }
         };

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

               // Shift history and try to compute Catmull-Rom interpolation.
               // We need 4 samples: hist[0..3] = [p0, p1, p2, p3].
               // Interpolation is over the interval [p1, p2].
               hist[0] = hist[1];
               hist[1] = hist[2];
               hist[2] = hist[3];
               hist[3] = v;
               if ( histFill < 3 )
                  ++histFill;
               else
                  updateTruePeak( hist[0], hist[1], hist[2], hist[3] );

               // Also track the true-peak of the samples themselves
               if ( a > truePeakLin ) truePeakLin = a;
            }
            nSamples += (long)block;
            position += block;

            if ( windowLen.as_double() > 0 )
               context.Progress(
                  ( position - s0 ).as_double() / windowLen.as_double() );
         }

         const double rmsLinear      = ( nSamples > 0 ) ? std::sqrt( sumSq / nSamples ) : 0.0;
         const double dcOffset       = ( nSamples > 0 ) ? ( sampleSum / nSamples ) : 0.0;
         const double peakDBFS       = ( peakLinear > 0.0f )
            ? LINEAR_TO_DB( (double)peakLinear ) : kSilenceFloor;
         const double rmsDBFS        = ( rmsLinear > 0.0 )
            ? LINEAR_TO_DB( rmsLinear ) : kSilenceFloor;
         const double truePeakDBFS   = ( truePeakLin > 0.0f )
            ? LINEAR_TO_DB( (double)truePeakLin ) : kSilenceFloor;

         EmitStats( context, wt->GetName(), trackIndex, channelIndex,
            nChannels, sampleRate, t0, t1, (double)nSamples,
            (double)peakLinear, peakDBFS, rmsLinear, rmsDBFS,
            dcOffset, (double)clipCount,
            (double)truePeakLin, truePeakDBFS );

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
