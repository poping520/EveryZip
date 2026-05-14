#pragma once

#include <windows.h>
#include <shellapi.h>

struct MainWindowState;

/**
 * 向系统托盘添加应用图标并注册回调消息。
 * @param hWnd 接收托盘消息的窗口句柄。
 * @param uID 托盘图标标识。
 * @param uCallbackMessage 托盘事件回调消息编号。
 */
void AddTrayIcon(HWND hWnd, UINT uID, UINT uCallbackMessage);

/** 从系统托盘移除当前应用图标。 */
void RemoveTrayIcon();

/**
 * 在鼠标当前位置显示托盘右键菜单。
 * @param hWnd 拥有菜单的窗口句柄。
 * @param s 主窗口状态，用于加载当前语言文本。
 */
void ShowTrayMenu(HWND hWnd, MainWindowState* s);
