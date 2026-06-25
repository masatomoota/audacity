/**********************************************************************

   Audacity - A Digital Audio Editor
   Copyright 1999-2024 Audacity Team
   License: wxwidgets

******************************************************************//**

\file DetectOnsetsCommand.cpp
\brief Contains definitions for DetectOnsetsCommand.

\class DetectOnsetsCommand
\brief Detects note onsets using a simple energy envelope approach:
       divide into non-overlapping windows, compute RMS-dB per window,
       report onset when the energy rises >= Threshold dB vs the previous
       window and is above the rolling 5-window baseline.

*//*******************************************************************/

#include "DetectOnsetsCommand.h"

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

const ComponentInterfaceSymbol DetectOnsetsCommand::Symbol
{ XO("Detect Onsets") };

namespace{ BuiltinCommandsModule::Registration< DetectOnsetsCommand > reg; }

template<bool Const>
bool DetectOnsetsCommand::VisitSettings( SettingsVisitorBase<Const> &S )
{
   S.OptionalN( bHasThreshold    ).Define( mThreshold,    wxT("Threshold"),    6.0,  0.1, 60.0 );
   S.OptionalN( bHasWindowMs     ).Define( mWindowMs,     wxT("WindowMs"),    20.0,  1.0, 500.0 );
   S.OptionalY( bHasUseSelection ).Define( mUseSelection, wxT("UseSelection"), true );
   return true;
}

bool DetectOnsetsCommand::VisitSettings( SettingsVisitor &S )
   { return VisitSettings<false>(S); }

bool DetectOnsetsCommand::VisitSettings( ConstSettingsVisitor &S )
   { return VisitSettings<true>(S); }

void DetectOnsetsCommand::PopulateOrExchange( ShuttleGui &S )
{
   S.AddSpace(0, 5);
   S.StartMultiColumn(2, wxALIGN_CENTER);
   {
      S.TieNumericTextBox( XXO("Threshold (dB rise):"),    mThreshold, 10 );
      S.TieNumericTextBox( XXO("Window (ms):"),            mWindowMs,  10 );
      S.TieCheckBox(       XXO("Use current selection:"),  mUseSelection );
   }
   S.EndMultiColumn();
}

bool DetectOnsetsCommand::Apply( const CommandContext &context )
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

   context.StartArray();  // outer array — one struct per track

   int trackIndex = 0;
   for ( const WaveTrack *wt : trackRange )
   {
      double t0, t1;
      if ( mUseSelection && haveSelection )
         { t0 = selT0; t1 = selT1; }
      else
         { t0 = wt->GetStartTime(); t1 = wt->GetEndTime(); }

      const double rate       = wt->GetRate();
      const size_t winSamples = std::max( size_t(1),
                                          size_t( rate * mWindowMs / 1000.0 ) );
      const auto channel = *wt->Channels().begin();

      const sampleCount s0 = wt->TimeToLongSamples( t0 );
      const sampleCount s1 = wt->TimeToLongSamples( t1 );

      const size_t buffSize = wt->GetMaxBlockSize();
      Floats buf{ buffSize };

      // Build RMS-dB energy vector across all windows
      std::vector<double> energyDB;
      std::vector<double> windowTimes;

      sampleCount wStart = s0;
      double      wAccSq = 0.0;
      size_t      wCount = 0;

      auto flushWindow = [&]()
      {
         if ( wCount == 0 ) return;
         const double rms = std::sqrt( wAccSq / wCount );
         const double db  = ( rms > 1e-10 ) ? 20.0 * std::log10( rms ) : -200.0;
         energyDB.push_back( db );
         windowTimes.push_back( wStart.as_double() / rate );
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
      flushWindow();

      // ── onset detection pass ──────────────────────────────────────────────
      constexpr int kBaselineLen = 5;
      std::vector<double> onsets;

      for ( size_t k = 1; k < energyDB.size(); ++k )
      {
         // Compute local baseline (mean of up to kBaselineLen previous windows)
         double baseline = 0.0;
         int    bcount   = 0;
         for ( int j = (int)k - kBaselineLen; j < (int)k; ++j )
         {
            if ( j >= 0 )
            {
               baseline += energyDB[ j ];
               ++bcount;
            }
         }
         if ( bcount > 0 ) baseline /= bcount;

         const double rise = energyDB[k] - energyDB[k - 1];
         const bool aboveBaseline = ( energyDB[k] > baseline + mThreshold * 0.5 );

         if ( rise >= mThreshold && aboveBaseline )
            onsets.push_back( windowTimes[k] );
      }

      // ── emit JSON struct for this track ───────────────────────────────────
      context.StartStruct();
      context.AddItem( wt->GetName(),            wxT("name") );
      context.AddItem( (double)trackIndex,       wxT("track_index") );
      context.AddItem( (double)onsets.size(),    wxT("count") );

      // Per R5: use struct-wrapped times since AddItem(double, wxString) requires
      // a name and bare array elements are not supported by the API.
      context.StartArray();  // "onsets"
      for ( double t : onsets )
      {
         context.StartStruct();
         context.AddItem( t, wxT("time") );
         context.EndStruct();
      }
      context.EndArray();

      context.EndStruct();
      ++trackIndex;
   }

   context.EndArray();
   return true;
}

namespace {
using namespace MenuRegistry;

AttachedItem sAttachment{
   Command( wxT("DetectOnsets"), XXO("Detect Onsets..."),
      CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag() ),
   wxT("Optional/Extra/Part2/Scriptables2")
};
} // namespace
