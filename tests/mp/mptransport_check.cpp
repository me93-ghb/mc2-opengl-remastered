// tests/mp/mptransport_check.cpp
//
// Proof-of-life for the ENet transport behind MultiPlayer's stubbed DirectPlay seam.
// Game-free: links only code/mptransport.cpp + 3rdparty/enet. Host and client live in
// one process on loopback. Asserts that a reliable and an unreliable payload each cross
// the wire intact, that the reliable flag survives the trip, and that a host broadcast
// reaches the client. Exit 0 == pass; any assert == fail.
//
//   build: see the mp_transport_check target in CMakeLists.txt
//   run:   ./mp_transport_check

#include "mptransport.h"

#include <stdlib.h>

// Always-on check. assert() is stripped under NDEBUG, which would silently delete the
// host()/connect()/pumpUntil() calls that live inside it and turn this into a no-op that
// still prints "ok". CHECK evaluates unconditionally and aborts on failure.
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "CHECK FAILED: %s  (%s:%d)\n", #x, __FILE__, __LINE__); abort(); } } while (0)
#include <stdio.h>
#include <string.h>

struct Got {
	int		count;
	char	last[64];
	bool	lastReliable;
	int		peersUp;
};

static void onRecv(void* user, void* /*peer*/, const void* data, int size, bool reliable)
{
	Got* g = (Got*)user;
	g->count++;
	int n = size < (int)sizeof(g->last) - 1 ? size : (int)sizeof(g->last) - 1;
	memcpy(g->last, data, n);
	g->last[n] = 0;
	g->lastReliable = reliable;
}

static void onPeer(void* user, void* /*peer*/, bool connected)
{
	Got* g = (Got*)user;
	g->peersUp += connected ? 1 : -1;
}

// Pump both ends until pred(server, client) holds or we give up.
template <class Pred>
static bool pumpUntil(MPTransport& srv, MPTransport& cli, Got& gs, Got& gc, Pred pred, int maxIters = 200)
{
	for (int i = 0; i < maxIters; ++i)
	{
		srv.poll(&gs, onRecv, onPeer, 5);
		cli.poll(&gc, onRecv, onPeer, 5);
		if (pred(gs, gc))
			return true;
	}
	return false;
}

int main()
{
	CHECK(MPTransport::initLibrary());

	const unsigned short port = MC2_MP_DEFAULT_PORT + 1;	// don't collide with a running game
	MPTransport srv, cli;
	Got gs = {}, gc = {};

	CHECK(srv.host(port, 2));
	CHECK(srv.isHost() && srv.isOpen());

	CHECK(cli.connect("127.0.0.1", port));
	CHECK(pumpUntil(srv, cli, gs, gc, [](Got& s, Got& c) { return s.peersUp == 1 && c.peersUp == 1; }));
	CHECK(cli.isConnected() && cli.getServerPeer() != 0);
	CHECK(srv.getPeerCount() == 1);
	printf("connect      ok\n");

	// client -> server, reliable (channel 0). This is what //GUARANTEED messages use.
	const char* msgR = "\x01reliable-order";		// leading type byte, like every MCMSG_*
	cli.send(cli.getServerPeer(), msgR, (int)strlen(msgR), true);
	CHECK(pumpUntil(srv, cli, gs, gc, [](Got& s, Got&) { return s.count == 1; }));
	CHECK(strcmp(gs.last, msgR) == 0);
	CHECK(gs.lastReliable == true);
	printf("reliable     ok  (flag survived the wire)\n");

	// client -> server, unreliable (channel 1). Loopback won't drop it; we check the flag.
	const char* msgU = "\x21mover-update";
	cli.send(cli.getServerPeer(), msgU, (int)strlen(msgU), false);
	CHECK(pumpUntil(srv, cli, gs, gc, [](Got& s, Got&) { return s.count == 2; }));
	CHECK(strcmp(gs.last, msgU) == 0);
	CHECK(gs.lastReliable == false);
	printf("unreliable   ok  (flag survived the wire)\n");

	// server -> all clients via broadcast (peer == NULL), reliable.
	const char* msgB = "\x02chat-from-host";
	srv.send(0, msgB, (int)strlen(msgB), true);
	CHECK(pumpUntil(srv, cli, gs, gc, [](Got&, Got& c) { return c.count == 1; }));
	CHECK(strcmp(gc.last, msgB) == 0 && gc.lastReliable);
	printf("broadcast    ok\n");

	// clean teardown: client leaves, server sees the disconnect.
	cli.close();
	CHECK(pumpUntil(srv, cli, gs, gc, [](Got& s, Got&) { return s.peersUp == 0; }));
	CHECK(srv.getPeerCount() == 0);
	srv.close();
	printf("disconnect   ok\n");

	MPTransport::shutdownLibrary();
	printf("mptransport_check: PASS\n");
	return 0;
}
