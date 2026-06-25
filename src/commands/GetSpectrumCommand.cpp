/**********************************************************************

   Audacity - A Digital Audio Editor
   Copyright 1999-2024 Audacity Team
   License: wxwidgets

******************************************************************//**

\file GetSpectrumCommand.cpp
\brief Contains definitions for GetSpectrumCommand.

\class GetSpectrumCommand
\brief Computes the frequency spectrum of the selected audio using
       SpectrumAnalyst with a Hann window. Emits log-spaced frequency
       bands, the dominant frequency, and the spectral centroid.

*//*******************************************************************/

#include "GetSpectrumCommand.h"

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

#include "SettingsVisitor.h"
#include "ShuttleGui.h"
#include "CommandContext.h"

#include "SpectrumAnalyst.h"
#include "FFT.h"

const ComponentInterfaceSymbol GetSpectrumCommand::Symbol
{ XO("Get Spectrum") };

namespace{ BuiltinCommandsModule::Registration< GetSpectrumCommand > reg; }

template<bool Const>
bool GetSpectrumCommand::VisitSettings( SettingsVisitorBase<Const> &S )
{
   S.OptionalN( bHasBands        ).Define( mBands,        wxT("Bands"),        48,  1, 64 );
   S.OptionalY( bHasUseSelection ).Define( mUseSelection, wxT("UseSelection"), true );
   return true;
}

bool GetSpectrumCommand::VisitSettings( SettingsVisitor &S )
   { return VisitSettings<false>(S); }

bool GetSpectrumCommand::VisitSettings( ConstSettingsVisitor &S )
   { return VisitSettings<true>(S); }

void GetSpectrumCommand::PopulateOrExchange( ShuttleGui &S )
{
   S.AddSpace(0, 5);
   S.StartMultiColumn(2, wxALIGN_CENTER);
   {
      S.TieNumericTextBox( XXO("Number of bands:"), mBands, 10 );
      S.TieCheckBox( XXO("Use current selection:"), mUseSelection );
   }
   S.EndMultiColumn();
}

bool GetSpectrumCommand::Apply( const CommandContext &context )
{
   AudacityProject &proj = context.project;

   // ── selection bounds ──────────────────────────────────────────────────────
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

   // ── use first selected track only ────────────────────────────────────────
   const WaveTrack *wt = *trackRange.first;

   double t0, t1;
   if ( mUseSelection && haveSelection )
      { t0 = selT0; t1 = selT1; }
   else
      { t0 = wt->GetStartTime(); t1 = wt->GetEndTime(); }

   if ( t1 <= t0 )
   {
      context.Error( wxT("Selection is empty.") );
      return false;
   }

   const double mRate  = wt->GetRate();
   const auto   start  = wt->TimeToLongSamples( t0 );
   const auto   end    = wt->TimeToLongSamples( t1 );

   // Cap at ~46-minute limit (same as PlotSpectrumBase)
   constexpr size_t kMaxDataLen = size_t(2) << 26;
   size_t dataLen = std::min( (end - start).as_size_t(), kMaxDataLen );

   // ── collect samples: mix all channels per-channel read and accumulate ─────
   std::vector<float> mData( dataLen, 0.f );
   {
      const size_t nChannels = wt->NChannels();
      const size_t buffSize  = wt->GetMaxBlockSize();
      Floats buf{ buffSize };

      size_t chIdx = 0;
      for ( const auto &channel : wt->Channels() )
      {
         if ( chIdx >= 2 ) break;  // only mix up to 2 channels

         auto position = start;
         size_t dataOffset = 0;
         while ( position < end && dataOffset < dataLen )
         {
            sampleCount remaining = end - position;
            auto block = limitSampleBufferSize(
               channel->GetBestBlockSize( position ),
               std::min( remaining, sampleCount( dataLen - dataOffset ) ) );
            channel->GetFloats( buf.get(), position, block );

            for ( size_t i = 0; i < block && dataOffset < dataLen; ++i, ++dataOffset )
               mData[dataOffset] += buf[i];

            position += block;
         }
         ++chIdx;
      }
   }

   // ── adapt window size downward for short selections ───────────────────────
   size_t windowSize = 2048;
   while ( windowSize > dataLen && windowSize > 32 )
      windowSize >>= 1;

   if ( dataLen < windowSize )
   {
      context.Error( wxT("Selection too short to compute spectrum (need >= 32 samples).") );
      return false;
   }

   // ── run SpectrumAnalyst ───────────────────────────────────────────────────
   SpectrumAnalyst analyst;
   float yMin = 0.f, yMax = 0.f;
   const bool ok = analyst.Calculate(
      SpectrumAnalyst::Spectrum,
      eWinFuncHann,
      windowSize,
      mRate,
      mData.data(),
      dataLen,
      &yMin, &yMax );

   if ( !ok )
   {
      context.Error( wxT("SpectrumAnalyst::Calculate failed.") );
      return false;
   }

   const int    processedSize = analyst.GetProcessedSize(); // == windowSize/2
   const float *processed     = analyst.GetProcessed();

   // ── dominant frequency: scan for global peak bin (R4: don't use FindPeak(0)) ─
   int peakBin = 1;
   float peakDB = processed[1];
   for ( int i = 2; i < processedSize; ++i )
      if ( processed[i] > peakDB ) { peakDB = processed[i]; peakBin = i; }
   const double dominantHz = peakBin * mRate / (double)windowSize;

   // ── spectral centroid (manual from raw dB array) ──────────────────────────
   double weightedSum = 0.0, totalPower = 0.0;
   for ( int i = 1; i < processedSize; ++i )
   {
      double freq       = i * mRate / (double)windowSize;
      double linearPow  = std::pow( 10.0, processed[i] / 10.0 );
      weightedSum      += freq * linearPow;
      totalPower       += linearPow;
   }
   const double centroidHz = ( totalPower > 0.0 ) ? weightedSum / totalPower : 0.0;

   // ── reduce to mBands log-spaced output bins ───────────────────────────────
   const int    bands        = std::max( 1, std::min( mBands, 64 ) );
   const double nyquist      = mRate / 2.0;
   const double lowestBinHz  = mRate / (double)windowSize;
   const double logLo  = std::log( lowestBinHz );
   const double logHi  = std::log( nyquist );
   const double logStep = ( logHi - logLo ) / bands;

   // ── emit JSON ─────────────────────────────────────────────────────────────
   context.StartStruct();
   context.AddItem( mRate,                 wxT("sample_rate") );
   context.AddItem( (double)dominantHz,    wxT("dominant_freq_hz") );
   context.AddItem( centroidHz,            wxT("centroid_hz") );

   context.StartArray();
   for ( int k = 0; k < bands; ++k )
   {
      double fLo   = std::exp( logLo + k       * logStep );
      double fHi   = std::exp( logLo + (k + 1) * logStep );
      float  dbVal = analyst.GetProcessedValue( (float)fLo, (float)fHi );

      context.StartStruct();
      context.AddItem( fLo,            wxT("freq_hz") );
      context.AddItem( (double)dbVal,  wxT("magnitude_db") );
      context.EndStruct();
   }
   context.EndArray();

   context.EndStruct();
   return true;
}

namespace {
using namespace MenuRegistry;

AttachedItem sAttachment{
   Command( wxT("GetSpectrum"), XXO("Get Spectrum..."),
      CommandDispatch::OnAudacityCommand, AudioIONotBusyFlag() ),
   wxT("Optional/Extra/Part2/Scriptables2")
};
} // namespace
