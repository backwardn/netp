#ifndef _BARE_H
#define _BARE_H

#include "bare_gen.h"

struct BareThread
	: public BareGen
{
	int main();

	void recvHello( Hello *msg );
};

#endif

