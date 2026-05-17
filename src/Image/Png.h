#ifndef _PNG_H_
#define _PNG_H_

#include "Decoder.h"

#define PNG_SIGNATURE_LENGTH 8
#define PNG_COMPRESSED_PADDING 4
#define PNG_MAX_COMPRESSED_SIZE 65531U
#define PNG_MAX_INFLATED_SIZE 65535UL

class PngDecoder : public ImageDecoder
{
public:
	PngDecoder();
	virtual void Process(uint8_t* data, size_t dataLength) override;

private:
	enum InternalState
	{
		ParseSignature,
		ParseChunkHeader,
		ParseImageHeader,
		ParseChunkData,
		SkipChunkData,
		SkipChunkCRC
	};

	enum ChunkKind
	{
		ChunkOther,
		ChunkIHDR,
		ChunkPLTE,
		ChunkTRNS,
		ChunkIDAT,
		ChunkIEND
	};

#pragma pack(push, 1)
	struct ChunkHeader
	{
		uint32_be length;
		char type[4];
	};

	struct ImageHeader
	{
		uint32_be width;
		uint32_be height;
		uint8_t bitDepth;
		uint8_t colourType;
		uint8_t compressionMethod;
		uint8_t filterMethod;
		uint8_t interlaceMode;
	};
#pragma pack(pop)

	void SetError();
	void FinishSuccess();
	void Cleanup();
	bool BeginImage();
	bool AllocateOutputImage();
	bool ProcessChunkData(uint8_t** data, size_t& dataLength);
	bool ProcessPalette(uint8_t** data, size_t& dataLength);
	bool ProcessTransparency(uint8_t** data, size_t& dataLength);
	bool AppendIDAT(uint8_t* data, size_t length);
	bool EnsureCompressedCapacity(uint32_t desiredSize);
	bool DecodeImage();
	bool UnfilterScanlines(uint8_t* inflatedData);
	void EmitSourceRow(uint8_t* sourceRow, uint16_t sourceY);
	void EmitRow(uint8_t* sourceRow, int outputY);
	void GetSourcePixel(uint8_t* sourceRow, uint16_t x, uint8_t& red, uint8_t& green, uint8_t& blue, uint8_t& alpha);
	uint8_t GetPackedSample(uint8_t* sourceRow, uint16_t x, uint8_t depth);
	uint8_t ScaleSample(uint8_t sample, uint8_t depth);
	uint8_t MapColour(uint8_t red, uint8_t green, uint8_t blue, int dither);

	InternalState internalState;
	ChunkKind chunkKind;

	uint8_t signature[PNG_SIGNATURE_LENGTH];
	ChunkHeader chunkHeader;
	ImageHeader imageHeader;

	uint32_t chunkRemaining;
	uint32_t chunkPosition;
	uint8_t crcRemaining;

	bool headerParsed;
	bool decoded;
	bool seenIDAT;
	bool hasTransparentColour;

	uint16_t sourceWidth;
	uint16_t sourceHeight;
	uint16_t rowBytes;
	uint32_t inflatedSize;
	uint8_t bitDepth;
	uint8_t colourType;
	uint8_t channels;
	uint8_t filterBytesPerPixel;

	uint8_t palette[256 * 3];
	uint8_t paletteAlpha[256];
	uint16_t paletteEntries;
	uint16_t alphaEntries;
	uint16_t transparentGrey;
	uint16_t transparentRed;
	uint16_t transparentGreen;
	uint16_t transparentBlue;

	uint8_t* compressedData;
	uint16_t compressedSize;
	uint16_t compressedCapacity;

	uint8_t* rowBuffer;
	uint8_t* prevRowBuffer;
};

#endif
