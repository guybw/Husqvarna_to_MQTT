// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// All HTTP routes: /login /setup /pair /debug /update + /api/*.
// HTTP Basic Auth on everything but /login. OTA via Update.h.
// SSE stream of the debug ring buffer.
// Implemented in M2 (skeleton) and progressively through M7.

namespace web_server {
    void begin();
    void loop();   // call from main loop() — drives ArduinoOTA
}
