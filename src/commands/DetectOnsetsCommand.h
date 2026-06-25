/**********************************************************************

   Audacity - A Digital Audio Editor
   Copyright 1999-2024 Audacity Team
   License: wxwidgets

******************************************************************//**

\file DetectOnsetsCommand.h
\brief Contains declaration of DetectOnsetsCommand class.

*//*******************************************************************/

#ifndef __DETECTONSETSCOMMAND__
#define __DETECTONSETSCOMMAND__

#include "Command.h"
#include "CommandType.h"

class DetectOnsetsCommand final : public AudacityCommand
{
public:
   static const ComponentInterfaceSymbol Symbol;

   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override
      { return XO("Detects note onsets in selected tracks using RMS envelope analysis."); }
   template<bool Const> bool VisitSettings( SettingsVisitorBase<Const> &S );
   bool VisitSettings( SettingsVisitor &S ) override;
   bool VisitSettings( ConstSettingsVisitor &S ) override;
   void PopulateOrExchange( ShuttleGui &S ) override;

   ManualPageID ManualPage() override
      { return L"Extra_Menu:_Scriptables_II"; }
   bool Apply( const CommandContext &context ) override;

private:
   double mThreshold;
   double mWindowMs;
   bool   mUseSelection;
   bool   bHasThreshold;
   bool   bHasWindowMs;
   bool   bHasUseSelection;
};

#endif /* End of include guard: __DETECTONSETSCOMMAND__ */
