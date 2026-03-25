#include <cstring>

namespace
{
	static char *g_e2pInput = 0;
	static char *g_e2pOutput = 0;
}

extern "C" void e2p_initio(char *input, char *output)
{
	g_e2pInput = input;
	g_e2pOutput = output;
	if (g_e2pOutput)
	{
		g_e2pOutput[0] = 0;
	}
}

extern "C" void e2p_main()
{
	if (!g_e2pOutput)
	{
		return;
	}

	// The public source drop does not include the original English-to-phoneme
	// helper sources. Keep manual phoneme editing available instead of emitting
	// misleading pseudo-conversions here.
	g_e2pOutput[0] = 0;

	if (g_e2pInput)
	{
		(void)g_e2pInput;
	}
}
