#include <stdlib.h>
#include <string.h>
#include "Png.h"
#include "Image.h"
#include "../Colour.h"
#include "../Memory/Memory.h"
#include "../Platform.h"

extern "C"
{
#include "../../lib/inflate/inflate.h"
}

static uint8_t pngSignature[PNG_SIGNATURE_LENGTH] =
{
	137, 80, 78, 71, 13, 10, 26, 10
};

static uint8_t PngPaethPredictor(uint8_t a, uint8_t b, uint8_t c)
{
	int p = (int)a + (int)b - (int)c;
	int pa = p - (int)a;
	int pb = p - (int)b;
	int pc = p - (int)c;

	if (pa < 0)
	{
		pa = -pa;
	}
	if (pb < 0)
	{
		pb = -pb;
	}
	if (pc < 0)
	{
		pc = -pc;
	}

	if (pa <= pb && pa <= pc)
	{
		return a;
	}
	if (pb <= pc)
	{
		return b;
	}
	return c;
}

static size_t PngMinSize(size_t a, size_t b)
{
	return a < b ? a : b;
}

static size_t PngChunkTakeSize(size_t available, uint32_t remaining)
{
	if (remaining < (uint32_t)available)
	{
		return (size_t)remaining;
	}
	return available;
}

PngDecoder::PngDecoder()
	: internalState(ParseSignature)
	, chunkKind(ChunkOther)
	, chunkRemaining(0)
	, chunkPosition(0)
	, crcRemaining(0)
	, headerParsed(false)
	, decoded(false)
	, seenIDAT(false)
	, hasTransparentColour(false)
	, sourceWidth(0)
	, sourceHeight(0)
	, rowBytes(0)
	, inflatedSize(0)
	, bitDepth(0)
	, colourType(0)
	, channels(0)
	, filterBytesPerPixel(0)
	, paletteEntries(0)
	, alphaEntries(0)
	, transparentGrey(0)
	, transparentRed(0)
	, transparentGreen(0)
	, transparentBlue(0)
	, compressedData(NULL)
	, compressedSize(0)
	, compressedCapacity(0)
	, rowBuffer(NULL)
	, prevRowBuffer(NULL)
{
	memset(palette, 0, sizeof(palette));
	memset(paletteAlpha, 255, sizeof(paletteAlpha));
}

void PngDecoder::SetError()
{
	Cleanup();
	state = ImageDecoder::Error;
}

void PngDecoder::FinishSuccess()
{
	Cleanup();
	state = ImageDecoder::Success;
}

void PngDecoder::Cleanup()
{
	if (compressedData)
	{
		free(compressedData);
		compressedData = NULL;
	}
	if (rowBuffer)
	{
		free(rowBuffer);
		rowBuffer = NULL;
	}
	if (prevRowBuffer)
	{
		free(prevRowBuffer);
		prevRowBuffer = NULL;
	}
}

void PngDecoder::Process(uint8_t* data, size_t dataLength)
{
	if (state != ImageDecoder::Decoding)
	{
		return;
	}

	while (dataLength > 0 && state == ImageDecoder::Decoding)
	{
		switch (internalState)
		{
		case ParseSignature:
			if (FillStruct(&data, dataLength, signature, PNG_SIGNATURE_LENGTH))
			{
				if (memcmp(signature, pngSignature, PNG_SIGNATURE_LENGTH))
				{
					SetError();
					return;
				}
				internalState = ParseChunkHeader;
			}
			break;

		case ParseChunkHeader:
			if (FillStruct(&data, dataLength, &chunkHeader, sizeof(ChunkHeader)))
			{
				chunkRemaining = chunkHeader.length;
				chunkPosition = 0;
				crcRemaining = 4;
				chunkKind = ChunkOther;

				if (!memcmp(chunkHeader.type, "IHDR", 4))
				{
					chunkKind = ChunkIHDR;
				}
				else if (!memcmp(chunkHeader.type, "PLTE", 4))
				{
					chunkKind = ChunkPLTE;
				}
				else if (!memcmp(chunkHeader.type, "tRNS", 4))
				{
					chunkKind = ChunkTRNS;
				}
				else if (!memcmp(chunkHeader.type, "IDAT", 4))
				{
					chunkKind = ChunkIDAT;
				}
				else if (!memcmp(chunkHeader.type, "IEND", 4))
				{
					chunkKind = ChunkIEND;
				}

				if (!headerParsed && chunkKind != ChunkIHDR)
				{
					SetError();
					return;
				}

				if (chunkKind == ChunkIEND)
				{
					if (chunkRemaining != 0 || !DecodeImage())
					{
						SetError();
						return;
					}
					FinishSuccess();
					return;
				}

				if (chunkKind == ChunkIHDR)
				{
					if (headerParsed || chunkRemaining != sizeof(ImageHeader))
					{
						SetError();
						return;
					}
					internalState = ParseImageHeader;
				}
				else if (chunkKind == ChunkPLTE)
				{
					if ((chunkRemaining % 3) != 0 || chunkRemaining > sizeof(palette))
					{
						SetError();
						return;
					}
					internalState = chunkRemaining ? ParseChunkData : SkipChunkCRC;
				}
				else if (chunkKind == ChunkTRNS)
				{
					if ((colourType == 0 && chunkRemaining != 2) ||
						(colourType == 2 && chunkRemaining != 6) ||
						(colourType == 3 && chunkRemaining > 256) ||
						(colourType == 4 || colourType == 6))
					{
						SetError();
						return;
					}
					internalState = chunkRemaining ? ParseChunkData : SkipChunkCRC;
				}
				else if (chunkKind == ChunkIDAT)
				{
					seenIDAT = true;
					internalState = chunkRemaining ? ParseChunkData : SkipChunkCRC;
				}
				else
				{
					internalState = chunkRemaining ? SkipChunkData : SkipChunkCRC;
				}
			}
			break;

		case ParseImageHeader:
			if (FillStruct(&data, dataLength, &imageHeader, sizeof(ImageHeader)))
			{
				chunkRemaining = 0;
				if (!BeginImage())
				{
					SetError();
					return;
				}
				if (state != ImageDecoder::Decoding)
				{
					return;
				}
				internalState = SkipChunkCRC;
			}
			break;

		case ParseChunkData:
			if (!ProcessChunkData(&data, dataLength))
			{
				SetError();
				return;
			}
			if (chunkRemaining == 0)
			{
				internalState = SkipChunkCRC;
			}
			break;

		case SkipChunkData:
			{
				size_t bytes = PngChunkTakeSize(dataLength, chunkRemaining);
				data += bytes;
				dataLength -= bytes;
				chunkRemaining -= bytes;
				if (chunkRemaining == 0)
				{
					internalState = SkipChunkCRC;
				}
			}
			break;

		case SkipChunkCRC:
			{
				size_t bytes = PngMinSize(dataLength, crcRemaining);
				data += bytes;
				dataLength -= bytes;
				crcRemaining = (uint8_t)(crcRemaining - bytes);
				if (crcRemaining == 0)
				{
					internalState = ParseChunkHeader;
				}
			}
			break;
		}
	}
}

bool PngDecoder::BeginImage()
{
	uint32_t width = imageHeader.width;
	uint32_t height = imageHeader.height;
	uint32_t bitsPerPixel = 0;
	uint32_t rawRowBytes;
	uint32_t rawSize;

	if (width == 0 || height == 0 || width > 65535UL || height > 65535UL)
	{
		return false;
	}
	if (imageHeader.compressionMethod != 0 || imageHeader.filterMethod != 0 || imageHeader.interlaceMode != 0)
	{
		return false;
	}

	bitDepth = imageHeader.bitDepth;
	colourType = imageHeader.colourType;

	switch (colourType)
	{
	case 0:
		if (bitDepth != 1 && bitDepth != 2 && bitDepth != 4 && bitDepth != 8)
		{
			return false;
		}
		channels = 1;
		bitsPerPixel = bitDepth;
		break;
	case 2:
		if (bitDepth != 8)
		{
			return false;
		}
		channels = 3;
		bitsPerPixel = 24;
		break;
	case 3:
		if (bitDepth != 1 && bitDepth != 2 && bitDepth != 4 && bitDepth != 8)
		{
			return false;
		}
		channels = 1;
		bitsPerPixel = bitDepth;
		break;
	case 4:
		if (bitDepth != 8)
		{
			return false;
		}
		channels = 2;
		bitsPerPixel = 16;
		break;
	case 6:
		if (bitDepth != 8)
		{
			return false;
		}
		channels = 4;
		bitsPerPixel = 32;
		break;
	default:
		return false;
	}

	rawRowBytes = (width * bitsPerPixel + 7) >> 3;
	if (height > 0 && rawRowBytes + 1 > PNG_MAX_INFLATED_SIZE / height)
	{
		return false;
	}
	rawSize = (rawRowBytes + 1) * height;
	if (rawRowBytes == 0 || rawRowBytes > 65534UL || rawSize == 0 || rawSize > PNG_MAX_INFLATED_SIZE)
	{
		return false;
	}

	sourceWidth = (uint16_t)width;
	sourceHeight = (uint16_t)height;
	rowBytes = (uint16_t)rawRowBytes;
	inflatedSize = rawSize;
	filterBytesPerPixel = (uint8_t)((bitsPerPixel + 7) >> 3);
	if (filterBytesPerPixel == 0)
	{
		filterBytesPerPixel = 1;
	}

	CalculateImageDimensions(sourceWidth, sourceHeight);
	headerParsed = true;

	if (onlyDownloadDimensions)
	{
		state = ImageDecoder::Success;
		return true;
	}

	return AllocateOutputImage();
}

bool PngDecoder::AllocateOutputImage()
{
	uint32_t linesSize;
	MemBlockHandle* lines;

	if (outputImage->width == 0 || outputImage->height == 0)
	{
		return false;
	}

	outputImage->pitch = outputImage->bpp == 1 ? (outputImage->width + 7) / 8 : outputImage->width;
	linesSize = (uint32_t)sizeof(MemBlockHandle) * (uint32_t)outputImage->height;
	if (linesSize > 65535UL)
	{
		return false;
	}

	outputImage->lines = MemoryManager::pageBlockAllocator.Allocate((uint16_t)linesSize);
	if (!outputImage->lines.IsAllocated())
	{
		return false;
	}

	lines = outputImage->lines.Get<MemBlockHandle*>();
	for (int y = 0; y < outputImage->height; y++)
	{
		lines[y] = MemoryManager::pageBlockAllocator.Allocate(outputImage->pitch);
		if (!lines[y].IsAllocated())
		{
			outputImage->lines.type = MemBlockHandle::Unallocated;
			return false;
		}
	}

	outputImage->lines.Commit();
	for (int y = 0; y < outputImage->height; y++)
	{
		lines = outputImage->lines.Get<MemBlockHandle*>();
		MemBlockHandle line = lines[y];
		uint8_t* pixels = line.Get<uint8_t*>();
		if (pixels)
		{
			memset(pixels, outputImage->bpp == 1 ? 0xff : TRANSPARENT_COLOUR_VALUE, outputImage->pitch);
			line.Commit();
		}
	}

	return true;
}

bool PngDecoder::ProcessChunkData(uint8_t** data, size_t& dataLength)
{
	switch (chunkKind)
	{
	case ChunkPLTE:
		return ProcessPalette(data, dataLength);
	case ChunkTRNS:
		return ProcessTransparency(data, dataLength);
	case ChunkIDAT:
		{
			size_t bytes = PngChunkTakeSize(dataLength, chunkRemaining);
			if (!AppendIDAT(*data, bytes))
			{
				return false;
			}
			*data += bytes;
			dataLength -= bytes;
			chunkRemaining -= bytes;
			chunkPosition += bytes;
		}
		return true;
	default:
		return false;
	}
}

bool PngDecoder::ProcessPalette(uint8_t** data, size_t& dataLength)
{
	while (dataLength > 0 && chunkRemaining > 0)
	{
		uint8_t value = NextByte(data, dataLength);
		uint16_t index = (uint16_t)(chunkPosition / 3);
		uint8_t component = (uint8_t)(chunkPosition % 3);

		if (index >= 256)
		{
			return false;
		}

		palette[index * 3 + component] = value;
		if (component == 2)
		{
			paletteEntries = index + 1;
		}

		chunkPosition++;
		chunkRemaining--;
	}
	return true;
}

bool PngDecoder::ProcessTransparency(uint8_t** data, size_t& dataLength)
{
	while (dataLength > 0 && chunkRemaining > 0)
	{
		uint8_t value = NextByte(data, dataLength);

		if (colourType == 3)
		{
			if (chunkPosition < 256)
			{
				paletteAlpha[chunkPosition] = value;
				alphaEntries = (uint16_t)(chunkPosition + 1);
				if (value != 255)
				{
					outputImage->hasTransparency = true;
				}
			}
		}
		else if (colourType == 0)
		{
			transparentGrey = (uint16_t)((transparentGrey << 8) | value);
			if (chunkPosition == 1)
			{
				hasTransparentColour = true;
				outputImage->hasTransparency = true;
			}
		}
		else if (colourType == 2)
		{
			if (chunkPosition < 2)
			{
				transparentRed = (uint16_t)((transparentRed << 8) | value);
			}
			else if (chunkPosition < 4)
			{
				transparentGreen = (uint16_t)((transparentGreen << 8) | value);
			}
			else
			{
				transparentBlue = (uint16_t)((transparentBlue << 8) | value);
			}
			if (chunkPosition == 5)
			{
				hasTransparentColour = true;
				outputImage->hasTransparency = true;
			}
		}

		chunkPosition++;
		chunkRemaining--;
	}
	return true;
}

bool PngDecoder::AppendIDAT(uint8_t* data, size_t length)
{
	uint32_t desiredSize;

	if (length == 0)
	{
		return true;
	}

	desiredSize = (uint32_t)compressedSize + (uint32_t)length;
	if (desiredSize > PNG_MAX_COMPRESSED_SIZE || !EnsureCompressedCapacity(desiredSize))
	{
		return false;
	}

	memcpy(compressedData + compressedSize, data, length);
	compressedSize = (uint16_t)desiredSize;
	return true;
}

bool PngDecoder::EnsureCompressedCapacity(uint32_t desiredSize)
{
	uint32_t newCapacity;
	uint8_t* newData;

	if (desiredSize <= compressedCapacity)
	{
		return true;
	}

	newCapacity = compressedCapacity ? compressedCapacity : 1024;
	while (newCapacity < desiredSize)
	{
		newCapacity <<= 1;
		if (newCapacity > PNG_MAX_COMPRESSED_SIZE)
		{
			newCapacity = PNG_MAX_COMPRESSED_SIZE;
			break;
		}
	}

	newData = (uint8_t*)realloc(compressedData, (size_t)newCapacity + PNG_COMPRESSED_PADDING);
	if (!newData)
	{
		return false;
	}

	compressedData = newData;
	compressedCapacity = (uint16_t)newCapacity;
	return true;
}

bool PngDecoder::DecodeImage()
{
	uint8_t* inflatedData;
	int32_t inflateResult;
	bool result;

	if (decoded)
	{
		return true;
	}
	if (!seenIDAT || compressedSize < 4 || (colourType == 3 && paletteEntries == 0))
	{
		return false;
	}

	inflatedData = (uint8_t*)malloc((size_t)inflatedSize);
	if (!inflatedData)
	{
		return false;
	}

	memset(compressedData + compressedSize, 0, PNG_COMPRESSED_PADDING);
	inflateResult = inflate_zlib(compressedData, compressedSize, inflatedData, (uint16_t)inflatedSize);
	if (inflateResult < 0 || (uint32_t)inflateResult != inflatedSize)
	{
		free(inflatedData);
		return false;
	}

	result = UnfilterScanlines(inflatedData);
	free(inflatedData);
	decoded = result;
	return result;
}

bool PngDecoder::UnfilterScanlines(uint8_t* inflatedData)
{
	uint8_t* source = inflatedData;

	rowBuffer = (uint8_t*)malloc(rowBytes);
	prevRowBuffer = (uint8_t*)malloc(rowBytes);
	if (!rowBuffer || !prevRowBuffer)
	{
		return false;
	}
	memset(prevRowBuffer, 0, rowBytes);

	for (uint16_t y = 0; y < sourceHeight; y++)
	{
		uint8_t filter = *source++;
		memcpy(rowBuffer, source, rowBytes);
		source += rowBytes;

		switch (filter)
		{
		case 0:
			break;
		case 1:
			for (uint16_t i = 0; i < rowBytes; i++)
			{
				uint8_t left = i >= filterBytesPerPixel ? rowBuffer[i - filterBytesPerPixel] : 0;
				rowBuffer[i] = (uint8_t)(rowBuffer[i] + left);
			}
			break;
		case 2:
			for (uint16_t i = 0; i < rowBytes; i++)
			{
				rowBuffer[i] = (uint8_t)(rowBuffer[i] + prevRowBuffer[i]);
			}
			break;
		case 3:
			for (uint16_t i = 0; i < rowBytes; i++)
			{
				uint8_t left = i >= filterBytesPerPixel ? rowBuffer[i - filterBytesPerPixel] : 0;
				uint8_t up = prevRowBuffer[i];
				rowBuffer[i] = (uint8_t)(rowBuffer[i] + ((left + up) >> 1));
			}
			break;
		case 4:
			for (uint16_t i = 0; i < rowBytes; i++)
			{
				uint8_t left = i >= filterBytesPerPixel ? rowBuffer[i - filterBytesPerPixel] : 0;
				uint8_t up = prevRowBuffer[i];
				uint8_t upLeft = i >= filterBytesPerPixel ? prevRowBuffer[i - filterBytesPerPixel] : 0;
				rowBuffer[i] = (uint8_t)(rowBuffer[i] + PngPaethPredictor(left, up, upLeft));
			}
			break;
		default:
			return false;
		}

		EmitSourceRow(rowBuffer, y);

		uint8_t* temp = prevRowBuffer;
		prevRowBuffer = rowBuffer;
		rowBuffer = temp;
	}

	return true;
}

void PngDecoder::EmitSourceRow(uint8_t* sourceRow, uint16_t sourceY)
{
	int firstOutputY = (int)(((uint32_t)sourceY * (uint32_t)outputImage->height) / sourceHeight);
	int lastOutputY = (int)((((uint32_t)sourceY + 1) * (uint32_t)outputImage->height) / sourceHeight);

	for (int y = firstOutputY; y < lastOutputY && y < outputImage->height; y++)
	{
		EmitRow(sourceRow, y);
		linesDecoded = y + 1;
	}
}

void PngDecoder::EmitRow(uint8_t* sourceRow, int outputY)
{
	MemBlockHandle* lines = outputImage->lines.Get<MemBlockHandle*>();
	MemBlockHandle lineOutput = lines[outputY];
	uint8_t* output = lineOutput.Get<uint8_t*>();

	if (outputImage->bpp == 8)
	{
		for (int x = 0; x < outputImage->width; x++)
		{
			uint16_t sourceX = (uint16_t)(((uint32_t)x * sourceWidth) / outputImage->width);
			uint8_t red, green, blue, alpha;
			GetSourcePixel(sourceRow, sourceX, red, green, blue, alpha);

			if (alpha < 128)
			{
				output[x] = TRANSPARENT_COLOUR_VALUE;
				outputImage->hasTransparency = true;
			}
			else
			{
				int dither = colourDitherMatrix[((outputY & 3) << 2) + (x & 3)];
				output[x] = MapColour(red, green, blue, dither);
			}
		}
	}
	else
	{
		uint8_t* out = output;
		uint8_t buffer = 0;
		uint8_t mask = 0x80;
		const uint8_t* ditherPattern = greyDitherMatrix + 16 * (outputY & 15);

		for (int x = 0; x < outputImage->width; x++)
		{
			uint16_t sourceX = (uint16_t)(((uint32_t)x * sourceWidth) / outputImage->width);
			uint8_t red, green, blue, alpha;
			uint8_t grey;
			GetSourcePixel(sourceRow, sourceX, red, green, blue, alpha);
			grey = RGB_TO_GREY(red, green, blue);

			if (alpha < 128 || grey > ditherPattern[x & 15])
			{
				buffer |= mask;
			}

			mask >>= 1;
			if (!mask)
			{
				*out++ = buffer;
				buffer = 0;
				mask = 0x80;
			}
		}

		if (mask != 0x80)
		{
			*out = buffer;
		}
	}

	lineOutput.Commit();
}

void PngDecoder::GetSourcePixel(uint8_t* sourceRow, uint16_t x, uint8_t& red, uint8_t& green, uint8_t& blue, uint8_t& alpha)
{
	alpha = 255;

	switch (colourType)
	{
	case 0:
		{
			uint16_t sample = bitDepth == 8 ? sourceRow[x] : GetPackedSample(sourceRow, x, bitDepth);
			uint8_t value = ScaleSample((uint8_t)sample, bitDepth);
			red = green = blue = value;
			if (hasTransparentColour && sample == transparentGrey)
			{
				alpha = 0;
			}
		}
		break;
	case 2:
		{
			uint16_t offset = (uint16_t)((uint32_t)x * 3);
			uint8_t* pixel = sourceRow + offset;
			red = pixel[0];
			green = pixel[1];
			blue = pixel[2];
			if (hasTransparentColour &&
				transparentRed <= 255 && transparentGreen <= 255 && transparentBlue <= 255 &&
				red == transparentRed && green == transparentGreen && blue == transparentBlue)
			{
				alpha = 0;
			}
		}
		break;
	case 3:
		{
			uint8_t index = bitDepth == 8 ? sourceRow[x] : GetPackedSample(sourceRow, x, bitDepth);
			if (index < paletteEntries)
			{
				red = palette[index * 3];
				green = palette[index * 3 + 1];
				blue = palette[index * 3 + 2];
				alpha = paletteAlpha[index];
			}
			else
			{
				red = green = blue = 0;
				alpha = 255;
			}
		}
		break;
	case 4:
		{
			uint16_t offset = (uint16_t)((uint32_t)x * 2);
			uint8_t* pixel = sourceRow + offset;
			red = green = blue = pixel[0];
			alpha = pixel[1];
		}
		break;
	default:
		{
			uint16_t offset = (uint16_t)((uint32_t)x * 4);
			uint8_t* pixel = sourceRow + offset;
			red = pixel[0];
			green = pixel[1];
			blue = pixel[2];
			alpha = pixel[3];
		}
		break;
	}
}

uint8_t PngDecoder::GetPackedSample(uint8_t* sourceRow, uint16_t x, uint8_t depth)
{
	uint32_t bit = (uint32_t)x * depth;
	uint8_t byteValue = sourceRow[bit >> 3];
	uint8_t shift = (uint8_t)(8 - depth - (bit & 7));
	uint8_t mask = (uint8_t)((1 << depth) - 1);
	return (uint8_t)((byteValue >> shift) & mask);
}

uint8_t PngDecoder::ScaleSample(uint8_t sample, uint8_t depth)
{
	if (depth == 8)
	{
		return sample;
	}
	return (uint8_t)(((uint16_t)sample * 255) / ((1 << depth) - 1));
}

uint8_t PngDecoder::MapColour(uint8_t red, uint8_t green, uint8_t blue, int dither)
{
	int r = red + dither;
	int g = green + dither;
	int b = blue + dither;

	if (r < 0)
	{
		r = 0;
	}
	else if (r > 255)
	{
		r = 255;
	}
	if (g < 0)
	{
		g = 0;
	}
	else if (g > 255)
	{
		g = 255;
	}
	if (b < 0)
	{
		b = 0;
	}
	else if (b > 255)
	{
		b = 255;
	}

	return Platform::video->paletteLUT[RGB332(r, g, b)];
}
