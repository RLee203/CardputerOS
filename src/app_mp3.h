#pragma once

void appMp3Enter();
void appMp3Loop();

// Background audio — call bgAudioLoop() from main loop every tick.
// bgAudioSuspend() must be called before any SD.end() in SD-using apps.
bool bgAudioIsActive();   // true while audio plays outside the MP3 app
void bgAudioLoop();       // services audio->loop(); no-op when inactive
void bgAudioSuspend();    // stops audio + frees SD file (safe for SD apps)
void bgAudioResume();     // restarts last track after SD app exits
