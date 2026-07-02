/**********************************************************************

  Audacity: A Digital Audio Editor

  ScriptModalGuard.cpp

**********************************************************************/

#include "ScriptModalGuard.h"

#include <wx/app.h>
#include <wx/msgdlg.h>

namespace
{
   int sDepth = 0;
   wxString sCaptured;
}

bool ScriptModalGuard::IsActive()
{
   return sDepth > 0;
}

wxString ScriptModalGuard::TakeCaptured()
{
   wxString result = sCaptured;
   sCaptured.clear();
   return result;
}

int ScriptModalGuard::RecordAndSuppress(const wxString &message, long wxStyle)
{
   wxASSERT(wxIsMainThread());

   if (!sCaptured.empty())
      sCaptured += wxT("\n");
   sCaptured += message;

   if (wxStyle & wxCANCEL)
      return wxCANCEL;
   if (wxStyle & wxYES_NO)
      return wxNO;
   return wxOK;
}

ScriptModalGuard::Scope::Scope()
{
   wxASSERT(wxIsMainThread());
   ++sDepth;
}

ScriptModalGuard::Scope::~Scope()
{
   wxASSERT(wxIsMainThread());
   // Note: does not clear sCaptured. Nested scopes must not discard
   // messages captured by inner scopes; only TakeCaptured() clears it.
   --sDepth;
}
