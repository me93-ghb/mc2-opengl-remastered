#ifndef MPTRANSPORT_H
#define MPTRANSPORT_H
/*************************************************************************************************\
mptransport.h			: Cross-platform UDP transport (ENet) behind MultiPlayer's stubbed
						  DirectPlay seam. Peer handles are ENetPeer* stored as void*, which is
						  exactly what NETPLAYER already is (multplyr.h), so the game code needs
						  no adaptation. Game-free on purpose: the standalone check
						  (tests/mp/mptransport_check.cpp) links only this + 3rdparty/enet.
\*************************************************************************************************/

#define MC2_MP_DEFAULT_PORT		27500
#define MC2_MP_MAX_PEERS		8		// MAX_MC_PLAYERS

class MPTransport
{
	public:

		// Fired from poll(): data is valid only for the duration of the callback.
		typedef void (*RecvFn)(void* user, void* peer, const void* data, int size, bool reliable);
		// connected=true on a new peer, false when it leaves (or the connect attempt fails).
		typedef void (*PeerFn)(void* user, void* peer, bool connected);

		MPTransport();
		~MPTransport();

		// Process-wide ENet init; safe to call more than once (ref-counted).
		static bool		initLibrary();
		static void		shutdownLibrary();

		bool			host(unsigned short port, int maxPeers);	// become the server
		bool			connect(const char* hostName, unsigned short port);	// async; PeerFn(true) on success
		void			close();									// disconnect everyone, drop the socket

		// peer == NULL broadcasts to every connected peer.
		void			send(void* peer, const void* data, int size, bool reliable);
		void			kick(void* peer);

		// Pump events. timeoutMs=0 is non-blocking (per-frame use).
		void			poll(void* user, RecvFn onRecv, PeerFn onPeer, int timeoutMs = 0);

		bool			isHost() const	{ return hosting; }
		bool			isOpen() const	{ return enetHost != 0; }
		bool			isConnected() const { return hosting || serverPeer != 0; }
		void*			getServerPeer() const { return serverPeer; }
		int				getPeerCount() const;

	private:

		void*			enetHost;		// ENetHost*
		void*			serverPeer;		// ENetPeer* (clients only)
		void*			pendingPeer;	// ENetPeer* while a connect() is in flight
		bool			hosting;

		static int		libraryRefs;
};

#endif
