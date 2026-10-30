#ifndef MANBO_H_
#define MANBO_H_

// Convenience API for application consumers linking manbo::api.
// Module implementations include their specific headers instead of this facade.
// Terminal UI is deliberately opt-in: include ui/terminal/terminal.h or view.h
// and link manbo_terminal_ui when developing a terminal application.
#include "core/browser.h"
#include "core/command.h"
#include "core/interfaces.h"
#include "core/result.h"
#include "core/types.h"
#include "documents/content.h"
#include "library/library.h"
#include "platform/console.h"
#include "platform/wake_signal.h"
#include "repositories/manual_repository.h"
#include "repositories/offline_repository.h"
#include "repositories/sample_repository.h"
#include "runtime/channel.h"
#include "runtime/jobs.h"
#include "runtime/session.h"

#endif  // MANBO_H_
