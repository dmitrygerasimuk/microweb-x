//
// Copyright (C) 2021 James Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//

#include <dos.h>
#include <stdio.h>
#include <i86.h>
#include <conio.h>
#include <memory.h>
#include <stdint.h>
#include <time.h>
#include "DOSInput.h"
#include "../Keycodes.h"
#include "../DataPack.h"
#include "../Draw/Surface.h"
#include "../VidModes.h"
#include "../Memory/MemoryLog.h"

DOSInputDriver::DOSInputDriver()
{
	mouseDisabled = false;
	skipMouseReset = false;
	mouseStatusLogCount = 0;
	cachedMouseStatusValid = false;
	lastMouseStatusPollTime = -1;
	lastMousePressPollTime = -1;
	lastMouseReleasePollTime = -1;
	cachedMouseButtons = 0;
	cachedMouseX = 0;
	cachedMouseY = 0;
}

void DOSInputDriver::DisableMouse()
{
	mouseDisabled = true;
}

void DOSInputDriver::DisableMouseReset()
{
	skipMouseReset = true;
}

static void SetMouseDriverRange(int maxX, int maxY)
{
	union REGS inreg, outreg;

	inreg.w.ax = 0x7;
	inreg.w.cx = 0;
	inreg.w.dx = maxX;
	int86(0x33, &inreg, &outreg);

	inreg.w.ax = 0x8;
	inreg.w.cx = 0;
	inreg.w.dx = maxY;
	int86(0x33, &inreg, &outreg);
}

static uint8_t RemapCP866ToCP1251(uint8_t code)
{
	if (code >= 0x80 && code <= 0x9f)
	{
		return (uint8_t)(0xc0 + code - 0x80);
	}
	if (code >= 0xa0 && code <= 0xaf)
	{
		return (uint8_t)(0xe0 + code - 0xa0);
	}
	if (code >= 0xe0 && code <= 0xef)
	{
		return (uint8_t)(0xf0 + code - 0xe0);
	}
	if (code == 0xf0)
	{
		return 0xa8;
	}
	if (code == 0xf1)
	{
		return 0xb8;
	}
	return code;
}

void DOSInputDriver::Init()
{
	union REGS inreg, outreg;
	bool forceSoftwareMouse = Platform::video->drawSurface->format == DrawSurface::Format_2BPP;
	MemoryDebugLog("BOOT mouse init begin disabled=%d forceSoft=%d", mouseDisabled, forceSoftwareMouse);

	hasMouse = false;
	useMouseDriverCursor = false;
	currentCursor = MouseCursor::Hand;
	mouseHideCount = 1;
	mouseVisible = false;
	lastMouseX = -1;
	lastMouseY = -1;
	queuedPressX = -1;
	queuedPressY = -1;
	mouseStatusLogCount = 0;
	cachedMouseStatusValid = false;
	lastMouseStatusPollTime = -1;
	lastMousePressPollTime = -1;
	lastMouseReleasePollTime = -1;
	cachedMouseButtons = 0;
	cachedMouseX = Platform::video->screenWidth / 2;
	cachedMouseY = Platform::video->screenHeight / 2;
	if (mouseDisabled)
	{
		MemoryDebugLog("BOOT mouse disabled");
		return;
	}

	if (skipMouseReset)
	{
		hasMouse = true;
		MemoryDebugLog("BOOT mouse reset skipped");
	}
	else
	{
		MemoryDebugLog("BOOT mouse reset begin");
		inreg.x.ax = 0;
		int86(0x33, &inreg, &outreg);
		hasMouse = (outreg.x.ax == 0xffff);
		MemoryDebugLog("BOOT mouse reset done ax=%04x bx=%04x has=%d", (unsigned)outreg.x.ax, (unsigned)outreg.x.bx, hasMouse);
	}

	if (!hasMouse)
	{
		MemoryDebugLog("BOOT mouse absent");
		return;
	}

	useMouseDriverCursor = Platform::video->GetVideoModeInfo()->useMouseDriverCursor
		&& !forceSoftwareMouse;
	MemoryDebugLog("BOOT mouse cursor mode driver=%d", useMouseDriverCursor);
	SetMouseCursor(MouseCursor::Pointer);

	int horizontalRange = Platform::video->screenWidth;
	if (horizontalRange == 320) horizontalRange = 640;			// Due some strangeness of how DOS mouse drivers work
	MemoryDebugLog("BOOT mouse range begin max=%d,%d", horizontalRange - 1, Platform::video->screenHeight - 1);
	SetMouseDriverRange(horizontalRange - 1, Platform::video->screenHeight - 1);
	MemoryDebugLog("BOOT mouse range done");

	MemoryDebugLog("BOOT mouse position begin");
	SetMousePosition(Platform::video->screenWidth / 2, Platform::video->screenHeight / 2);
	MemoryDebugLog("BOOT mouse position done");

	MemoryDebugLog("BOOT mouse queues clear begin");
	ClearMouseButtonQueues();
	MemoryDebugLog("BOOT mouse queues clear done");

	// Set mouse mickey ratio
	//inreg.w.ax = 0xf;
	//inreg.w.cx = 8;
	//inreg.w.dx = 16;
	//int86(0x33, &inreg, &outreg);

	ShowMouse();
	MemoryDebugLog("BOOT mouse init done visible=%d", mouseVisible);
}

void DOSInputDriver::Shutdown()
{
	MemoryDebugLog("BOOT mouse shutdown begin has=%d visible=%d", hasMouse, mouseVisible);
	HideMouse();
	if (hasMouse)
	{
		MemoryDebugLog("BOOT mouse restore range begin");
		SetMouseDriverRange(639, 199);
		SetMousePosition(0, 0);
		ClearMouseButtonQueues();
		MemoryDebugLog("BOOT mouse restore range done");
	}
	MemoryDebugLog("BOOT mouse shutdown done");
}

void DOSInputDriver::ShowMouse()
{
	if (!hasMouse)
		return;

	mouseHideCount--;
	if (mouseHideCount > 0)
		return;

	if (useMouseDriverCursor)
	{
		union REGS inreg, outreg;
		inreg.x.ax = 1;
		int86(0x33, &inreg, &outreg);
	}

	lastMouseX = -1;
	mouseVisible = true;
}

void DOSInputDriver::SetMousePosition(int x, int y)
{
	if (!hasMouse)
		return;

	ClampMousePosition(x, y);

	union REGS inreg, outreg;
	inreg.x.ax = 4;
	inreg.x.cx = (Platform::video->screenWidth == 320) ? x * 2 : x;
	inreg.x.dx = y;
	int86(0x33, &inreg, &outreg);
}

bool DOSInputDriver::GetMouseButtonPress(int& x, int& y)
{
	if (!hasMouse)
		return false;

	if (queuedPressX != -1)
	{
		x = queuedPressX;
		y = queuedPressY;
		queuedPressX = -1;
		queuedPressY = -1;
		return true;
	}

	clock_t now = clock();
	if (now == lastMousePressPollTime)
	{
		return false;
	}
	lastMousePressPollTime = now;

	union REGS inreg, outreg;
	inreg.x.ax = 5;
	inreg.x.bx = 0;
	int86(0x33, &inreg, &outreg);
	x = outreg.x.cx;
	y = outreg.x.dx;

	if (Platform::video->screenWidth == 320)
	{
		x /= 2;
	}
	ClampMousePosition(x, y);

	if (outreg.x.bx > 0)
	{
		cachedMouseStatusValid = true;
		cachedMouseButtons = 1;
		cachedMouseX = x;
		cachedMouseY = y;
		lastMouseStatusPollTime = now;
	}

	return (outreg.x.bx > 0);
}

bool DOSInputDriver::GetMouseButtonRelease(int& x, int& y)
{
	if (!hasMouse)
		return false;

	clock_t now = clock();
	if (now == lastMouseReleasePollTime)
	{
		return false;
	}
	lastMouseReleasePollTime = now;

	union REGS inreg, outreg;
	inreg.x.ax = 6;
	inreg.x.bx = 0;
	int86(0x33, &inreg, &outreg);
	x = outreg.x.cx;
	y = outreg.x.dx;

	if (Platform::video->screenWidth == 320)
	{
		x /= 2;
	}
	ClampMousePosition(x, y);

	if (outreg.x.bx > 0)
	{
		cachedMouseStatusValid = true;
		cachedMouseButtons = 0;
		cachedMouseX = x;
		cachedMouseY = y;
		lastMouseStatusPollTime = now;
	}

	return (outreg.x.bx > 0);
}


void DOSInputDriver::HideMouse()
{
	if (!hasMouse)
		return;

	mouseHideCount++;
	if (mouseHideCount > 1)
		return;

	if (useMouseDriverCursor)
	{
		union REGS inreg, outreg;
		inreg.x.ax = 2;
		int86(0x33, &inreg, &outreg);
	}
	else
	{
		Platform::video->drawSurface->HideCursor();
	}

	mouseVisible = false;
	lastMouseX = -1;
}

static void SetMouseCursorDriverShape(unsigned short far* data, uint16_t hotSpotX, uint16_t hotSpotY)
{
	union REGS inreg, outreg;
	struct SREGS sreg;

	segread(&sreg);
	inreg.x.ax = 9;
	inreg.x.bx = hotSpotX;
	inreg.x.cx = hotSpotY;
	inreg.x.dx = FP_OFF(data);
	sreg.es = FP_SEG(data);
	int86x(0x33, &inreg, &outreg, &sreg);
}

void DOSInputDriver::SetMouseCursor(MouseCursor::Type type)
{
	if (!hasMouse)
		return;
	if (type == currentCursor)
	{
		return;
	}

	MouseCursorData* cursor = Assets.GetMouseCursorData(type);
	if (cursor && useMouseDriverCursor)
	{
		SetMouseCursorDriverShape(cursor->data, cursor->hotSpotX, cursor->hotSpotY);
	}

	currentCursor = type;
}

void DOSInputDriver::GetMouseStatus(int& buttons, int& x, int& y)
{
	if (!hasMouse)
	{
		buttons = x = y = 0;
		return;
	}
	union REGS inreg, outreg;
	clock_t now = clock();
	if (cachedMouseStatusValid && now == lastMouseStatusPollTime)
	{
		buttons = cachedMouseButtons;
		x = cachedMouseX;
		y = cachedMouseY;
		return;
	}

	inreg.x.ax = 3;
	int86(0x33, &inreg, &outreg);
	x = outreg.x.cx;
	y = outreg.x.dx;
	buttons = outreg.x.bx;
	int rawX = x;
	int rawY = y;

	if (Platform::video->screenWidth == 320)
	{
		x /= 2;
	}
	ClampMousePosition(x, y);

	int cookedRawX = rawX;
	if (Platform::video->screenWidth == 320)
	{
		cookedRawX /= 2;
	}
	bool wasClamped = (cookedRawX != x) || rawY != y;
	if (mouseStatusLogCount < 8 || (wasClamped && mouseStatusLogCount < 64))
	{
		MemoryDebugLog("MOUSE status raw=%d,%d cooked=%d,%d buttons=%d clamp=%d", rawX, rawY, x, y, buttons, wasClamped);
		mouseStatusLogCount++;
	}
	cachedMouseStatusValid = true;
	cachedMouseButtons = buttons;
	cachedMouseX = x;
	cachedMouseY = y;
	lastMouseStatusPollTime = now;
}

void DOSInputDriver::ClearMouseButtonQueues()
{
	if (!hasMouse)
	{
		return;
	}

	union REGS inreg, outreg;
	inreg.x.ax = 5;
	inreg.x.bx = 0;
	int86(0x33, &inreg, &outreg);

	inreg.x.ax = 6;
	inreg.x.bx = 0;
	int86(0x33, &inreg, &outreg);

	queuedPressX = -1;
	queuedPressY = -1;
	cachedMouseStatusValid = false;
}

void DOSInputDriver::ClampMousePosition(int& x, int& y)
{
	if (!Platform::video)
	{
		return;
	}

	if (x < 0)
	{
		x = 0;
	}
	else if (x >= Platform::video->screenWidth)
	{
		x = Platform::video->screenWidth - 1;
	}

	if (y < 0)
	{
		y = 0;
	}
	else if (y >= Platform::video->screenHeight)
	{
		y = Platform::video->screenHeight - 1;
	}
}

InputButtonCode DOSInputDriver::GetKeyPress() 
{
	if (kbhit())
	{
		union REGS inreg, outreg;
		inreg.h.ah = 2;
		int86(0x16, &inreg, &outreg);
		bool shiftPressed = (outreg.h.al & 0x03) != 0;

		InputButtonCode keyPress = getch();
		if (keyPress == 0)
		{
			keyPress = getch() << 8;
		}
		else if (keyPress == KEYCODE_SPACE && shiftPressed)
		{
			keyPress = KEYCODE_SHIFT_SPACE;
		}
		else if (keyPress >= 0x80 && keyPress <= 0xff)
		{
			keyPress = RemapCP866ToCP1251((uint8_t)keyPress);
		}
		return keyPress;
	}

	return 0;
}

bool DOSInputDriver::HasInputPending()
{
	if (kbhit())
		return true;

	int x, y;

	if (GetMouseButtonPress(x, y))
	{
		queuedPressX = x;
		queuedPressY = y;
		return true;
	}

	return false;
}

void DOSInputDriver::RefreshMouse()
{
	if(mouseVisible && !useMouseDriverCursor)
	{
		int mouseButtons, mouseX, mouseY;
		GetMouseStatus(mouseButtons, mouseX, mouseY);

		if (mouseX != lastMouseX || mouseY != lastMouseY)
		{
			Platform::video->drawSurface->DrawCursor(Assets.GetMouseCursorData(currentCursor), mouseX, mouseY);
			lastMouseX = mouseX;
			lastMouseY = mouseY;
		}
	}
}
