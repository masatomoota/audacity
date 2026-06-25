/**********************************************************************

   Audacity - A Digital Audio Editor
   Copyright 1999-2024 Audacity Team
   License: wxwidgets

******************************************************************//**

\file GetLoudnessCommand.h
\brief Contains declaration of GetLoudnessCommand class.

*//*******************************************************************/

#ifndef __GETLOUDNESSCOMMAND__
#define __GETLOUDNESSCOMMAND__

#include "Command.h"
#include "CommandType.h"

class GetLoudnessCommand final : public AudacityCommand
{
public:
   static const ComponentInterfaceSymbol Symbol;

   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override
      { return XO("Measures EBU R128 integrated loudness (LUFS) for selected tracks."); }
   template<bool Const> bool VisitSettings( SettingsVisitorBase<Const> &S );
   bool VisitSettings( SettingsVisitor &S ) override;
   bool VisitSettings( ConstSettingsVisitor &S ) override;
   void PopulateOrExchange( ShuttleGui &S ) override;

   ManualPageID ManualPage() override
      { return L"Extra_Menu:_Scriptables_II"; }
   bool Apply( const CommandContext &context ) override;

private:
   bool mUseSelection;
   bool bHasUseSelection;
};

#endif /* End of include guard: __GETLOUDNESSCOMMAND__ */
