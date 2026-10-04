// tests/video/video_audio_slot_check.cpp
//
// Two movies that are open at the same time must not fight over the mixer's one video
// PCM slot. The campaign opens the briefing-screen video and, in the same frame, the
// full-screen cinematic; only the cinematic is updated until it ends. The movie that is
// being played must get the slot, and the waiting one must get it afterwards.
//
// Game-free: links only code/mc2video.cpp + FFmpeg. GameOS textures and the mixer are
// stubbed below; the mixer stub keeps the real one-stream rule (begin fails while a
// stream is active). Exit 0 == pass; any CHECK == fail.
//
//   build: see the video_audio_slot_check target in CMakeLists.txt
//   run:   ./video_audio_slot_check ../run/data/movies/node_10.mp4   (any movie with an audio track)

#include "mc2video.h"
#include "gameos.hpp"
#include "gos_crashbundle.h"
#include "soundsys.h"

#include <stdio.h>
#include <stdlib.h>
#include <vector>

// Always-on check: assert() is stripped under NDEBUG.
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK FAILED: %s  (%s:%d)\n", #x, __FILE__, __LINE__); abort(); } } while (0)

//---------------------------------------------------------------------------
// Mixer stub. mc2video only calls these non-virtual SoundSystem methods and
// none of them touch `this` here, so soundSystem just has to be non-null.
static bool streamActive = false;
static long framesPushed = 0;

bool SoundSystem::queryNativeFormat(int* rate, int* channels) const { *rate = 44100; *channels = 2; return true; }
bool SoundSystem::beginVideoPCMStream(int, int)
{
	if (streamActive) return false;
	streamActive = true;
	framesPushed = 0;
	return true;
}
void SoundSystem::pushVideoPCMSamples(const int16_t*, int frameCount) { CHECK(streamActive); framesPushed += frameCount; }
void SoundSystem::endVideoPCMStream() { streamActive = false; }
int  SoundSystem::videoPCMSamplesConsumed() const { return 0; }
long SoundSystem::playDigitalStream(const char*) { return -1; }

class GameSoundSystem;
static char mixerStub;
GameSoundSystem* soundSystem = (GameSoundSystem*)&mixerStub;

//---------------------------------------------------------------------------
// GameOS / mclib stubs.
char moviePath[80] = "";
char soundPath[80] = "";
long fileExists(const char*, long) { return 0; }	// no sidecar .wav: embedded audio only
void crashbundle_append(const char*) {}

static std::vector<DWORD> texels;
static DWORD texWidth = 0, texHeight = 0;
DWORD gos_NewEmptyTexture(gos_TextureFormat, const char*, DWORD HeightWidth, DWORD, gos_RebuildFunction, void*)
{
	texWidth = HeightWidth & 0xffff;
	texHeight = HeightWidth >> 16;
	texels.assign((size_t)texWidth * texHeight, 0);
	return 1;
}
void gos_DestroyTexture(DWORD) {}
void gos_LockTexture(DWORD, DWORD, bool, TEXTUREPTR* info)
{
	info->pTexture = texels.data();
	info->Width = texWidth;
	info->Height = texHeight;
	info->Pitch = texWidth;
}
void gos_UnLockTexture(DWORD) {}
void gos_SetRenderState(gos_RenderState, int) {}
void gos_DrawQuads(gos_VERTEX*, int) {}

//---------------------------------------------------------------------------
static VideoSession* openMovie(const char* path)
{
	VideoOpenParams p = {};
	p.resolvedPath = path;
	p.destRectW = 640;
	p.destRectH = 480;
	VideoOpenResult r = {};
	VideoSession* s = video_open(p, &r);
	CHECK(s);
	CHECK(r.hasAudio);	// the check needs a movie with an audio track
	return s;
}

int main(int argc, char** argv)
{
	if (argc != 2) { fprintf(stderr, "usage: %s <movie with an audio track>\n", argv[0]); return 2; }

	// Same frame: the briefing video opens first, then the cinematic.
	VideoSession* briefing  = openMovie(argv[1]);
	VideoSession* cinematic = openMovie(argv[1]);
	CHECK(!streamActive);		// an opened movie that is not playing holds nothing

	// Only the cinematic is updated while it is on screen: it must have sound.
	CHECK(!video_update(cinematic));
	CHECK(streamActive);
	CHECK(framesPushed > 0);

	// The cinematic ends; the briefing video starts and gets the slot.
	video_close(cinematic);
	CHECK(!streamActive);
	CHECK(!video_update(briefing));
	CHECK(streamActive);
	CHECK(framesPushed > 0);

	// A restart gives the slot back until the movie plays again.
	video_restart(briefing);
	CHECK(!streamActive);
	CHECK(!video_update(briefing));
	CHECK(streamActive);

	video_close(briefing);
	CHECK(!streamActive);

	printf("video_audio_slot_check: ok\n");
	return 0;
}
