/**********************************************************************

   Audacity - A Digital Audio Editor
   Copyright 1999-2024 Audacity Team
   License: wxwidgets

******************************************************************//**

\file DetectSilenceCommand.cpp
\brief Contains definitions for DetectSilenceCommand.

\class DetectSilenceCommand
\brief Detects silent regions in selected tracks using a 50ms sliding
       window RMS measurement. Reports intervals below a dBFS threshold
       that last at least MinDuration seconds.

*//*******************************************************************/

#include "DetectSilenceCommand.h"

#include "CommandDispatch.h"
#include "MenuRegistry.h"
#include "../CommonCommandFlags.h"
#include "LoadCommands.h"
#include "ViewInfo.h"
#include "WaveTrack.h"
#include "MemoryX.h"
#include "SampleCount.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "SettingsVisitor.h"
#include "ShuttleGui.h"
#include "CommandContext.h"

const ComponentInterfaceSymbol DetectSilenceCommand::Symbol
{ XO("Detect Silence") };

namespace{ BuiltinCommandsModule::Registration< DetectSilenceCommand > reg; }

template<bool Const>
bool DetectSilenceCommand::VisitSettings( SettingsVisitorBase<Const> &S )
{
   S.OptionalN( bHasThreshold    ).Define( mThreshold,    wxT("Threshold"),    -60.0, -120.0, 0.0 );
   S.OptionalN( bHasMinDuration  ).Define( mMinDuration,  wxT("MinDuration"),    0.5,  0.001, 3600.0 );
   S.OptionalY( bHasUseSelection ).Define( mUseSelection, wxT("UseSelection"),  true );
   return true;
}

bool DetectSilenceCommand::VisitSettings( SettingsVisitor &S )
   { return VisitSettings<false>(S); }

bool DetectSilenceCommand::VisitSettings( ConstSettingsVisitor &S )
   { return VisitSettings<true>(S); }

void DetectSilenceCommand::PopulateOrExchange( ShuttleGui &S )
{
   S.AddSpace(0, 5);
   S.StartMultiColumn(2, wxALIGN_CENTER);
   {
      S.TieNumericTextBox( XXO("Threshold (dBFS):"),      mThreshold,   10 );
      S.TieNumericTextBox( XXO("Min duration (sec):"),    mMinDuration, 10 );
      S.TieCheckBox(       XXO("Use current selection:"), mUseSelection );
   }
   S.EndMultiColumn();
}

bool DetectSilenceCommand::Apply( const CommandContext &context )
{
   AudacityProject &proj = context.project;

   double selT0 = 0.0, selT1 = 0.0;
   bool   haveSelection = false;
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

   // Threshold as linear RMS
   const double thresholdLinear = std::pow( 10.0, mThreshold / 20.0 );

   context.StartArray();  // outer array — one struct per track

   int trackIndex = 0;
   for ( const WaveTrack *wt : trackRange )
   {
      double t0, t1;
      if ( mUseSelection && haveSelection )
         { t0 = selT0; t1 = selT1; }
      else
         { t0 = wt->GetStartTime(); t1 = wt->GetEndTime(); }

      const double rate      = wt->GetRate();
      const size_t winSamples = std::max( size_t(1), size_t( rate * 0.050 ) );  // 50 ms

      // Operate on channel 0 (first channel)
      const auto channel = *wt->Channels().begin();

      const sampleCount s0 = wt->TimeToLongSamples( t0 );
      const sampleCount s1 = wt->TimeToLongSamples( t1 );

      const size_t buffSize = wt->GetMaxBlockSize();
      Floats buf{ buffSize };

      // Rolling window accumulator
      sampleCount wStart = s0;
      double      wAccSq = 0.0;
      size_t      wCount = 0;

      struct Interval { double start; double end; };
      std::vector<Interval> rawIntervals;

      auto flushWindow = [&]()
      {
         if ( wCount == 0 ) return;
         const double rms = std::sqrt( wAccSq / wCount );
         const bool silent = ( rms <= thresholdLinear );
         if ( silent )
         {
            double wStartSec = wStart.as_double() / rate;
            double wEndSec   = (wStart + (sampleCount)wCount).as_double() / rate;
            rawIntervals.push_back( { wStartSec, wEndSec } );
         }
         wStart += (sampleCount)wCount;
         wAccSq  = 0.0;
         wCount  = 0;
      };

      auto position = s0;
      while ( position < s1 )
      {
         auto block = limitSampleBufferSize(
            channel->GetBestBlockSize( position ), s1 - position );
         channel->GetFloats( buf.get(), position, block );

         for ( size_t i = 0; i < block; ++i )
         {
            const float v = buf[i];
            wAccSq += (double)v * (double)v;
            ++wCount;

            if ( wCount >= winSamples )
               flushWindow();
         }
         position += block;
      }
      // Flush any partial trailing window
      flushWindow();

      // ── merge consecutive silent windows ──────────────────────────────────
      std::vector<Interval> merged;
      for ( const auto &iv : rawIntervals )
      {
         if ( !merged.empty() && iv.start <= merged.back().end + 1e-9 )
            merged.back().end = iv.end;
         else
            merged.push_back( iv );
      }

      // ── apply min-duration filter ─────────────────────────────────────────
      std::vector<Interval> reported;
      for ( const auto &iv : merged )
         if ( (iv.end - iv.start) >= mMinDuration )
            reported.push_back( iv );

      // ── leading / trailing silence ────────────────────────────────────────
      double leadingSilence  = 0.0;
      double trailingSilence = 0.0;
      if ( !reported.empty() && reported.front().start <= t0 + 1e-9 )
         leadingSilence = reported.front().end - t0;
      if ( !reported.empty() && reported.back().end >= t1 - 1e-9 )
         trailingSilence = t1 - reported.back().start;

      // ── emit JSON struct for this track ───────────────────────────────────
      context.StartStruct();
      context.AddItem( wt->GetName(),         wxT("name") );
      context.AddItem( (double)trackIndex,    wxT("track_index") );
      context.AddItem( leadingSilence,        wxT("leading_silence") );
      context.AddItem( trailingSilence,       wxT("trailing_silence") );

      context.StartArray();  // "intervals"
      for ( const auto &iv : reported )
      {
         context.StartStruct();
         context.AddItem( iv.start, wxT("start") );
         context.AddItem( iv.end,   wxT("end") );
         context.EndStruct();
      }
      context.EndArray();  // "intervals"

      context.EndStruct();
      ++trackIndex;
   }

   context.EndArray();
   return true;
}

namespace {
using namespace MenuRegistry;

AttachedItem sAttachment{
   Command( wxT("DetectSilence"), XXO("Detect Silence..."),
      CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag() ),
   wxT("Optional/Extra/Part2/Scriptables2")
};
} // namespace
