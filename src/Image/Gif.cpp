#include "Gif.h"
#include "Image.h"
#include "../Platform.h"
#include "../Colour.h"
#include "../Memory/Memory.h"
#include "../Page.h"
#include "../App.h"
#include "../Draw/Surface.h"
#include <stdio.h>

#ifdef _WIN32
#define DEBUG_MESSAGE(...) printf(__VA_ARGS__);
#else
#define DEBUG_MESSAGE(...)
#endif

#define BLOCK_TYPE_EXTENSION_INTRODUCER 0x21
#define BLOCK_TYPE_IMAGE_DESCRIPTOR 0x2C
#define BLOCK_TYPE_TRAILER 0x3B
#define BLOCK_TYPE_GRAPHIC_CONTROL_EXTENSION 0xF9

GifDecoder::GifDecoder() 
{
	internalState = ParseHeader;
	lineBufferSkipCount = 0;
	transparentColourIndex = -1;
}

void GifDecoder::Process(uint8_t* data, size_t dataLength)
{
	if(state != ImageDecoder::Decoding)
	{
		return;
	}
	
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
						return;
					}

					MemBlockHandle* lines = outputImage->lines.Get<MemBlockHandle*>();

					for (int j = 0; j < outputImage->height; j++)
					{
						lines[j] = MemoryManager::pageBlockAllocator.Allocate(outputImage->pitch);

						if (!lines[j].IsAllocated())
						{
							// Allocation error
							DEBUG_MESSAGE("Could not allocate!\n");
							outputImage->lines.type = MemBlockHandle::Unallocated;
							state = ImageDecoder::Error;
							return;
						}
					}

					outputImage->lines.Commit();
					for (int j = 0; j < outputImage->height; j++)
					{
						lines = outputImage->lines.Get<MemBlockHandle*>();
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
					return;
					
					case BLOCK_TYPE_EXTENSION_INTRODUCER:
					internalState = ParseExtension;
					break;
					
					default:
						DEBUG_MESSAGE("Invalid block type: %x\n", (int)(blockType));
					state = ImageDecoder::Error;
					return;
				}
			}
			break;
			
			case ParseImageDescriptor:
			{
				if(FillStruct(&data, dataLength, &imageDescriptor, sizeof(ImageDescriptor)))
				{
					DEBUG_MESSAGE("Image: %d, %d %d, %d\n", imageDescriptor.x, imageDescriptor.y, imageDescriptor.width, imageDescriptor.height);
					
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
				bitBuffer = 0;
				bitCount = 0;
				prev = -1;
				ClearDictionary();

				internalState = ParseImageSubBlockSize;
			}
			break;
			
			case ParseImageSubBlockSize:
			{
				imageSubBlockSize = NextByte(&data, dataLength);
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

					bitBuffer |= ((uint32_t)dataByte << bitCount);
					bitCount += 8;

					while (bitCount >= codeLength)
					{
						int currentCode = (int)(bitBuffer & ((1 << codeLength) - 1));
						bitBuffer >>= codeLength;
						bitCount -= codeLength;

						//DEBUG_MESSAGE("code: %x [len=%d]\n", currentCode, codeLength);

						// Code complete
						if (currentCode == clearCode)
						{
							DEBUG_MESSAGE("CLEAR\n");
							codeLength = resetCodeLength;
							ClearDictionary();
							prev = -1;
							continue;
						}
						else if (currentCode == stopCode)
						{
							DEBUG_MESSAGE("STOP\n");
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
							return;
						}

						if (prev > -1 && dictionaryIndex < GIF_MAX_DICTIONARY_ENTRIES)
						{
							if (currentCode > dictionaryIndex)
							{
								DEBUG_MESSAGE("Error: code = %x, but dictionaryIndex = %x\n", currentCode, dictionaryIndex);
								state = ImageDecoder::Error;
								return;
							}

							int ptr = (currentCode == dictionaryIndex) ? prev : currentCode;
							dictionary[dictionaryIndex].byte = dictionary[ptr].first;
							dictionary[dictionaryIndex].prev = prev;
							dictionary[dictionaryIndex].first = dictionary[prev].first;

							dictionaryIndex++;

							if(dictionaryIndex == (1 << (codeLength)) && codeLength < 12)
							{
								codeLength++;
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
								//DEBUG_MESSAGE(" value: %x\n", dictionary[code].byte);

								code = dictionary[code].prev;
							}

							while (stackSize)
							{
								if (lineBufferSkipCount == lineBufferDivider - 1)
								{
									lineBuffer[lineBufferSize++] = decodeStack[stackSize - 1];
									lineBufferSkipCount = 0;
								}
								else
								{
									lineBufferSkipCount++;
								}

								lineBufferFlushCount++;
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

void GifDecoder::ClearDictionary()
{
	for(dictionaryIndex = 0; dictionaryIndex < (1 << lzwCodeSize); dictionaryIndex++)
	{
		dictionary[dictionaryIndex].byte = (uint8_t) dictionaryIndex;
		dictionary[dictionaryIndex].prev = -1;
		dictionary[dictionaryIndex].first = (uint8_t) dictionaryIndex;
	}
	
	dictionaryIndex += 2;
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
	uint8_t* videoPaletteLUT = Platform::video->paletteLUT;

	for (int dither = 0; dither < 16; dither++)
	{
		int offset = colourDitherMatrix[dither];
		for (int n = 0; n < colourCount; n++)
		{
			int index = n * 3;
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
	int p; /* number of lines in current pass */

	p = (header.height - 1) / 8 + 1;
	if (y < p) /* pass 1 */
		return y * 8;
	y -= p;
	p = (header.height - 5) / 8 + 1;
	if (y < p) /* pass 2 */
		return y * 8 + 4;
	y -= p;
	p = (header.height - 3) / 4 + 1;
	if (y < p) /* pass 3 */
		return y * 4 + 2;
	y -= p;
	/* pass 4 */
	return y * 2 + 1;
}

void GifDecoder::ProcessLineBuffer()
{
	int outputY = linesProcessed;
	
	if (imageDescriptor.fields & GIF_INTERLACE_BIT)
	{
		outputY = CalculateLineIndex(linesProcessed);

		if (outputImage->height == header.height)
		{
			EmitLine(outputY);
		}
		else
		{
			int first = outputY * (long)outputImage->height / header.height;
			int last = (outputY + 1) * (long)outputImage->height / header.height;

			for (int y = first; y < last; y++)
			{
				EmitLine(y);
			}
		}
	}
	else if (outputImage->height == header.height)
	{
		EmitLine(outputY);
		linesDecoded = outputY + 1;
	}
	else
	{
		verticalScaleError += outputImage->height;
		while (verticalScaleError >= header.height)
		{
			if (nextScaledOutputY < outputImage->height)
			{
				EmitLine(nextScaledOutputY);
			}
			nextScaledOutputY++;
			verticalScaleError -= header.height;
		}
		linesDecoded = nextScaledOutputY;
	}

	linesProcessed++;
}

void GifDecoder::EmitLine(int y)
{
	MemBlockHandle* lines = outputImage->lines.Get<MemBlockHandle*>();
	MemBlockHandle lineOutput = lines[y];
	uint8_t* output = lineOutput.Get<uint8_t*>();
	
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
			for (int i = 0; i < lineBufferSize; i++)
			{
				uint8_t value = paletteLUT[lineBuffer[i]];
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
			}

			if (mask != 0x80)
			{
				*output = buffer;
			}
		}
		else if (useXScaleBuffer && lineBufferSize == scaledLineBufferSize)
		{
			for (int i = 0; i < outputImage->width; i++)
			{
				uint8_t value = paletteLUT[lineBuffer[xScaleBuffer[i]]];
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

