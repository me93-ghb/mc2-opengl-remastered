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
#include"contact.h"
#include"gvehicl.h"
#include"weaponfx.h"	// MINE_EXPLOSION_ID
#include"dbldng.h"	// GENERIC_HQ_BUILDING_OBJNUM
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
				if (cid >= 0 && mp->mode == MULTIPLAYER_MODE_MISSION) {
					// Mid-match drop: keep team and roster so the lance stays in play under host AI
					// and calcMissionStatus can still eliminate it; the slot is wiped at Mission::destroy.
					mp->playerInfo[cid].player = NULL;
					mp->playerInfo[cid].leftSession = true;
					mp->playerList[cid].player = NULL;
				}
				else if (cid >= 0) {
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
	// Direct-connect target: the browser's host:port field (MP-5) or MC2_MP_CONNECT=host[:port].
	char s_directAddr[128] = "";
	const char* mpConnectAddr (void) {
		if (s_directAddr[0]) return s_directAddr;
		const char* a = getenv("MC2_MP_CONNECT");
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
				sendTurretUpdate();
			}
			sendMoverWeaponFireUpdate();	// each early-returns when there is nothing queued
			sendTurretWeaponFireUpdate();
			sendMoverCriticalUpdate();
			sendWeaponHitUpdate();
			sendWorldUpdate();
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

void MultiPlayer::setDirectAddress (const char* hostPort) {
	strncpy(s_directAddr, hostPort ? hostPort : "", sizeof(s_directAddr) - 1);
	s_directAddr[sizeof(s_directAddr) - 1] = 0;
	if (getenv("MC2_LOG")) printf("[MP] direct address set: '%s'\n", s_directAddr);
}

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
		// [MP_SENSOR]: per-team sensor contacts (client visibility). [MP_TGT] every 10 s: does
		// each pilot have a target / order on its own (fire-at-will), and how many visible enemies.
		for (long t = 0; t < Team::numTeams; t++) {
			TeamSensorSystemPtr ts = SensorManager ? SensorManager->getTeamSensor(t) : NULL;
			if (ts)
				printf("[MP_SENSOR] t=%.0f team=%ld home=%d sensors=%ld contacts=%ld\n", mission->actualTime, t,
					(Team::home && Team::home->getId() == t) ? 1 : 0, ts->numSensors, ts->numContacts);
		}
		static unsigned long s_lastTgt = 0;
		if (now - s_lastTgt >= 10000) {
			s_lastTgt = now;
			for (long i = 0; i < MAX_MULTIPLAYER_MOVERS; i++) {
				MoverPtr m = moverRoster[i];
				if (!m || m->isDestroyed() || !m->getPilot()) continue;
				int cl[MAX_CONTACTS_PER_SENSOR];
				GameObjectPtr tgt = m->getPilot()->getLastTarget();
				printf("[MP_TGT] t=%.0f cid=%ld idx=%ld ctl=%d tgt=%ld order=%d enemiesVisible=%ld dis=%d\n", mission->actualTime, m->getCommanderId(), i,
					(int)m->control.getType(), tgt ? (long)tgt->getWatchID() : 0L, (int)m->getPilot()->getCurTacOrder()->code,
					m->getContacts(cl, CONTACT_CRITERIA_ENEMY | CONTACT_CRITERIA_VISUAL, CONTACT_SORT_NONE), m->isDisabled() ? 1 : 0);
			}
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
		if (strcmp(getenv("MC2_MP_SCRIPT_ORDERS"), "recover") == 0) {
			// Eject the second local pilot, then buy a Karnov recovery for the empty mech.
			static bool s_ejected = false, s_requested = false;
			const long kKarnovID = 147, kKarnovCost = 6000;
			MoverPtr victim = (numLocalMovers > 1) ? localMovers[1] : NULL;
			if (!s_ejected && victim && !victim->isDestroyed() && now - s_missionT0 > 15000) {
				s_ejected = true;
				TacticalOrder eject;
				eject.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_EJECT, true);
				eject.pack(NULL, NULL);
				sendPlayerOrder(&eject, false, 1, &victim);
				printf("[MP_KARNOV] eject sent idx=%ld\n", victim->getNetRosterIndex()); fflush(stdout);
			}
			else if (s_ejected && !s_requested && victim && victim->isDisabled() && !victim->isDestroyed()
			         && playerInfo[commanderID].resourcePoints >= kKarnovCost) {
				s_requested = true;
				const char* pilot = LogisticsData::instance ? LogisticsData::instance->getBestPilot(victim->tonnage) : NULL;
				if (!pilot) { printf("[MP_KARNOV] no pilot available\n"); fflush(stdout); }
				else {
					LogisticsData::instance->decrementResourcePoints((int)kKarnovCost);
					sendReinforcement(-kKarnovCost, 0, "noname", commanderID, victim->getPosition(), 6);
					sendReinforcement(kKarnovID, victim->getNetRosterIndex(), pilot, commanderID, victim->getPosition(), 3);
					printf("[MP_KARNOV] request sent idx=%ld pilot=%s\n", victim->getNetRosterIndex(), pilot); fflush(stdout);
				}
			}
		}
		if (strcmp(getenv("MC2_MP_SCRIPT_ORDERS"), "vtol") == 0) {
			static bool s_bought = false, s_struck = false;
			const long kMinelayerID = 120, kMinelayerCost = 2000;
			if (!s_bought && numLocalMovers > 0 && localMovers[0] && playerInfo[commanderID].resourcePoints >= kMinelayerCost) {
				s_bought = true;
				// Same three steps the vehicle button + map click make: pay, tell the host, ask for the drop.
				if (LogisticsData::instance) LogisticsData::instance->decrementResourcePoints((int)kMinelayerCost);
				Stuff::Vector3D dropPos = localMovers[0]->getPosition();
				dropPos.x += 60.0f;
				sendReinforcement(-kMinelayerCost, 0, "noname", commanderID, dropPos, 6);
				requestReinforcement(kMinelayerID, dropPos);
				printf("[MP_VTOL] purchase minelayer rp=%ld\n", playerInfo[commanderID].resourcePoints); fflush(stdout);
			}
			else if (s_bought && !s_struck && now - s_missionT0 > 50000) {
				s_struck = true;
				sendPlayerArtillery(ARTILLERY_LARGE, enemy->getPosition(), 5);
				printf("[MP_ART] request\n"); fflush(stdout);
			}
		}
		if (strcmp(getenv("MC2_MP_SCRIPT_ORDERS"), "recover") == 0)
			return;	// nobody moves: the Karnov comes to the ejected mech, no fight needed
		for (long i = 0; i < numLocalMovers; i++) {
			MoverPtr m = localMovers[i];
			if (!m || m->isDestroyed()) continue;
			TacticalOrder tacOrder;
			const bool vtolMode = (strcmp(getenv("MC2_MP_SCRIPT_ORDERS"), "vtol") == 0);
			if (vtolMode && m->getObjectClass() == GROUNDVEHICLE && ((GroundVehiclePtr)m)->mineLayer) {
				// The bought minelayer: lay mines on the way to the enemy.
				tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_MOVETO_POINT, false);
				Stuff::Vector3D p = enemy->getPosition();
				tacOrder.setWayPoint(0, p);
				tacOrder.moveParams.wait = false;
				tacOrder.moveParams.mode = MOVE_MODE_MINELAYING;
				tacOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_SLOW;
				tacOrder.pack(NULL, NULL);
				sendPlayerOrder(&tacOrder, false, 1, &m);
				printf("[MP_VTOL] minelayer ordered to lay mines\n"); fflush(stdout);
				continue;
			}
			GameObjectPtr capTarget = NULL;
			if (strcmp(getenv("MC2_MP_SCRIPT_ORDERS"), "capture") == 0) {
				// Nearest building this lance can still capture (turret controls, resource buildings).
				float best = 1.0e30f;
				for (long b = 0; b < ObjectManager->getNumBuildings(); b++) {
					BuildingPtr bld = ObjectManager->getBuilding(b);
					if (!bld || !bld->isCaptureable(m->getTeamId())) continue;
					float d = m->distanceFrom(bld->getPosition());
					if (d < best) { best = d; capTarget = bld; }
				}
			}
			if (capTarget) {
				tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_CAPTURE);
				tacOrder.targetWID = capTarget->getWatchID();
				tacOrder.attackParams.type = ATTACK_NONE;
				tacOrder.attackParams.method = ATTACKMETHOD_RAMMING;
				tacOrder.attackParams.pursue = true;
				tacOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
				}
			else if (strcmp(getenv("MC2_MP_SCRIPT_ORDERS"), "move") == 0 || strcmp(getenv("MC2_MP_SCRIPT_ORDERS"), "capture") == 0 || vtolMode || strcmp(getenv("MC2_MP_SCRIPT_ORDERS"), "recover") == 0) {
				// Plain move toward the enemy: exercises fire-at-will on both lances.
				tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_MOVETO_POINT, false);
				Stuff::Vector3D p = enemy->getPosition();
				tacOrder.setWayPoint(0, p);
				tacOrder.moveParams.wait = false;
				tacOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
				}
			else {
				tacOrder.init(ORDER_ORIGIN_PLAYER, TACTICAL_ORDER_ATTACK_OBJECT);
				tacOrder.targetWID = enemy->getWatchID();
				tacOrder.attackParams.type = ATTACK_TO_DESTROY;
				tacOrder.attackParams.method = ATTACKMETHOD_RANGED;
				tacOrder.attackParams.range = FIRERANGE_OPTIMAL;
				tacOrder.attackParams.pursue = true;
				tacOrder.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
			}
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
	memset(rosterReserved, 0, sizeof(rosterReserved));
	for (long i = 0; i < MAX_MC_PLAYERS; i++) reinforcements[i][0] = reinforcements[i][1] = -1;
	numMovers = numLocalMovers = numTurrets = 0;
	numWeaponHitChunks = 0;
	numWorldChunks = 0;
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
	for (long i = 0; i < MAX_MC_PLAYERS; i++) {
		playerInfo[i].ready = false;
		if (!playerInfo[i].player && playerInfo[i].leftSession) {	// dropped mid-match: free the slot now
			memset(&playerInfo[i], 0, sizeof(MC2Player));
			playerInfo[i].commanderID = -1;
			playerList[i].commanderID = -1;
		}
	}
	inProgress = false;
	missionSettings.inProgress = false;
	if (mode == MULTIPLAYER_MODE_MISSION)
		mode = MULTIPLAYER_MODE_RESULTS;
	if (getenv("MC2_LOG")) printf("[MP] rosters reset for results/rematch\n");
}

void MultiPlayer::logRoster (void) {
	// Turrets are map objects, created in the same order on every machine: index them once here.
	if (numTurrets == 0 && ObjectManager)
		for (long i = 0; i < ObjectManager->getNumTurrets(); i++) {
			TurretPtr t = ObjectManager->getTurret(i);
			if (t) { t->setNetRosterIndex(numTurrets); addToTurretRoster(t); }
		}
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
	printf("[MP] roster hash=%08lx movers=%ld local=%ld turrets=%ld seed=0x%08lx\n", h & 0xffffffffUL, count, numLocalMovers, numTurrets, (unsigned long)randomSeed);
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

void MultiPlayer::addToMoverRoster (MoverPtr mover, long index) {

	if (index >= 0 && index < MAX_MULTIPLAYER_MOVERS && !moverRoster[index]) {
		moverRoster[index] = mover;
		mover->setNetRosterIndex(index);
		rosterReserved[index] = false;
		numMovers++;
		return;
	}
	for (long i = 0; i < MAX_MULTIPLAYER_MOVERS; i++)
		if (!moverRoster[i] && !rosterReserved[i]) {
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

// MP-3 world relay: the host queues one MpWorldEntry per event and drains them into
// MCMSG_WorldUpdate every frame; clients replay them through the same game calls.
long MultiPlayer::queueWorld (long kind, long a, long b, long c, long d, float x, float y, float z) {
	if (!iAmHost || !inSession)
		return(0);
	if (numWorldChunks >= MAX_WORLD_CHUNKS)
		return(0);	// drop rather than overflow; the event still happened on the host
	MpWorldEntry& e = worldChunks[numWorldChunks++];
	e.kind = (unsigned char)kind; e.a = (int)a; e.b = (int)b; e.c = (int)c; e.d = (int)d; e.x = x; e.y = y; e.z = z;
	return(numWorldChunks);
}

void MultiPlayer::applyKillLoss (long killerCID, long loserCID) {
	if (killerCID >= 0 && killerCID < MAX_MC_PLAYERS)
		playerInfo[killerCID].kills++;
	if (loserCID >= 0 && loserCID < MAX_MC_PLAYERS)
		playerInfo[loserCID].losses++;
}

void MultiPlayer::applyWorldEntry (const MpWorldEntry& e) {
	switch (e.kind) {
		case MP_WORLD_MINE: {
			if (getenv("MC2_LOG")) printf("[MP_MINE] r=%d c=%d state=%d\n", e.a, e.b, e.c);
			GameMap->setMine(e.a, e.b, (unsigned long)e.c);
			if (e.d == 2) {	// mine went off: effect only, the host relays the damage as weapon hits
				Stuff::Vector3D p;
				land->cellToWorld(e.a, e.b, p);
				ObjectManager->createExplosion(MINE_EXPLOSION_ID, NULL, p, 0.0f, MineSplashRange * worldUnitsPerMeter);
			}
			break;
		}
		case MP_WORLD_FIRE: {
			GameObjectPtr o = ObjectManager->findByPartId(e.a);
			if (o && o->getObjectClass() == TERRAINOBJECT)
				((TerrainObjectPtr)o)->lightOnFire((float)e.b);
			break;
		}
		case MP_WORLD_ARTILLERY: {
			Stuff::Vector3D loc(e.x, e.y, e.z);
			if (getenv("MC2_LOG")) printf("[MP_ART] cid=%d type=%d\n", e.a, e.b);
			CallArtillery(e.a, e.b, loc, e.c, false);	// not the server: creates the strike, queues nothing
			break;
		}
		case MP_WORLD_CAPTURE: {
			GameObjectPtr o = ObjectManager->findByPartId(e.a);
			if (o && o->isBuilding()) {
				if (o->getObjectType()->getObjTypeNum() == GENERIC_HQ_BUILDING_OBJNUM && missionSettings.missionType == MISSION_TYPE_CAPTURE_BASE)
					o->setFlag(OBJECT_FLAG_CAPTURABLE, false);
				o->setCommanderId(e.b);
				o->setTeamId(e.c, false);	// same pair the host's capture order applies; turrets follow their parent's team
				if (getenv("MC2_LOG")) printf("[MP_CAP] pid=%d cid=%d team=%d\n", e.a, e.b, e.c);
			}
			break;
		}
		case MP_WORLD_SCRIPT_MSG:
			if (mission)
				mission->handleMultiplayMessage(e.a, e.b);
			break;
		case MP_WORLD_KILL_LOSS:
			applyKillLoss(e.a, e.b);
			break;
		default:
			break;
	}
}

long MultiPlayer::addWorldChunk (WorldChunkPtr chunk) {

	return(0);
}

//---------------------------------------------------------------------------

long MultiPlayer::addMissionScriptMessageChunk (long code, long param) {
	return(queueWorld(MP_WORLD_SCRIPT_MSG, code, param));
}

//---------------------------------------------------------------------------

long MultiPlayer::addArtilleryChunk (long commanderId, long artilleryType, Stuff::Vector3D location, long seconds) 
{
	if (getenv("MC2_LOG") && iAmHost) printf("[MP_ART] cid=%ld type=%ld\n", commanderId, artilleryType);
	return(queueWorld(MP_WORLD_ARTILLERY, commanderId, artilleryType, seconds, 0, location.x, location.y, location.z));
}

//---------------------------------------------------------------------------

long MultiPlayer::addMineChunk (long tileR, long tileC, long teamId, long mineState, long explosionType) {
	if (getenv("MC2_LOG") && iAmHost) printf("[MP_MINE] r=%ld c=%ld state=%ld\n", tileR, tileC, mineState);
	return(queueWorld(MP_WORLD_MINE, tileR, tileC, mineState, explosionType));
}

//---------------------------------------------------------------------------

long MultiPlayer::addLightOnFireChunk (GameObjectPtr object, long seconds) {
	return(queueWorld(MP_WORLD_FIRE, object ? object->getPartId() : 0, seconds));
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
	applyKillLoss(killerCID, loserCID);	// the host keeps its own tally too
	return(queueWorld(MP_WORLD_KILL_LOSS, killerCID, loserCID));
}

//---------------------------------------------------------------------------

long MultiPlayer::addEndMissionChunk (void) {

	return(0);
}

//---------------------------------------------------------------------------

long MultiPlayer::addCaptureBuildingChunk (BuildingPtr building, long prevCommanderID, long newCommanderID) {
	long team = (newCommanderID >= 0 && newCommanderID < MAX_MC_PLAYERS) ? playerInfo[newCommanderID].team : -1;
	if (getenv("MC2_LOG") && building) printf("[MP_CAP] pid=%ld cid=%ld team=%ld\n", (long)building->getPartId(), newCommanderID, team);
	return(queueWorld(MP_WORLD_CAPTURE, building ? building->getPartId() : 0, newCommanderID, team));
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

void MultiPlayer::assignReinforcementSlot (MCMSG_Reinforcement* msg) {
	// Host only: reserve the first free roster slot for the vehicle that will land later.
	for (long i = 0; i < MAX_MULTIPLAYER_MOVERS; i++)
		if (!moverRoster[i] && !rosterReserved[i]) {
			rosterReserved[i] = true;
			msg->rosterIndex = (unsigned char)i;
			return;
		}
	msg->rosterIndex = 255;
}

void MultiPlayer::requestReinforcement (long vehicleID, Stuff::Vector3D pos) {
	sendReinforcement(vehicleID, 255, "noone", commanderID, pos, 0);
}

void MultiPlayer::applyReinforcement (MCMSG_Reinforcement* msg, bool fromNetwork) {
	long cid = msg->commanderID;
	switch (msg->stage) {
		case 0: {	// VTOL drop requested: every machine flies the VTOL for that commander
			if (cid < 0 || cid >= MAX_MC_PLAYERS || !MissionInterfaceManager::instance())
				break;
			reinforcements[cid][0] = msg->rosterIndex;
			Stuff::Vector3D pos(msg->location[0], msg->location[1], 0.0f);
			pos.z = land->getTerrainElevation(pos);
			if (getenv("MC2_LOG")) printf("[MP_VTOL] request cid=%ld vehicle=%ld idx=%d\n", cid, (long)msg->vehicleID, (int)msg->rosterIndex);
			MissionInterfaceManager::instance()->beginVtol(msg->vehicleID, cid, &pos);
			break;
		}
		case 2: {	// VTOL dropped on the owner's machine: create the vehicle in the assigned slot everywhere
			if (cid < 0 || cid >= MAX_MC_PLAYERS || msg->rosterIndex >= MAX_MULTIPLAYER_MOVERS)
				break;
			Stuff::Vector3D pos(msg->location[0], msg->location[1], 0.0f);
			pos.z = land->getTerrainElevation(pos);
			MoverPtr m = BringInReinforcement(msg->vehicleID, msg->rosterIndex, cid, pos, true);
			if (getenv("MC2_LOG")) printf("[MP_VTOL] landed cid=%ld vehicle=%ld idx=%d wid=%ld\n", cid, (long)msg->vehicleID, (int)msg->rosterIndex, m ? (long)m->getWatchID() : -1L);
			break;
		}
		case 3: {	// Karnov recovery requested for moverRoster[rosterIndex]: every machine flies the Karnov
			if (cid < 0 || cid >= MAX_MC_PLAYERS || msg->rosterIndex >= MAX_MULTIPLAYER_MOVERS || !MissionInterfaceManager::instance())
				break;
			reinforcements[cid][1] = msg->rosterIndex;
			strncpy(reinforcementPilot[cid], msg->pilotName, sizeof(reinforcementPilot[cid]) - 1);
			Stuff::Vector3D pos(msg->location[0], msg->location[1], 0.0f);
			pos.z = land->getTerrainElevation(pos);
			if (getenv("MC2_LOG")) printf("[MP_KARNOV] request cid=%ld idx=%d pilot=%s\n", cid, (int)msg->rosterIndex, reinforcementPilot[cid]);
			MissionInterfaceManager::instance()->beginVtol(msg->vehicleID, cid, &pos, NULL);
			break;
		}
		case 5:	// the owner's Karnov finished: repair, re-crew and power up the mech here too
			if (cid >= 0 && cid < MAX_MC_PLAYERS && MissionInterfaceManager::instance())
				MissionInterfaceManager::instance()->completeRecovery(cid);
			break;
		case 6:	// resource points delta (vehicleID carries the amount: kills, captured buildings, purchases)
			if (cid >= 0 && cid < MAX_MC_PLAYERS) {
				playerInfo[cid].resourcePoints += msg->vehicleID;
				if (msg->vehicleID > 0) playerInfo[cid].resourcePointsGained += msg->vehicleID;
				if (fromNetwork && cid == commanderID && LogisticsData::instance)
					LogisticsData::instance->addResourcePoints((int)msg->vehicleID);
				if (getenv("MC2_LOG")) printf("[MP_RP] cid=%ld delta=%ld total=%ld\n", cid, (long)msg->vehicleID, playerInfo[cid].resourcePoints);
			}
			break;
		case 7: {	// mover dropped from its force group on the owner's machine
			MoverPtr m = (msg->rosterIndex < MAX_MULTIPLAYER_MOVERS) ? moverRoster[msg->rosterIndex] : NULL;
			if (m && fromNetwork)
				m->addToUnitGroup(-1);
			break;
		}
		default:
			break;
	}
}

void MultiPlayer::sendReinforcement (long vehicleID, long rosterIndex, const char pilotName[16], long commanderID, Stuff::Vector3D pos, unsigned char stage) {
	if (!inSession)
		return;
	if (stage > 7 || stage == 1 || stage == 4) {
		if (getenv("MC2_LOG")) printf("[MP] reinforcement stage %d not relayed\n", (int)stage);
		return;
	}
	MCMSG_Reinforcement msg;
	msg.init();
	msg.stage = stage;
	msg.rosterIndex = (unsigned char)rosterIndex;
	msg.vehicleID = vehicleID;
	strncpy(msg.pilotName, pilotName ? pilotName : "", sizeof(msg.pilotName) - 1);
	msg.commanderID = (char)commanderID;
	msg.location[0] = pos.x; msg.location[1] = pos.y;
	if (stage == 0 || stage == 3) {
		// A drop / recovery request. The host answers it (slot, broadcast); a client just asks.
		if (iAmHost) { if (stage == 0) assignReinforcementSlot(&msg); applyReinforcement(&msg, true); }
		sendMessage(NULL, &msg, sizeof(msg), true /*GUARANTEED*/, false);
		return;
	}
	if (stage != 5)	// stage 2 creates the vehicle here too; 6/7 were applied by the caller; 5 runs completeRecovery in updateVTol
		applyReinforcement(&msg, stage == 2);
	sendMessage(NULL, &msg, sizeof(msg), true /*GUARANTEED*/, false);
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
	// Client side of ControlGui::toggleHoldPosition: the host owns the tac orders, so ship the
	// selection plus the UI's range settings and let it patch the live orders (handleHoldPosition).
	if (iAmHost || !inSession || !Team::home || !Commander::home)
		return;
	MCMSG_HoldPosition msg;
	msg.init();
	msg.commanderID = (char)commanderID;
	ControlGui* gui = MissionInterfaceManager::instance() ? MissionInterfaceManager::instance()->getControlGui() : NULL;
	msg.fireFromCurrentPos = (gui && gui->getFireFromCurrentPos()) ? 1 : 0;
	for (long i = 0; i < Team::home->getRosterSize() && msg.numMovers < MAX_LOCAL_MOVERS; i++) {
		MoverPtr m = Team::home->getMover(i);
		if (!m || !m->isSelected() || !m->getCommander() || m->getCommander()->getId() != Commander::home->getId())
			continue;
		long idx = m->getNetRosterIndex();
		if (idx < 0 || idx >= MAX_MULTIPLAYER_MOVERS) continue;
		msg.moverIndex[msg.numMovers] = (unsigned char)idx;
		msg.attackRange[msg.numMovers] = (unsigned char)m->attackRange;
		msg.numMovers++;
	}
	if (!msg.numMovers)
		return;
	sendMessage(NULL, &msg, sizeof(msg), true /*GUARANTEED*/, false);
}

//---------------------------------------------------------------------------

void MultiPlayer::sendPlayerMoverGroup (long groupId,
										long numMovers,
										MoverPtr* moverList,
										long point) {

}

//---------------------------------------------------------------------------

void MultiPlayer::sendPlayerArtillery (long strikeType, Stuff::Vector3D location, long seconds) {
	if (iAmHost || !inSession)
		return;
	MCMSG_PlayerArtillery msg;
	msg.init();
	msg.location[0] = location.x; msg.location[1] = location.y; msg.location[2] = location.z;
	msg.chunk = ((unsigned long)strikeType & 0xff) | (((unsigned long)seconds & 0xffff) << 8);
	sendMessage(NULL, &msg, sizeof(msg), true /*GUARANTEED*/, false);
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
	if (!iAmHost || !inSession || mode != MULTIPLAYER_MODE_MISSION || numTurrets <= 0)
		return;
	static unsigned short s_turretUpdateId = 0;
	unsigned char buf[sizeof(MCMSG_TurretUpdate) + MAX_MULTIPLAYER_TURRETS];
	MCMSG_TurretUpdate* msg = (MCMSG_TurretUpdate*)buf;
	msg->init();
	msg->updateId = ++s_turretUpdateId;
	for (long i = 0; i < numTurrets; i++) {
		TurretPtr t = turretRoster[i];
		GameObjectPtr target = (t && t->targetWID) ? ObjectManager->getByWatchID(t->targetWID) : NULL;
		long idx = (target && target->isMover()) ? ((MoverPtr)target)->getNetRosterIndex() : -1;
		msg->targetList[i] = (char)((idx >= 0 && idx < 127) ? idx : -1);
	}
	sendMessage(NULL, msg, (int)(sizeof(MCMSG_TurretUpdate) + numTurrets), false /*unreliable*/, false);
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
	if (!iAmHost || !inSession || mode != MULTIPLAYER_MODE_MISSION || numTurrets <= 0)
		return;
	unsigned char buf[sizeof(MCMSG_TurretWeaponFireUpdate) + MAX_MULTIPLAYER_TURRETS * (2 + MAX_TURRET_WEAPONFIRE_CHUNKS * sizeof(unsigned long))];
	MCMSG_TurretWeaponFireUpdate* msg = (MCMSG_TurretWeaponFireUpdate*)buf;
	msg->init();
	unsigned char* p = msg->data;
	long count = 0;
	for (long i = 0; i < numTurrets; i++) {
		TurretPtr t = turretRoster[i];
		if (!t || t->getNumWeaponFireChunks(CHUNK_SEND) <= 0) continue;
		unsigned long chunks[MAX_TURRET_WEAPONFIRE_CHUNKS];
		long n = t->grabWeaponFireChunks(CHUNK_SEND, chunks);
		if (n <= 0) continue;
		*p++ = (unsigned char)i;
		*p++ = (unsigned char)n;
		memcpy(p, chunks, n * sizeof(unsigned long));
		p += n * sizeof(unsigned long);
		count++;
	}
	if (!count)
		return;
	msg->numTurrets = (char)count;
	sendMessage(NULL, msg, (int)(p - buf), true /*GUARANTEED*/, false);
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
	if (!iAmHost || !inSession || mode != MULTIPLAYER_MODE_MISSION)
		return;
	const long kBatch = 48;
	long sent = 0;
	while (sent < numWorldChunks) {
		unsigned char buf[sizeof(MCMSG_WorldUpdate) + kBatch * sizeof(MpWorldEntry)];
		MCMSG_WorldUpdate* msg = (MCMSG_WorldUpdate*)buf;
		msg->init();
		long n = numWorldChunks - sent;
		if (n > kBatch) n = kBatch;
		memcpy(msg->entry, &worldChunks[sent], n * sizeof(MpWorldEntry));
		msg->numWorldChanges = (unsigned char)n;
		sendMessage(NULL, msg, (int)(sizeof(MCMSG_WorldUpdate) + n * sizeof(MpWorldEntry)), true /*GUARANTEED*/, false);
		sent += n;
	}
	if (sent && getenv("MC2_LOG")) printf("[MP] world updates sent=%ld\n", sent);
	numWorldChunks = 0;
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
	if (mode != MULTIPLAYER_MODE_MISSION || !mission)
		return;
	if (iAmHost) {
		if (findPlayer(sender) < 0)
			return;
		if (msg->stage == 0 || msg->stage == 3) {	// drop / recovery request from a client: everyone (origin included) flies it
			if (msg->stage == 0)
				assignReinforcementSlot(msg);
			else {
				MoverPtr t = (msg->rosterIndex < MAX_MULTIPLAYER_MOVERS) ? moverRoster[msg->rosterIndex] : NULL;
				if (!t || t->isDestroyed() || !t->isDisabled())
					return;	// nothing recoverable there
			}
			applyReinforcement(msg, true);
			sendMessage(NULL, msg, sizeof(*msg), true /*GUARANTEED*/, false);
			return;
		}
		applyReinforcement(msg, true);
		for (long i = 0; i < MAX_MC_PLAYERS; i++)	// forward to everyone but the origin
			if (playerInfo[i].player && playerInfo[i].player != sender && playerInfo[i].player != myPlayer)
				sendMessage(playerInfo[i].player, msg, sizeof(*msg), true /*GUARANTEED*/, false);
		}
	else if (sender == serverPlayer)
		applyReinforcement(msg, true);
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
	if (!iAmHost)
		return;
	long cid = findPlayer(sender);
	if (cid < 0 || cid != msg->commanderID || mode != MULTIPLAYER_MODE_MISSION || !mission)
		return;
	for (long i = 0; i < msg->numMovers && i < MAX_LOCAL_MOVERS; i++) {
		long idx = msg->moverIndex[i];
		MoverPtr m = (idx < MAX_MULTIPLAYER_MOVERS) ? moverRoster[idx] : NULL;
		if (!m || m->getCommanderId() != cid || m->isDestroyed() || !m->getPilot())
			continue;
		TacticalOrderPtr order = m->getPilot()->getCurTacOrder();
		if (order->code == TACTICAL_ORDER_NONE)
			continue;
		// Same edit ControlGui makes locally for the host's own lance.
		order->attackParams.range = (FireRangeType)msg->attackRange[i];
		if (order->attackParams.range == FIRERANGE_CURRENT)
			order->attackParams.pursue = false;
		else if (msg->fireFromCurrentPos)
			order->attackParams.pursue = false;
		else
			order->attackParams.pursue = true;
		order->pack(NULL, NULL);
		m->handleTacticalOrder(*order);
	}
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
	if (!iAmHost)
		return;
	long cid = findPlayer(sender);
	if (cid < 0 || mode != MULTIPLAYER_MODE_MISSION || !mission)
		return;
	Stuff::Vector3D loc(msg->location[0], msg->location[1], msg->location[2]);
	// The host's CallArtillery queues the world entry, so every client (the caller included) gets the strike.
	CallArtillery(cid, (long)(msg->chunk & 0xff), loc, (long)((msg->chunk >> 8) & 0xffff), false);
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
	if (iAmHost || sender != serverPlayer)
		return;
	if (mode != MULTIPLAYER_MODE_MISSION || !mission || !ObjectManager)
		return;
	static unsigned short s_last = 0; static bool s_have = false;
	if (s_have && (short)(msg->updateId - s_last) <= 0)
		return;
	s_last = msg->updateId; s_have = true;
	for (long i = 0; i < numTurrets; i++) {
		TurretPtr t = turretRoster[i];
		if (!t) continue;
		long idx = msg->targetList[i];
		MoverPtr target = (idx >= 0 && idx < MAX_MULTIPLAYER_MOVERS) ? moverRoster[idx] : NULL;
		t->targetWID = target ? target->getWatchID() : 0;	// client turrets only aim; the host relays the fire
	}
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
	if (iAmHost || sender != serverPlayer)
		return;
	if (mode != MULTIPLAYER_MODE_MISSION || !mission || !ObjectManager)
		return;
	const unsigned char* p = msg->data;
	for (long k = 0; k < msg->numTurrets; k++) {
		long idx = *p++;
		long n = *p++;
		if (n > MAX_TURRET_WEAPONFIRE_CHUNKS) return;	// malformed
		unsigned long chunks[MAX_TURRET_WEAPONFIRE_CHUNKS];
		memcpy(chunks, p, n * sizeof(unsigned long));
		p += n * sizeof(unsigned long);
		TurretPtr t = (idx < numTurrets) ? turretRoster[idx] : NULL;
		if (!t) continue;
		if (t->getNumWeaponFireChunks(CHUNK_RECEIVE) + n >= MAX_TURRET_WEAPONFIRE_CHUNKS)
			t->clearWeaponFireChunks(CHUNK_RECEIVE);
		t->addWeaponFireChunks(CHUNK_RECEIVE, chunks, n);
	}
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
	if (iAmHost || sender != serverPlayer)
		return;
	if (mode != MULTIPLAYER_MODE_MISSION || !mission || !ObjectManager)
		return;	// late packet after Mission::destroy
	for (long i = 0; i < msg->numWorldChanges; i++)
		applyWorldEntry(msg->entry[i]);
	if (getenv("MC2_LOG")) printf("[MP] world updates applied=%d\n", (int)msg->numWorldChanges);
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
			case MCMSG_HOLD_POSITION:
				if (m.bytes.size() >= sizeof(MCMSG_HoldPosition))
					handleHoldPosition(m.sender, (MCMSG_HoldPosition*)raw);
				break;
			case MCMSG_PLAYER_ARTILLERY:
				if (m.bytes.size() >= sizeof(MCMSG_PlayerArtillery))
					handlePlayerArtillery(m.sender, (MCMSG_PlayerArtillery*)raw);
				break;
			case MCMSG_TURRET_UPDATE:
				if (m.bytes.size() >= sizeof(MCMSG_TurretUpdate))
					handleTurretUpdate(m.sender, (MCMSG_TurretUpdate*)raw);
				break;
			case MCMSG_TURRET_WEAPONFIRE_UPDATE:
				if (m.bytes.size() >= sizeof(MCMSG_TurretWeaponFireUpdate))
					handleTurretWeaponFireUpdate(m.sender, (MCMSG_TurretWeaponFireUpdate*)raw);
				break;
			case MCMSG_WORLD_UPDATE: {
				MCMSG_WorldUpdate* wu = (MCMSG_WorldUpdate*)raw;
				if (m.bytes.size() >= sizeof(MCMSG_WorldUpdate) + wu->numWorldChanges * sizeof(MpWorldEntry))
					handleWorldUpdate(m.sender, wu);
				break;
			}
			case MCMSG_REINFORCEMENT:
				if (m.bytes.size() >= sizeof(MCMSG_Reinforcement))
					handleReinforcement(m.sender, (MCMSG_Reinforcement*)raw);
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
		if (!playerInfo[cid].player && !playerInfo[cid].leftSession)
			continue;	// a dropped player's lance still counts until it is eliminated
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
		const char* rp = getenv("MC2_MP_RP");	// harness: starting resource points (reinforcement purchases)
		if (rp && rp[0]) missionSettings.resourcePoints = atol(rp);
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
	numWorldChunks = 0;
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
