#pragma once

void appTimerEnter();
void appTimerLoop();
void appTimerService();
bool appTimerTakeoverRequested();
void appTimerConsumeTakeover();
