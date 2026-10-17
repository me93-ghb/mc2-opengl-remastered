#include "mpwire.h"
#include "multplyr.h"
#include "mover.h"
#include "turret.h"
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <cmath>

namespace MpWire {
namespace {
// Native objects stay inside the process. Only named fields enter the packet.
struct Cursor {
    bool writing, ok = true;
    std::vector<unsigned char>& native;
    std::vector<unsigned char>& wire;
    size_t pos = 8, expected = 0;
    void require(bool condition) { ok = ok && condition; }
    void extent(size_t size) {
        require(size <= MaxPacket * 2);
        expected = size;
        if (!ok) return;
        if (writing) require(native.size() >= size);
        else native.resize(size, 0);
    }
    template<class T> T get(size_t off) {
        T value{};
        if (off + sizeof(T) > native.size()) { ok = false; return value; }
        memcpy(&value, native.data() + off, sizeof(T));
        return value;
    }
    template<class T> void field(size_t off) {
        if constexpr (std::is_array<T>::value) {
            using E = typename std::remove_extent<T>::type;
            for (size_t i = 0; i < std::extent<T>::value; ++i) field<E>(off + i * sizeof(E));
        } else {
            static_assert(!std::is_pointer<T>::value, "Process pointers cannot enter packets");
            constexpr size_t width = sizeof(T) > 4 ? 4 : sizeof(T);
            static_assert(std::is_integral<T>::value || std::is_same<T,float>::value, "Explicit schema required");
            if (!ok || off + sizeof(T) > native.size()) { ok = false; return; }
            uint32_t bits = 0;
            T value{};
            if (writing) {
                memcpy(&value, native.data() + off, sizeof(T));
                if constexpr (std::is_same<T,float>::value) {
                    require(std::isfinite(value));
                    memcpy(&bits, &value, 4);
                } else bits = static_cast<uint32_t>(value);
                for (size_t i = 0; i < width; ++i) wire.push_back(static_cast<unsigned char>(bits >> (8*i)));
            } else {
                if (pos + width > wire.size()) { ok = false; return; }
                for (size_t i = 0; i < width; ++i) bits |= uint32_t(wire[pos++]) << (8*i);
                if constexpr (std::is_same<T,float>::value) {
                    memcpy(&value, &bits, 4);
                    require(std::isfinite(value));
                } else if constexpr (std::is_same<T,bool>::value) {
                    require(bits <= 1); value = bits != 0;
                } else if constexpr (std::is_signed<T>::value && sizeof(T) > 4) {
                    value = static_cast<T>(static_cast<int32_t>(bits));
                } else value = static_cast<T>(bits);
                memcpy(native.data() + off, &value, sizeof(T));
            }
        }
    }
    void string(size_t off, size_t count) {
        if (!ok || off + count > native.size()) { ok = false; return; }
        if (writing) {
            const auto* first = native.data() + off;
            const auto* end = static_cast<const unsigned char*>(memchr(first, 0, count));
            if (!end) { ok = false; return; }
            wire.insert(wire.end(), first, end);
            wire.insert(wire.end(), count - (end - first), 0);
        } else {
            for (size_t i=0; i<count; ++i) field<unsigned char>(off+i);
            require(memchr(native.data()+off, 0, count) != nullptr);
        }
    }
    void bytes(size_t off, size_t count) {
        for (size_t i=0; i<count && ok; ++i) field<unsigned char>(off+i);
    }
};
#define F(member) c.field<decltype(T::member)>(base + offsetof(T, member))
#define S(member) c.string(base + offsetof(T, member), sizeof(T::member))
void fields(Cursor& c, size_t base, MC2Player*) {
    using T = MC2Player;
    F(checkedIn);
    F(commanderID);
    F(dropZone);
    S(name);
    S(ipAddress);
    S(unitName);
    S(insigniaFile);
    F(baseColor);
    F(stripeColor);
    F(teamSelected);
    F(team);
    F(teamSeniority);
    F(faction);
    F(cBills);
    F(resourcePointsAtStart);
    F(resourcePointsGained);
    F(resourcePoints);
    F(rank);
    F(ready);
    F(score);
    F(kills);
    F(losses);
    F(winner);
    F(leftSession);
    F(booted);
}
void fields(Cursor& c, size_t base, GUID*) {
    using T = GUID;
    F(Data1);
    F(Data2);
    F(Data3);
    F(Data4);
}
void fields(Cursor& c, size_t base, MissionSettings*) {
    using T = MissionSettings;
    S(map);
    S(name);
    fields(c, base + offsetof(T, mapGuid), (GUID*)nullptr);
    S(url);
    F(defaultCBills);
    F(resourcePoints);
    F(timeLimit);
    F(unlimitedAmmo);
    F(variants);
    F(allTech);
    F(airStrike);
    F(guardTower);
    F(repairVehicle);
    F(recoveryTeam);
    F(sensorProbe);
    F(scoutCopter);
    F(mineLayer);
    F(resourceBuilding);
    F(resourceForMechs);
    F(missionType);
    F(quickStart);
    F(maxTeams);
    F(maxPlayers);
    F(numPlayers);
    F(dropWeight);
    F(locked);
    F(inProgress);
}
void fields(Cursor& c, size_t base, CompressedMech*) {
    using T = CompressedMech;
    F(lastMech);
    F(objNumber);
    F(commanderID);
    F(baseColor);
    F(highlightColor1);
    F(highlightColor2);
    S(pilotFile);
    S(mechFile);
    S(variantName);
    F(variantNum);
    F(pos);
    F(cBills);
    F(designerMech);
    F(numComponents);
    F(components);
    c.require(c.get<long>(base + offsetof(T, numComponents)) >= 0 && c.get<long>(base + offsetof(T, numComponents)) <= 50);
}
void fields(Cursor& c, size_t base, MpMoverEntry*) {
    using T = MpMoverEntry;
    F(idx);
    F(flags);
    F(status);
    F(move);
}
void fields(Cursor& c, size_t base, MpWorldEntry*) {
    using T = MpWorldEntry;
    F(kind);
    F(a);
    F(b);
    F(c);
    F(d);
    F(x);
    F(y);
    F(z);
}

bool message(Cursor& c, unsigned char type) {
    size_t base = 0;
    switch (type) {
    case MCMSG_PLAYER_CID: {
        using T = MCMSG_PlayerCID;
        c.extent(sizeof(T));
        F(commanderID);
        F(subType);
        break;
    }
    case MCMSG_PLAYER_UPDATE: {
        using T = MCMSG_PlayerUpdate;
        c.extent(sizeof(T));
        F(stage);
        F(senderTime);
        S(sessionIPAddress);
        S(versionStamp);
        fields(c, offsetof(T, info), (MC2Player*)nullptr);
        break;
    }
    case MCMSG_MISSION_SETTINGS_UPDATE: {
        using T = MCMSG_MissionSettingsUpdate;
        c.extent(sizeof(T));
        fields(c, offsetof(T, missionSettings), (MissionSettings*)nullptr);
        break;
    }
    case MCMSG_MISSION_SETUP: {
        using T = MCMSG_MissionSetup;
        c.extent(sizeof(T));
        F(randomSeed);
        F(subType);
        F(commandersToLoad);
        fields(c, offsetof(T, mechData), (CompressedMech*)nullptr);
        break;
    }
    case MCMSG_PLAYER_INFO: {
        using T = MCMSG_PlayerInfo;
        c.extent(sizeof(T));
        F(commanderID);
        break;
    }
    case MCMSG_PLAYER_CHECK_IN: {
        using T = MCMSG_PlayerCheckIn;
        c.extent(sizeof(T));
        F(commanderID);
        break;
    }
    case MCMSG_START_MISSION: {
        using T = MCMSG_StartMission;
        c.extent(sizeof(T));
        F(huh);
        break;
    }
    case MCMSG_END_MISSION: {
        using T = MCMSG_EndMission;
        c.extent(sizeof(T));
        F(teamScore);
        F(playerScore);
        F(result);
        break;
    }
    case MCMSG_LEAVE_SESSION: {
        using T = MCMSG_LeaveSession;
        c.extent(sizeof(T));
        F(subType);
        F(commanderID);
        break;
    }
    case MCMSG_PLAYER_ORDER: {
        using T = MCMSG_PlayerOrder;
        c.extent(sizeof(T));
        F(commanderID);
        F(flags);
        F(location);
        F(tacOrderChunk);
        F(numMovers);
        F(moverIndex);
        c.require(c.get<unsigned char>(offsetof(T, numMovers)) <= MAX_LOCAL_MOVERS);
        break;
    }
    case MCMSG_HOLD_POSITION: {
        using T = MCMSG_HoldPosition;
        c.extent(sizeof(T));
        F(commanderID);
        F(flags);
        F(numMovers);
        F(moverIndex);
        F(attackRange);
        F(fireFromCurrentPos);
        c.require(c.get<unsigned char>(offsetof(T, numMovers)) <= MAX_LOCAL_MOVERS);
        break;
    }
    case MCMSG_PLAYER_ARTILLERY: {
        using T = MCMSG_PlayerArtillery;
        c.extent(sizeof(T));
        F(location);
        F(chunk);
        break;
    }
    case MCMSG_REINFORCEMENT: {
        using T = MCMSG_Reinforcement;
        c.extent(sizeof(T));
        F(stage);
        F(rosterIndex);
        F(vehicleID);
        S(pilotName);
        F(commanderID);
        F(location);
        break;
    }
    case MCMSG_CHAT: {
        using T = MCMSG_Chat;
        c.extent(sizeof(T));
        F(allPlayers);
        F(isDeadChat);
        F(hideName);
        F(senderCID);
        size_t count = c.writing ? c.native.size() - sizeof(T) : c.wire.size() - c.pos;
        c.require(count > 0 && count <= MAX_CHAT_LENGTH);
        c.extent(sizeof(T) + count);
        c.string(sizeof(T), count);
        if (c.ok) c.require(strlen(reinterpret_cast<const char*>(c.native.data()+sizeof(T))) + 1 == count);
        break;
    }
    case MCMSG_MOVER_UPDATE: {
        using T = MCMSG_MoverUpdate;
        c.extent(sizeof(T));
        F(teamScore);
        F(playerScore);
        F(updateId);
        F(numRLEs);
        unsigned count = c.get<unsigned char>(offsetof(T, numRLEs));
        c.require(count <= MAX_MULTIPLAYER_MOVERS);
        c.extent(sizeof(T) + count * sizeof(MpMoverEntry));
        for (unsigned i=0; i<count && c.ok; ++i)
            fields(c, sizeof(T) + i*sizeof(MpMoverEntry), (MpMoverEntry*)nullptr);
        break;
    }
    case MCMSG_TURRET_UPDATE: {
        using T = MCMSG_TurretUpdate;
        c.extent(sizeof(T));
        F(updateId);
        size_t count = c.writing ? c.native.size() - sizeof(T) : c.wire.size() - c.pos;
        c.require(count <= MAX_MULTIPLAYER_TURRETS);
        c.extent(sizeof(T) + count);
        c.bytes(sizeof(T), count);
        // The legacy handler iterates the local turret roster, not packet length.
        if (!c.writing && c.ok) {
            c.native.resize(sizeof(T) + MAX_MULTIPLAYER_TURRETS, 255);
            c.expected = c.native.size();
        }
        break;
    }
    case MCMSG_MOVER_WEAPONFIRE_UPDATE:
    case MCMSG_TURRET_WEAPONFIRE_UPDATE: {
        c.extent(2);
        c.field<unsigned char>(1);
        unsigned groups = c.get<unsigned char>(1);
        bool turret = type == MCMSG_TURRET_WEAPONFIRE_UPDATE;
        unsigned limit = turret ? MAX_MULTIPLAYER_TURRETS : MAX_MULTIPLAYER_MOVERS;
        c.require(groups <= limit);
        size_t off = 2;
        for (unsigned i=0; i<groups && c.ok; ++i) {
            c.extent(off+2);
            c.bytes(off, 2);
            unsigned idx = c.get<unsigned char>(off), n = c.get<unsigned char>(off+1);
            c.require(idx < limit && n <= (turret ? MAX_TURRET_WEAPONFIRE_CHUNKS : MAX_WEAPONFIRE_CHUNKS));
            off += 2;
            c.extent(off + n*sizeof(unsigned long));
            for (unsigned j=0; j<n && c.ok; ++j) c.field<unsigned long>(off+j*sizeof(unsigned long));
            off += n*sizeof(unsigned long);
        }
        break;
    }
    case MCMSG_MOVER_CRITICAL_UPDATE: {
        using T = MCMSG_MoverCriticalUpdate;
        c.extent(sizeof(T));
        F(numCritHitChunks);
        F(numRadioChunks);
        size_t count = 0;
        for (unsigned i=0; i<MAX_MULTIPLAYER_MOVERS; ++i) {
            unsigned nc = c.get<unsigned char>(offsetof(T, numCritHitChunks)+i);
            unsigned nr = c.get<unsigned char>(offsetof(T, numRadioChunks)+i);
            c.require(nc <= MAX_CRITICALHIT_CHUNKS && nr <= MAX_RADIO_CHUNKS);
            count += nc + nr;
        }
        c.extent(sizeof(T) + count);
        c.bytes(sizeof(T), count);
        break;
    }
    case MCMSG_WEAPONHIT_UPDATE: {
        using T = MCMSG_WeaponHitUpdate;
        c.extent(sizeof(T));
        F(numWeaponHits);
        unsigned count = c.get<unsigned char>(offsetof(T, numWeaponHits));
        c.extent(sizeof(T) + count*sizeof(unsigned long));
        for (unsigned i=0; i<count && c.ok; ++i) c.field<unsigned long>(sizeof(T)+i*sizeof(unsigned long));
        break;
    }
    case MCMSG_WORLD_UPDATE: {
        using T = MCMSG_WorldUpdate;
        c.extent(sizeof(T));
        F(numWorldChanges);
        F(numArtilleryStrikes);
        unsigned count = c.get<unsigned char>(offsetof(T, numWorldChanges));
        c.require(c.get<unsigned char>(offsetof(T, numArtilleryStrikes)) == 0);
        c.extent(sizeof(T) + count*sizeof(MpWorldEntry));
        for (unsigned i=0; i<count && c.ok; ++i)
            fields(c, sizeof(T)+i*sizeof(MpWorldEntry), (MpWorldEntry*)nullptr);
        break;
    }
    default: return false;
    }
    return c.ok && (c.writing ? c.expected == c.native.size() : c.pos == c.wire.size());
}
#undef F
#undef S
} // namespace

bool encode(const void* data, size_t size, std::vector<unsigned char>& wire) {
    wire.clear();
    if (!data || size == 0 || size > MaxPacket*2) return false;
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::vector<unsigned char> native(bytes, bytes+size);
    wire = {'M','C','2','N', Version & 255, Version >> 8, bytes[0], 0};
    Cursor c{true, true, native, wire};
    if (!message(c, bytes[0]) || wire.size() > MaxPacket) { wire.clear(); return false; }
    return true;
}

bool decode(const void* data, size_t size, std::vector<unsigned char>& native) {
    native.clear();
    if (!data || size < 8 || size > MaxPacket) return false;
    const auto* bytes = static_cast<const unsigned char*>(data);
    if (memcmp(bytes, "MC2N", 4) || bytes[4] != (Version & 255) || bytes[5] != (Version >> 8) || bytes[7]) return false;
    std::vector<unsigned char> wire(bytes, bytes+size);
    native.push_back(bytes[6]);
    Cursor c{false, true, native, wire};
    if (!message(c, bytes[6])) { native.clear(); return false; }
    return true;
}
} // namespace MpWire
