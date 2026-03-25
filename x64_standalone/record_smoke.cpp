#include <cstring>

#include "../types.h"
#include "../sounddef.h"
#include "../tool/file.h"
#include "midi_backend.h"

static DWORD MakeMidi(unsigned char status, unsigned char data1, unsigned char data2)
{
	return static_cast<DWORD>(status) |
		(static_cast<DWORD>(data1) << 8) |
		(static_cast<DWORD>(data2) << 16);
}

int main(int argc, char **argv)
{
	const char *outPath = (argc > 1) ? argv[1] : "record_smoke.v2m";

	sdInit();
	msInit();

#ifdef RONAN
	strcpy(speech[0], "!h eh l ow");
	msStartAudio(0, soundmem, globals, const_cast<const char **>(speechptrs));
#else
	msStartAudio(0, soundmem, globals);
#endif

	msStartRecord();
	msProcessEvent(0, MakeMidi(0x90, 60, 100));
	msProcessEvent(22050, MakeMidi(0x80, 60, 0));

#ifdef RONAN
	msProcessEvent(33075, MakeMidi(0x9f, 48, 100));
	msProcessEvent(44100, MakeMidi(0x8f, 48, 0));
#endif

	msStopRecord();

	fileS outFile;
	if (!outFile.open(outPath, fileS::cr | fileS::wr))
	{
		msClose();
		sdClose();
		return 2;
	}

	const int writeResult = msWriteLastRecord(outFile);
	const int outSize = outFile.size();
	outFile.close();

	msClose();
	sdClose();

	return (writeResult && outSize > 0) ? 0 : 3;
}
