/**********************************************************************

  Audacity: A Digital Audio Editor

  ScriptModalGuard.h

  Suppresses modal dialogs (message boxes, error dialogs) while a
  scripting command is executing, so that a script/LLM-driven command
  can never wedge the application waiting on a human to dismiss a
  dialog. Captured messages are made available to the caller so they
  can be surfaced as a visible command failure instead.

**********************************************************************/

#ifndef __AUDACITY_SCRIPT_MODAL_GUARD__
#define __AUDACITY_SCRIPT_MODAL_GUARD__

#include <wx/string.h>

//! Global suppression of modal dialogs while scripting commands run.
/*!
 Not thread safe; must be used only on the main (UI) thread, which is
 also the only thread that can show a modal dialog.
 */
namespace ScriptModalGuard
{
   //! Whether suppression is currently active (i.e. inside a Scope)
   WX_INIT_API bool IsActive();

   //! Retrieve and clear all messages captured since the last call
   WX_INIT_API wxString TakeCaptured();

   //! Record a would-be modal dialog's message and return a safe default
   //! result instead of actually showing the dialog.
   /*!
    @param message the text that would have been shown to the user
    @param wxStyle the wx button-style flags the dialog would have used
    @return wxCANCEL if wxStyle has wxCANCEL, else wxNO if wxStyle has
       wxYES_NO, else wxOK
    */
   WX_INIT_API int RecordAndSuppress(const wxString &message, long wxStyle);

   //! RAII scope that activates suppression for its lifetime.
   /*!
    Scopes may nest (e.g. a scripted command that itself invokes another
    scripted command); only the outermost Scope's destruction restores
    normal (non-suppressed) behaviour. Captured text accumulates across
    nested scopes and is only cleared by TakeCaptured().
    */
   class WX_INIT_API Scope
   {
   public:
      Scope();
      ~Scope();

      Scope(const Scope &) = delete;
      Scope &operator=(const Scope &) = delete;
   };
}

#endif
