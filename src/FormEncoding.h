#ifndef _FORM_ENCODING_H_
#define _FORM_ENCODING_H_

#include <string.h>

static inline bool FormUrlIsUnreserved(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') ||
		(c >= 'a' && c <= 'z') ||
		(c >= '0' && c <= '9') ||
		c == '-' || c == '_' || c == '.' || c == '~';
}

static inline bool FormAppendChar(char* buffer, char c, size_t bufferLength)
{
	size_t length = strlen(buffer);
	if (length + 1 >= bufferLength)
	{
		return false;
	}

	buffer[length] = c;
	buffer[length + 1] = '\0';
	return true;
}

static inline void FormAppendUrlEncodedString(char* buffer, const char* value, size_t bufferLength)
{
	static const char hex[] = "0123456789ABCDEF";

	if (!value)
	{
		return;
	}

	while (*value)
	{
		unsigned char c = (unsigned char)*value++;
		if (FormUrlIsUnreserved(c))
		{
			if (!FormAppendChar(buffer, (char)c, bufferLength))
			{
				return;
			}
		}
		else if (c == ' ')
		{
			if (!FormAppendChar(buffer, '+', bufferLength))
			{
				return;
			}
		}
		else
		{
			if (!FormAppendChar(buffer, '%', bufferLength) ||
				!FormAppendChar(buffer, hex[c >> 4], bufferLength) ||
				!FormAppendChar(buffer, hex[c & 0x0f], bufferLength))
			{
				return;
			}
		}
	}
}

#endif
