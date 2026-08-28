// data_tools_game_stubs.cpp — link stubs for the offline data tools.
//
// LINK-CONFIG-FIX: makefst / pak / aseconv / makersp link the full engine
// closure (mclib + gameos + renderer libs) purely to satisfy the symbol
// references those libraries make against each other. A handful of game-layer
// globals are *defined* only in code/ translation units (logmain.cpp,
// objmgr.cpp, terrobj.cpp, gameplay_pick.cpp) that the tools deliberately do
// not compile (they would drag in the whole game). The renderer/mclib object
// files still reference them, so the link fails with these few unresolved
// externals.
//
// The tools never exercise the code paths that touch these globals, so trivial
// definitions are sufficient and correct for the tools' purpose. The types are
// forward-declared to reproduce the exact decorated symbol names without
// pulling in any game headers.
//
// If a tool ever genuinely needs one of these at runtime, that is a sign it
// should link the real defining TU instead of this stub.

#include <atomic>

class Camera;
class GameObjectManager;

// aseconv.cpp defines `eye` and `MaxMinUV` itself; compile this TU with
// DATA_TOOLS_STUBS_HAVE_EYE_AND_MAXMINUV defined for that target to avoid a
// duplicate-symbol (LNK2005). The other tools define neither and need both.
#ifndef DATA_TOOLS_STUBS_HAVE_EYE_AND_MAXMINUV
// code/logmain.cpp:103  -> CameraPtr eye = NULL;
Camera* eye = nullptr;

// code/logmain.cpp:73 / code/mechcmd2.cpp:169 -> float MaxMinUV = 8.0f;
float MaxMinUV = 8.0f;
#endif

// code/terrobj.cpp:209 -> std::atomic<unsigned long long> g_tobjAngularCyc{0};
std::atomic<unsigned long long> g_tobjAngularCyc{0ULL};

// code/objmgr.cpp:469 -> GameObjectManagerPtr ObjectManager = NULL;
GameObjectManager* ObjectManager = nullptr;

// code/gameplay_pick.cpp:202 -> void RunGameplayPickSelfTest()
void RunGameplayPickSelfTest() {}

// macos-port: aseconv pulls mech3d.cpp + gos_terrain_indirect.cpp +
// debug_state_dump.cpp (which makefst does not), so the tool link needs these
// engine globals the owning TUs (code/mechcmd2.cpp, logisticsmissioninfo.cpp,
// unitprofile_fit.cpp) provide only in the full mc2 build.
// mclib/timing.h -> extern bool gamePaused;   (owner code/mechcmd2.cpp:120)
bool gamePaused = false;
// code/logisticsmissioninfo.cpp:19 -> char missionName[1024];
char missionName[1024] = {0};
// code/unitprofile.h:31 -> extern "C" int mc2_unitprofile_collect_witness(...)
extern "C" int mc2_unitprofile_collect_witness(void*, int) { return 0; }
