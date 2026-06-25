/**********************************************************************

   Audacity - A Digital Audio Editor
   Copyright 1999-2024 Audacity Team
   License: wxwidgets

******************************************************************//**

\file GetLoudnessCommand.cpp
\brief Contains definitions for GetLoudnessCommand.

\class GetLoudnessCommand
\brief Measures EBU R128 integrated loudness (LUFS) for selected tracks
       using the EBUR128 class from lib-math. Mirrors LoudnessBase::
       AnalyseBufferBlock exactly. Selections shorter than ~400ms may
       fall below the EBU R128 absolute gate; in that case lufs_integrated
       is reported as -999.0 (sentinel for -infinity, per JSON spec) and
       warning is set to "below_gate".

Note: EBUR128 only supports integrated loudness; there are no momentary
or short-term getters.

*//*******************************************************************/

#include "GetLoudnessCommand.h"

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
#include <limits>
#include <vector>

#include "SettingsVisitor.h"
#include "ShuttleGui.h"
#include "CommandContext.h"

#include "EBUR128.h"

const ComponentInterfaceSymbol GetLoudnessCommand::Symbol
{ XO("Get Loudness") };

namespace{ BuiltinCommandsModule::Registration< GetLoudnessCommand > reg; }

template<bool Const>
bool GetLoudnessCommand::VisitSettings( SettingsVisitorBase<Const> &S )
{
   S.OptionalY( bHasUseSelection ).Define( mUseSelection, wxT("UseSelection"), true );
   return true;
}

bool GetLoudnessCommand::VisitSettings( SettingsVisitor &S )
   { return VisitSettings<false>(S); }

bool GetLoudnessCommand::VisitSettings( ConstSettingsVisitor &S )
   { return VisitSettings<true>(S); }

void GetLoudnessCommand::PopulateOrExchange( ShuttleGui &S )
{
   S.AddSpace(0, 5);
   S.StartMultiColumn(2, wxALIGN_CENTER);
   {
      S.TieCheckBox( XXO("Use current selection:"), mUseSelection );
   }
   S.EndMultiColumn();
}

bool GetLoudnessCommand::Apply( const CommandContext &context )
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

      if ( t1 <= t0 )
      {
         ++trackIndex;
         continue;
      }

      const double rate      = wt->GetRate();
      // EBUR128 constructor takes size_t channels; clamp to 2 (stereo max for BS.1770)
      const size_t nChannels = std::min( wt->NChannels(), size_t(2) );

      EBUR128 meter( rate, nChannels );

      const sampleCount s0 = wt->TimeToLongSamples( t0 );
      const sampleCount s1 = wt->TimeToLongSamples( t1 );

      // Allocate one buffer per channel (at most 2)
      const size_t buffSize = wt->GetMaxBlockSize();
      Floats bufs[2] { Floats{ buffSize }, Floats{ buffSize } };

      // Collect pointers to channel objects (at most nChannels)
      std::vector<const WaveChannel *> channels;
      {
         size_t ch = 0;
         for ( const auto &chan : wt->Channels() )
         {
            if ( ch >= nChannels ) break;
            channels.push_back( chan.get() );
            ++ch;
         }
      }

      auto position = s0;
      while ( position < s1 )
      {
         // Use channel 0's block size (all channels share clip boundaries)
         auto block = limitSampleBufferSize(
            channels[0]->GetBestBlockSize( position ), s1 - position );

         // Read each channel
         for ( size_t ch = 0; ch < nChannels; ++ch )
            channels[ch]->GetFloats( bufs[ch].get(), position, block );

         // Feed EBUR128 — mirror LoudnessBase::AnalyseBufferBlock exactly
         for ( size_t i = 0; i < block; ++i )
         {
            for ( size_t ch = 0; ch < nChannels; ++ch )
               meter.ProcessSampleFromChannel( bufs[ch][i], ch );
            meter.NextSample();
         }

         position += block;
      }

      // ── read integrated loudness ──────────────────────────────────────────
      const double linearLoudness = meter.IntegrativeLoudness();
      // JSON cannot represent -infinity; use -999.0 as sentinel (broadcast LUFS convention)
      const double lufs = ( linearLoudness > 0.0 )
         ? meter.IntegrativeLoudnessToLUFS( linearLoudness )
         : -999.0;

      const bool belowGate = ( linearLoudness == 0.0 );

      // ── sliding-window max loudness (short-term and momentary) ────────────
      // These are sliding-window maxima computed using EBUR128's gated integrated
      // loudness over each window — a practical approximation of true ungated
      // momentary/short-term loudness as described in EBU R128 §2.2-§2.3.
      // short_term: 3.0 s window / 1.0 s hop
      // momentary:  0.4 s window / 0.1 s hop
      struct WindowDef { double winSec; double hopSec; };
      const WindowDef wdefs[2] = { { 3.0, 1.0 }, { 0.4, 0.1 } };
      double maxLufs[2] = { -999.0, -999.0 };

      for ( int wi = 0; wi < 2; ++wi )
      {
         const double winSec = wdefs[wi].winSec;
         const double hopSec = wdefs[wi].hopSec;
         const double selLen = t1 - t0;

         if ( selLen < winSec )
         {
            // Selection shorter than window — emit sentinel (-999.0)
            maxLufs[wi] = -999.0;
            continue;
         }

         // Slide windows across [t0, t1]
         double winStart = t0;
         while ( winStart + winSec <= t1 + 1e-9 )
         {
            const double winEnd = std::min( winStart + winSec, t1 );
            const sampleCount ws0 = wt->TimeToLongSamples( winStart );
            const sampleCount ws1 = wt->TimeToLongSamples( winEnd );

            EBUR128 winMeter( rate, nChannels );

            auto wpos = ws0;
            while ( wpos < ws1 )
            {
               auto block = limitSampleBufferSize(
                  channels[0]->GetBestBlockSize( wpos ), ws1 - wpos );
               for ( size_t ch = 0; ch < nChannels; ++ch )
                  channels[ch]->GetFloats( bufs[ch].get(), wpos, block );
               for ( size_t i = 0; i < block; ++i )
               {
                  for ( size_t ch = 0; ch < nChannels; ++ch )
                     winMeter.ProcessSampleFromChannel( bufs[ch][i], ch );
                  winMeter.NextSample();
               }
               wpos += block;
            }

            const double wLinear = winMeter.IntegrativeLoudness();
            if ( wLinear > 0.0 )
            {
               const double wLufs = winMeter.IntegrativeLoudnessToLUFS( wLinear );
               if ( std::isfinite(wLufs) && wLufs > maxLufs[wi] )
                  maxLufs[wi] = wLufs;
            }

            winStart += hopSec;
            if ( winStart + winSec > t1 + 1e-9 ) break;
         }
      }

      // ── emit JSON struct ──────────────────────────────────────────────────
      context.StartStruct();
      context.AddItem( wt->GetName(),          wxT("name") );
      context.AddItem( (double)trackIndex,     wxT("track_index") );
      context.AddItem( (double)nChannels,      wxT("n_channels") );
      context.AddItem( t0,                     wxT("start") );
      context.AddItem( t1,                     wxT("end") );
      context.AddItem( lufs,                   wxT("lufs_integrated") );
      context.AddItem( maxLufs[0],             wxT("short_term_max_lufs") );
      context.AddItem( maxLufs[1],             wxT("momentary_max_lufs") );
      context.AddItem( belowGate ? wxString(wxT("below_gate")) : wxString(wxT("")),
                                               wxT("warning") );
      context.EndStruct();

      ++trackIndex;
   }

   context.EndArray();
   return true;
}

namespace {
using namespace MenuRegistry;

AttachedItem sAttachment{
   Command( wxT("GetLoudness"), XXO("Get Loudness..."),
      CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag() ),
   wxT("Optional/Extra/Part2/Scriptables2")
};
} // namespace
