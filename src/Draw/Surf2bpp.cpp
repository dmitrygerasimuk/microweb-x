#include <memory.h>
#include "Surf2BPP.h"
#include "../Font.h"
#include "../Image/Image.h"
#include "../Memory/MemBlock.h"
#include "../Colour.h"
#include "../Cursor.h"

static uint8_t bitmaskTable[] =
{
	0xc0, 0x30, 0x0c, 0x03
};

static uint8_t cgaPackLeft[256];
static uint8_t cgaPackRight[256];
static uint8_t cgaExpandedPixel[16];
static bool cgaPackTablesBuilt = false;

static uint8_t PackCgaColour(uint8_t colour)
{
	colour &= 0xf;
	return (uint8_t)(colour | (colour << 4));
}

static uint8_t ExpandCgaColour(uint8_t colour)
{
	if (colour <= 3)
	{
		colour = (uint8_t)(colour * 5);
	}
	return PackCgaColour(colour);
}

static void SetCgaPixel(uint8_t* line, int x, uint8_t colour);

static void BuildCgaPackTables()
{
	if (cgaPackTablesBuilt)
	{
		return;
	}

	for (int n = 0; n < 256; n++)
	{
		uint8_t left = (uint8_t)(n >> 4);
		uint8_t right = (uint8_t)(n & 0xf);
		uint8_t leftExpanded = PackCgaColour(left);
		uint8_t rightExpanded = PackCgaColour(right);

		cgaPackLeft[n] = (uint8_t)((leftExpanded & 0xc0) | (rightExpanded & 0x30));
		cgaPackRight[n] = (uint8_t)((leftExpanded & 0x0c) | (rightExpanded & 0x03));
	}
	for (int n = 0; n < 16; n++)
	{
		cgaExpandedPixel[n] = PackCgaColour((uint8_t)n);
	}

	cgaPackTablesBuilt = true;
}

DrawSurface_2BPP::DrawSurface_2BPP(int inWidth, int inHeight)
	: DrawSurface(inWidth, inHeight)
{
	lines = new uint8_t * [height];
	format = DrawSurface::Format_2BPP;
	cursorBufferX = -1;
	BuildCgaPackTables();
}

void DrawSurface_2BPP::HLine(DrawContext& context, int x, int y, int count, uint8_t colour)
{
	x += context.drawOffsetX;
	y += context.drawOffsetY;

	if (y < context.clipTop || y >= context.clipBottom)
	{
		return;
	}
	if (x < context.clipLeft)
	{
		count -= (context.clipLeft - x);
		x = context.clipLeft;
	}
	if (x + count >= context.clipRight)
	{
		count = context.clipRight - x;
	}
	if (count <= 0)
	{
		return;
	}

	colour = ExpandCgaColour(colour);

	uint8_t* VRAMptr = lines[y];
	VRAMptr += (x >> 2);

	uint8_t data = *VRAMptr;
	uint8_t mask = bitmaskTable[x & 3];

	while (count--)
	{
		data = (data & (~mask)) | (colour & mask);
		mask >>= 2;
		if (!mask)
		{
			*VRAMptr++ = data;
			while (count >= 4)
			{
				*VRAMptr++ = colour;
				count -= 4;
			}
			if (count > 0)
			{
				mask = 0xc0;
				data = *VRAMptr;
			}
		}
	}

	if (mask)
	{
		*VRAMptr = data;
	}
}

void DrawSurface_2BPP::VLine(DrawContext& context, int x, int y, int count, uint8_t colour)
{
	x += context.drawOffsetX;
	y += context.drawOffsetY;

	if (x >= context.clipRight || x < context.clipLeft)
	{
		return;
	}
	if (y < context.clipTop)
	{
		count -= (context.clipTop - y);
		y = context.clipTop;
	}
	if (y >= context.clipBottom)
	{
		return;
	}
	if (y + count > context.clipBottom)
	{
		count = context.clipBottom - y;
	}
	if (count <= 0)
	{
		return;
	}

	colour = ExpandCgaColour(colour);

	uint8_t mask = bitmaskTable[x & 3];
	uint8_t andMask = ~mask;
	uint8_t orMask = mask & colour;
	int index = x >> 2;

	while (count--)
	{
		(lines[y])[index] = ((lines[y])[index] & andMask) | orMask;
		y++;
	}
}

void DrawSurface_2BPP::FillRect(DrawContext& context, int x, int y, int width, int height, uint8_t colour)
{
	x += context.drawOffsetX;
	y += context.drawOffsetY;

	if (x < context.clipLeft)
	{
		width -= (context.clipLeft - x);
		x = context.clipLeft;
	}
	if (y < context.clipTop)
	{
		height -= (context.clipTop - y);
		y = context.clipTop;
	}
	if (x + width > context.clipRight)
	{
		width = context.clipRight - x;
	}
	if (y + height > context.clipBottom)
	{
		height = context.clipBottom - y;
	}
	if (width <= 0 || height <= 0)
	{
		return;
	}

	colour = ExpandCgaColour(colour);

	while (height)
	{
		uint8_t* VRAMptr = lines[y];
		VRAMptr += (x >> 2);

		uint8_t data = *VRAMptr;
		uint8_t mask = bitmaskTable[x & 3];
		int count = width;

		while (count--)
		{
			data = (data & (~mask)) | (colour & mask);
			mask >>= 2;
			if (!mask)
			{
				*VRAMptr++ = data;
				while (count >= 4)
				{
					*VRAMptr++ = colour;
					count -= 4;
				}
				if (count > 0)
				{
					mask = 0xc0;
					data = *VRAMptr;
				}
			}
		}

		if (mask)
		{
			*VRAMptr = data;
		}

		height--;
		y++;
	}
}

void DrawSurface_2BPP::DrawString(DrawContext& context, Font* font, const char* text, int x, int y, uint8_t colour, FontStyle::Type style)
{
	x += context.drawOffsetX;
	y += context.drawOffsetY;

	int startX = x;
	uint8_t lastLine = font->glyphHeight;

	if (x >= context.clipRight)
	{
		return;
	}
	if (y >= context.clipBottom)
	{
		return;
	}
	if (y + lastLine > context.clipBottom)
	{
		lastLine = (uint8_t)(context.clipBottom - y);
	}
	if (y + lastLine < context.clipTop)
	{
		return;
	}

	colour = ExpandCgaColour(colour);

	uint8_t firstLine = 0;
	if (y < context.clipTop)
	{
		firstLine += context.clipTop - y;
		y += firstLine;
	}

	while (*text)
	{
		unsigned char c = (unsigned char) *text++;

		if (c < 32)
		{
			continue;
		}

		int index = c - 32;
		uint8_t glyphWidth = font->glyphs[index].width;
		uint8_t glyphTop = font->glyphs[index].top;
		uint8_t glyphBottom = font->glyphs[index].bottom;
		uint8_t glyphWidthBytes = (glyphWidth + 7) >> 3;
		uint8_t* glyphData = font->glyphData + font->glyphs[index].offset;
		int outY = y;

		if (glyphBottom > lastLine - 1)
		{
			glyphBottom = lastLine - 1;
		}
		if (firstLine < glyphTop)
		{
			outY += glyphTop - firstLine;
		}
		else if (firstLine > glyphTop)
		{
			glyphData += ((firstLine - glyphTop) * glyphWidthBytes);
			glyphTop = firstLine;
		}

		if (glyphWidth == 0)
		{
			continue;
		}

		if (x + glyphWidth > context.clipRight)
		{
			break;
		}

		for (uint8_t j = glyphTop; j <= glyphBottom; j++)
		{
			int rowX = x;

			if ((style & FontStyle::Italic) && j < (font->glyphHeight >> 1))
			{
				rowX++;
			}

			for (uint8_t i = 0; i < glyphWidthBytes; i++)
			{
				uint8_t glyphPixels = *glyphData++;

				for (uint8_t k = 0; k < 8; k++)
				{
					int glyphX = (i << 3) + k;
					if (glyphX >= glyphWidth)
					{
						break;
					}

					int outX = rowX + glyphX;
					if (outX >= context.clipRight)
					{
						break;
					}

					if (outX >= context.clipLeft && (glyphPixels & (0x80 >> k)))
					{
						SetCgaPixel(lines[outY], outX, colour);
					}
				}
			}

			outY++;
		}

		x += glyphWidth;

		//if (x >= context.clipRight)
		//{
		//	break;
		//}
	}

	if ((style & FontStyle::Underline) && y - firstLine + font->glyphHeight - 1 < context.clipBottom)
	{
		HLine(context, startX, y - firstLine + font->glyphHeight - 1 - context.drawOffsetY, x - startX - context.drawOffsetX, colour);
	}
}

void DrawSurface_2BPP::BlitImage(DrawContext& context, Image* image, int x, int y)
{
	if (!image->lines.IsAllocated())
		return;

	x += context.drawOffsetX;
	y += context.drawOffsetY;

	int srcWidth = image->width;
	int srcHeight = image->height;
	int startX = 0;
	int srcX = 0;
	int srcY = 0;

	// Calculate the destination width and height to copy, considering clipping region
	int destWidth = srcWidth;
	int destHeight = srcHeight;

	if (x < context.clipLeft)
	{
		srcX += (context.clipLeft - x);
		destWidth -= (context.clipLeft - x);
		x = context.clipLeft;
	}

	if (x + destWidth > context.clipRight)
	{
		destWidth = context.clipRight - x;
	}

	if (y < context.clipTop)
	{
		srcY += (context.clipTop - y);
		destHeight -= (context.clipTop - y);
		y = context.clipTop;
	}

	if (y + destHeight > context.clipBottom)
	{
		destHeight = context.clipBottom - y;
	}

	if (destWidth <= 0 || destHeight <= 0)
	{
		return; // Nothing to draw if fully outside the clipping region.
	}

	if (image->bpp == 8)
	{
		MemBlockHandle* imageLines = image->lines.Get<MemBlockHandle*>();
		for (int j = 0; j < destHeight; j++)
		{
			MemBlockHandle imageLine = imageLines[j + srcY];
			uint8_t* src = imageLine.Get<uint8_t*>() + srcX;
			uint8_t* dest = lines[y + j] + (x >> 2);
			uint8_t destMask = bitmaskTable[x & 3];
			uint8_t destBuffer = *dest;
			int widthLeft = destWidth;

			if ((x & 3) == 0 && !image->hasTransparency)
			{
				while (widthLeft >= 4)
				{
					*dest++ = (uint8_t)(cgaPackLeft[((src[0] & 0xf) << 4) | (src[1] & 0xf)] |
						cgaPackRight[((src[2] & 0xf) << 4) | (src[3] & 0xf)]);
					src += 4;
					widthLeft -= 4;
				}

				destMask = 0xc0;
				if (widthLeft)
				{
					destBuffer = *dest;
				}
			}
			else if ((x & 3) == 0)
			{
				while (widthLeft >= 4)
				{
					uint8_t c0 = src[0];
					uint8_t c1 = src[1];
					uint8_t c2 = src[2];
					uint8_t c3 = src[3];

					if (c0 != TRANSPARENT_COLOUR_VALUE &&
						c1 != TRANSPARENT_COLOUR_VALUE &&
						c2 != TRANSPARENT_COLOUR_VALUE &&
						c3 != TRANSPARENT_COLOUR_VALUE)
					{
						*dest++ = (uint8_t)(cgaPackLeft[((c0 & 0xf) << 4) | (c1 & 0xf)] |
							cgaPackRight[((c2 & 0xf) << 4) | (c3 & 0xf)]);
					}
					else
					{
						destBuffer = *dest;
						if (c0 != TRANSPARENT_COLOUR_VALUE)
							destBuffer = (destBuffer & 0x3f) | (cgaExpandedPixel[c0 & 0xf] & 0xc0);
						if (c1 != TRANSPARENT_COLOUR_VALUE)
							destBuffer = (destBuffer & 0xcf) | (cgaExpandedPixel[c1 & 0xf] & 0x30);
						if (c2 != TRANSPARENT_COLOUR_VALUE)
							destBuffer = (destBuffer & 0xf3) | (cgaExpandedPixel[c2 & 0xf] & 0x0c);
						if (c3 != TRANSPARENT_COLOUR_VALUE)
							destBuffer = (destBuffer & 0xfc) | (cgaExpandedPixel[c3 & 0xf] & 0x03);
						*dest++ = destBuffer;
					}

					src += 4;
					widthLeft -= 4;
				}

				destMask = 0xc0;
				if (widthLeft)
				{
					destBuffer = *dest;
				}
			}

			for (int i = 0; i < widthLeft; i++)
			{
				uint8_t srcBuffer = *src;
				if (!image->hasTransparency || srcBuffer != TRANSPARENT_COLOUR_VALUE)
				{
					srcBuffer = PackCgaColour(srcBuffer);
					destBuffer = (destBuffer & (~destMask)) | ((srcBuffer)&destMask);
				}
				src++;
				destMask >>= 2;
				if (!destMask)
				{
					*dest++ = destBuffer;
					destMask = 0xc0;
					if (i + 1 < widthLeft)
					{
						destBuffer = *dest;
					}
				}
			}
			if (destMask != 0xc0)
			{
				*dest = destBuffer;
			}
		}
	}
	else if (image->bpp == 1)
	{
		MemBlockHandle* imageLines = image->lines.Get<MemBlockHandle*>();
		for (int j = 0; j < destHeight; j++)
		{
			MemBlockHandle imageLine = imageLines[j + srcY];
			uint8_t* src = imageLine.Get<uint8_t*>() + (srcX >> 3);
			uint8_t* dest = lines[y + j] + (x >> 2);
			uint8_t srcMask = 0x80 >> (srcX & 7);
			uint8_t destMask = bitmaskTable[x & 3];
			uint8_t destBuffer = *dest;
			uint8_t srcBuffer = *src++;

			for (int i = 0; i < destWidth; i++)
			{
				if ((srcBuffer & srcMask))
				{
					destBuffer |= destMask;
				}
				else
				{
					destBuffer &= ~destMask;
				}
				srcMask >>= 1;
				if (!srcMask)
				{
					srcMask = 0x80;
					srcBuffer = *src++;
				}

				destMask >>= 2;
				if (!destMask)
				{
					*dest++ = destBuffer;
					destMask = 0xc0;
					if (i + 1 < destWidth)
					{
						destBuffer = *dest;
					}
				}
			}
			if (destMask != 0xc0)
			{
				*dest = destBuffer;
			}
		}
	}
}


void DrawSurface_2BPP::InvertRect(DrawContext& context, int x, int y, int width, int height)
{
	x += context.drawOffsetX;
	y += context.drawOffsetY;

	if (x < context.clipLeft)
	{
		width -= (context.clipLeft - x);
		x = context.clipLeft;
	}
	if (y < context.clipTop)
	{
		height -= (context.clipTop - y);
		y = context.clipTop;
	}
	if (x + width > context.clipRight)
	{
		width = context.clipRight - x;
	}
	if (y + height > context.clipBottom)
	{
		height = context.clipBottom - y;
	}
	if (width <= 0 || height <= 0)
	{
		return;
	}

	while (height)
	{
		uint8_t* VRAMptr = lines[y];
		VRAMptr += (x >> 2);
		int count = width;
		uint8_t data = *VRAMptr;
		uint8_t mask = bitmaskTable[x & 3];

		while (count--)
		{
			data ^= mask;
			mask >>= 2;
			if (!mask)
			{
				*VRAMptr++ = data;
				while (count >= 4)
				{
					*VRAMptr++ ^= 0xff;
					count -= 4;
				}
				if (count > 0)
				{
					mask = 0xc0;
					data = *VRAMptr;
				}
			}
		}

		if (mask)
		{
			*VRAMptr = data;
		}

		height--;
		y++;
	}
}

static void DrawCgaScrollPattern(DrawSurface_2BPP* surface, DrawContext& context, int x, int y, const uint8_t* pattern)
{
	int drawX = x + context.drawOffsetX;
	int drawY = y + context.drawOffsetY;

	if (drawY < context.clipTop || drawY >= context.clipBottom)
	{
		return;
	}

	if ((drawX & 3) == 0 && drawX >= context.clipLeft && drawX + 16 <= context.clipRight)
	{
		memcpy(surface->lines[drawY] + (drawX >> 2), pattern, 4);
		return;
	}

	for (int col = 0; col < 16; col++)
	{
		uint8_t packed = pattern[col >> 2];
		uint8_t colour = (uint8_t)(((packed >> (6 - ((col & 3) << 1))) & 3) * 5);
		surface->HLine(context, x + col, y, 1, colour);
	}
}

void DrawSurface_2BPP::VerticalScrollBar(DrawContext& context, int x, int y, int height, int position, int size)
{
	if (height <= 0)
	{
		return;
	}
	if (size < 1)
	{
		size = 1;
	}
	if (size > height)
	{
		size = height;
	}
	if (position < 0)
	{
		position = 0;
	}
	if (position > height - size)
	{
		position = height - size;
	}

	static const uint8_t inner[4] = { 0x3f, 0xff, 0xff, 0xfc };
	static const uint8_t widgetEdge[4] = { 0x3f, 0x00, 0x00, 0xfc };
	static const uint8_t widgetInner[4] = { 0x3c, 0xff, 0xff, 0x3c };
	static const uint8_t grab[4] = { 0x3c, 0xc0, 0x03, 0x3c };
	const int grabSize = 7;

	int widgetTop = position;
	int widgetBottom = position + size;
	int grabTop = widgetTop + ((size - grabSize) >> 1);
	int grabBottom = grabTop + grabSize;

	for (int row = 0; row < height; row++)
	{
		const uint8_t* pattern = inner;

		if (row == widgetTop || row == widgetBottom - 1)
		{
			pattern = widgetEdge;
		}
		else if (row > widgetTop && row < widgetBottom - 1)
		{
			pattern = (row >= grabTop && row < grabBottom && ((row - grabTop) & 1)) ? grab : widgetInner;
		}

		DrawCgaScrollPattern(this, context, x, y + row, pattern);
	}
}

void DrawSurface_2BPP::Clear()
{
	int widthBytes = width >> 2;
	for (int y = 0; y < height; y++)
	{
		memset(lines[y], 0xff, widthBytes);
	}
}


void DrawSurface_2BPP::ScrollScreen(int top, int bottom, int width, int amount)
{
	width >>= 2;
	if (width <= 0 || amount == 0)
	{
		return;
	}
	if (width > (this->width >> 2))
	{
		width = this->width >> 2;
	}
	if (top < 0)
	{
		top = 0;
	}
	if (bottom > height)
	{
		bottom = height;
	}
	if (top >= bottom)
	{
		return;
	}

	if (amount > 0)
	{
		if (top + amount >= height)
		{
			return;
		}
		if (bottom + amount > height)
		{
			bottom = height - amount;
		}
		for (int y = top; y < bottom; y++)
		{
			memcpy(lines[y], lines[y + amount], width);
		}
	}
	else if (amount < 0)
	{
		if (bottom + amount <= 0)
		{
			return;
		}
		if (top + amount < 0)
		{
			top = -amount;
		}
		for (int y = bottom - 1; y >= top; y--)
		{
			memcpy(lines[y], lines[y + amount], width);
		}
	}
}

static void SetCgaPixel(uint8_t* line, int x, uint8_t colour)
{
	uint8_t mask = bitmaskTable[x & 3];
	uint8_t packedColour = ExpandCgaColour(colour);
	uint8_t* pixel = line + (x >> 2);
	*pixel = (uint8_t)((*pixel & ~mask) | (packedColour & mask));
}

void DrawSurface_2BPP::DrawCursor(struct MouseCursorData* cursor, int x, int y)
{
	if (!cursor)
	{
		return;
	}

	HideCursor();

	x -= cursor->hotSpotX;
	y -= cursor->hotSpotY;

	int widthBytes = width >> 2;
	cursorBufferX = x >> 2;
	if (cursorBufferX < 0)
	{
		cursorBufferX = 0;
	}
	if (cursorBufferX > widthBytes - 5)
	{
		cursorBufferX = widthBytes - 5;
	}

	cursorBufferY = y;
	if (cursorBufferY < 0)
	{
		cursorBufferY = 0;
	}
	if (cursorBufferY > height - 16)
	{
		cursorBufferY = height - 16;
	}

	uint8_t* bufferPtr = cursorBuffer;
	for (int row = 0; row < 16; row++)
	{
		memcpy(bufferPtr, lines[cursorBufferY + row] + cursorBufferX, 5);
		bufferPtr += 5;
	}

	for (int row = 0; row < 16; row++)
	{
		int outY = y + row;
		if (outY < 0 || outY >= height)
		{
			continue;
		}

		uint16_t cursorMask = cursor->data[row];
		uint16_t cursorColour = cursor->data[16 + row];
		uint16_t bit = 0x8000;

		for (int col = 0; col < 16; col++, bit >>= 1)
		{
			int outX = x + col;
			if (outX < 0 || outX >= width)
			{
				continue;
			}
			if (!(cursorMask & bit))
			{
				SetCgaPixel(lines[outY], outX, (cursorColour & bit) ? 3 : 0);
			}
		}
	}
}

void DrawSurface_2BPP::HideCursor()
{
	if (cursorBufferX < 0)
	{
		return;
	}

	uint8_t* bufferPtr = cursorBuffer;
	for (int row = 0; row < 16; row++)
	{
		memcpy(lines[cursorBufferY + row] + cursorBufferX, bufferPtr, 5);
		bufferPtr += 5;
	}

	cursorBufferX = -1;
}
