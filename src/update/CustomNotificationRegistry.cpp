/*!********************************************************************
 Audacity: A Digital Audio Editor

 @file CustomNotificationRegistry.cpp

 **********************************************************************/

#include "CustomNotificationRegistry.h"

// Otis: the "Audacity 4.0" release promo dialog is removed — Otis ships no
// custom upsell/notification dialogs.
bool CustomNotificationRegistry::HasCustomDialog(const wxString& uuid)
{
    (void)uuid;
    return false;
}

int CustomNotificationRegistry::ShowCustomDialog(wxWindow* parent, const Notification& notification)
{
    (void)parent;
    (void)notification;
    return wxID_NONE;
}
