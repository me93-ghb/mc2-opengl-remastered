/*************************************************************************************************\
mptransport.cpp			: ENet-backed implementation of MPTransport. See mptransport.h.

	Channel 0 carries reliable traffic, channel 1 unreliable, so a dropped unreliable
	mover-update can never head-of-line-block a guaranteed order behind it.
\*************************************************************************************************/

#include "mptransport.h"

#include <enet/enet.h>
#include <string.h>

enum { CH_RELIABLE = 0, CH_UNRELIABLE = 1, CH_COUNT = 2 };

int MPTransport::libraryRefs = 0;

MPTransport::MPTransport()
	: enetHost(0), serverPeer(0), pendingPeer(0), hosting(false)
{
}

MPTransport::~MPTransport()
{
	close();
}

bool MPTransport::initLibrary()
{
	if (libraryRefs++ == 0)
	{
		if (enet_initialize() != 0)
		{
			libraryRefs = 0;
			return false;
		}
	}
	return true;
}

void MPTransport::shutdownLibrary()
{
	if (libraryRefs > 0 && --libraryRefs == 0)
		enet_deinitialize();
}

bool MPTransport::host(unsigned short port, int maxPeers)
{
	close();
	if (maxPeers < 1) maxPeers = 1;
	if (maxPeers > MC2_MP_MAX_PEERS) maxPeers = MC2_MP_MAX_PEERS;

	ENetAddress addr;
	addr.host = ENET_HOST_ANY;
	addr.port = port;

	ENetHost* h = enet_host_create(&addr, maxPeers, CH_COUNT, 0, 0);
	if (!h)
		return false;

	enetHost = h;
	hosting = true;
	return true;
}

bool MPTransport::connect(const char* hostName, unsigned short port)
{
	close();

	ENetHost* h = enet_host_create(NULL, 1, CH_COUNT, 0, 0);
	if (!h)
		return false;

	ENetAddress addr;
	if (enet_address_set_host(&addr, hostName) != 0)
	{
		enet_host_destroy(h);
		return false;
	}
	addr.port = port;

	ENetPeer* p = enet_host_connect(h, &addr, CH_COUNT, 0);
	if (!p)
	{
		enet_host_destroy(h);
		return false;
	}

	enetHost = h;
	pendingPeer = p;		// promoted to serverPeer on ENET_EVENT_TYPE_CONNECT
	hosting = false;
	return true;
}

void MPTransport::close()
{
	ENetHost* h = (ENetHost*)enetHost;
	if (h)
	{
		for (size_t i = 0; i < h->peerCount; ++i)
		{
			if (h->peers[i].state == ENET_PEER_STATE_CONNECTED)
				enet_peer_disconnect_now(&h->peers[i], 0);
		}
		enet_host_destroy(h);
	}
	enetHost = 0;
	serverPeer = 0;
	pendingPeer = 0;
	hosting = false;
}

void MPTransport::send(void* peer, const void* data, int size, bool reliable)
{
	ENetHost* h = (ENetHost*)enetHost;
	if (!h || !data || size <= 0)
		return;

	ENetPacket* pkt = enet_packet_create(data, (size_t)size,
		reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
	if (!pkt)
		return;

	enet_uint8 channel = reliable ? CH_RELIABLE : CH_UNRELIABLE;
	if (peer)
		enet_peer_send((ENetPeer*)peer, channel, pkt);
	else
		enet_host_broadcast(h, channel, pkt);
	// A packet nobody took ownership of (e.g. broadcast with zero peers) must be freed.
	if (pkt->referenceCount == 0)
		enet_packet_destroy(pkt);
}

void MPTransport::kick(void* peer)
{
	if (peer)
		enet_peer_disconnect((ENetPeer*)peer, 0);
}

int MPTransport::getPeerCount() const
{
	ENetHost* h = (ENetHost*)enetHost;
	if (!h)
		return 0;
	int n = 0;
	for (size_t i = 0; i < h->peerCount; ++i)
		if (h->peers[i].state == ENET_PEER_STATE_CONNECTED)
			++n;
	return n;
}

void MPTransport::poll(void* user, RecvFn onRecv, PeerFn onPeer, int timeoutMs)
{
	ENetHost* h = (ENetHost*)enetHost;
	if (!h)
		return;

	ENetEvent ev;
	// First call may block up to timeoutMs; drain the rest without blocking.
	int wait = timeoutMs;
	while (enet_host_service(h, &ev, wait) > 0)
	{
		wait = 0;
		switch (ev.type)
		{
			case ENET_EVENT_TYPE_CONNECT:
				// Spec MP-3 wants a drop noticed fast (ENet default: ~40 s measured); 5/10 s still tolerates an 8 GB box paging.
				enet_peer_timeout(ev.peer, 0, 5000, 10000);
				if (!hosting && ev.peer == pendingPeer)
				{
					serverPeer = ev.peer;
					pendingPeer = 0;
				}
				if (onPeer) onPeer(user, ev.peer, true);
				break;

			case ENET_EVENT_TYPE_RECEIVE:
				if (onRecv)
					onRecv(user, ev.peer, ev.packet->data, (int)ev.packet->dataLength,
						(ev.packet->flags & ENET_PACKET_FLAG_RELIABLE) != 0);
				enet_packet_destroy(ev.packet);
				break;

			case ENET_EVENT_TYPE_DISCONNECT:
				if (ev.peer == serverPeer) serverPeer = 0;
				if (ev.peer == pendingPeer) pendingPeer = 0;
				if (onPeer) onPeer(user, ev.peer, false);
				break;

			default:
				break;
		}
	}
}
