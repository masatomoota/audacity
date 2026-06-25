/**********************************************************************

   Audacity - A Digital Audio Editor
   Copyright 1999-2024 Audacity Team
   License: wxwidgets

******************************************************************//**

\file GetAudioStatsCommand.h
\brief Contains declaration of GetAudioStatsCommand class.

*//*******************************************************************/

#ifndef __GETAUDIOSTATSCOMMAND__
#define __GETAUDIOSTATSCOMMAND__

#include "Command.h"
#include "CommandType.h"

/**
 \class GetAudioStatsCommand
 \brief Reports per-channel audio measurement statistics (peak, RMS, DC offset,
        clipping sample count) for every selected WaveTrack over the current
        time selection (or whole-track extent when UseSelection=false).

 The results are emitted as a JSON array via the CommandContext structured
 output API so that scripting/MCP callers can parse them unambiguously.
 Each array element is a struct with: name, track_index, channel_index,
 n_channels, sample_rate, start, end, n_samples, peak_linear, peak_dbfs,
 rms_linear, rms_dbfs, dc_offset, clip_count.
 */
class GetAudioStatsCommand final : public AudacityCommand
{
public:
   static const ComponentInterfaceSymbol Symbol;

   // ComponentInterface overrides
   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override
      { return XO("Reports audio measurement statistics for selected tracks."); }
   template<bool Const> bool VisitSettings( SettingsVisitorBase<Const> &S );
   bool VisitSettings( SettingsVisitor &S ) override;
   bool VisitSettings( ConstSettingsVisitor &S ) override;
   void PopulateOrExchange( ShuttleGui &S ) override;

   // AudacityCommand overrides
   ManualPageID ManualPage() override
      { return L"Extra_Menu:_Scriptables_II"; }
   bool Apply( const CommandContext &context ) override;

private:
   // When true (default) use the project's current time selection; when false,
   // measure the whole extent of each track.
   bool mUseSelection;
   bool bHasUseSelection; // OptionalY flag
};

#endif /* End of include guard: __GETAUDIOSTATSCOMMAND__ */
