#pragma once
#ifndef _FORM_H_
#define _FORM_H_


#include <stdint.h>
#include "../Node.h"

class FormNode : public NodeHandler
{
public:
	class Data : public Node
	{
	public:
		enum MethodType
		{
			Get,
			Post
		};
		struct HiddenInput
		{
			char* name;
			char* value;
			HiddenInput* next;
		};

		char* action;
		MethodType method;
		HiddenInput* hiddenInputs;

		Data() : Node(Node::Form), action(NULL), method(Get), hiddenInputs(NULL) {}
	};

	static FormNode::Data* Construct(Allocator& allocator);
	static void AddHiddenInput(Node* formNode, char* name, char* value);

	static void SubmitForm(Node* node);

	static void OnSubmitButtonPressed(Node* node);

private:
	static void BuildAddressParameterList(Node* node, char* address, int& numParams, size_t bufferLength);
	static void AppendParameter(char* address, const char* name, const char* value, int& numParams, size_t bufferLength);
};

#endif
