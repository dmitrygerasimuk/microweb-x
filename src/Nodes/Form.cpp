#include "Form.h"
#include <string.h>
#include "../Memory/Memory.h"
#include "../App.h"
#include "../Interface.h"
#include "Field.h"
#include "CheckBox.h"
#include "Select.h"

#include "../HTTP.h"

static bool IsFormUrlUnreserved(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') ||
		(c >= 'a' && c <= 'z') ||
		(c >= '0' && c <= '9') ||
		c == '-' || c == '_' || c == '.' || c == '~';
}

static bool AppendChar(char* buffer, char c, size_t bufferLength)
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

static void AppendUrlEncodedString(char* buffer, const char* value, size_t bufferLength)
{
	static const char hex[] = "0123456789ABCDEF";

	if (!value)
	{
		return;
	}

	while (*value)
	{
		unsigned char c = (unsigned char)*value++;
		if (IsFormUrlUnreserved(c))
		{
			if (!AppendChar(buffer, (char)c, bufferLength))
			{
				return;
			}
		}
		else if (c == ' ')
		{
			if (!AppendChar(buffer, '+', bufferLength))
			{
				return;
			}
		}
		else
		{
			if (!AppendChar(buffer, '%', bufferLength) ||
				!AppendChar(buffer, hex[c >> 4], bufferLength) ||
				!AppendChar(buffer, hex[c & 0x0f], bufferLength))
			{
				return;
			}
		}
	}
}

FormNode::Data* FormNode::Construct(Allocator& allocator)
{
	return allocator.Alloc<FormNode::Data>();
}

void FormNode::AppendParameter(char* address, const char* name, const char* value, int& numParams, size_t bufferLength)
{
	if (!name)
		return;

	if (!AppendChar(address, numParams == 0 ? '?' : '&', bufferLength))
	{
		return;
	}

	AppendUrlEncodedString(address, name, bufferLength);
	AppendChar(address, '=', bufferLength);
	AppendUrlEncodedString(address, value, bufferLength);
	numParams++;
}

void FormNode::BuildAddressParameterList(Node* node, char* address, int& numParams, size_t bufferLength)
{
	switch(node->type)
	{
		case Node::TextField:
		{
			TextFieldNode::Data* fieldData = static_cast<TextFieldNode::Data*>(node);

			if (fieldData->name && fieldData->buffer)
			{
				AppendParameter(address, fieldData->name, fieldData->buffer, numParams, bufferLength);
			}
		}
		break;
		case Node::CheckBox:
		{
			CheckBoxNode::Data* checkboxData = static_cast<CheckBoxNode::Data*>(node);

			if (checkboxData && checkboxData->isChecked && checkboxData->name && checkboxData->value)
			{
				AppendParameter(address, checkboxData->name, checkboxData->value, numParams, bufferLength);
			}
		}
		break;
		case Node::Select:
		{
			SelectNode::Data* selectData = static_cast<SelectNode::Data*>(node);

			if (selectData && selectData->selected)
			{
				AppendParameter(address, selectData->name, selectData->selected->text, numParams, bufferLength);
			}
		}
		break;
	}

	for (node = node->firstChild.Get(); node; node = node->next.Get())
	{
		BuildAddressParameterList(node, address, numParams, bufferLength);
	}
}

void FormNode::SubmitForm(Node* node)
{
	FormNode::Data* data = static_cast<FormNode::Data*>(node);
	App& app = App::Get();

	URL address(app.ui.addressBarURL);
	if (data->action)
	{
		if (!strcmp(data->action, "download://"))
		{
			for (Node* child = node->firstChild.Get(); child; child = child->next.Get())
			{
				if(child->type == Node::TextField)
				{
					TextFieldNode::Data* fieldData = static_cast<TextFieldNode::Data*>(child);
					if (!strcmp(fieldData->name, "path"))
					{
						App::Get().BeginFileDownload(fieldData->buffer);
					}
				}
			}
			return;
		}
		else if (!strcmp(data->action, "cancel://"))
		{
			App::Get().CancelFileDownload();
			return;
		}
		address = URL::GenerateFromRelative(app.ui.addressBarURL.url, data->action);
	}
	int numParams = 0;

	// Remove anything after existing ?
	char* questionMark = strstr(address.url, "?");
	if (questionMark)
	{
		*questionMark = '\0';
	}

	char* paramStart = address.url + strlen(address.url);

	BuildAddressParameterList(node, address.url, numParams, MAX_URL_LENGTH);

	if (data->method == FormNode::Data::Post)
	{
		static HTTPOptions postOptions;
		static char postContentData[MAX_URL_LENGTH];

		strcpy(postContentData, paramStart[0] == '?' ? paramStart + 1 : "");

		postOptions.contentData = postContentData;
		postOptions.keepAlive = false;
		postOptions.headerParams = NULL;
		postOptions.postContentType = "application/x-www-form-urlencoded";

		// Remove '?' from URL as params are passed as part of the POST request
		*paramStart = '\0';

		app.OpenURL(HTTPRequest::Post, URL::GenerateFromRelative(app.page.pageURL.url, address.url).url, &postOptions);
	}
	else
	{
		app.OpenURL(HTTPRequest::Get, URL::GenerateFromRelative(app.page.pageURL.url, address.url).url);
	}

}

void FormNode::OnSubmitButtonPressed(Node* node)
{
	Node* formNode = node->FindParentOfType(Node::Form);
	if (formNode)
	{
		SubmitForm(formNode);
	}
}
