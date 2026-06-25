/**********************************************************************

   Audacity - A Digital Audio Editor
   Copyright 1999-2024 Audacity Team
   License: wxwidgets

******************************************************************//**

\file DetectSilenceCommand.h
\brief Contains declaration of DetectSilenceCommand class.

*//*******************************************************************/

#ifndef __DETECTSILENCECOMMAND__
#define __DETECTSILENCECOMMAND__

#include "Command.h"
#include "CommandType.h"

class DetectSilenceCommand final : public AudacityCommand
{
public:
   static const ComponentInterfaceSymbol Symbol;

   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override
      { return XO("Detects silent regions in selected tracks."); }
   template<bool Const> bool VisitSettings( SettingsVisitorBase<Const> &S );
   bool VisitSettings( SettingsVisitor &S ) override;
   bool VisitSettings( ConstSettingsVisitor &S ) override;
   void PopulateOrExchange( ShuttleGui &S ) override;

   ManualPageID ManualPage() override
      { return L"Extra_Menu:_Scriptables_II"; }
   bool Apply( const CommandContext &context ) override;

private:
   double mThreshold;
   double mMinDuration;
   bool   mUseSelection;
   bool   bHasThreshold;
   bool   bHasMinDuration;
   bool   bHasUseSelection;
};

#endif /* End of include guard: __DETECTSILENCECOMMAND__ */
