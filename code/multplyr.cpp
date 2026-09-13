//***************************************************************************
//
//	multplyr.cpp - This file contains the MultiPlayer Class
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef LINUX_BUILD
#include"crtdbg.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef TURRET_H
#include"turret.h"
#endif

#ifndef MISSION_H
#include"mission.h"
#include"tacordr.h"
#endif

#ifndef MECH_H
#include"mech.h"
#endif

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef LOGISTICSDATA_H
#include"logisticsdata.h"
#endif

#ifndef BLDNG_H
#include"bldng.h"
#endif

#ifndef TERROBJ_H
#include"terrobj.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#ifndef GROUP_H
#include"group.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#ifndef CARNAGE_H
#include"carnage.h"
#endif

#ifndef ARTLRY_H
#include"artlry.h"
#endif

#ifndef CONTROLGUI_H
#include"controlgui.h"
#endif

#ifndef LOGISTICS_H
#include"logistics.h"
#endif

#include"missionbegin.h"

#ifndef PREFS_H
#include"prefs.h"
#endif

#ifndef MISSIONGUI_H
#include"missiongui.h"
#endif

#ifndef GAMELOG_H
#include"gamelog.h"
#endif

#ifndef LINUX_BUILD
#include"dplay8.h"
#endif


// MP-ENET-1: cross-platform UDP transport behind the stubbed DirectPlay seam.
#include"mptransport.h"
#include<stdlib.h>
#include<vector>
#include<stdint.h>
#include<time.h>
#include<unistd.h>
#include"build_fingerprint.h"	// MC2_BUILD_GIT_SHA: lobby version stamp
#include"prefs.h"

namespace {
	MPTransport s_transport;

	// MCMSG_PlayerCID.subType. Retail semantics were never released; these are ours.
	enum { MP_CID_ASSIGN = 0 };		// host -> joiner: "your commanderID is <commanderID>"
	enum { MP_LEAVE_PLAYER = 0, MP_LEAVE_HOST = 1 };	// MCMSG_LeaveSession.subType

	struct InboundMsg {
		NETPLAYER			sender;
		std::vector<unsigned char>	bytes;
	};
	std::vector<InboundMsg> s_inbound;		// filled by poll callbacks, drained in processMessages()

	void mpOnRecv (void* /*user*/, void* peer, const void* data, int size, bool /*reliable*/) {
		InboundMsg m;
		m.sender = (NETPLAYER)peer;
		m.bytes.assign((const unsigned char*)data, (const unsigned char*)data + size);
		s_inbound.push_back(m);
	}

	void mpOnPeer (void* user, void* peer, bool connected) {
		MultiPlayer* mp = (MultiPlayer*)user;

		if (s_transport.isHost()) {
			if (connected) {
				// Lowest free slot. Slot 0 is the host (set in hostSession).
				long cid = -1;
				for (long i = 0; i < MAX_MC_PLAYERS; i++)
					if (mp->playerInfo[i].player == NULL) { cid = i; break; }
				if (cid < 0) {
					if (getenv("MC2_LOG")) printf("[MP] session full, kicking %p\n", peer);
					s_transport.kick(peer);
					return;
				}
				MC2Player& slot = mp->playerInfo[cid];
				slot.player = (NETPLAYER)peer;
				slot.commanderID = (char)cid;
				slot.leftSession = false;
				slot.booted = false;
				mp->playerList[cid].player = slot.player;
				mp->playerList[cid].commanderID = slot.commanderID;
				mp->sendPlayerCID((NETPLAYER)peer, MP_CID_ASSIGN, (char)cid);
				if (getenv("MC2_LOG")) printf("[MP] peer %p joined -> commanderID %ld\n", peer, cid);
			} else {
				long cid = mp->findPlayer((NETPLAYER)peer);
				if (cid >= 0) {
					// Free the whole slot; a rejoin into it must start clean (checkedIn etc.).
					memset(&mp->playerInfo[cid], 0, sizeof(MC2Player));
					mp->playerInfo[cid].commanderID = -1;
					mp->playerList[cid].player = NULL;
					mp->playerList[cid].commanderID = -1;
				}
				if (cid >= 0) mp->sendLeaveSession(MP_LEAVE_PLAYER, (char)cid);
				if (getenv("MC2_LOG")) printf("[MP] peer %p left (was commanderID %ld)\n", peer, cid);
			}
		} else {
			if (connected && peer == s_transport.getServerPeer())
				mp->serverPlayer = (NETPLAYER)peer;
			if (!connected && peer == mp->serverPlayer)
				mp->hostDroppedOut = true;
			if (getenv("MC2_LOG"))
				printf("[MP] %s server %p\n", connected ? "connected to" : "lost", peer);
		}
	}

	// MP-1 lobby sync. MCMSG_PlayerUpdate.stage: the screens pass 5/6 ("my row changed");
	// MP_PU_HELLO is a joiner's first message (carries its version stamp).
	enum { MP_PU_HELLO = 1 };
	MissionSettings s_lastSentSettings;
	bool s_lastSentValid = false;
	NETPLAYER remotePlayerHandle (long cid) {	// non-NULL 'slot occupied' marker on clients
		return (NETPLAYER)(uintptr_t)(0x1000 + cid);
	}
	// Direct-connect target: MC2_MP_CONNECT=host[:port], or the MC2_MP_AUTOJOIN harness hook.
	const char* mpConnectAddr (void) {
		const char* a = getenv("MC2_MP_CONNECT");
		if (!a || !a[0]) a = getenv("MC2_MP_AUTOJOIN");
		return (a && a[0]) ? a : NULL;
	}
	// MCMSG_MissionSetup.subType (retail values unknown; ours). Host -> clients unless noted.
	enum {
		MP_SETUP_ZONES      = 0,	// commandersToLoad + randomSeed: start loading the mission
		MP_SETUP_MECHDATA   = 1,	// client -> host: one CompressedMech of my logistics lance
		MP_SETUP_LOADED     = 2,	// client -> host: Mission::init done
		MP_SETUP_STARTED    = 4,	// client -> host: mission->start done
		MP_SETUP_ALL_STARTED= 5,	// everyone has started: enter MULTIPLAYER_MODE_MISSION
		MP_SETUP_GO_LOBBY   = 6,	// Launch pressed: leave the parameter screen for the load screen
	};
	const int kMpWaitTimeoutMs = 120000;
	enum { MP_ORDER_QUEUED = 1, MP_ORDER_NEEDS_SELECTION = 2 };	// MCMSG_PlayerOrder.flags
	// MCMSG_MoverUpdate.moveData entry: roster index + the mover's packed status and move chunks.
	#pragma pack(push, 1)
	struct MpMoverEntry { unsigned char idx; unsigned char flags; unsigned long status; unsigned long move; };
	#pragma pack(pop)
	enum { MP_ENTRY_STATUS = 1, MP_ENTRY_MOVE = 2 };
	const int kMpMoverUpdateMs = 100;		// 10 Hz poll, unreliable; only changed chunks go out
	const int kMpMoverKeyframeEvery = 10;	// ...plus everything once a second (packet loss)
	unsigned long s_lastSentStatus[MAX_MULTIPLAYER_MOVERS];
	unsigned long s_lastSentMove[MAX_MULTIPLAYER_MOVERS];
	bool s_lastSentValidChunk[MAX_MULTIPLAYER_MOVERS];
	long s_hitsSent = 0, s_hitsApplied = 0, s_fireChunksSent = 0, s_fireChunksReceived = 0;
	const float kMpEntryQuad[4] = {0.0f, 180.0f, -90.0f, 90.0f};	// WeaponHitChunk.entryAngle
	unsigned short s_moverUpdateId = 0;		// host: last sent
	unsigned short s_lastMoverUpdateId = 0;	// client: last applied
	bool s_haveMoverUpdate = false;
	unsigned short mpPort (void) {
		const char* e = getenv("MC2_MP_PORT");
		long v = e ? atol(e) : 0;
		return (v > 0 && v < 65536) ? (unsigned short)v : (unsigned short)MC2_MP_DEFAULT_PORT;
	}
}
#include"mpparameterscreen.h"

//sebi: commented include
//#ifndef VERSION_H
//#include"version.h"
//#endif

#include "../resource.h"

#ifdef USE_MISSION_RESULTS_SCREEN
extern bool EventsToMissionResultsScreen;
#else
bool EventsToMissionResultsScreen = false;
#endif

#ifdef USE_LOGISTICS
extern bool whackTimer;
void CancelBool(long value);
#endif

#ifndef MPPREFS_H
#include"mpprefs.h"
#endif

#ifdef USE_STRING_RESOURCES
extern HINSTANCE thisInstance;
long cLoadString (HINSTANCE hInstance,  UINT uID, LPTSTR lpBuffer, int nBufferMax );
#endif

#include"gamesound.h"

#define	MAX_MSG_SIZE		10240

extern CPrefs prefs;
extern bool quitGame;

extern float loadProgress;
extern bool aborted;

//***************************************************************************

DWORD ServerPlayerNum = 1; //commanderId (or checkInId) of server
bool MultiPlayer::launchedFromLobby = false;
bool MultiPlayer::registerZone = false;

long MultiPlayer::presetDropZones[MAX_MC_PLAYERS] = {-1, -1, -1, -1, -1, -1, -1, -1};
long MultiPlayer::colors[MAX_COLORS] = {0};

float ResourceBuildingRefreshRate = 60.0f;

// {35DC7890-C5EF-4171-B0CF-4D5C7AE7C2D7}
static const GUID MC2GUID = { 0x35dc7890, 0xc5ef, 0x4171, { 0xb0, 0xcf, 0x4d, 0x5c, 0x7a, 0xe7, 0xc2, 0xd7 } };
// {1F8251BB-1436-44b3-A5B0-0FCF6858C176}
static const GUID MC2DEMOGUID = { 0x1f8251bb, 0x1436, 0x44b3, { 0xa5, 0xb0, 0xf, 0xcf, 0x68, 0x58, 0xc1, 0x76 } };

void* MC2NetLib = NULL;
bool OnLAN = false;
NETMESSAGE ReceiveMsg;

//------------
// EXTERN vars
extern GameLog* CombatLog;
extern GameLog* NetLog;

extern bool LaunchedFromLobby;

extern UserHeapPtr systemHeap;

#ifdef _DEBUG
//extern DebugFileStream Debug;
#endif

extern char* startupPakFile;

// ConnectUsingDialog is a function defined in dpdialog.cpp.  
// It allows the user to choose the type of connection.
//extern HRESULT ConnectUsingDialog(HINSTANCE );

extern void SortMoverList (long numMovers, MoverPtr* moverList, Stuff::Vector3D dest);

extern void killTheGame(void);

#ifndef TERRAINEDIT
extern DebuggerPtr debugger;
#endif

//------------
// GLOBAL vars
MultiPlayer* MPlayer = NULL;
//extern long NumMissionScriptMessages;

///extern Scenario* scenario;
///extern Logistics* globalLogPtr;

void DEBUGWINS_print (const char* s, long window = 0);

//***************************************************************************
// MISC functions
//***************************************************************************

bool StartupNetworking (void) {

	return false;
}

//-----------------------------------------------------------------------------

void ResetNetworking (void) {

}

//-----------------------------------------------------------------------------

void ShutdownNetworking (void) {

}

//***************************************************************************
// WORLD CHUNK class
//***************************************************************************

void* WorldChunk::operator new (size_t ourSize) {

	void* result = systemHeap->Malloc(ourSize);
	return(result);
}

//---------------------------------------------------------------------------

void WorldChunk::operator delete (void* us) {

	systemHeap->Free(us);
}	

//---------------------------------------------------------------------------

void WorldChunk::buildMine (long worldCellR,
							long worldCellC,
							long teamId,
							long mineState,
							long explosionType) {

}

//---------------------------------------------------------------------------

void WorldChunk::buildTerrainFire (GameObjectPtr object,
								   long seconds) {

}

//---------------------------------------------------------------------------

void WorldChunk::buildArtillery (long commanderId,
								 long artilleryType,
								 Stuff::Vector3D location,
								 long seconds) {

}

//---------------------------------------------------------------------------

void WorldChunk::buildMissionScriptMessage (long messageCode,
											long messageParam) {

}

//---------------------------------------------------------------------------

void WorldChunk::buildPilotKillStat (
									 long moverIndex,
									 long vehicleClass) {

}

//--------------------------------------------------------------------------

void WorldChunk::buildScore (long commanderID, long score) {

}

//--------------------------------------------------------------------------

void WorldChunk::buildKillLoss (long killerCID, long loserCID) {

}

//--------------------------------------------------------------------------

void WorldChunk::buildCaptureBuilding (BuildingPtr building, long newCommanderID) {

}

//---------------------------------------------------------------------------

void WorldChunk::buildEndMission (void) {

}

//--------------------------------------------------------------------------

void WorldChunk::pack (void) {

}

//---------------------------------------------------------------------------
		
void WorldChunk::unpack (void) {

}

//---------------------------------------------------------------------------

bool WorldChunk::equalTo (WorldChunkPtr chunk) {

	return false;
}

//***************************************************************************
// MECHCOMMANDER MESSAGE handlers
//***************************************************************************

//***************************************************************************
// MULTIPLAYER class
//***************************************************************************

void* MultiPlayer::operator new (size_t ourSize) {

	void* result = systemHeap->Malloc(ourSize);
	return(result);
}

//---------------------------------------------------------------------------

void MultiPlayer::operator delete (void* us) {

	systemHeap->Free(us);
}	

//---------------------------------------------------------------------------

void MultiPlayer::init (void) {
	initStartupParameters(true);
}

//---------------------------------------------------------------------------

long MultiPlayer:: setup (void) {

	return(MPLAYER_NO_ERR);
}

//---------------------------------------------------------------------------

void MultiPlayer::initUpdateFrequencies() {

}

//---------------------------------------------------------------------------

long MultiPlayer::update (void) {
	if (!inSession)
		return(MPLAYER_NO_ERR);
	processMessages();
	if (iAmHost) {
		// The lobby screens poke missionSettings directly; diff it once a frame and push changes.
		missionSettings.locked = locked;
		missionSettings.inProgress = inProgress;
		if (!s_lastSentValid || memcmp(&s_lastSentSettings, &missionSettings, sizeof(missionSettings)) != 0)
			sendMissionSettingsUpdate(NULL);
	}
	else if (hostDroppedOut && !hostLeft) {
		hostLeft = true;
		if (getenv("MC2_LOG")) printf("[MP] host connection lost\n");
	}
	if (mode == MULTIPLAYER_MODE_MISSION && mission) {
		if (iAmHost) {
			static unsigned long s_lastMoverUpdate = 0;
			unsigned long now = (unsigned long)timeGetTime();
			if (now - s_lastMoverUpdate >= (unsigned long)kMpMoverUpdateMs) {
				s_lastMoverUpdate = now;
				sendMoverUpdate();
			}
			sendMoverWeaponFireUpdate();	// each early-returns when there is nothing queued
			sendMoverCriticalUpdate();
			sendWeaponHitUpdate();
		}
		missionDiagnostics();
	}
	return(MPLAYER_NO_ERR);
}

//---------------------------------------------------------------------------

long MultiPlayer::beginSessionScan (char* ipAddress, bool persistent) {

	return(MPLAYER_NO_ERR);
}

//---------------------------------------------------------------------------

long MultiPlayer::endSessionScan (void) {

	return(MPLAYER_NO_ERR);
}

//---------------------------------------------------------------------------

MC2Session* MultiPlayer::getSessions (long& sessionCount) {

	// MP-ENET-1: there is no LAN/lobby discovery yet. With MC2_MP_CONNECT=host[:port]
	// set, offer a single synthetic "direct connect" entry so the browser's Join
	// button reaches joinSession(), which reads the same variable.
	sessionCount = 0;
	const char* addr = mpConnectAddr();
	if (addr && !s_transport.isHost()) {
		MC2Session& ses = sessionList[0];
		memset(&ses, 0, sizeof(ses));
		strncpy(ses.name, "Direct: ", MAXLEN_SESSION_NAME - 1);
		strncat(ses.name, addr, MAXLEN_SESSION_NAME - 1 - strlen(ses.name));
		ses.maxPlayers = MAX_MC_PLAYERS;
		ses.numPlayers = 1;
		ses.ping = 0;
		ses.locked = false;
		ses.inProgress = false;
		ses.cancelled = false;
		sessionCount = 1;
	}
	numSessions = sessionCount;
	return(sessionList);
}

//---------------------------------------------------------------------------

bool MultiPlayer::hostSession (char* sessionName, char* playerName, long mxPlayers) {

	if (!MPTransport::initLibrary())
		return(false);
	if (!s_transport.host(mpPort(), (int)mxPlayers))
		return(false);

	iAmHost = true;
	inSession = true;
	myPlayer = (NETPLAYER)this;		// host has no ENetPeer for itself; stable non-null sentinel
	serverPlayer = myPlayer;
	commanderID = 0;
	for (long i = 0; i < MAX_MC_PLAYERS; i++) {
		playerInfo[i].player = NULL;
		playerInfo[i].leftSession = false;
		playerInfo[i].booted = false;
		playerList[i].player = NULL;
	}
	playerInfo[0].player = myPlayer;
	playerInfo[0].commanderID = 0;
	playerList[0].player = myPlayer;
	playerList[0].commanderID = 0;
	strncpy(this->sessionName, sessionName ? sessionName : "", sizeof(this->sessionName) - 1);
	strncpy(this->playerName, playerName ? playerName : "", sizeof(this->playerName) - 1);
	missionSettings.maxPlayers = (char)mxPlayers;
	snprintf(sessionIPAddress, sizeof(sessionIPAddress), "port %u", (unsigned)mpPort());	// ponytail: no local-IP probe yet
	versionStatus = VERSION_STATUS_GOOD;
	setDefaultPlayerInfo();
	s_lastSentValid = false;
	if (getenv("MC2_LOG"))
		printf("[MP] hosting \"%s\" on udp/%u (max %ld)\n", this->sessionName, (unsigned)mpPort(), mxPlayers);
	return(true);
}

//---------------------------------------------------------------------------

void MultiPlayer::updateSessionData (MC2SessionData* sessionData) {

}

//---------------------------------------------------------------------------

void MultiPlayer::setLocked (bool set) {
	locked = set;
	missionSettings.locked = set;
}

//---------------------------------------------------------------------------

void MultiPlayer::setInProgress (bool set) {
	inProgress = set;
	missionSettings.inProgress = set;
}
//---------------------------------------------------------------------------

void MultiPlayer::setCancelled (bool set) {

}

//---------------------------------------------------------------------------

void MultiPlayer::addTeamScore (int teamID, int score) {

}

//---------------------------------------------------------------------------

long MultiPlayer::joinSession (MC2Session* session, char* playerName) {

	const char* addr = mpConnectAddr();		// "host" or "host:port"
	if (!addr)
		return(MPLAYER_ERR_SESSION_NOT_FOUND);

	char host[256];
	strncpy(host, addr, sizeof(host) - 1);
	host[sizeof(host) - 1] = 0;
	unsigned short port = mpPort();
	if (char* colon = strrchr(host, ':')) {
		*colon = 0;
		long v = atol(colon + 1);
		if (v > 0 && v < 65536) port = (unsigned short)v;
	}

	if (!MPTransport::initLibrary())
		return(MPLAYER_ERR_SESSION_NOT_FOUND);
	if (!s_transport.connect(host, port))
		return(MPLAYER_ERR_SESSION_NOT_FOUND);

	iAmHost = false;
	inSession = true;
	myPlayer = (NETPLAYER)this;
	serverPlayer = NULL;				// set by mpOnPeer once the connect completes
	commanderID = -1;						// assigned by the host via MCMSG_PlayerCID
	versionStatus = VERSION_STATUS_UNKNOWN;	// GOOD/BAD once the host's first PlayerUpdate arrives
	hostLeft = false;
	hostDroppedOut = false;
	for (long i = 0; i < MAX_MC_PLAYERS; i++) { playerInfo[i].player = NULL; playerList[i].player = NULL; }
	strncpy(this->playerName, playerName ? playerName : "", sizeof(this->playerName) - 1);
	if (session && session->name[0])
		strncpy(this->sessionName, session->name, sizeof(this->sessionName) - 1);
	if (getenv("MC2_LOG"))
		printf("[MP] connecting to %s:%u\n", host, (unsigned)port);
	return(MPLAYER_NO_ERR);
}

//-----------------------------------------------------------------------------

long MultiPlayer::closeSession (void) {

	if (inSession && getenv("MC2_LOG"))
		printf("[MP] closeSession (was %s, commanderID %ld)\n", iAmHost ? "host" : "client", commanderID);
	s_transport.close();
	s_inbound.clear();
	for (long i = 0; i < MAX_MC_PLAYERS; i++) { playerInfo[i].player = NULL; playerList[i].player = NULL; }
	inSession = false;
	iAmHost = false;
	myPlayer = serverPlayer = NULL;
	return 0;
}

//-----------------------------------------------------------------------------

void MultiPlayer::leaveSession (void) {
	if (inSession && commanderID >= 0)
		sendLeaveSession(iAmHost ? MP_LEAVE_HOST : MP_LEAVE_PLAYER, (char)commanderID);
	closeSession();
}

//-----------------------------------------------------------------------------

  long MultiPlayer::bootPlayer (NETPLAYER bootedPlayer) {

	if (bootedPlayer && bootedPlayer != myPlayer)
		s_transport.kick(bootedPlayer);
	return(MPLAYER_NO_ERR);
}

//-----------------------------------------------------------------------------

// Blocking barrier used by Logistics::beginMission (retail did the same over DirectPlay):
// pump messages until `done` says so, the host drops, or the timeout expires.
static bool mpWaitUntil (MultiPlayer* mp, bool (*done)(MultiPlayer*), const char* what) {
	unsigned long t0 = (unsigned long)timeGetTime();
	while (!done(mp)) {
		mp->update();
		if (mp->hostLeft || mp->hostDroppedOut) {
			if (getenv("MC2_LOG")) printf("[MP] wait %s: host gone\n", what);
			return(false);
		}
		if ((unsigned long)timeGetTime() - t0 > (unsigned long)kMpWaitTimeoutMs) {
			if (getenv("MC2_LOG")) printf("[MP] wait %s: timed out\n", what);
			return(false);
		}
		usleep(5000);
	}
	return(true);
}
static bool mpAllPeers (MultiPlayer* mp, const bool* flags) {
	for (long i = 0; i < MAX_MC_PLAYERS; i++)
		if (mp->playerInfo[i].player && !mp->playerInfo[i].leftSession && !flags[i])
			return(false);
	return(true);
}
static bool doneStartLoading (MultiPlayer* mp)   { return mp->startLoading; }
static bool doneMechData (MultiPlayer* mp)       { return mpAllPeers(mp, mp->mechDataReceived); }
static bool doneAllLoaded (MultiPlayer* mp)      { return mpAllPeers(mp, mp->missionDataLoaded); }
static bool doneAllStarted (MultiPlayer* mp)     { return mpAllPeers(mp, mp->missionFullySetup); }
static bool doneStartMission (MultiPlayer* mp)   { return mp->startMission; }
static bool doneSetupMission (MultiPlayer* mp)   { return mp->setupMission; }

// [MP_POS] every 2 s: one line per roster mover, same format both sides, so a runner can
// compare host and client worlds. MC2_MP_SCRIPT_ORDERS=1 on a client: every 20 s send a
// move order for each local mover toward the first enemy mover (harness only).
void MultiPlayer::missionDiagnostics (void) {
	static unsigned long s_lastPos = 0, s_lastOrder = 0, s_missionT0 = 0;
	unsigned long now = (unsigned long)timeGetTime();
	if (!s_missionT0) s_missionT0 = now;
	const bool logOn = (getenv("MC2_LOG") != NULL);
	if (logOn && now - s_lastPos >= 2000) {
		s_lastPos = now;
		for (long i = 0; i < MAX_MULTIPLAYER_MOVERS; i++) {
			MoverPtr m = moverRoster[i];
			if (!m) continue;
			int r = 0, c = 0;
			m->getCellPosition(r, c);
			printf("[MP_POS] t=%.0f cid=%ld idx=%ld r=%d c=%d dead=%d\n", mission->actualTime, m->getCommanderId(), i, r, c, m->isDestroyed() ? 1 : 0);
		}
		fflush(stdout);
	}
	if (!iAmHost && getenv("MC2_MP_SCRIPT_ORDERS") && now - s_missionT0 > 10000 && now - s_lastOrder >= 20000) {
		s_lastOrder = now;
		MoverPtr enemy = NULL;
		for (long i = 0; i < MAX_MULTIPLAYER_MOVERS && !enemy; i++)
			if (moverRoster[i] && moverRoster[i]->getCommanderId() != commanderID && !moverRoster[i]->isDestroyed())
				enemy = moverRoster[i];
		if (!enemy) return;
		for (long i = 0; i < numLocalMovers; i++) {
			MoverPtr m = localMovers[i];
			if (!m || m->isDestroyed()) continue;
			TacticalOrder tacOrder;
			tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_ATTACK_OBJECT);
			tacOrder.targetWID = enemy->getWatchID();
			tacOrder.attackParams.type = ATTACK_TO_DESTROY;
			tacOrder.attackParams.method = ATTACKMETHOD_RANGED;
			tacOrder.attackParams.range = FIRERANGE_OPTIMAL;
			tacOrder.attackParams.pursue = true;
			tacOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
			tacOrder.pack(NULL, NULL);
			sendPlayerOrder(&tacOrder, false, 1, &m);
		}
		printf("[MP] scripted attack orders sent for %ld movers\n", numLocalMovers);
		fflush(stdout);
	}
}

void MultiPlayer::resetForNewGame (void) {
	// Mission::destroy calls this: the session lives on for a rematch, the world does not.
	memset(moverRoster, 0, sizeof(moverRoster));
	memset(playerMoverRoster, 0, sizeof(playerMoverRoster));
	memset(localMovers, 0, sizeof(localMovers));
	memset(turretRoster, 0, sizeof(turretRoster));
	numMovers = numLocalMovers = numTurrets = 0;
	numWeaponHitChunks = 0;
	memset(s_lastSentValidChunk, 0, sizeof(s_lastSentValidChunk));
	s_haveMoverUpdate = false;
	startLogistics = startLoading = startMission = setupMission = endMission = false;
	winningTeam = -1;
	memset(readyToLoad, 0, sizeof(readyToLoad));
	memset(mechDataReceived, 0, sizeof(mechDataReceived));
	memset(missionDataLoaded, 0, sizeof(missionDataLoaded));
	memset(missionFullySetup, 0, sizeof(missionFullySetup));
	memset(allUnitsDestroyed, 0, sizeof(allUnitsDestroyed));
	memset(mechData, 0, sizeof(mechData));
	for (long i = 0; i < MAX_MC_PLAYERS; i++)
		playerInfo[i].ready = false;
	inProgress = false;
	missionSettings.inProgress = false;
	if (mode == MULTIPLAYER_MODE_MISSION)
		mode = MULTIPLAYER_MODE_RESULTS;
	if (getenv("MC2_LOG")) printf("[MP] rosters reset for results/rematch\n");
}

void MultiPlayer::logRoster (void) {
	// One line both sides can be compared on: order-sensitive hash of type + start cell per roster slot.
	unsigned long h = 2166136261UL;
	long count = 0;
	for (long i = 0; i < MAX_MULTIPLAYER_MOVERS; i++) {
		MoverPtr m = moverRoster[i];
		if (!m) continue;
		int r = 0, c = 0;
		m->getCellPosition(r, c);
		unsigned long v = ((unsigned long)i << 24) ^ ((unsigned long)m->getObjectType()->getObjTypeNum() << 12)
		                ^ ((unsigned long)(r & 0xfff) << 6) ^ (unsigned long)(c & 0xfff) ^ ((unsigned long)m->getCommanderId() << 30);
		h = (h ^ v) * 16777619UL;
		count++;
	}
	printf("[MP] roster hash=%08lx movers=%ld local=%ld seed=0x%08lx\n", h & 0xffffffffUL, count, numLocalMovers, (unsigned long)randomSeed);
	fflush(stdout);
}

bool MultiPlayer::waitTillStartLoading (void) {
	if (iAmHost)
		return(true);
	return(mpWaitUntil(this, doneStartLoading, "start loading"));
}

//-----------------------------------------------------------------------------

bool MultiPlayer::waitTillMechDataReceived (void) {
	if (!iAmHost)
		return(true);
	mechDataReceived[commanderID] = true;
	return(mpWaitUntil(this, doneMechData, "mech data"));
}

//-----------------------------------------------------------------------------

bool MultiPlayer::waitTillMissionLoaded (void) {
	logRoster();
	if (iAmHost) {
		missionDataLoaded[commanderID] = true;
		if (!mpWaitUntil(this, doneAllLoaded, "all loaded"))
			return(false);
		sendStartMission();
		return(true);
	}
	return(mpWaitUntil(this, doneStartMission, "start mission"));
}

//-----------------------------------------------------------------------------

bool MultiPlayer::waitTillMissionSetup (void) {
	if (iAmHost) {
		missionFullySetup[commanderID] = true;
		if (!mpWaitUntil(this, doneAllStarted, "all started"))
			return(false);
		sendMissionSetup(NULL, MP_SETUP_ALL_STARTED, NULL);
		return(true);
	}
	return(mpWaitUntil(this, doneSetupMission, "all started"));
}

//-----------------------------------------------------------------------------

bool MultiPlayer::waitForSessionEntry (void) {

	sessionEntry = -1;
	return(sessionEntry == 1);
}

//-----------------------------------------------------------------------------

bool MultiPlayer::playersReadyToLoad (void) {
	return(allPlayersReady());
}

//-----------------------------------------------------------------------------

bool MultiPlayer::launchBrowser (const char* link) {

	return(false);
}

//-----------------------------------------------------------------------------

void MultiPlayer::initParametersScreen (void) {

}

//-----------------------------------------------------------------------------

void MultiPlayer::setDefaultPlayerInfo (void) {
	// Fill my own row from prefs.cfg. commanderID must already be assigned.
	if (commanderID < 0 || commanderID >= MAX_MC_PLAYERS)
		return;
	MC2Player& me = playerInfo[commanderID];
	me.player = myPlayer;
	me.commanderID = (char)commanderID;
	strncpy(me.name, prefs.playerName[0], MAXLEN_PLAYER_NAME - 1);
	me.name[MAXLEN_PLAYER_NAME - 1] = 0;
	strncpy(me.unitName, prefs.unitName[0], MAXLEN_UNIT_NAME - 1);
	me.unitName[MAXLEN_UNIT_NAME - 1] = 0;
	strncpy(me.insigniaFile, prefs.insigniaFile, MAXLEN_INSIGNIA_FILE - 1);
	me.insigniaFile[MAXLEN_INSIGNIA_FILE - 1] = 0;
	me.team = (char)commanderID;		// everyone on their own team until changed in the lobby
	me.teamSelected = false;
	me.faction = 0;
	me.cBills = missionSettings.defaultCBills;
	me.resourcePoints = missionSettings.resourcePoints;
	me.ready = false;
	me.checkedIn = false;
	me.leftSession = false;
	me.booted = false;
	// ponytail: colour table (MPlayer->colors) is only populated by the prefs screen;
	// pick the first free index and let the prefs screen refine it.
	long idx = setNextFreeColor(commanderID);
	me.baseColor[BASECOLOR_PREFERENCE] = (char)idx;
	me.baseColor[BASECOLOR_SELF] = (char)idx;
	me.baseColor[BASECOLOR_TEAM] = (char)idx;
	me.stripeColor = (char)idx;
	playerList[commanderID].player = myPlayer;
	playerList[commanderID].commanderID = (char)commanderID;
	strncpy(playerList[commanderID].name, me.name, MAXLEN_PLAYER_NAME - 1);
}

//-----------------------------------------------------------------------------

long MultiPlayer::setClosestColor (long colorIndex, long commanderID) {
	for (long j = 0; j < MAX_MC_PLAYERS; j++)
		if (j != commanderID && playerInfo[j].player && playerInfo[j].baseColor[BASECOLOR_TEAM] == colorIndex)
			return(setNextFreeColor(commanderID));
	return(colorIndex);
}

//-----------------------------------------------------------------------------

long MultiPlayer::setNextFreeColor (long commanderID) {
	for (long i = 0; i < MAX_COLORS; i++) {
		bool taken = false;
		for (long j = 0; j < MAX_MC_PLAYERS; j++)
			if (j != commanderID && playerInfo[j].player && playerInfo[j].baseColor[BASECOLOR_TEAM] == i)
				taken = true;
		if (!taken)
			return(i);
	}
	return(0);
}

//-----------------------------------------------------------------------------

void MultiPlayer::setPlayerBaseColor (long commanderID, long colorIndex) {
	if (commanderID < 0 || commanderID >= MAX_MC_PLAYERS)
		return;
	MC2Player& row = playerInfo[commanderID];
	row.baseColor[BASECOLOR_PREFERENCE] = (char)colorIndex;
	row.baseColor[BASECOLOR_SELF] = (char)colorIndex;
	row.baseColor[BASECOLOR_TEAM] = (char)setClosestColor(colorIndex, commanderID);
	if (iAmHost)
		sendPlayerUpdate(NULL, 6, commanderID);
}

//-----------------------------------------------------------------------------

void MultiPlayer::setPlayerTeam (long commanderID, long teamID) {
	if (commanderID < 0 || commanderID >= MAX_MC_PLAYERS)
		return;
	playerInfo[commanderID].team = (char)teamID;
	playerInfo[commanderID].teamSelected = true;
	if (iAmHost)
		sendPlayerUpdate(NULL, 5, commanderID);
}

//-----------------------------------------------------------------------------

void MultiPlayer::setColor (long colorIndex, long commanderID) {

}

//-----------------------------------------------------------------------------

MC2Player GetPlayersList[MAX_MC_PLAYERS];

const MC2Player* MultiPlayer::getPlayers (long& playerCount) {
	playerCount = 0;
	for (long i = 0; i < MAX_MC_PLAYERS; i++)
		if (playerInfo[i].player && !playerInfo[i].leftSession)
			GetPlayersList[playerCount++] = playerInfo[i];
	return(GetPlayersList);
}

//-----------------------------------------------------------------------------

void MultiPlayer::logMessage (NETMESSAGE* message, bool sent) {

}

//-----------------------------------------------------------------------------

void MultiPlayer::sendMessage (NETPLAYER player,
							   void* data,
							   int dataSize,
							   bool guaranteed,
							   bool toSelf) {

	if (!data || dataSize <= 0)
		return;

	// NULL / myPlayer means "everyone": host broadcasts, a client talks to the server.
	void* target = (player && player != myPlayer) ? (void*)player
	             : (s_transport.isHost() ? (void*)NULL : s_transport.getServerPeer());
	if (s_transport.isHost() || target)
		s_transport.send(target, data, dataSize, guaranteed);

	if (toSelf)
		mpOnRecv(this, myPlayer, data, dataSize, guaranteed);
}

//-----------------------------------------------------------------------------

bool MultiPlayer::hostGame (char* sessionName, char* playerName, long nPlayers) {

	return(true);
}

//---------------------------------------------------------------------------

long MultiPlayer::joinGame (char* ipAddress, char* sessionName, char* playerName) {

	return(MPLAYER_NO_ERR);
}

//-----------------------------------------------------------------------------

void MultiPlayer::addToLocalMovers (MoverPtr mover) {
	if (!mover || numLocalMovers >= MAX_LOCAL_MOVERS)
		return;
	localMovers[numLocalMovers++] = mover;
}

//---------------------------------------------------------------------------

void MultiPlayer::removeFromLocalMovers (MoverPtr mover) {
	for (long i = 0; i < numLocalMovers; i++)
		if (localMovers[i] == mover) {
			for (long j = i; j < numLocalMovers - 1; j++)
				localMovers[j] = localMovers[j + 1];
			localMovers[--numLocalMovers] = NULL;
			return;
		}
}

//---------------------------------------------------------------------------

void MultiPlayer::addToMoverRoster (MoverPtr mover) {

	for (long i = 0; i < MAX_MULTIPLAYER_MOVERS; i++)
		if (!moverRoster[i]) {
			moverRoster[i] = mover;
			mover->setNetRosterIndex(i);
			numMovers++;
			return;
		}
	STOP(("MultiPlayer.addToMoverRoster: too many movers"));
}

//---------------------------------------------------------------------------

void MultiPlayer::removeFromMoverRoster (MoverPtr mover) {
	if (!mover)
		return;
	long i = mover->getNetRosterIndex();
	if (i >= 0 && i < MAX_MULTIPLAYER_MOVERS && moverRoster[i] == mover) {
		moverRoster[i] = NULL;
		numMovers--;
	}
}

//---------------------------------------------------------------------------

void MultiPlayer::addToPlayerMoverRoster (long playerCommanderID, MoverPtr mover) {
	if (playerCommanderID < 0 || playerCommanderID >= MAX_MC_PLAYERS || !mover)
		return;
	for (long i = 0; i < MAX_LOCAL_MOVERS; i++)
		if (!playerMoverRoster[playerCommanderID][i]) {
			playerMoverRoster[playerCommanderID][i] = mover;
			return;
		}
}

//---------------------------------------------------------------------------

void MultiPlayer::removeFromPlayerMoverRoster (MoverPtr mover) {
	for (long c = 0; c < MAX_MC_PLAYERS; c++)
		for (long i = 0; i < MAX_LOCAL_MOVERS; i++)
			if (playerMoverRoster[c][i] == mover)
				playerMoverRoster[c][i] = NULL;
}

//---------------------------------------------------------------------------

void MultiPlayer::addToTurretRoster (TurretPtr turret) {
	if (!turret || numTurrets >= MAX_MULTIPLAYER_TURRETS)
		return;
	turretRoster[numTurrets++] = turret;
}

//---------------------------------------------------------------------------

void MultiPlayer::initSpecialBuildings (char commandersToLoad[8][3]) {

}

//***************************************************************************
// WORLD CHUNK maintenance functions
//***************************************************************************

long MultiPlayer::addWorldChunk (WorldChunkPtr chunk) {

	return(0);
}

//---------------------------------------------------------------------------

long MultiPlayer::addMissionScriptMessageChunk (long code, long param) {

	return(0);
}

//---------------------------------------------------------------------------

long MultiPlayer::addArtilleryChunk (long commanderId, long artilleryType, Stuff::Vector3D location, long seconds) 
{
	return(0);
}

//---------------------------------------------------------------------------

long MultiPlayer::addMineChunk (long tileR, long tileC, long teamId, long mineState, long explosionType) {

	return(0);
}

//---------------------------------------------------------------------------

long MultiPlayer::addLightOnFireChunk (GameObjectPtr object, long seconds) {

	return(0);
}

//---------------------------------------------------------------------------

long MultiPlayer::addPilotKillStat (MoverPtr mover, long vehicleClass) {

	return(0);
}

//---------------------------------------------------------------------------

long MultiPlayer::addScoreChunk (long commanderID, long score) {

	return(0);
}

//---------------------------------------------------------------------------

long MultiPlayer::addKillLossChunk (long killerCID, long loserCID) {

	return(0);
}

//---------------------------------------------------------------------------

long MultiPlayer::addEndMissionChunk (void) {

	return(0);
}

//---------------------------------------------------------------------------

long MultiPlayer::addCaptureBuildingChunk (BuildingPtr building, long prevCommanderID, long newCommanderID) {

	return(0);
}

//---------------------------------------------------------------------------

long MultiPlayer::grabWorldChunks (unsigned long* packedChunkBuffer) {

	return(0);
}

//---------------------------------------------------------------------------
// WEAPON HIT CHUNK maintenance functions
//---------------------------------------------------------------------------

long MultiPlayer::addWeaponHitChunk (WeaponHitChunkPtr chunk) {
	if (!iAmHost || !chunk)
		return(0);
	if (numWeaponHitChunks >= MAX_WEAPONHIT_CHUNKS)
		return(0);	// drop rather than overflow; the hit still happened on the host
	weaponHitChunks[numWeaponHitChunks++] = chunk->data;
	return(numWeaponHitChunks);
}

//---------------------------------------------------------------------------

long MultiPlayer::addWeaponHitChunk (GameObjectPtr target, WeaponShotInfoPtr shotInfo, bool isRefit) {
	if (!iAmHost || !target || !shotInfo)
		return(0);
	WeaponHitChunk chunk;
	chunk.init();
	chunk.build(target, shotInfo, isRefit);
	chunk.pack();
	return(addWeaponHitChunk(&chunk));
}

//---------------------------------------------------------------------------

void MultiPlayer::grabWeaponHitChunks (unsigned long* packedChunkBuffer, long numChunks) {
	long n = (numChunks < numWeaponHitChunks) ? numChunks : numWeaponHitChunks;
	for (long i = 0; i < n; i++)
		packedChunkBuffer[i] = weaponHitChunks[i];
	memmove(weaponHitChunks, weaponHitChunks + n, sizeof(unsigned long) * (numWeaponHitChunks - n));
	numWeaponHitChunks -= n;
}

//---------------------------------------------------------------------------
// MESSAGE SENDERS
//---------------------------------------------------------------------------

void MultiPlayer::sendPlayerInfo (NETPLAYER receiver) {
	if (!iAmHost)
		return;
	for (long i = 0; i < MAX_MC_PLAYERS; i++)
		if (playerInfo[i].player)
			sendPlayerUpdate(receiver, MP_PU_HELLO, i);
}

//---------------------------------------------------------------------------

void MultiPlayer::sendPlayerCID (NETPLAYER receiver, unsigned char subType, char CID) {

	MCMSG_PlayerCID msg;
	msg.init();
	msg.subType = subType;
	msg.commanderID = CID;
	sendMessage(receiver, &msg, sizeof(msg), true /*GUARANTEED*/, false);
}

//---------------------------------------------------------------------------

void MultiPlayer::sendPlayerUpdate (NETPLAYER receiver, long stage, long newCommanderID) {
	if (newCommanderID < 0)
		newCommanderID = commanderID;
	if (newCommanderID < 0 || newCommanderID >= MAX_MC_PLAYERS || !inSession)
		return;
	MCMSG_PlayerUpdate msg;
	msg.init();
	msg.stage = (char)stage;
	msg.senderTime = 0.0f;
	strncpy(msg.sessionIPAddress, sessionIPAddress, sizeof(msg.sessionIPAddress) - 1);
	msg.sessionIPAddress[sizeof(msg.sessionIPAddress) - 1] = 0;
	strncpy(msg.versionStamp, MC2_BUILD_GIT_SHA, sizeof(msg.versionStamp) - 1);
	msg.versionStamp[sizeof(msg.versionStamp) - 1] = 0;
	msg.info = playerInfo[newCommanderID];
	sendMessage(receiver, &msg, sizeof(msg), true /*GUARANTEED*/, false);
}

//---------------------------------------------------------------------------

void MultiPlayer::sendMissionSettingsUpdate (NETPLAYER receiver) {
	if (!iAmHost || !inSession)
		return;
	MCMSG_MissionSettingsUpdate msg;
	msg.init();
	msg.missionSettings = missionSettings;
	sendMessage(receiver, &msg, sizeof(msg), true /*GUARANTEED*/, false);
	if (!receiver) {
		s_lastSentSettings = missionSettings;
		s_lastSentValid = true;
	}
}

//---------------------------------------------------------------------------

void MultiPlayer::sendChat (NETPLAYER receiver, char team, char* chatMessage) {

	if (!chatMessage)
		return;
	// MCMSG_Chat ends in a flexible array (char string[]); sizeof() is the header size.
	unsigned char buf[sizeof(MCMSG_Chat) + 256];
	MCMSG_Chat* msg = (MCMSG_Chat*)buf;
	msg->init();
	msg->allPlayers = (receiver == NULL);
	msg->senderCID = (char)commanderID;
	strncpy(msg->string, chatMessage, MAX_CHAT_LENGTH - 1);
	msg->string[MAX_CHAT_LENGTH - 1] = 0;
	int size = (int)(sizeof(MCMSG_Chat) + strlen(msg->string) + 1);
	sendMessage(receiver, msg, size, true /*GUARANTEED*/, true /*toSelf: show my own line*/);
	(void)team;	// ponytail: team-only chat routes to everyone for now
}

void MultiPlayer::sendPlayerActionChat(NETPLAYER receiver, const char* playerName, unsigned long resID )
{
}


//---------------------------------------------------------------------------

void MultiPlayer::sendPlayerCheckIn (void) {
	if (commanderID < 0 || !inSession)
		return;
	MCMSG_PlayerCheckIn msg;
	msg.init();
	msg.commanderID = (char)commanderID;
	sendMessage(NULL, &msg, sizeof(msg), true /*GUARANTEED*/, false);
}

//---------------------------------------------------------------------------

void MultiPlayer::sendPlayerSetup (void) {

}

//---------------------------------------------------------------------------

void MultiPlayer::sendPlayerInsignia (char* insigniaFileName, unsigned char* insigniaData, long dataSize) {

}

//---------------------------------------------------------------------------

void MultiPlayer::sendMissionSetup (NETPLAYER receiver, long subType, CompressedMech* mechData) {
	if (!inSession)
		return;
	MCMSG_MissionSetup msg;
	msg.init();
	msg.subType = (char)subType;
	if (iAmHost && subType == MP_SETUP_ZONES) {
		if (!randomSeed)
			randomSeed = (long)(time(NULL) ^ (getpid() << 8));
		startLoading = true;
	}
	msg.randomSeed = randomSeed;
	memcpy(msg.commandersToLoad, commandersToLoad, sizeof(commandersToLoad));
	if (mechData)
		msg.mechData = *mechData;
	else
		memset(&msg.mechData, 0, sizeof(msg.mechData));
	sendMessage(receiver, &msg, sizeof(msg), true /*GUARANTEED*/, false);
	if (getenv("MC2_LOG") && subType != MP_SETUP_MECHDATA)
		printf("[MP] mission setup sent subType=%ld seed=0x%08lx\n", subType, (unsigned long)randomSeed);
}

//---------------------------------------------------------------------------

void MultiPlayer::setServer (NETPLAYER player, char playerIPAddress[16]) {

}

//---------------------------------------------------------------------------

void MultiPlayer::sendStartMission (void) {
	if (!iAmHost || !inSession)
		return;
	MCMSG_StartMission msg;
	msg.init();
	msg.huh = 0;
	sendMessage(NULL, &msg, sizeof(msg), true /*GUARANTEED*/, false);
	startMission = true;
	if (getenv("MC2_LOG")) printf("[MP] start mission sent\n");
}

//---------------------------------------------------------------------------

void MultiPlayer::sendEndMission (long result) {
	if (!iAmHost || !inSession)
		return;
	MCMSG_EndMission msg;
	msg.init();
	for (long i = 0; i < MAX_MC_PLAYERS; i++) {
		msg.teamScore[i] = (i < MAX_TEAMS) ? teamScore[i] : 0;
		msg.playerScore[i] = playerInfo[i].score;
	}
	msg.result = (int)result;
	sendMessage(NULL, &msg, sizeof(msg), true /*GUARANTEED*/, false);
	if (getenv("MC2_LOG")) printf("[MP] end mission sent winningTeam=%ld\n", winningTeam);
}

//---------------------------------------------------------------------------
extern MoverPtr BringInReinforcement (long vehicleID, long rosterIndex, long commanderID, Stuff::Vector3D pos, bool exists);

void MultiPlayer::sendReinforcement (long vehicleID, long rosterIndex, const char pilotName[16], long commanderID, Stuff::Vector3D pos, unsigned char stage) {

}

//-----------------------------------------------------------------------------

void MultiPlayer::sendNewServer (void) {

}

//-----------------------------------------------------------------------------

void MultiPlayer::sendLeaveSession (char subType, char commanderID) {
	if (!inSession)
		return;
	MCMSG_LeaveSession msg;
	msg.init();
	msg.subType = (unsigned char)subType;
	msg.commanderID = commanderID;
	sendMessage(NULL, &msg, sizeof(msg), true /*GUARANTEED*/, false);
}

//-----------------------------------------------------------------------------

void MultiPlayer::sendPlayerOrder (TacticalOrderPtr tacOrder,
								   bool needsSelection,
								   long numMovers,
								   MoverPtr* moverList,
								   long numGroups,
								   MoverGroupPtr* groupList,
   								   bool queuedOrder) {
	if (!tacOrder || !inSession || numMovers <= 0 || !moverList)
		return;
	(void)numGroups; (void)groupList;	// ponytail: group orders arrive expanded in moverList by every current caller
	if (iAmHost) {
		for (long i = 0; i < numMovers; i++)
			if (moverList[i])
				moverList[i]->handleTacticalOrder(*tacOrder, 1, queuedOrder);
		return;
	}
	MCMSG_PlayerOrder msg;
	msg.init();
	msg.commanderID = (char)commanderID;
	msg.flags = (queuedOrder ? MP_ORDER_QUEUED : 0) | (needsSelection ? MP_ORDER_NEEDS_SELECTION : 0);
	Stuff::Vector3D wp = tacOrder->getWayPoint(0);
	msg.location[0] = wp.x;
	msg.location[1] = wp.y;
	msg.tacOrderChunk[0] = tacOrder->data[0];
	msg.tacOrderChunk[1] = tacOrder->data[1];
	for (long i = 0; i < numMovers && msg.numMovers < MAX_LOCAL_MOVERS; i++) {
		if (!moverList[i]) continue;
		long idx = moverList[i]->getNetRosterIndex();
		if (idx < 0 || idx >= MAX_MULTIPLAYER_MOVERS) continue;
		msg.moverIndex[msg.numMovers++] = (unsigned char)idx;
	}
	if (!msg.numMovers)
		return;
	sendMessage(NULL, &msg, sizeof(msg), true /*GUARANTEED*/, false);
	if (getenv("MC2_LOG"))
		printf("[MP] order sent code=%d movers=%d\n", (int)tacOrder->code, (int)msg.numMovers);
}

//---------------------------------------------------------------------------

void MultiPlayer::sendHoldPosition (void) {

}

//---------------------------------------------------------------------------

void MultiPlayer::sendPlayerMoverGroup (long groupId,
										long numMovers,
										MoverPtr* moverList,
										long point) {

}

//---------------------------------------------------------------------------

void MultiPlayer::sendPlayerArtillery (long strikeType, Stuff::Vector3D location, long seconds) {

}


//---------------------------------------------------------------------------

void MultiPlayer::sendMoverUpdate (void) {
	if (!iAmHost || !inSession || mode != MULTIPLAYER_MODE_MISSION)
		return;
	unsigned char buf[sizeof(MCMSG_MoverUpdate) + MAX_MULTIPLAYER_MOVERS * sizeof(MpMoverEntry)];
	MCMSG_MoverUpdate* msg = (MCMSG_MoverUpdate*)buf;
	msg->init();
	msg->updateId = ++s_moverUpdateId;
	const bool keyframe = (s_moverUpdateId % kMpMoverKeyframeEvery) == 0;
	for (long i = 0; i < MAX_MC_PLAYERS; i++) {
		msg->teamScore[i] = (i < MAX_TEAMS) ? teamScore[i] : 0;
		msg->playerScore[i] = playerInfo[i].score;
	}
	MpMoverEntry* e = (MpMoverEntry*)msg->moveData;
	long n = 0;
	for (long i = 0; i < MAX_MULTIPLAYER_MOVERS; i++) {
		MoverPtr m = moverRoster[i];
		if (!m) continue;
		m->buildStatusChunk();
		m->buildMoveChunk();
		unsigned long st = m->statusChunk.data, mv = m->getMoveChunk()->data;
		unsigned char flags = 0;
		if (keyframe || !s_lastSentValidChunk[i] || st != s_lastSentStatus[i]) flags |= MP_ENTRY_STATUS;
		if (keyframe || !s_lastSentValidChunk[i] || mv != s_lastSentMove[i])   flags |= MP_ENTRY_MOVE;
		s_lastSentStatus[i] = st; s_lastSentMove[i] = mv; s_lastSentValidChunk[i] = true;
		if (!flags) continue;
		e[n].idx = (unsigned char)i;
		e[n].flags = flags;
		e[n].status = st;
		e[n].move = mv;
		n++;
	}
	if (!n && !keyframe)
		return;
	msg->numRLEs = (unsigned char)n;
	sendMessage(NULL, msg, (int)(sizeof(MCMSG_MoverUpdate) + n * sizeof(MpMoverEntry)), false /*unreliable*/, false);
}

//---------------------------------------------------------------------------

void MultiPlayer::sendTurretUpdate (void) {

}

//---------------------------------------------------------------------------

void MultiPlayer::sendMoverWeaponFireUpdate (void) {
	if (!iAmHost || !inSession || mode != MULTIPLAYER_MODE_MISSION)
		return;
	unsigned char buf[sizeof(MCMSG_MoverWeaponFireUpdate) + MAX_MULTIPLAYER_MOVERS * (2 + MAX_WEAPONFIRE_CHUNKS * sizeof(unsigned long))];
	MCMSG_MoverWeaponFireUpdate* msg = (MCMSG_MoverWeaponFireUpdate*)buf;
	msg->init();
	unsigned char* p = msg->weaponFireData;
	long count = 0, total = 0;
	for (long i = 0; i < MAX_MULTIPLAYER_MOVERS; i++) {
		MoverPtr m = moverRoster[i];
		if (!m || m->getNumWeaponFireChunks(CHUNK_SEND) <= 0) continue;
		unsigned long chunks[MAX_WEAPONFIRE_CHUNKS];
		long n = m->grabWeaponFireChunks(CHUNK_SEND, chunks, MAX_WEAPONFIRE_CHUNKS);
		if (n <= 0) continue;
		*p++ = (unsigned char)i;
		*p++ = (unsigned char)n;
		memcpy(p, chunks, n * sizeof(unsigned long));
		p += n * sizeof(unsigned long);
		count++; total += n;
	}
	if (!count)
		return;
	msg->numRLEs = (unsigned char)count;
	sendMessage(NULL, msg, (int)(p - buf), true /*GUARANTEED*/, false);
	s_fireChunksSent += total;
	if (getenv("MC2_LOG")) printf("[MP] fire chunks sent=%ld movers=%ld total=%ld\n", total, count, s_fireChunksSent);
}

//---------------------------------------------------------------------------

void MultiPlayer::sendTurretWeaponFireUpdate (void) {

}

//---------------------------------------------------------------------------

void MultiPlayer::sendMoverCriticalUpdate (void) {
	if (!iAmHost || !inSession || mode != MULTIPLAYER_MODE_MISSION)
		return;
	unsigned char buf[sizeof(MCMSG_MoverCriticalUpdate) + MAX_MULTIPLAYER_MOVERS * (MAX_CRITICALHIT_CHUNKS + MAX_RADIO_CHUNKS)];
	MCMSG_MoverCriticalUpdate* msg = (MCMSG_MoverCriticalUpdate*)buf;
	msg->init();
	unsigned char* p = msg->chunk;
	long total = 0;
	for (long i = 0; i < MAX_MULTIPLAYER_MOVERS; i++) {
		MoverPtr m = moverRoster[i];
		if (!m) continue;
		long nc = m->getNumCriticalHitChunks(CHUNK_SEND);
		if (nc > 0) { m->grabCriticalHitChunks(CHUNK_SEND, p); m->clearCriticalHitChunks(CHUNK_SEND); p += nc; }
		long nr = m->getNumRadioChunks(CHUNK_SEND);
		if (nr > 0) { m->grabRadioChunks(CHUNK_SEND, p); m->clearRadioChunks(CHUNK_SEND); p += nr; }
		msg->numCritHitChunks[i] = (unsigned char)nc;
		msg->numRadioChunks[i] = (unsigned char)nr;
		total += nc + nr;
	}
	if (!total)
		return;
	sendMessage(NULL, msg, (int)(p - buf), true /*GUARANTEED*/, false);
}

//---------------------------------------------------------------------------

void MultiPlayer::sendWeaponHitUpdate (void) {
	if (!iAmHost || !inSession)
		return;
	while (numWeaponHitChunks > 0) {
		unsigned char buf[sizeof(MCMSG_WeaponHitUpdate) + 255 * sizeof(unsigned long)];
		MCMSG_WeaponHitUpdate* msg = (MCMSG_WeaponHitUpdate*)buf;
		msg->init();
		long n = (numWeaponHitChunks < 255) ? numWeaponHitChunks : 255;
		grabWeaponHitChunks(msg->weaponHitChunk, n);
		msg->numWeaponHits = (unsigned char)n;
		sendMessage(NULL, msg, (int)(sizeof(MCMSG_WeaponHitUpdate) + n * sizeof(unsigned long)), true /*GUARANTEED*/, false);
		s_hitsSent += n;
		if (getenv("MC2_LOG")) printf("[MP] weapon hits sent=%ld total=%ld\n", n, s_hitsSent);
	}
}

//---------------------------------------------------------------------------

void MultiPlayer::sendWorldUpdate (void) {

}

//---------------------------------------------------------------------------
// MESSAGE HANDLERS
//---------------------------------------------------------------------------

void MultiPlayer::handleChat (NETPLAYER sender, MCMSG_Chat* msg) {
	long cid = msg->senderCID;
	if (cid < 0 || cid >= MAX_MC_PLAYERS)
		cid = findPlayer(sender);
	// Host relays a client's line to the other peers (the origin already showed it via toSelf).
	if (iAmHost && sender != myPlayer) {
		int size = (int)(sizeof(MCMSG_Chat) + strlen(msg->string) + 1);
		for (long i = 0; i < MAX_MC_PLAYERS; i++)
			if (playerInfo[i].player && playerInfo[i].player != myPlayer && playerInfo[i].player != sender)
				sendMessage(playerInfo[i].player, msg, size, true /*GUARANTEED*/, false);
	}
	if (chatCount >= MAX_STORED_CHATS) {
		memmove(currentChatMessages[0], currentChatMessages[1], sizeof(currentChatMessages) - sizeof(currentChatMessages[0]));
		memmove(&currentChatMessagePlayerIDs[0], &currentChatMessagePlayerIDs[1], sizeof(long) * (MAX_STORED_CHATS - 1));
		chatCount = MAX_STORED_CHATS - 1;
	}
	strncpy(currentChatMessages[chatCount], msg->string, MAX_CHAT_LENGTH - 1);
	currentChatMessages[chatCount][MAX_CHAT_LENGTH - 1] = 0;
	currentChatMessagePlayerIDs[chatCount] = (cid < 0 ? 0 : cid) | (msg->hideName ? 0x01000000 : 0);
	chatCount++;
	if (getenv("MC2_LOG"))
		printf("[MP] chat from commanderID %ld: %s\n", cid, msg->string);
}

//---------------------------------------------------------------------------

void MultiPlayer::handleMissionSettingsUpdate (NETPLAYER sender, MCMSG_MissionSettingsUpdate* msg) {
	if (iAmHost || sender != serverPlayer)
		return;
	missionSettings = msg->missionSettings;
	locked = missionSettings.locked;
	inProgress = missionSettings.inProgress;
	if (getenv("MC2_LOG"))
		printf("[MP] settings applied map=%s cbills=%ld rp=%ld drop=%ld time=%.0f quick=%d locked=%d\n",
			missionSettings.map, missionSettings.defaultCBills, missionSettings.resourcePoints,
			missionSettings.dropWeight, missionSettings.timeLimit, missionSettings.quickStart ? 1 : 0, missionSettings.locked ? 1 : 0);
}

//---------------------------------------------------------------------------

void MultiPlayer::handlePlayerCID (NETPLAYER sender, MCMSG_PlayerCID* msg) {

	// Only the host hands out IDs, and only a client takes one.
	if (iAmHost || sender != serverPlayer || msg->subType != MP_CID_ASSIGN)
		return;
	if (msg->commanderID < 0 || msg->commanderID >= MAX_MC_PLAYERS)
		return;

	commanderID = msg->commanderID;
	MC2Player& me = playerInfo[commanderID];
	me.player = myPlayer;
	me.commanderID = (char)commanderID;
	me.leftSession = false;
	me.booted = false;
	strncpy(me.name, playerName, MAXLEN_PLAYER_NAME - 1);
	me.name[MAXLEN_PLAYER_NAME - 1] = 0;
	playerList[commanderID].player = myPlayer;
	playerList[commanderID].commanderID = (char)commanderID;
	setDefaultPlayerInfo();
	sendPlayerUpdate(NULL, MP_PU_HELLO, commanderID);	// hello: name, colours, version stamp
	if (getenv("MC2_LOG"))
		printf("[MP] assigned commanderID %ld by host\n", commanderID);
}

//---------------------------------------------------------------------------

void MultiPlayer::handlePlayerUpdate (NETPLAYER sender, MCMSG_PlayerUpdate* msg) {
	long cid = msg->info.commanderID;
	if (cid < 0 || cid >= MAX_MC_PLAYERS)
		return;
	bool sameBuild = (strncmp(msg->versionStamp, MC2_BUILD_GIT_SHA, sizeof(msg->versionStamp) - 1) == 0);

	if (iAmHost) {
		// A client may only describe its own slot.
		if (findPlayer(sender) != cid)
			return;
		MC2Player& row = playerInfo[cid];
		if (!sameBuild) {
			if (getenv("MC2_LOG"))
				printf("[MP] version mismatch from commanderID %ld (%s vs %s), booting\n", cid, msg->versionStamp, MC2_BUILD_GIT_SHA);
			sendPlayerUpdate(sender, MP_PU_HELLO, commanderID);	// let it see our stamp -> VERSION_STATUS_BAD dialog
			bootPlayer(sender);
			return;
		}
		bool firstHello = (msg->stage == MP_PU_HELLO);	// a hello always gets the full lobby state (idempotent)
		bool readyChanged = (row.ready != msg->info.ready);
		// Merge the client-owned fields; the host owns identity, slot and seniority.
		strncpy(row.name, msg->info.name, MAXLEN_PLAYER_NAME - 1);
		row.name[MAXLEN_PLAYER_NAME - 1] = 0;
		strncpy(row.unitName, msg->info.unitName, MAXLEN_UNIT_NAME - 1);
		row.unitName[MAXLEN_UNIT_NAME - 1] = 0;
		strncpy(row.insigniaFile, msg->info.insigniaFile, MAXLEN_INSIGNIA_FILE - 1);
		row.insigniaFile[MAXLEN_INSIGNIA_FILE - 1] = 0;
		memcpy(row.baseColor, msg->info.baseColor, sizeof(row.baseColor));
		row.stripeColor = msg->info.stripeColor;
		row.team = msg->info.team;
		row.teamSelected = msg->info.teamSelected;
		row.faction = msg->info.faction;
		row.cBills = msg->info.cBills;
		row.ready = msg->info.ready;
		row.checkedIn = true;
		strncpy(playerList[cid].name, row.name, MAXLEN_PLAYER_NAME - 1);
		if (readyChanged && getenv("MC2_LOG"))
			printf("[MP] player %ld ready=%d\n", cid, row.ready ? 1 : 0);
		if (firstHello) {
			// New arrival: it needs the settings and every occupied row, others need its row.
			sendMissionSettingsUpdate(sender);
			sendPlayerInfo(sender);
		}
		sendPlayerUpdate(NULL, msg->stage, cid);	// rebroadcast the merged row to everyone
		return;
	}

	// Client: only the host may tell us about rows.
	if (sender != serverPlayer)
		return;
	if (versionStatus == VERSION_STATUS_UNKNOWN) {
		versionStatus = sameBuild ? VERSION_STATUS_GOOD : VERSION_STATUS_BAD;
		if (!sameBuild && getenv("MC2_LOG"))
			printf("[MP] host build %s differs from ours %s\n", msg->versionStamp, MC2_BUILD_GIT_SHA);
	}
	MC2Player& row = playerInfo[cid];
	NETPLAYER keep = (cid == commanderID) ? myPlayer : (cid == 0 ? serverPlayer : remotePlayerHandle(cid));
	row = msg->info;
	row.player = keep;
	row.commanderID = (char)cid;
	playerList[cid].player = keep;
	playerList[cid].commanderID = (char)cid;
	strncpy(playerList[cid].name, row.name, MAXLEN_PLAYER_NAME - 1);
}

//---------------------------------------------------------------------------

void MultiPlayer::handleMissionSetup (NETPLAYER sender, MCMSG_MissionSetup* msg) {
	if (iAmHost) {
		long cid = findPlayer(sender);
		if (cid < 0)
			return;
		switch (msg->subType) {
			case MP_SETUP_MECHDATA: {
				long j = 0;
				while (j < 12 && mechData[cid][j].objNumber > 0) j++;
				if (j < 12) mechData[cid][j] = msg->mechData;
				if (msg->mechData.lastMech) mechDataReceived[cid] = true;
				break;
			}
			case MP_SETUP_LOADED:  missionDataLoaded[cid] = true;  break;
			case MP_SETUP_STARTED: missionFullySetup[cid] = true;  break;
			default: break;
		}
		if (getenv("MC2_LOG"))
			printf("[MP] mission setup from commanderID %ld subType=%d\n", cid, (int)msg->subType);
		return;
	}
	if (sender != serverPlayer)
		return;
	switch (msg->subType) {
		case MP_SETUP_GO_LOBBY:
			startLogistics = true;
			break;
		case MP_SETUP_ZONES:
			memcpy(commandersToLoad, msg->commandersToLoad, sizeof(commandersToLoad));
			randomSeed = msg->randomSeed;
			startLoading = true;
			if (getenv("MC2_LOG"))
				printf("[MP] mission setup: zones=%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld seed=0x%08lx\n",
					(long)commandersToLoad[0][0], (long)commandersToLoad[1][0], (long)commandersToLoad[2][0], (long)commandersToLoad[3][0],
					(long)commandersToLoad[4][0], (long)commandersToLoad[5][0], (long)commandersToLoad[6][0], (long)commandersToLoad[7][0],
					(unsigned long)randomSeed);
			break;
		case MP_SETUP_ALL_STARTED:
			setupMission = true;
			break;
		default:
			break;
	}
}

//---------------------------------------------------------------------------

void MultiPlayer::handlePlayerInfo (NETPLAYER sender, MCMSG_PlayerInfo* msg) {
	// Client asks the host to resend the roster.
	if (iAmHost && findPlayer(sender) == msg->commanderID)
		sendPlayerInfo(sender);
}

//---------------------------------------------------------------------------

void MultiPlayer::handlePlayerCheckIn (NETPLAYER sender, MCMSG_PlayerCheckIn* msg) {
	if (!iAmHost || findPlayer(sender) != msg->commanderID)
		return;
	playerInfo[msg->commanderID].checkedIn = true;
}

//---------------------------------------------------------------------------

void MultiPlayer::handlePlayerSetup (NETPLAYER sender, MCMSG_PlayerSetup* msg) {

}

//---------------------------------------------------------------------------

void MultiPlayer::handlePlayerInsignia (NETPLAYER sender, MCMSG_PlayerInsignia* msg) {

}

//---------------------------------------------------------------------------

void MultiPlayer::handleStartMission (NETPLAYER sender) {
	if (iAmHost || sender != serverPlayer)
		return;
	startMission = true;
	if (getenv("MC2_LOG")) printf("[MP] start mission received\n");
}

//---------------------------------------------------------------------------

void MultiPlayer::handleEndMission (NETPLAYER sender, MCMSG_EndMission* msg) {
	if (iAmHost || sender != serverPlayer)
		return;
	for (long i = 0; i < MAX_MC_PLAYERS; i++) {
		if (i < MAX_TEAMS) teamScore[i] = msg->teamScore[i];
		playerInfo[i].score = msg->playerScore[i];
	}
	winningTeam = msg->result;
	for (long i = 0; i < MAX_MC_PLAYERS; i++)
		if (playerInfo[i].player)
			playerInfo[i].winner = (winningTeam >= 0 && playerInfo[i].team == winningTeam);
	// Mirror the host's verdict on our copy of the rosters so the results screen agrees.
	for (long cid = 0; cid < MAX_MC_PLAYERS; cid++) {
		if (!playerInfo[cid].player) continue;
		long alive = 0, total = 0;
		for (long i = 0; i < MAX_LOCAL_MOVERS; i++) {
			MoverPtr m = playerMoverRoster[cid][i];
			if (!m) continue;
			total++;
			if (!m->isDestroyed() && !m->isDisabled()) alive++;
		}
		allUnitsDestroyed[cid] = (total > 0 && alive == 0);
	}
	endMission = true;
}

//---------------------------------------------------------------------------

void MultiPlayer::handleReinforcement (NETPLAYER sender, MCMSG_Reinforcement* msg) {

}

//-----------------------------------------------------------------------------

void MultiPlayer::handleNewServer (NETPLAYER sender, MCMSG_NewServer* msg) {

}

//-----------------------------------------------------------------------------

void MultiPlayer::handleLeaveSession (NETPLAYER sender, MCMSG_LeaveSession* msg) {
	long cid = msg->commanderID;
	if (cid < 0 || cid >= MAX_MC_PLAYERS)
		return;
	if (iAmHost) {
		if (findPlayer(sender) != cid)
			return;
		bootPlayer(sender);	// mpOnPeer's disconnect path frees the slot and rebroadcasts
		return;
	}
	if (sender != serverPlayer)
		return;
	if (msg->subType == MP_LEAVE_HOST) {
		hostLeft = true;
		if (getenv("MC2_LOG")) printf("[MP] host left the session\n");
		return;
	}
	if (cid == commanderID) {
		playerInfo[cid].leftSession = true;	// the lobby screen reads this as 'I was booted'
		return;
	}
	handlePlayerLeftSession(playerInfo[cid].player);
}

//-----------------------------------------------------------------------------

void MultiPlayer::handleHoldPosition (NETPLAYER sender, MCMSG_HoldPosition* msg) {

}

//-----------------------------------------------------------------------------

void MultiPlayer::handlePlayerOrder (NETPLAYER sender, MCMSG_PlayerOrder* msg) {
	if (!iAmHost)
		return;
	long cid = findPlayer(sender);
	if (cid < 0 || cid != msg->commanderID)
		return;
	if (mode != MULTIPLAYER_MODE_MISSION || !mission || !ObjectManager)
		return;	// late packet after Mission::destroy (resetForNewGame drops the rosters)
	TacticalOrder tacOrder;
	tacOrder.data[0] = msg->tacOrderChunk[0];
	tacOrder.data[1] = msg->tacOrderChunk[1];
	tacOrder.unpack();
	long applied = 0;
	for (long i = 0; i < msg->numMovers && i < MAX_LOCAL_MOVERS; i++) {
		long idx = msg->moverIndex[i];
		if (idx >= MAX_MULTIPLAYER_MOVERS) continue;
		MoverPtr mover = moverRoster[idx];
		if (!mover || mover->getCommanderId() != cid || mover->isDestroyed())
			continue;	// not theirs (or gone): drop silently
		mover->handleTacticalOrder(tacOrder, 1, (msg->flags & MP_ORDER_QUEUED) != 0);
		applied++;
	}
	if (getenv("MC2_LOG"))
		printf("[MP] order from commanderID %ld: code=%d movers=%d applied=%ld\n", cid, (int)tacOrder.code, (int)msg->numMovers, applied);
}

//---------------------------------------------------------------------------

void MultiPlayer::handlePlayerMoverGroup (NETPLAYER sender, MCMSG_PlayerMoverGroup* msg) {

}

//---------------------------------------------------------------------------

void MultiPlayer::handlePlayerArtillery (NETPLAYER sender, MCMSG_PlayerArtillery* msg) {

}

//---------------------------------------------------------------------------

void MultiPlayer::handleMoverUpdate (NETPLAYER sender, MCMSG_MoverUpdate* msg) {
	if (iAmHost || sender != serverPlayer)
		return;
	// Drop stale/duplicate updates (unreliable channel); ids wrap at 16 bits.
	if (mode != MULTIPLAYER_MODE_MISSION || !mission || !ObjectManager)
		return;	// late packet after Mission::destroy (resetForNewGame drops the rosters)
	if (s_haveMoverUpdate && (short)(msg->updateId - s_lastMoverUpdateId) <= 0)
		return;
	long age = s_haveMoverUpdate ? (short)(msg->updateId - s_lastMoverUpdateId) : 1;
	s_lastMoverUpdateId = msg->updateId;
	s_haveMoverUpdate = true;
	for (long i = 0; i < MAX_MC_PLAYERS; i++) {
		if (i < MAX_TEAMS) teamScore[i] = msg->teamScore[i];
		playerInfo[i].score = msg->playerScore[i];
	}
	const MpMoverEntry* e = (const MpMoverEntry*)msg->moveData;
	for (long k = 0; k < msg->numRLEs; k++) {
		long idx = e[k].idx;
		if (idx >= MAX_MULTIPLAYER_MOVERS) continue;
		MoverPtr m = moverRoster[idx];
		if (!m) continue;
		if (e[k].flags & MP_ENTRY_STATUS)
			m->handleStatusChunk(age, e[k].status);
		// Re-applying an identical move chunk rebuilds the path and re-targets the mech
		// (visible jitter), so keyframes only matter when the chunk actually differs.
		if ((e[k].flags & MP_ENTRY_MOVE) && e[k].move != m->getMoveChunk()->data)
			m->handleMoveChunk(e[k].move);
	}
}

//---------------------------------------------------------------------------

void MultiPlayer::handleTurretUpdate (NETPLAYER sender, MCMSG_TurretUpdate* msg) {

}

//---------------------------------------------------------------------------

void MultiPlayer::handleMoverWeaponFireUpdate (NETPLAYER sender, MCMSG_MoverWeaponFireUpdate* msg) {
	if (iAmHost || sender != serverPlayer)
		return;
	const unsigned char* p = msg->weaponFireData;
	if (mode != MULTIPLAYER_MODE_MISSION || !mission || !ObjectManager)
		return;	// late packet after Mission::destroy (resetForNewGame drops the rosters)
	for (long k = 0; k < msg->numRLEs; k++) {
		long idx = *p++;
		long n = *p++;
		unsigned long chunks[MAX_WEAPONFIRE_CHUNKS];
		if (n > MAX_WEAPONFIRE_CHUNKS) return;	// malformed
		memcpy(chunks, p, n * sizeof(unsigned long));
		p += n * sizeof(unsigned long);
		MoverPtr m = (idx < MAX_MULTIPLAYER_MOVERS) ? moverRoster[idx] : NULL;
		if (!m) continue;
		if (m->getNumWeaponFireChunks(CHUNK_RECEIVE) + n >= MAX_WEAPONFIRE_CHUNKS)
			m->clearWeaponFireChunks(CHUNK_RECEIVE);	// never trip the Fatal in addWeaponFireChunks
		m->addWeaponFireChunks(CHUNK_RECEIVE, chunks, n);
		s_fireChunksReceived += n;
	}
}

//---------------------------------------------------------------------------

void MultiPlayer::handleTurretWeaponFireUpdate (NETPLAYER sender, MCMSG_TurretWeaponFireUpdate* msg) {

}

//---------------------------------------------------------------------------

void MultiPlayer::handleMoverCriticalUpdate (NETPLAYER sender, MCMSG_MoverCriticalUpdate* msg) {
	if (iAmHost || sender != serverPlayer)
		return;
	unsigned char* p = msg->chunk;
	if (mode != MULTIPLAYER_MODE_MISSION || !mission || !ObjectManager)
		return;	// late packet after Mission::destroy (resetForNewGame drops the rosters)
	for (long i = 0; i < MAX_MULTIPLAYER_MOVERS; i++) {
		long nc = msg->numCritHitChunks[i], nr = msg->numRadioChunks[i];
		MoverPtr m = moverRoster[i];
		if (m) {
			if (nc > 0 && m->getNumCriticalHitChunks(CHUNK_RECEIVE) + nc < MAX_CRITICALHIT_CHUNKS)
				m->addCriticalHitChunks(CHUNK_RECEIVE, p, nc);
			if (nr > 0)
				m->addRadioChunks(CHUNK_RECEIVE, p + nc, nr);
		}
		p += nc + nr;
	}
}

//---------------------------------------------------------------------------

extern bool FromMP;

void MultiPlayer::handleWeaponHitUpdate (NETPLAYER sender, MCMSG_WeaponHitUpdate* msg) {
	if (iAmHost || sender != serverPlayer)
		return;
	long applied = 0;
	if (mode != MULTIPLAYER_MODE_MISSION || !mission || !ObjectManager)
		return;	// late packet after Mission::destroy (resetForNewGame drops the rosters)
	for (long i = 0; i < msg->numWeaponHits; i++) {
		WeaponHitChunk chunk;
		chunk.init();
		chunk.data = msg->weaponHitChunk[i];
		chunk.unpack();
		if (!chunk.valid(1))
			continue;
		if (chunk.refit)
			continue;	// ponytail: repair-truck refits not relayed yet (Elimination v1)
		GameObjectPtr target = NULL;
		switch (chunk.targetType) {
			case WEAPONHITCHUNK_TARGET_MOVER:
				target = (chunk.targetId >= 0 && chunk.targetId < MAX_MULTIPLAYER_MOVERS) ? (GameObjectPtr)moverRoster[chunk.targetId] : NULL;
				break;
			case WEAPONHITCHUNK_TARGET_TERRAIN:
			case WEAPONHITCHUNK_TARGET_SPECIAL:
				target = ObjectManager->findByPartId(chunk.targetId);
				break;
			default:
				break;
		}
		if (!target)
			continue;
		WeaponShotInfo shotInfo;
		shotInfo.init(0, chunk.cause, chunk.damage, chunk.hitLocation, kMpEntryQuad[chunk.entryAngle & 3]);
		target->handleWeaponHit(&shotInfo, false);
		applied++;
	}
	s_hitsApplied += applied;
	if (getenv("MC2_LOG")) printf("[MP] weapon hits applied=%ld total=%ld\n", applied, s_hitsApplied);
}

//---------------------------------------------------------------------------

void MultiPlayer::handleWorldUpdate (NETPLAYER sender, MCMSG_WorldUpdate* msg) {

}

//---------------------------------------------------------------------------

void MultiPlayer::handlePlayerLeftSession (NETPLAYER leftPlayer) {
	long cid = findPlayer(leftPlayer);
	if (cid < 0)
		return;
	memset(&playerInfo[cid], 0, sizeof(MC2Player));
	playerInfo[cid].commanderID = -1;
	playerList[cid].player = NULL;
	playerList[cid].commanderID = -1;
	if (getenv("MC2_LOG")) printf("[MP] commanderID %ld left\n", cid);
}

//---------------------------------------------------------------------------

void MultiPlayer::handleTerminateSession (void) {

}

//---------------------------------------------------------------------------

bool MultiPlayer::isServerMissing (void) {

	return(false);
}

//---------------------------------------------------------------------------

bool MultiPlayer::processGameMessage (NETMESSAGE* msg) {

	return(true);
}

//---------------------------------------------------------------------------

void MultiPlayer::processMessages (void) {

	s_transport.poll(this, mpOnRecv, mpOnPeer, 0);

	for (size_t i = 0; i < s_inbound.size(); ++i) {
		InboundMsg& m = s_inbound[i];
		if (m.bytes.empty())
			continue;
		unsigned char* raw = &m.bytes[0];
		switch (raw[0]) {
			// MP-ENET-1 proof-of-life: chat is the one message wired end to end.
			// Every other MCMSG_* type still needs its send/handle body written.
			case MCMSG_CHAT:
				handleChat(m.sender, (MCMSG_Chat*)raw);
				break;
			case MCMSG_PLAYER_CID:
				if (m.bytes.size() >= sizeof(MCMSG_PlayerCID))
					handlePlayerCID(m.sender, (MCMSG_PlayerCID*)raw);
				break;
			case MCMSG_PLAYER_UPDATE:
				if (m.bytes.size() >= sizeof(MCMSG_PlayerUpdate))
					handlePlayerUpdate(m.sender, (MCMSG_PlayerUpdate*)raw);
				break;
			case MCMSG_MISSION_SETTINGS_UPDATE:
				if (m.bytes.size() >= sizeof(MCMSG_MissionSettingsUpdate))
					handleMissionSettingsUpdate(m.sender, (MCMSG_MissionSettingsUpdate*)raw);
				break;
			case MCMSG_PLAYER_INFO:
				if (m.bytes.size() >= sizeof(MCMSG_PlayerInfo))
					handlePlayerInfo(m.sender, (MCMSG_PlayerInfo*)raw);
				break;
			case MCMSG_PLAYER_CHECK_IN:
				if (m.bytes.size() >= sizeof(MCMSG_PlayerCheckIn))
					handlePlayerCheckIn(m.sender, (MCMSG_PlayerCheckIn*)raw);
				break;
			case MCMSG_MISSION_SETUP:
				if (m.bytes.size() >= sizeof(MCMSG_MissionSetup))
					handleMissionSetup(m.sender, (MCMSG_MissionSetup*)raw);
				break;
			case MCMSG_START_MISSION:
				if (m.bytes.size() >= sizeof(MCMSG_StartMission))
					handleStartMission(m.sender);
				break;
			case MCMSG_MOVER_UPDATE:
				if (m.bytes.size() >= sizeof(MCMSG_MoverUpdate)) {
					MCMSG_MoverUpdate* mu = (MCMSG_MoverUpdate*)raw;
					if (m.bytes.size() >= sizeof(MCMSG_MoverUpdate) + mu->numRLEs * sizeof(MpMoverEntry))
						handleMoverUpdate(m.sender, mu);
				}
				break;
			case MCMSG_MOVER_WEAPONFIRE_UPDATE:
				if (m.bytes.size() >= sizeof(MCMSG_MoverWeaponFireUpdate))
					handleMoverWeaponFireUpdate(m.sender, (MCMSG_MoverWeaponFireUpdate*)raw);
				break;
			case MCMSG_MOVER_CRITICAL_UPDATE:
				if (m.bytes.size() >= sizeof(MCMSG_MoverCriticalUpdate))
					handleMoverCriticalUpdate(m.sender, (MCMSG_MoverCriticalUpdate*)raw);
				break;
			case MCMSG_WEAPONHIT_UPDATE:
				if (m.bytes.size() >= sizeof(MCMSG_WeaponHitUpdate)) {
					MCMSG_WeaponHitUpdate* wh = (MCMSG_WeaponHitUpdate*)raw;
					if (m.bytes.size() >= sizeof(MCMSG_WeaponHitUpdate) + wh->numWeaponHits * sizeof(unsigned long))
						handleWeaponHitUpdate(m.sender, wh);
				}
				break;
			case MCMSG_END_MISSION:
				if (m.bytes.size() >= sizeof(MCMSG_EndMission))
					handleEndMission(m.sender, (MCMSG_EndMission*)raw);
				break;
			case MCMSG_PLAYER_ORDER:
				if (m.bytes.size() >= sizeof(MCMSG_PlayerOrder))
					handlePlayerOrder(m.sender, (MCMSG_PlayerOrder*)raw);
				break;
			case MCMSG_LEAVE_SESSION:
				if (m.bytes.size() >= sizeof(MCMSG_LeaveSession))
					handleLeaveSession(m.sender, (MCMSG_LeaveSession*)raw);
				break;
			default:
				if (getenv("MC2_LOG"))
					printf("[MP] unhandled msg type %u (%d bytes)\n", raw[0], (int)m.bytes.size());
				break;
		}
	}
	s_inbound.clear();
}

//-----------------------------------------------------------------------------
// Misc. routines
//-----------------------------------------------------------------------------

void MultiPlayer::buildMoverRosterRLE (void) {

}

//---------------------------------------------------------------------------

long MultiPlayer::updateClients (bool forceIt) {

	return(MPLAYER_NO_ERR);
}

//---------------------------------------------------------------------------

bool MultiPlayer::calcMissionStatus (void) {
	// Elimination. The host decides; clients end when MCMSG_EndMission arrives.
	// A commander is out when every mover in its roster is destroyed or disabled;
	// a team is alive while any of its commanders still has a unit.
	if (!inSession || mode != MULTIPLAYER_MODE_MISSION || !mission)
		return(false);
	if (!iAmHost) {
		if (!endMission)
			return(false);
		if (getenv("MC2_LOG"))
			printf("[MP] mission over: winningTeam=%ld timeUp=0 t=%.1f (from host)\n", winningTeam, mission->actualTime);
		return(true);
	}
	bool teamAlive[MAX_TEAMS];
	memset(teamAlive, 0, sizeof(teamAlive));
	long teamsInPlay = 0;
	for (long cid = 0; cid < MAX_MC_PLAYERS; cid++) {
		if (!playerInfo[cid].player || playerInfo[cid].leftSession)
			continue;
		long alive = 0, total = 0;
		for (long i = 0; i < MAX_LOCAL_MOVERS; i++) {
			MoverPtr m = playerMoverRoster[cid][i];
			if (!m) continue;
			total++;
			if (!m->isDestroyed() && !m->isDisabled())
				alive++;
		}
		allUnitsDestroyed[cid] = (total > 0 && alive == 0);
		long team = playerInfo[cid].team;
		if (team >= 0 && team < MAX_TEAMS && total > 0) {
			if (!teamAlive[team]) teamsInPlay++;	// counts teams that started with units (dead ones too)
			if (alive > 0) teamAlive[team] = true;
		}
	}
	long teamsAlive = 0, lastTeam = -1;
	for (long t = 0; t < MAX_TEAMS; t++)
		if (teamAlive[t]) { teamsAlive++; lastTeam = t; }
	bool timeUp = (missionSettings.timeLimit > 0.0f && mission->actualTime >= missionSettings.timeLimit);
	// ponytail: teamsInPlay counts teams that have units on the roster; a solo host
	// with nobody else loaded never ends (nothing to eliminate).
	if (teamsInPlay < 2 && !timeUp)
		return(false);
	if (teamsAlive > 1 && !timeUp)
		return(false);
	winningTeam = (teamsAlive == 1) ? lastTeam : -1;
	for (long cid = 0; cid < MAX_MC_PLAYERS; cid++)
		if (playerInfo[cid].player)
			playerInfo[cid].winner = (winningTeam >= 0 && playerInfo[cid].team == winningTeam);
	if (getenv("MC2_LOG"))
		printf("[MP] mission over: winningTeam=%ld timeUp=%d t=%.1f\n", winningTeam, timeUp ? 1 : 0, mission->actualTime);
	if (!endMission) {
		endMission = true;
		sendEndMission(winningTeam);
	}
	return(true);
}

//-----------------------------------------------------------------------------

int __cdecl sortPlayerRanks (const void* player1, const void* player2) {

	return(0);
}

//-----------------------------------------------------------------------------

void MultiPlayer::calcPlayerRanks (void) {

}

//-----------------------------------------------------------------------------

bool MultiPlayer::allPlayersCheckedIn (void) {
	for (long i = 0; i < MAX_MC_PLAYERS; i++)
		if (playerInfo[i].player && !playerInfo[i].leftSession && i != commanderID && !playerInfo[i].checkedIn)
			return(false);
	return(true);
}

//---------------------------------------------------------------------------

bool MultiPlayer::allPlayersReady (void) {
	for (long i = 0; i < MAX_MC_PLAYERS; i++)
		if (playerInfo[i].player && !playerInfo[i].leftSession && i != commanderID && !playerInfo[i].ready)
			return(false);
	return(true);
}

//---------------------------------------------------------------------------

void MultiPlayer::switchServers (void) {

}


//---------------------------------------------------------------------------

void MultiPlayer::calcDropZones (char dropZonesCID[MAX_MC_PLAYERS], char hqs[MAX_TEAMS]) {
	// Drop zone k gets the k-th occupied commander slot (host first); the rest stay empty.
	long k = 0;
	for (long i = 0; i < MAX_MC_PLAYERS; i++)
		if (playerInfo[i].player && !playerInfo[i].leftSession)
			dropZonesCID[k++] = (char)i;
	while (k < MAX_MC_PLAYERS)
		dropZonesCID[k++] = -1;
	for (long t = 0; t < MAX_TEAMS; t++)
		hqs[t] = (char)t;
}

//---------------------------------------------------------------------------

void MultiPlayer::initStartupParameters (bool fresh) {
	(void)fresh;
	memset(playerInfo, 0, sizeof(playerInfo));
	memset(playerList, 0, sizeof(playerList));
	for (long i = 0; i < MAX_MC_PLAYERS; i++) {
		playerInfo[i].commanderID = -1;
		playerList[i].commanderID = -1;
		availableCIDs[i] = true;
		insigniaList[i] = false;
		playerReady[i] = false;
	}
	memset(&missionSettings, 0, sizeof(missionSettings));
	missionSettings.maxPlayers = MAX_MC_PLAYERS;
	missionSettings.maxTeams = MAX_TEAMS;
	missionSettings.defaultCBills = 100000;
	missionSettings.dropWeight = 300;
	missionSettings.timeLimit = -1.0f;
	missionSettings.quickStart = true;	// v1: map-defined lances, no lobby purchasing
	{
		const char* map = getenv("MC2_MP_MAP");
		strncpy(missionSettings.map, (map && map[0]) ? map : "mc2_m01", MAXLEN_MAP_NAME - 1);
	}
	chatCount = 0;
	memset(currentChatMessages, 0, sizeof(currentChatMessages));
	memset(currentChatMessagePlayerIDs, 0, sizeof(currentChatMessagePlayerIDs));
	missionSettings.unlimitedAmmo = true;
	missionSettings.allTech = true;
	missionSettings.variants = true;
	onLAN = true;		// retail probed for a LAN adapter; ENet works anywhere, so keep the LAN panel enabled
	warpFactor = 128.0f;	// ~3 cells: clients warp a mech to its chunk cell only beyond this (was never set)
	iAmHost = false;
	inSession = false;
	hostLeft = false;
	hostDroppedOut = false;
	locked = false;
	inProgress = false;
	cancelled = false;
	commanderID = -1;
	myPlayer = serverPlayer = NULL;
	sessionName[0] = playerName[0] = 0;
	versionStatus = VERSION_STATUS_UNKNOWN;
	numTeams = 0;
	// Mission-phase flags: the lobby screens branch on these every frame (a garbage
	// startLoading launched the client straight into the load screen).
	startLogistics = startLoading = startPlanning = setupMission = false;
	startMission = endMission = waitingToStartMission = preparingMission = false;
	fitStart = false;
	numFitPlayers = 0;
	randomSeed = 0;
	memset(readyToLoad, 0, sizeof(readyToLoad));
	memset(mechDataReceived, 0, sizeof(mechDataReceived));
	memset(missionDataLoaded, 0, sizeof(missionDataLoaded));
	memset(missionFullySetup, 0, sizeof(missionFullySetup));
	memset(leaveSessionConfirmed, 0, sizeof(leaveSessionConfirmed));
	memset(allUnitsDestroyed, 0, sizeof(allUnitsDestroyed));
	memset(inSessionScreen, 0, sizeof(inSessionScreen));
	memset(commandersToLoad, 0, sizeof(commandersToLoad));
	memset(moverRoster, 0, sizeof(moverRoster));
	memset(playerMoverRoster, 0, sizeof(playerMoverRoster));
	memset(localMovers, 0, sizeof(localMovers));
	memset(turretRoster, 0, sizeof(turretRoster));
	memset(mechData, 0, sizeof(mechData));
	numMovers = numLocalMovers = numTurrets = 0;
	numWeaponHitChunks = 0;
	s_hitsSent = s_hitsApplied = s_fireChunksSent = s_fireChunksReceived = 0;
	memset(s_lastSentValidChunk, 0, sizeof(s_lastSentValidChunk));
	s_moverUpdateId = 0; s_lastMoverUpdateId = 0; s_haveMoverUpdate = false;
	mode = MULTIPLAYER_MODE_NONE;
	s_lastSentValid = false;
}


//---------------------------------------------------------------------------
extern char* GetTime();

extern const char *SpecialtySkillsTable[NUM_SPECIALTY_SKILLS];

long MultiPlayer::saveTranscript (const char* fileName, bool debugging) {

	return(MPLAYER_NO_ERR);
}

//---------------------------------------------------------------------------

void MultiPlayer::destroy (void) {

}

//---------------------------------------------------------------------------

void MultiPlayer::getChatMessages( char** buffer, long* playerIDs, long& count )
{
	// Hands over the messages that arrived since the last call and clears them;
	// ChatWindow::update appends whatever it gets every frame.
	long n = (count < chatCount) ? count : chatCount;
	for (long i = 0; i < n; i++) {
		buffer[i] = currentChatMessages[i];
		playerIDs[i] = currentChatMessagePlayerIDs[i];
	}
	count = n;
	chatCount = 0;
}

void MultiPlayer::redistributeRP( )
{
	for (long i = 0; i < MAX_MC_PLAYERS; i++) {
		if (!playerInfo[i].player)
			continue;
		playerInfo[i].cBills = missionSettings.defaultCBills;
		playerInfo[i].resourcePoints = missionSettings.resourcePoints;
		playerInfo[i].resourcePointsAtStart = missionSettings.resourcePoints;
		if (iAmHost)
			sendPlayerUpdate(NULL, 5, i);
	}
}


//***************************************************************************
