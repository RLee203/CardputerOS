#pragma once
void appEspnowEnter();
void appEspnowLoop();
int  espnowUnreadCount();        // messages received since last app entry (used by launcher)
void espnowInitBackground();     // silently init receiver while in launcher (radio mode only)
void espnowProcessBackground();  // drain single-slot queue into chat log; call from main loop
