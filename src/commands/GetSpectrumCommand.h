/**********************************************************************

   Audacity - A Digital Audio Editor
   Copyright 1999-2024 Audacity Team
   License: wxwidgets

******************************************************************//**

\file GetSpectrumCommand.h
\brief Contains declaration of GetSpectrumCommand class.

*//*******************************************************************/

#ifndef __GETSPECTRUMCOMMAND__
#define __GETSPECTRUMCOMMAND__

#include "Command.h"
#include "CommandType.h"

class GetSpectrumCommand final : public AudacityCommand
{
public:
   static const ComponentInterfaceSymbol Symbol;

   ComponentInterfaceSymbol GetSymbol() const override { return Symbol; }
   TranslatableString GetDescription() const override
      { return XO("Computes the frequency spectrum of the selected audio."); }
   template<bool Const> bool VisitSettings( SettingsVisitorBase<Const> &S );
   bool VisitSettings( SettingsVisitor &S ) override;
   bool VisitSettings( ConstSettingsVisitor &S ) override;
   void PopulateOrExchange( ShuttleGui &S ) override;

   ManualPageID ManualPage() override
      { return L"Extra_Menu:_Scriptables_II"; }
   bool Apply( const CommandContext &context ) override;

private:
   int    mBands;
   bool   mUseSelection;
   bool   bHasBands;
   bool   bHasUseSelection;
};

#endif /* End of include guard: __GETSPECTRUMCOMMAND__ */
