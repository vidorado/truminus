#pragma once

// Routes {id,value} frames sent by browsers to the matching p4display setters.
// Once the LCD state changes the diff in the main loop emits a `setting`
// broadcast so every other connected tab sees the same change.
void wsOnCommand(const char* id, const char* value);
