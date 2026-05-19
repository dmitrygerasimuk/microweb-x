#include <stdio.h>
#include <string.h>

#include "../src/FormEncoding.h"
#include "../src/URL.h"

static int failures = 0;

static void CheckString(const char* name, const char* actual, const char* expected)
{
	if (strcmp(actual, expected))
	{
		printf("FAIL %s\n  expected: %s\n  actual:   %s\n", name, expected, actual);
		failures++;
	}
	else
	{
		printf("PASS %s\n", name);
	}
}

static void TestUrlAmpCleanup()
{
	URL url("http://example.test/search?q=dos&amp;page=1");
	url.CleanUp();
	CheckString("url clean decodes amp", url.url, "http://example.test/search?q=dos&page=1");
}

static void TestRelativeUrlAmpCleanup()
{
	const URL& url = URL::GenerateFromRelative(
		"http://example.test/docs/index.html",
		"page.html?one=1&amp;two=2");
	CheckString("relative url decodes amp", url.url, "http://example.test/docs/page.html?one=1&two=2");
}

static void TestUrlPathCleanup()
{
	const URL& url = URL::GenerateFromRelative(
		"http://example.test/docs/dir/index.html",
		"..\\file.html");
	CheckString("relative url path cleanup", url.url, "http://example.test/docs/file.html");
}

static void TestFormUrlEncoding()
{
	char buffer[128];
	buffer[0] = '\0';

	FormAppendUrlEncodedString(buffer, "name with spaces&symbols=", sizeof(buffer));
	CheckString("form url encoding", buffer, "name+with+spaces%26symbols%3D");
}

static void TestFormUrlEncodingHighBytes()
{
	char buffer[128];
	buffer[0] = '\0';

	const char cp1251Text[] = { (char)0xcf, (char)0xf0, (char)0xe8, (char)0xe2, (char)0xe5, (char)0xf2, 0 };
	FormAppendUrlEncodedString(buffer, cp1251Text, sizeof(buffer));
	CheckString("form url encoding cp1251", buffer, "%CF%F0%E8%E2%E5%F2");
}

int main()
{
	TestUrlAmpCleanup();
	TestRelativeUrlAmpCleanup();
	TestUrlPathCleanup();
	TestFormUrlEncoding();
	TestFormUrlEncodingHighBytes();

	if (failures)
	{
		printf("%d test(s) failed\n", failures);
		return 1;
	}

	printf("All tests passed\n");
	return 0;
}
