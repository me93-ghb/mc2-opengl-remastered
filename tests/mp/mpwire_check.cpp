#include "mpwire.h"
#include "multplyr.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <limits>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
using Bytes = std::vector<unsigned char>;
static unsigned cases = 0;
static FILE* corpus = nullptr;
template<class T> void put(Bytes& b, size_t off, T value) { CHECK(off+sizeof(T)<=b.size()); memcpy(b.data()+off, &value, sizeof(T)); }
template<class T> T get(const Bytes& b, size_t off) { T value{}; CHECK(off+sizeof(T)<=b.size()); memcpy(&value,b.data()+off,sizeof(T)); return value; }
static Bytes packet(unsigned char type, size_t size) { Bytes n(size,0); n[0]=type; return n; }
static Bytes check(const Bytes& native, size_t expectedSize, bool variableTail=false) {
    Bytes wire, decoded, again;
    CHECK(MpWire::encode(native.data(),native.size(),wire));
    if (wire.size()!=expectedSize) std::fprintf(stderr,"type=%u actual=%zu expected=%zu\n",native[0],wire.size(),expectedSize); CHECK(wire.size()==expectedSize);
    CHECK(MpWire::decode(wire.data(),wire.size(),decoded));
    CHECK(MpWire::encode(decoded.data(),decoded.size(),again));
    CHECK(wire==again);
    if (corpus) { for (auto b:wire) std::fprintf(corpus,"%02x",b); std::fprintf(corpus,"\n"); }
    // Every truncated fixed-layout packet must fail before a handler sees it.
    if (!variableTail) for (size_t i=0;i<wire.size();++i) CHECK(!MpWire::decode(wire.data(),i,again));
    auto bad=wire; bad[4]++; CHECK(!MpWire::decode(bad.data(),bad.size(),again));
    bad=wire; bad[6]=255; CHECK(!MpWire::decode(bad.data(),bad.size(),again));
    if (!variableTail) { bad=wire; bad.push_back(0); CHECK(!MpWire::decode(bad.data(),bad.size(),again)); }
    CHECK(!MpWire::decode(native.data(),native.size(),again));
    ++cases; return decoded;
}
#define PACK(id,T) packet(id,sizeof(T))
int main(int argc, char** argv) {
    if (argc==2) { corpus=std::fopen(argv[1],"wb"); CHECK(corpus); }
    auto n=PACK(MCMSG_PLAYER_CID,MCMSG_PlayerCID); n[1]=7;
    check(n,10);
    Bytes w; CHECK(MpWire::encode(n.data(),n.size(),w));
    const Bytes cidGolden={'M','C','2','N',1,0,3,0,7,0}; CHECK(w==cidGolden);
    n=PACK(MCMSG_PLAYER_UPDATE,MCMSG_PlayerUpdate);
    size_t p=offsetof(MCMSG_PlayerUpdate,info);
    put<NETPLAYER>(n,p+offsetof(MC2Player,player),reinterpret_cast<NETPLAYER>(uintptr_t(0x123456789abcdef0ULL)));
    put<long>(n,p+offsetof(MC2Player,cBills),-123456);
    put<unsigned long>(n,p+offsetof(MC2Player,teamSeniority),0xfedcba98UL);
    strcpy(reinterpret_cast<char*>(n.data()+p+offsetof(MC2Player,name)),"Venus");
    auto decoded=check(n,195);
    CHECK(get<NETPLAYER>(decoded,p+offsetof(MC2Player,player))==nullptr);
    CHECK(get<long>(decoded,p+offsetof(MC2Player,cBills))==-123456);
    CHECK(get<unsigned long>(decoded,p+offsetof(MC2Player,teamSeniority))==0xfedcba98UL);
    Bytes pointerChanged=n; put<NETPLAYER>(pointerChanged,p+offsetof(MC2Player,player),nullptr);
    Bytes a,b; CHECK(MpWire::encode(n.data(),n.size(),a)); CHECK(MpWire::encode(pointerChanged.data(),pointerChanged.size(),b)); CHECK(a==b);
    n=PACK(MCMSG_MISSION_SETTINGS_UPDATE,MCMSG_MissionSettingsUpdate);
    p=offsetof(MCMSG_MissionSettingsUpdate,missionSettings);
    put<decltype(GUID::Data1)>(n,p+offsetof(MissionSettings,mapGuid)+offsetof(GUID,Data1),0x12345678);
    put<float>(n,p+offsetof(MissionSettings,timeLimit),-1.0f);
    decoded=check(n,372);
    CHECK(get<decltype(GUID::Data1)>(decoded,p+offsetof(MissionSettings,mapGuid)+offsetof(GUID,Data1))==0x12345678);
    n=PACK(MCMSG_MISSION_SETUP,MCMSG_MissionSetup);
    put<long>(n,offsetof(MCMSG_MissionSetup,randomSeed),0x12345678);
    decoded=check(n,287);
    CHECK(get<long>(decoded,offsetof(MCMSG_MissionSetup,randomSeed))==0x12345678);
    check(PACK(MCMSG_PLAYER_INFO,MCMSG_PlayerInfo),9);
    check(PACK(MCMSG_PLAYER_CHECK_IN,MCMSG_PlayerCheckIn),9);
    check(PACK(MCMSG_START_MISSION,MCMSG_StartMission),12);
    check(PACK(MCMSG_END_MISSION,MCMSG_EndMission),76);
    check(PACK(MCMSG_LEAVE_SESSION,MCMSG_LeaveSession),10);
    n=PACK(MCMSG_PLAYER_ORDER,MCMSG_PlayerOrder);
    put<float>(n,offsetof(MCMSG_PlayerOrder,location),1.0f);
    put<unsigned long>(n,offsetof(MCMSG_PlayerOrder,tacOrderChunk),0x89abcdefUL);
    decoded=check(n,43);
    CHECK(get<unsigned long>(decoded,offsetof(MCMSG_PlayerOrder,tacOrderChunk))==0x89abcdefUL);
    CHECK(MpWire::encode(n.data(),n.size(),w));
    CHECK(w[10]==0 && w[11]==0 && w[12]==0x80 && w[13]==0x3f);
    CHECK(w[18]==0xef && w[19]==0xcd && w[20]==0xab && w[21]==0x89);
    check(PACK(MCMSG_HOLD_POSITION,MCMSG_HoldPosition),45);
    check(PACK(MCMSG_PLAYER_ARTILLERY,MCMSG_PlayerArtillery),24);
    check(PACK(MCMSG_REINFORCEMENT,MCMSG_Reinforcement),47);
    n=packet(MCMSG_CHAT,sizeof(MCMSG_Chat)+3); memcpy(n.data()+sizeof(MCMSG_Chat),"hi",3); check(n,15);
    n.back()='x'; CHECK(!MpWire::encode(n.data(),n.size(),w));
    n=packet(MCMSG_MOVER_UPDATE,sizeof(MCMSG_MoverUpdate)+sizeof(MpMoverEntry));
    n[offsetof(MCMSG_MoverUpdate,numRLEs)]=1;
    put<unsigned long>(n,sizeof(MCMSG_MoverUpdate)+offsetof(MpMoverEntry,status),0xdeadbeefUL);
    decoded=check(n,85);
    CHECK(get<unsigned long>(decoded,sizeof(MCMSG_MoverUpdate)+offsetof(MpMoverEntry,status))==0xdeadbeefUL);
    n=packet(MCMSG_TURRET_UPDATE,sizeof(MCMSG_TurretUpdate)+MAX_MULTIPLAYER_TURRETS); check(n,138,true);
    // A short turret list is padded with 'no target' for the legacy roster loop.
    n.resize(sizeof(MCMSG_TurretUpdate)); CHECK(MpWire::encode(n.data(),n.size(),w)); CHECK(MpWire::decode(w.data(),w.size(),decoded)); CHECK(decoded.back()==255);
    for (unsigned char type : {MCMSG_MOVER_WEAPONFIRE_UPDATE,MCMSG_TURRET_WEAPONFIRE_UPDATE}) {
        n=packet(type,4+2*sizeof(unsigned long)); n[1]=1; n[2]=3; n[3]=2;
        put<unsigned long>(n,4,0x87654321UL); put<unsigned long>(n,4+sizeof(unsigned long),0xffffffffUL);
        decoded=check(n,19); CHECK(get<unsigned long>(decoded,4+sizeof(unsigned long))==0xffffffffUL);
        n[3]=255; CHECK(!MpWire::encode(n.data(),n.size(),w));
    }
    n=packet(MCMSG_MOVER_CRITICAL_UPDATE,sizeof(MCMSG_MoverCriticalUpdate)+2); n[1]=2; n[n.size()-1]=0xab; check(n,202);
    n=packet(MCMSG_WEAPONHIT_UPDATE,sizeof(MCMSG_WeaponHitUpdate)+2*sizeof(unsigned long)); n[1]=2;
    put<unsigned long>(n,2,0xffffffffUL); put<unsigned long>(n,2+sizeof(unsigned long),0x12345678UL);
    check(n,17);
    CHECK(MpWire::encode(n.data(),n.size(),w)); const Bytes hitGolden={'M','C','2','N',1,0,26,0,2,255,255,255,255,0x78,0x56,0x34,0x12}; CHECK(w==hitGolden);
    n=packet(MCMSG_WORLD_UPDATE,sizeof(MCMSG_WorldUpdate)+sizeof(MpWorldEntry)); n[1]=1;
    put<int>(n,sizeof(MCMSG_WorldUpdate)+offsetof(MpWorldEntry,a),-123);
    check(n,39);
    n=PACK(MCMSG_PLAYER_UPDATE,MCMSG_PlayerUpdate);
    CHECK(MpWire::encode(n.data(),n.size(),w));
    w[44]=2; CHECK(!MpWire::decode(w.data(),w.size(),decoded)); // checkedIn is a boolean
    n=PACK(MCMSG_PLAYER_ARTILLERY,MCMSG_PlayerArtillery);
    put<float>(n,offsetof(MCMSG_PlayerArtillery,location),std::numeric_limits<float>::infinity());
    CHECK(!MpWire::encode(n.data(),n.size(),w));
    n=packet(MCMSG_TURRET_WEAPONFIRE_UPDATE,2+128*2); n[1]=128;
    for (unsigned i=0;i<128;++i) n[2+i*2]=static_cast<unsigned char>(i);
    CHECK(MpWire::encode(n.data(),n.size(),w)); CHECK(MpWire::decode(w.data(),w.size(),decoded));
    CHECK(get<unsigned char>(decoded,offsetof(MCMSG_TurretWeaponFireUpdate,numTurrets))==128);
    n=PACK(MCMSG_START_MISSION,MCMSG_StartMission); CHECK(MpWire::encode(n.data(),n.size(),w));
    // Bounded malformed-input sweep must neither crash nor expose partial native data.
    uint32_t rng=0x12345678;
    for (unsigned i=0;i<10000;++i) {
        rng=rng*1664525U+1013904223U; Bytes bad(rng%256);
        for (auto& x:bad) { rng=rng*1664525U+1013904223U; x=static_cast<unsigned char>(rng>>24); }
        if (bad.size()>=8) { memcpy(bad.data(),"MC2N",4); bad[4]=1; bad[5]=bad[7]=0; bad[6]=static_cast<unsigned char>(i%42); }
        if (!MpWire::decode(bad.data(),bad.size(),decoded)) CHECK(decoded.empty());
    }
    if (corpus) std::fclose(corpus);
    std::printf("mp_wire_check: PASS (%u packet types; golden bytes, pointer omission, truncation, version rejection, 10000 malformed inputs)\n",cases);
}
