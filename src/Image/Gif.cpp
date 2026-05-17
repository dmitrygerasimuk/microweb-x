#include "Gif.h"
#include "Image.h"
#include "../Platform.h"
#include "../Colour.h"
#include "../Memory/Memory.h"
#include "../Memory/MemoryLog.h"
#include "../Page.h"
#include "../App.h"
#include "../Draw/Surface.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define DEBUG_MESSAGE(...) printf(__VA_ARGS__);
#else
#define DEBUG_MESSAGE(...)
#endif

#define BLOCK_TYPE_EXTENSION_INTRODUCER 0x21
#define BLOCK_TYPE_IMAGE_DESCRIPTOR 0x2C
#define BLOCK_TYPE_TRAILER 0x3B
#define BLOCK_TYPE_GRAPHIC_CONTROL_EXTENSION 0xF9

#define GIF_MONO_PACK_PIXEL(outByte, srcValue, threshold, bitMask) \
	do \
	{ \
		if (paletteLUT[(srcValue)] > (threshold)) \
		{ \
			(outByte) |= (bitMask); \
		} \
	} while (0)

#if GIF_PROFILE
#define GIF_PROFILE_FINISH(result) PrintProfile(result)
#define GIF_RESET_DICTIONARY() \
	do \
	{ \
		profileDictionaryResets++; \
		dictionaryIndex = clearCode + 2; \
	} while (0)
#else
#define GIF_PROFILE_FINISH(result)
#define GIF_RESET_DICTIONARY() \
	do \
	{ \
		dictionaryIndex = clearCode + 2; \
	} while (0)
#endif

GifDecoder::GifDecoder() 
{
	internalState = ParseHeader;
	lineBufferSkipCount = 0;
	transparentColourIndex = -1;
#if GIF_PROFILE
	ResetProfile();
#endif
}

void GifDecoder::Process(uint8_t* data, size_t dataLength)
{
	if(state != ImageDecoder::Decoding)
	{
		return;
	}

#if GIF_PROFILE
	if (profileProcessCalls == 0)
	{
		profileStartClock = clock();
	}
	profileProcessCalls++;
	profileInputBytes += (unsigned long)dataLength;
#endif
	
	while(dataLength > 0)
	{
		switch(internalState)
		{
			case ParseHeader:
			{
				if(FillStruct(&data, dataLength, &header, sizeof(Header)))
				{
					// Header structure is complete
					if(memcmp(header.versionTag, "GIF89a", 6) && memcmp(header.versionTag, "GIF87a", 6))
					{
						// Not a GIF89a
						state = ImageDecoder::Error;
						GIF_PROFILE_FINISH("bad-header");
						return;
					}

					// If the image width is wider than the buffer, we will skip every N pixels
					lineBufferDivider = 1;
					while (header.width / lineBufferDivider > GIF_LINE_BUFFER_MAX_SIZE)
					{
						lineBufferDivider++;
					}
					
					CalculateImageDimensions(header.width, header.height);

					if (onlyDownloadDimensions)
					{
						state = ImageDecoder::Success;
						GIF_PROFILE_FINISH("dimensions");
						return;
					}

					if (outputImage->bpp == 1)
					{
						outputImage->pitch = (outputImage->width + 7) / 8;
					}
					else // 8bpp
					{
						outputImage->pitch = outputImage->width;
					}

					outputImage->lines = MemoryManager::pageBlockAllocator.Allocate(sizeof(MemBlockHandle) * outputImage->height);
					if(!outputImage->lines.IsAllocated())
					{
						// Allocation error
						DEBUG_MESSAGE("Could not allocate!\n");
						state = ImageDecoder::Error;
						GIF_PROFILE_FINISH("alloc-lines");
						return;
					}

					MemBlockHandle* lines = outputImage->lines.GetDebug<MemBlockHandle*>(__FILE__, __LINE__);

					for (int j = 0; j < outputImage->height; j++)
					{
						lines[j] = MemoryManager::pageBlockAllocator.Allocate(outputImage->pitch);

						if (!lines[j].IsAllocated())
						{
							// Allocation error
							DEBUG_MESSAGE("Could not allocate!\n");
							outputImage->lines.type = MemBlockHandle::Unallocated;
							state = ImageDecoder::Error;
							GIF_PROFILE_FINISH("alloc-line");
							return;
						}
					}

					outputImage->lines.Commit();
					for (int j = 0; j < outputImage->height; j++)
					{
						lines = outputImage->lines.GetDebug<MemBlockHandle*>(__FILE__, __LINE__);
						MemBlockHandle line = lines[j];
						void* pixels = line.GetPtr();
						if (pixels)
						{
							memset(pixels, TRANSPARENT_COLOUR_VALUE, outputImage->pitch);
							line.Commit();
						}
					}
					
					backgroundColour = header.backgroundColour;
					
					DEBUG_MESSAGE("Image is %d x %d\n", outputImage->width, outputImage->height);
					
					if(header.fields & (1 << 7))
					{
						paletteSize = (1 << ((header.fields & 0x7) + 1));
						DEBUG_MESSAGE("Image has a palette of %d colours\n", (int) paletteSize);
						
						internalState = ParsePalette;
						paletteIndex = 0;
					}
					else
					{
						internalState = ParseDataBlock;
					}
				}
			}
			break;
			
			case ParsePalette:
			{
				if(FillStruct(&data, dataLength, rgb, 3))
				{
					//DEBUG_MESSAGE("RGB index %d: %x %x %x\n", paletteIndex, (int)(rgb[0]), (int)(rgb[1]), (int)(rgb[2]));

					//uint8_t grey = (uint8_t)(((uint16_t)rgb[0] * 76 + (uint16_t)rgb[1] * 150 + (uint16_t)rgb[2] * 30) >> 8);
					//palette[paletteIndex++] = grey;
					palette[paletteIndex * 3] = rgb[0];
					palette[paletteIndex * 3 + 1] = rgb[1];
					palette[paletteIndex * 3 + 2] = rgb[2];
					paletteIndex++;

					if(paletteIndex == paletteSize)
					{
						BuildPaletteLUT(paletteSize);
						internalState = ParseDataBlock;
					}
				}
			}
			break;
			
			case ParseDataBlock:
			{
				uint8_t blockType = NextByte(&data, dataLength);

				switch(blockType)
				{
					case BLOCK_TYPE_IMAGE_DESCRIPTOR:
					internalState = ParseImageDescriptor;
					break;
					
					case BLOCK_TYPE_TRAILER:
					// End of GIF
					state = ImageDecoder::Success;
					GIF_PROFILE_FINISH("trailer");
					return;
					
					case BLOCK_TYPE_EXTENSION_INTRODUCER:
					internalState = ParseExtension;
					break;
					
					default:
						DEBUG_MESSAGE("Invalid block type: %x\n", (int)(blockType));
					state = ImageDecoder::Error;
					GIF_PROFILE_FINISH("bad-block");
					return;
				}
			}
			break;
			
			case ParseImageDescriptor:
			{
				if(FillStruct(&data, dataLength, &imageDescriptor, sizeof(ImageDescriptor)))
				{
					DEBUG_MESSAGE("Image: %d, %d %d, %d\n", imageDescriptor.x, imageDescriptor.y, imageDescriptor.width, imageDescriptor.height);
					MemoryDebugLog("GIF descriptor x=%u y=%u w=%u h=%u screen=%ux%u output=%ux%u interlace=%u",
						(unsigned)imageDescriptor.x, (unsigned)imageDescriptor.y,
						(unsigned)imageDescriptor.width, (unsigned)imageDescriptor.height,
						(unsigned)header.width, (unsigned)header.height,
						(unsigned)outputImage->width, (unsigned)outputImage->height,
						(unsigned)((imageDescriptor.fields & GIF_INTERLACE_BIT) ? 1 : 0));
					if (imageDescriptor.width == 0 || imageDescriptor.height == 0)
					{
						state = ImageDecoder::Error;
						GIF_PROFILE_FINISH("bad-descriptor");
						return;
					}

					lineBufferDivider = 1;
					while (imageDescriptor.width / lineBufferDivider > GIF_LINE_BUFFER_MAX_SIZE)
					{
						lineBufferDivider++;
					}
					
					drawX = drawY = 0;
					outputLine = 0;

					linesProcessed = 0;
					lineBufferSize = 0;
					lineBufferFlushCount = 0;
					lineBufferSkipCount = 0;
					nextScaledOutputY = 0;
					verticalScaleError = 0;
					if (imageDescriptor.x || imageDescriptor.y || imageDescriptor.width != header.width || imageDescriptor.height != header.height)
					{
						outputImage->hasTransparency = true;
					}
					BuildXScaleBuffer();

					if (imageDescriptor.fields & 0x80)
					{
						internalState = ParseLocalColourTable;
						paletteIndex = 0;
						localColourTableLength = 1 << ((imageDescriptor.fields & 7) + 1);
					}
					else
					{
						internalState = ParseLZWCodeSize;
					}
				}
			}
			break;

			case ParseLocalColourTable:
			{
				if (FillStruct(&data, dataLength, &rgb, 3))
				{
					palette[paletteIndex * 3] = rgb[0];
					palette[paletteIndex * 3 + 1] = rgb[1];
					palette[paletteIndex * 3 + 2] = rgb[2];

					paletteIndex++;

					if (paletteIndex == localColourTableLength)
					{
						BuildPaletteLUT(localColourTableLength);
						internalState = ParseLZWCodeSize;
					}
				}
			}
			break;

			case ParseLZWCodeSize:
			{
				lzwCodeSize = NextByte(&data, dataLength);
				DEBUG_MESSAGE("LZW code size: %d\n", lzwCodeSize);

				// Init LZW vars
				code = 0;
				clearCode = 1 << lzwCodeSize;
				stopCode = clearCode + 1;
				codeLength = resetCodeLength = lzwCodeSize + 1;
				codeLimit = 1 << codeLength;
				codeMask = (uint16_t)(codeLimit - 1);
				bitBuffer = 0;
				bitBufferHigh = 0;
				bitCount = 0;
				prev = -1;

				for(dictionaryIndex = 0; dictionaryIndex < clearCode; dictionaryIndex++)
				{
					dictionary[dictionaryIndex].byte = (uint8_t) dictionaryIndex;
					dictionary[dictionaryIndex].prev = -1;
					dictionary[dictionaryIndex].first = (uint8_t) dictionaryIndex;
				}
				GIF_RESET_DICTIONARY();

				internalState = ParseImageSubBlockSize;
			}
			break;
			
			case ParseImageSubBlockSize:
			{
				imageSubBlockSize = NextByte(&data, dataLength);
#if GIF_PROFILE
				profileSubBlocks++;
#endif
				DEBUG_MESSAGE("Sub block size: %d bytes\n", imageSubBlockSize);
				if(imageSubBlockSize)
				{
					internalState = ParseImageSubBlock;
				}
				else
				{
					internalState = ParseDataBlock;

					// HACK: Finish decoding after first frame
					state = ImageDecoder::Success;
					GIF_PROFILE_FINISH("first-frame");
					return;
				}
			}
			break;
			
			case ParseImageSubBlock:
			{
				if(imageSubBlockSize)
				{
					imageSubBlockSize--;
					uint8_t dataByte = NextByte(&data, dataLength);
					//DEBUG_MESSAGE("-Data: %x\n", dataByte);
#if GIF_PROFILE
					profileLzwBytes++;
#endif

					if (bitCount < 8)
					{
						bitBuffer |= (uint16_t)((uint16_t)dataByte << bitCount);
					}
					else
					{
						bitBuffer |= (uint16_t)((uint16_t)dataByte << bitCount);
						bitBufferHigh |= (uint8_t)(dataByte >> (16 - bitCount));
					}
					bitCount += 8;

					while (bitCount >= codeLength)
					{
						int currentCode = (int)(bitBuffer & codeMask);
						bitBuffer = (uint16_t)((bitBuffer >> codeLength) | ((uint16_t)bitBufferHigh << (16 - codeLength)));
						bitBufferHigh = (uint8_t)(bitBufferHigh >> codeLength);
						bitCount -= codeLength;
#if GIF_PROFILE
						profileLzwCodes++;
#endif

						//DEBUG_MESSAGE("code: %x [len=%d]\n", currentCode, codeLength);

						// Code complete
						if (currentCode == clearCode)
						{
							DEBUG_MESSAGE("CLEAR\n");
#if GIF_PROFILE
							profileClearCodes++;
#endif
							codeLength = resetCodeLength;
							codeLimit = 1 << codeLength;
							codeMask = (uint16_t)(codeLimit - 1);
							GIF_RESET_DICTIONARY();
							prev = -1;
							continue;
						}
						else if (currentCode == stopCode)
						{
							DEBUG_MESSAGE("STOP\n");
#if GIF_PROFILE
							profileStopCodes++;
#endif
							if (imageSubBlockSize)
							{
								DEBUG_MESSAGE("Malformed GIF\n");
								// FIXME
								//state = ImageDecoder::Success;
								//return;
								//state = ImageDecoder::Error;
							}
							continue;
						}
						if (currentCode >= GIF_MAX_DICTIONARY_ENTRIES)
						{
							DEBUG_MESSAGE("Error: code = %x\n", currentCode);
							state = ImageDecoder::Error;
							GIF_PROFILE_FINISH("bad-code");
							return;
						}

						if (prev < 0 && currentCode < clearCode)
						{
							prev = currentCode;

							if (lineBufferSkipCount == lineBufferDivider - 1)
							{
								lineBuffer[lineBufferSize++] = (uint8_t)currentCode;
								lineBufferSkipCount = 0;
#if GIF_PROFILE
								profileStoredPixels++;
#endif
							}
							else
							{
								lineBufferSkipCount++;
#if GIF_PROFILE
								profileSkippedPixels++;
#endif
							}

							lineBufferFlushCount++;
#if GIF_PROFILE
							profileDecodedPixels++;
#endif

							if (lineBufferFlushCount == imageDescriptor.width)
							{
								ProcessLineBuffer();
								lineBufferSize = 0;
								lineBufferFlushCount = 0;
							}
							continue;
						}

						if (prev > -1 && dictionaryIndex < GIF_MAX_DICTIONARY_ENTRIES)
						{
							if (currentCode > dictionaryIndex)
							{
								DEBUG_MESSAGE("Error: code = %x, but dictionaryIndex = %x\n", currentCode, dictionaryIndex);
								state = ImageDecoder::Error;
								GIF_PROFILE_FINISH("bad-dict");
								return;
							}

							int ptr = (currentCode == dictionaryIndex) ? prev : currentCode;
							dictionary[dictionaryIndex].byte = dictionary[ptr].first;
							dictionary[dictionaryIndex].prev = prev;
							dictionary[dictionaryIndex].first = dictionary[prev].first;

							dictionaryIndex++;
#if GIF_PROFILE
							profileDictionaryAdds++;
#endif

							if(dictionaryIndex == codeLimit && codeLength < 12)
							{
								codeLength++;
								codeLimit <<= 1;
								codeMask = (uint16_t)(codeLimit - 1);
								//DEBUG_MESSAGE("Code length: %d\n", codeLength);
							}
						}

						prev = currentCode;

						{
							int stackSize = 0;
							code = currentCode;

							while(code != -1)
							{
								if (stackSize >= GIF_MAX_DICTIONARY_ENTRIES)
								{
									Platform::FatalError("Stack overflow in GIF decoding");
								}

								decodeStack[stackSize++] = dictionary[code].byte;
#if GIF_PROFILE
								profileDictionarySteps++;
#endif
								//DEBUG_MESSAGE(" value: %x\n", dictionary[code].byte);

								code = dictionary[code].prev;
							}
#if GIF_PROFILE
							if ((unsigned long)stackSize > profileMaxStackSize)
							{
								profileMaxStackSize = (unsigned long)stackSize;
							}
#endif

							while (stackSize)
							{
								if (lineBufferSkipCount == lineBufferDivider - 1)
								{
									lineBuffer[lineBufferSize++] = decodeStack[stackSize - 1];
									lineBufferSkipCount = 0;
#if GIF_PROFILE
									profileStoredPixels++;
#endif
								}
								else
								{
									lineBufferSkipCount++;
#if GIF_PROFILE
									profileSkippedPixels++;
#endif
								}

								lineBufferFlushCount++;
#if GIF_PROFILE
								profileDecodedPixels++;
#endif
								stackSize--;

								if (lineBufferFlushCount == imageDescriptor.width)
								{
									ProcessLineBuffer();
									lineBufferSize = 0;
									lineBufferFlushCount = 0;
								}
							}
						}
					}
				}
				else
				{
				//DEBUG_MESSAGE("--\n");
					internalState = ParseImageSubBlockSize;
				}
			}
			break;
			
			case ParseExtension:
			{
				if(FillStruct(&data, dataLength, &extensionHeader, sizeof(ExtensionHeader)))
				{
					DEBUG_MESSAGE("Extension: %x Size: %d\n", (int) extensionHeader.code, (int) extensionHeader.size);

					if (extensionHeader.code == BLOCK_TYPE_GRAPHIC_CONTROL_EXTENSION)
					{
						internalState = ParseGraphicControlExtension;
					}
					else
					{
						internalState = ParseExtensionContents;
					}
				}
			}
			break;
			
			case ParseExtensionContents:
			{
				if(SkipBytes(&data, dataLength, extensionHeader.size))
				{
					internalState = ParseExtensionSubBlockSize;
				}
			}
			break;

			case ParseGraphicControlExtension:
			{
				if (FillStruct(&data, dataLength, &graphicControlExtension, sizeof(GraphicControlExtension)))
				{
					if (graphicControlExtension.packedFields & 1)
					{
						transparentColourIndex = graphicControlExtension.transparentColourIndex;
						outputImage->hasTransparency = true;
						paletteLUT[transparentColourIndex] = TRANSPARENT_COLOUR_VALUE;
						for (int n = 0; n < 16; n++)
						{
							colourDitherLUT[n][transparentColourIndex] = TRANSPARENT_COLOUR_VALUE;
						}
					}
					internalState = ParseExtensionSubBlockSize;
				}
			}
			break;
			
			case ParseExtensionSubBlockSize:
			{
				extensionSubBlockSize = NextByte(&data, dataLength);
				if(extensionSubBlockSize > 0)
				{
					internalState = ParseExtensionSubBlock;
				}
				else
				{
					internalState = ParseDataBlock;
				}
			}
			break;
			
			case ParseExtensionSubBlock:
			{
				if(SkipBytes(&data, dataLength, extensionSubBlockSize))
				{
					internalState = ParseExtensionSubBlockSize;
				}
			}
			break;
		}
	}
}

#if GIF_PROFILE
void GifDecoder::ResetProfile()
{
	profileProcessCalls = 0;
	profileInputBytes = 0;
	profileSubBlocks = 0;
	profileLzwBytes = 0;
	profileLzwCodes = 0;
	profileClearCodes = 0;
	profileStopCodes = 0;
	profileDictionaryAdds = 0;
	profileDictionaryResets = 0;
	profileDictionarySteps = 0;
	profileMaxStackSize = 0;
	profileDecodedPixels = 0;
	profileStoredPixels = 0;
	profileSkippedPixels = 0;
	profileProcessLines = 0;
	profileEmitLines = 0;
	profileEmitPixels = 0;
	profileDirectEmitLines = 0;
	profileXScaleEmitLines = 0;
	profileGenericScaleEmitLines = 0;
	profilePaletteBuilds = 0;
	profileColourDitherBuilds = 0;
	profileStartClock = 0;
	profilePrinted = 0;
}

void GifDecoder::PrintProfile(const char* result)
{
	FILE* profileFile;

	if (profilePrinted)
	{
		return;
	}

	profilePrinted = 1;
	profileFile = fopen("C:\\GIFPROF.TXT", "a");
	if (!profileFile)
	{
		profileFile = fopen("GIFPROF.TXT", "a");
	}
	if (!profileFile)
	{
		profileFile = stdout;
	}

	long elapsedTicks = clock() - profileStartClock;
	unsigned long elapsedMs = 0;
	if (CLOCKS_PER_SEC)
	{
		elapsedMs = ((unsigned long)elapsedTicks * 1000UL) / (unsigned long)CLOCKS_PER_SEC;
	}

	fprintf(profileFile, "\nGIF profile [%s]\n", result);
	fprintf(profileFile, "image: %u x %u -> %u x %u, bpp=%d, divider=%d\n",
		header.width, header.height, outputImage->width, outputImage->height,
		outputImage->bpp, lineBufferDivider);
	fprintf(profileFile, "time: %ld ticks, %lu ms, calls=%lu input=%lu\n",
		elapsedTicks, elapsedMs, profileProcessCalls, profileInputBytes);
	fprintf(profileFile, "lzw: blocks=%lu bytes=%lu codes=%lu clear=%lu stop=%lu\n",
		profileSubBlocks, profileLzwBytes, profileLzwCodes, profileClearCodes, profileStopCodes);
	fprintf(profileFile, "dict: resets=%lu adds=%lu steps=%lu maxStack=%lu\n",
		profileDictionaryResets, profileDictionaryAdds, profileDictionarySteps, profileMaxStackSize);
	fprintf(profileFile, "pixels: decoded=%lu stored=%lu skipped=%lu\n",
		profileDecodedPixels, profileStoredPixels, profileSkippedPixels);
	fprintf(profileFile, "lines: decoded=%lu emitted=%lu emitPixels=%lu direct=%lu xscale=%lu generic=%lu\n",
		profileProcessLines, profileEmitLines, profileEmitPixels,
		profileDirectEmitLines, profileXScaleEmitLines, profileGenericScaleEmitLines);
	fprintf(profileFile, "palette: builds=%lu ditherBuilds=%lu\n",
		profilePaletteBuilds, profileColourDitherBuilds);

	if (profileFile != stdout)
	{
		fflush(profileFile);
		fclose(profileFile);
	}
}
#endif

void GifDecoder::ClearDictionary()
{
	GIF_RESET_DICTIONARY();
}

void GifDecoder::BuildXScaleBuffer()
{
	scaledLineBufferSize = imageDescriptor.width / lineBufferDivider;
	useXScaleBuffer = 0;

	if (scaledLineBufferSize <= 0)
	{
		scaledLineBufferSize = 1;
	}

	if (outputImage->width != scaledLineBufferSize && outputImage->width <= GIF_LINE_BUFFER_MAX_SIZE)
	{
		int dy = scaledLineBufferSize << 1;
		int dx = outputImage->width << 1;
		int D = dy - outputImage->width;
		int x = 0;

		for (int i = 0; i < outputImage->width; i++)
		{
			xScaleBuffer[i] = (uint16_t)x;
			while (D > 0)
			{
				x++;
				D -= dx;
			}
			D += dy;
		}

		useXScaleBuffer = 1;
	}
}

void GifDecoder::BuildPaletteLUT(int colourCount)
{
#if GIF_PROFILE
	profilePaletteBuilds++;
#endif
	if (outputImage->bpp == 8)
	{
		for (int n = 0; n < colourCount; n++)
		{
			paletteLUT[n] = Platform::video->paletteLUT[RGB332(palette[n * 3], palette[n * 3 + 1], palette[n * 3 + 2])];
		}
		BuildColourDitherLUT(colourCount);
	}
	else
	{
		for (int n = 0; n < colourCount; n++)
		{
			paletteLUT[n] = RGB_TO_GREY(palette[n * 3], palette[n * 3 + 1], palette[n * 3 + 2]);
		}
	}

	if (transparentColourIndex >= 0)
	{
		paletteLUT[transparentColourIndex] = TRANSPARENT_COLOUR_VALUE;
		for (int n = 0; n < 16; n++)
		{
			colourDitherLUT[n][transparentColourIndex] = TRANSPARENT_COLOUR_VALUE;
		}
	}
}

void GifDecoder::BuildColourDitherLUT(int colourCount)
{
#if GIF_PROFILE
	profileColourDitherBuilds++;
#endif
	uint8_t* videoPaletteLUT = Platform::video->paletteLUT;

	for (int dither = 0; dither < 16; dither++)
	{
		int offset = colourDitherMatrix[dither];
		for (int n = 0; n < colourCount; n++)
		{
			int index = (n << 1) + n;
			int red = palette[index] + offset;
			if (red > 255)
				red = 255;
			else if (red < 0)
				red = 0;
			int green = palette[index + 1] + offset;
			if (green > 255)
				green = 255;
			else if (green < 0)
				green = 0;
			int blue = palette[index + 2] + offset;
			if (blue > 255)
				blue = 255;
			else if (blue < 0)
				blue = 0;

			colourDitherLUT[dither][n] = videoPaletteLUT[RGB332(red, green, blue)];
		}
	}
}

// Compute output index of y-th input line, in frame of height h. 
int GifDecoder::CalculateLineIndex(int y)
{
    int h;
    int p;

    h = imageDescriptor.height;

    /*
       pass 1: 0, 8, 16, ...
       count = ceil(h / 8)
    */
    p = (h + 7) >> 3;
    if (y < p)
        return y << 3;          /* y * 8 */

    y -= p;

    /*
       pass 2: 4, 12, 20, ...
       count = number of lines >= 4 with step 8
    */
    p = (h > 4) ? ((h + 3) >> 3) : 0;
    if (y < p)
        return (y << 3) + 4;    /* y * 8 + 4 */

    y -= p;

    /*
       pass 3: 2, 6, 10, ...
       count = number of lines >= 2 with step 4
    */
    p = (h + 1) >> 2;
    if (y < p)
        return (y << 2) + 2;    /* y * 4 + 2 */

    y -= p;

    /*
       pass 4: 1, 3, 5, ...
    */
    return (y << 1) + 1;        /* y * 2 + 1 */
}

void GifDecoder::ProcessLineBuffer()
{
#if GIF_PROFILE
	profileProcessLines++;
#endif
	if (linesProcessed >= imageDescriptor.height)
	{
		MemoryDebugLog("GIF skip-extra-line line=%d descH=%u screenH=%u outputH=%u",
			linesProcessed, (unsigned)imageDescriptor.height,
			(unsigned)header.height, (unsigned)outputImage->height);
		linesProcessed++;
		return;
	}

	int outputY = imageDescriptor.y + linesProcessed;
	int sourceHeight = header.height;
	if (sourceHeight <= 0)
	{
		sourceHeight = imageDescriptor.height;
	}
	
	if (imageDescriptor.fields & GIF_INTERLACE_BIT)
	{
		outputY = imageDescriptor.y + CalculateLineIndex(linesProcessed);

		if (outputImage->height == sourceHeight)
		{
			EmitLine(outputY);
		}
		else
		{
			int first = outputY * (long)outputImage->height / sourceHeight;
			int last = (outputY + 1) * (long)outputImage->height / sourceHeight;

			for (int y = first; y < last; y++)
			{
				EmitLine(y);
			}
		}
	}
	else if (outputImage->height == sourceHeight)
	{
		EmitLine(outputY);
		linesDecoded = outputY + 1;
	}
	else
	{
		verticalScaleError += outputImage->height;
		while (verticalScaleError >= sourceHeight)
		{
			if (nextScaledOutputY < outputImage->height)
			{
				EmitLine(nextScaledOutputY);
			}
			nextScaledOutputY++;
			verticalScaleError -= sourceHeight;
		}
		linesDecoded = nextScaledOutputY;
	}

	linesProcessed++;
}

void GifDecoder::EmitLine(int y)
{
	if (y < 0 || y >= outputImage->height)
	{
		MemoryDebugLog("GIF emit skip-y y=%d outputH=%u descH=%u screenH=%u processed=%d",
			y, (unsigned)outputImage->height, (unsigned)imageDescriptor.height,
			(unsigned)header.height, linesProcessed);
		return;
	}

#if GIF_PROFILE
	profileEmitLines++;
	profileEmitPixels += (unsigned long)outputImage->width;
	if (outputImage->width == lineBufferSize)
	{
		profileDirectEmitLines++;
	}
	else if (useXScaleBuffer && lineBufferSize == scaledLineBufferSize)
	{
		profileXScaleEmitLines++;
	}
	else
	{
		profileGenericScaleEmitLines++;
	}
#endif
	MemBlockHandle* lines = outputImage->lines.GetDebug<MemBlockHandle*>(__FILE__, __LINE__);
	MemBlockHandle lineOutput = lines[y];
	if (!lineOutput.IsAllocated())
	{
		const unsigned char* raw = (const unsigned char*)&lineOutput;
		MemoryDebugLog("GIF emit bad-line y=%d outputH=%u descH=%u screenH=%u processed=%d raw=%02x %02x %02x %02x %02x type=%u",
			y, (unsigned)outputImage->height, (unsigned)imageDescriptor.height,
			(unsigned)header.height, linesProcessed,
			(unsigned)raw[0], (unsigned)raw[1], (unsigned)raw[2], (unsigned)raw[3], (unsigned)raw[4],
			(unsigned)lineOutput.type);
		return;
	}

	uint8_t* output = lineOutput.GetDebug<uint8_t*>(__FILE__, __LINE__);
	
	if (outputImage->bpp == 8)
	{
		int ditherRow = (y & 3) << 2;
		int ditherIndex = 0;

		if (outputImage->width == lineBufferSize)
		{
			uint8_t* lut0 = colourDitherLUT[ditherRow];
			uint8_t* lut1 = colourDitherLUT[ditherRow + 1];
			uint8_t* lut2 = colourDitherLUT[ditherRow + 2];
			uint8_t* lut3 = colourDitherLUT[ditherRow + 3];
			int i = 0;

			while (i + 3 < lineBufferSize)
			{
				output[i] = lut0[lineBuffer[i]];
				output[i + 1] = lut1[lineBuffer[i + 1]];
				output[i + 2] = lut2[lineBuffer[i + 2]];
				output[i + 3] = lut3[lineBuffer[i + 3]];
				i += 4;
			}

			while (i < lineBufferSize)
			{
				output[i] = colourDitherLUT[ditherRow + ditherIndex][lineBuffer[i]];
				ditherIndex = (ditherIndex + 1) & 3;
				i++;
			}
		}
		else if (useXScaleBuffer && lineBufferSize == scaledLineBufferSize)
		{
			uint8_t* lut0 = colourDitherLUT[ditherRow];
			uint8_t* lut1 = colourDitherLUT[ditherRow + 1];
			uint8_t* lut2 = colourDitherLUT[ditherRow + 2];
			uint8_t* lut3 = colourDitherLUT[ditherRow + 3];
			int i = 0;

			while (i + 3 < outputImage->width)
			{
				output[i] = lut0[lineBuffer[xScaleBuffer[i]]];
				output[i + 1] = lut1[lineBuffer[xScaleBuffer[i + 1]]];
				output[i + 2] = lut2[lineBuffer[xScaleBuffer[i + 2]]];
				output[i + 3] = lut3[lineBuffer[xScaleBuffer[i + 3]]];
				i += 4;
			}

			while (i < outputImage->width)
			{
				output[i] = colourDitherLUT[ditherRow + (i & 3)][lineBuffer[xScaleBuffer[i]]];
				i++;
			}
		}
		else
		{
			int dy = lineBufferSize << 1;
			int dx = outputImage->width << 1;
			int D = dy - outputImage->width;
			int x = 0;

			for (int i = 0; i < outputImage->width; i++)
			{
				output[i] = colourDitherLUT[ditherRow + ditherIndex][lineBuffer[x]];
				ditherIndex = (ditherIndex + 1) & 3;

				while (D > 0)
				{
					x++;
					D -= dx;
				}
				D += dy;
			}
		}
	}
	else
	{
		const uint8_t* ditherPattern = greyDitherMatrix + 16 * (y & 15);
		uint8_t buffer = 0;
		uint8_t mask = 0x80;
		int ditherIndex = 0;

		if (outputImage->width == lineBufferSize)
		{
			uint8_t* src = lineBuffer;
			int byteCount = lineBufferSize >> 3;
			int i = 0;

			while (byteCount)
			{
				const uint8_t* dither = ditherPattern + ((i & 8) ? 8 : 0);
				buffer = 0;

				GIF_MONO_PACK_PIXEL(buffer, src[0], dither[0], 0x80);
				GIF_MONO_PACK_PIXEL(buffer, src[1], dither[1], 0x40);
				GIF_MONO_PACK_PIXEL(buffer, src[2], dither[2], 0x20);
				GIF_MONO_PACK_PIXEL(buffer, src[3], dither[3], 0x10);
				GIF_MONO_PACK_PIXEL(buffer, src[4], dither[4], 0x08);
				GIF_MONO_PACK_PIXEL(buffer, src[5], dither[5], 0x04);
				GIF_MONO_PACK_PIXEL(buffer, src[6], dither[6], 0x02);
				GIF_MONO_PACK_PIXEL(buffer, src[7], dither[7], 0x01);

				*output++ = buffer;
				src += 8;
				i += 8;
				byteCount--;
			}

			ditherIndex = i & 15;
			buffer = 0;
			while (i < lineBufferSize)
			{
				GIF_MONO_PACK_PIXEL(buffer, lineBuffer[i], ditherPattern[ditherIndex], mask);
				mask >>= 1;
				if (!mask)
				{
					*output++ = buffer;
					buffer = 0;
					mask = 0x80;
				}
				ditherIndex = (ditherIndex + 1) & 15;
				i++;
			}

			if (mask != 0x80)
			{
				*output = buffer;
			}
		}
		else if (useXScaleBuffer && lineBufferSize == scaledLineBufferSize)
		{
			int byteCount = outputImage->width >> 3;
			int i = 0;

			while (byteCount)
			{
				const uint8_t* dither = ditherPattern + ((i & 8) ? 8 : 0);
				buffer = 0;

				GIF_MONO_PACK_PIXEL(buffer, lineBuffer[xScaleBuffer[i]], dither[0], 0x80);
				GIF_MONO_PACK_PIXEL(buffer, lineBuffer[xScaleBuffer[i + 1]], dither[1], 0x40);
				GIF_MONO_PACK_PIXEL(buffer, lineBuffer[xScaleBuffer[i + 2]], dither[2], 0x20);
				GIF_MONO_PACK_PIXEL(buffer, lineBuffer[xScaleBuffer[i + 3]], dither[3], 0x10);
				GIF_MONO_PACK_PIXEL(buffer, lineBuffer[xScaleBuffer[i + 4]], dither[4], 0x08);
				GIF_MONO_PACK_PIXEL(buffer, lineBuffer[xScaleBuffer[i + 5]], dither[5], 0x04);
				GIF_MONO_PACK_PIXEL(buffer, lineBuffer[xScaleBuffer[i + 6]], dither[6], 0x02);
				GIF_MONO_PACK_PIXEL(buffer, lineBuffer[xScaleBuffer[i + 7]], dither[7], 0x01);

				*output++ = buffer;
				i += 8;
				byteCount--;
			}

			ditherIndex = i & 15;
			buffer = 0;
			while (i < outputImage->width)
			{
				GIF_MONO_PACK_PIXEL(buffer, lineBuffer[xScaleBuffer[i]], ditherPattern[ditherIndex], mask);
				mask >>= 1;
				if (!mask)
				{
					*output++ = buffer;
					buffer = 0;
					mask = 0x80;
				}
				ditherIndex = (ditherIndex + 1) & 15;
				i++;
			}

			if (mask != 0x80)
			{
				*output = buffer;
			}
		}
		else
		{
			
			int dy = lineBufferSize << 1;
			int dx = outputImage->width << 1;
			int D = dy - outputImage->width;
			int x = 0;

			for (int i = 0; i < outputImage->width; i++)
			{
				uint8_t value = paletteLUT[lineBuffer[x]];
				uint8_t threshold = ditherPattern[ditherIndex];

				if (value > threshold)
				{
					buffer |= mask;
				}

				ditherIndex = (ditherIndex + 1) & 15;

				mask >>= 1;
				if (!mask)
				{
					*output++ = buffer;
					buffer = 0;
					mask = 0x80;
				}

				while (D > 0)
				{
					x++;
					D -= dx;
				}
				D += dy;
			}
			
			if (mask != 0x80)
			{
				*output = buffer;
			}
		}
	}

	lineOutput.Commit();
}
