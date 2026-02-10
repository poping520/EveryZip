#pragma once

#include <windows.h>
#include <shellapi.h>

void AddTrayIcon(HWND hWnd, UINT uID, UINT uCallbackMessage);
void RemoveTrayIcon();
void ShowTrayMenu(HWND hWnd, HINSTANCE hInstance);
