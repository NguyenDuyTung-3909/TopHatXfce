#pragma once

#ifndef __POPUP_H__
#define __POPUP_H__

#include "panel_entry.h"


// Shows (or hides, if already visible) the detail popup of the given entry.
// The kind of popup (CPU / memory) is picked from the entry's collector.
void popup_toggle( panel_entry_t* entry );


#endif // __POPUP_H__
