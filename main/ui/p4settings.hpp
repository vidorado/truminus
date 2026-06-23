#pragma once

// Navigate to the settings menu. Must be called from an LVGL event callback
// (LVGL lock is already held by the port task).
void p4SettingsShow();
