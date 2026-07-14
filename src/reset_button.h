#pragma once

void resetButtonBegin();

// Returns true while the button is being held so the caller can pause normal
// display updates and avoid fighting the on-screen reset countdown.
bool resetButtonLoop();
