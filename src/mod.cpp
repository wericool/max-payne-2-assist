#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string_view>

#include "settings.hpp"
#include "game_panel.hpp"

namespace {
using Byte = unsigned char;
struct Vec { float x, y, z; };
struct Matrix { Vec row[4]; };
struct Point { float x, y; };
struct Vertex { float x, y, z, rhw; DWORD color; };
struct Viewport { DWORD x, y, width, height; float nearZ, farZ; };

template<class T> T& field(void* object, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<Byte*>(object) + offset);
}
template<class T> T at(HMODULE module, size_t offset) {
    return reinterpret_cast<T>(reinterpret_cast<Byte*>(module) + offset);
}
template<class T> T vtable(void* object, size_t index) {
    return reinterpret_cast<T>((*reinterpret_cast<void***>(object))[index]);
}

using CharacterDestruct = void(__thiscall*)(void*);
using CharacterTick = void(__thiscall*)(void*, const void*);
using CharacterWeapons = void(__thiscall*)(void*, const void*, const Matrix*);
using GetPointer = void*(__thiscall*)(void*);
using GetTransform = const Matrix*(__thiscall*)(void*);
using GetVec = const Vec*(__thiscall*)(void*);
using GetBool = bool(__thiscall*)(void*);
using GetFloat = float(__thiscall*)(void*);
using HeadPosition = void(__thiscall*)(void*, Vec*);
using BoneOffset = void(__thiscall*)(void*, const char*, Matrix*);
using Project = const Vec*(__thiscall*)(void*, const Vec*);
using MouseUpdate = void(__thiscall*)(void*, float);
using SwapBuffer = void(__thiscall*)(void*, const RECT*);
using FindBone = void*(__thiscall*)(void*, const void*);
using LevelProjectile = void(__thiscall*)(void*, void*, void*, const Vec*, const Vec*, const void*);
using LoadProjectiles = void(__thiscall*)(void*, void*, const void*, const void*);

HMODULE gameObjects, graphics, levelRuntime, sharedDb, kf2, physics, messages;
CharacterTick originalPost;
CharacterDestruct originalDestruct;
CharacterDestruct originalItemUpdate;
CharacterDestruct originalProjectileUnload;
LoadProjectiles originalProjectileLoad;
LevelProjectile originalLevelProjectile;
CharacterWeapons originalWeapons;
GetPointer originalFirstScene;
GetFloat originalAccuracy;
std::array<GetFloat, 4> originalShake;
SwapBuffer originalSwap;
MouseUpdate originalMouseUpdate;
bool playerWeaponUpdate;
AssistSettings settings;
bool diagnostic, menuOpen, insertWasDown, mouseDown, mouseClicked;
int draggedRangeRow = -1;
int menuRow;
AssistPanel::Tab menuTab = AssistPanel::Tab::enemies;
Point menuCursor{};
bool enemyEspActive() { return settings.boxes || settings.skeleton || settings.head || settings.health; }
bool itemEspActive() { return settings.painkillers || settings.ammo || settings.weapons || settings.explosives; }
bool espActive() { return enemyEspActive() || itemEspActive(); }
wchar_t profilePath[MAX_PATH];
AssistPanel::Panel panel;
constexpr DWORD boxColors[] = {0xffff524e, 0xff53e3d4, 0xffffd15c};
constexpr DWORD boneColors[] = {0xffffb166, 0xffa9fff3, 0xfffff0a5};
DWORD playerSeen;
Vec playerPosition{};
unsigned shotCount;
wchar_t logFile[MAX_PATH];

constexpr const char* boneNames[] = {
    "Bip01 Pelvis", "Bip01 Spine1", "Bip01 Neck", "Bip01 Head",
    "Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand",
    "Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand",
    "Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot",
    "Bip01 R Thigh", "Bip01 R Calf", "Bip01 R Foot",
};
constexpr int parents[] = {-1, 0, 1, 2, 2, 4, 5, 2, 7, 8, 0, 10, 11, 0, 13, 14};
constexpr size_t boneCount = std::size(boneNames);

// The convex collision shape associated with the animated head bone.
constexpr size_t maxHeadVertices = 256;
struct HeadMesh {
    std::array<Vec, maxHeadVertices> world;
    int count;
};

struct Target {
    void* id;
    DWORD seen;
    Vec head, feet;
    HeadMesh headMesh;
    std::array<Vec, boneCount> bones;
    unsigned validBones;
};
std::array<Target, 256> targets;
enum class ItemKind { painkiller, ammo, weapon, explosive, unknown };
enum class Icon { health, ammo, weapon, blast, canister, barrel, cylinder, crate, tank, monitor,
                  grenade, molotov, melee, beretta, berettaDual, desertEagle, desertEagleDual,
                  sawedOff, shotgun, striker, ingram, ingramDual, mp5, carbine, kalashnikov, sniper, dragunov };
struct ExplosiveRef { void* object = nullptr; unsigned slot = 0; };
struct Item { Vec position; ItemKind kind; Icon icon; ExplosiveRef explosive; };
struct ItemMarker { Point position; ItemKind kind; Icon icon; ExplosiveRef explosive; };
constexpr size_t itemCapacity = 1024;
std::array<void*, 32768> detonatedExplosives;
std::array<Item, itemCapacity> items;
std::array<ItemMarker, itemCapacity> itemMarkers;
size_t itemCount, itemMarkerCount;
DWORD itemsSeen;
std::array<unsigned, 4> projectedItems;

std::array<Vertex, (targets.size() * (boneCount + 4 + maxHeadVertices) + itemCapacity * 24) * 2> lines;
std::array<Vertex, targets.size() * 12 + itemCapacity * 6> healthVertices;
unsigned lineVertices, barVertices;
unsigned projectedTargets, projectedBones, projectedHeads, projectedHealth;

void log(const char* message) {
    if (!diagnostic) return;
    HANDLE file = CreateFileW(logFile, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD count;
    WriteFile(file, message, static_cast<DWORD>(std::strlen(message)), &count, nullptr);
    CloseHandle(file);
}

struct LegacyString {
    DWORD allocator;
    union { char inlineText[16]; const char* heapText; };
    unsigned length, capacity;
    const char* text() const { return capacity < 16 ? inlineText : heapText; }
};
static_assert(sizeof(LegacyString) == 28);

bool itemEnabled(ItemKind kind) {
    switch (kind) {
    case ItemKind::painkiller: return settings.painkillers;
    case ItemKind::ammo: return settings.ammo;
    case ItemKind::weapon: return settings.weapons;
    case ItemKind::explosive: return settings.explosives;
    default: return false;
    }
}

bool containsAscii(std::string_view text, std::string_view word) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; };
    return std::search(text.begin(), text.end(), word.begin(), word.end(),
                       [&](char a, char b) { return lower(a) == lower(b); }) != text.end();
}

struct Pickup { ItemKind kind; Icon icon; };

Icon weaponIconFor(std::string_view message, size_t opening) {
    auto name = message.substr(opening + 1);
    const auto first = name.find_first_not_of(" \t");
    if (first == std::string_view::npos) return Icon::weapon;
    name.remove_prefix(first);
    name = name.substr(0, name.find_first_of(" \t,)"));
    // The authored order in the game's HUD sprite sheet.
    constexpr const char* names[] = {
        "grenade", "molotov", "melee", "beretta", "berettadual", "deserteagle", "deserteagledual",
        "sawedshotgun", "pumpshotgun", "striker", "ingram", "ingramdual", "mp5", "coltcommando",
        "kalashnikov", "sniper", "dragunov",
    };
    for (size_t row = 0; row < std::size(names); ++row) {
        if (name.size() == std::strlen(names[row]) && _strnicmp(name.data(), names[row], name.size()) == 0) {
            return static_cast<Icon>(static_cast<size_t>(Icon::grenade) + row);
        }
    }
    return Icon::weapon;
}

Pickup identifyPickup(void* definition) {
    using GetInt = int(__thiscall*)(void*);
    using GetName = const LegacyString*(__thiscall*)(void*);
    using GetMessage = const LegacyString*(__thiscall*)(void*, int);
    const auto* name = at<GetName>(sharedDb, 0x22220)(definition);
    if (_stricmp(name->text(), "Painkiller") == 0) return {ItemKind::painkiller, Icon::health};
    const int count = at<GetInt>(sharedDb, 0x12d50)(definition);
    bool ammo = false;
    for (int index = 0; index < std::clamp(count, 0, 64); ++index) {
        const auto& message = *at<GetMessage>(sharedDb, 0x12d60)(definition, index);
        const std::string_view text(message.text(), message.length);
        const auto open = text.find('(');
        if (open == std::string_view::npos) continue;
        const auto command = text.substr(0, open);
        if (containsAscii(command, "c_pickupweapon")) return {ItemKind::weapon, weaponIconFor(text, open)};
        ammo |= containsAscii(command, "c_pickupammo");
    }
    return ammo ? Pickup{ItemKind::ammo, Icon::ammo} : Pickup{ItemKind::unknown, Icon::weapon};
}

// The runtime FSM keeps parsed message vectors and state-keyed trees after the level string pool is released.
template<class Predicate>
bool searchMessageTree(void* tree, const Predicate& predicate) {
    const auto sentinel = field<void*>(tree, 4);
    const auto count = field<unsigned>(tree, 8);
    if (!sentinel || !count || count > 4096) return false;
    const auto search = [&](const auto& self, void* node, unsigned depth) -> bool {
        if (!node || node == sentinel || depth == 32) return false;
        return predicate(node) || self(self, field<void*>(node, 0), depth + 1) ||
               self(self, field<void*>(node, 8), depth + 1);
    };
    return search(search, field<void*>(sentinel, 4), 0);
}

bool isExplosionMessage(void* message) {
    if (!message || field<void*>(message, 0) != at<void*>(gameObjects, 0x121b70)) return false;
    const auto* projectile = static_cast<const char*>(message) + 0x30;
    return containsAscii({projectile, strnlen_s(projectile, 32)}, "damage_explosion");
}

bool hasExplosionMessage(void* list, unsigned receiver) {
    auto* first = field<void**>(list, 4);
    auto* end = field<void**>(list, 8);
    const auto bytes = reinterpret_cast<uintptr_t>(end) - reinterpret_cast<uintptr_t>(first);
    if (!first || bytes > 512 * sizeof(void*) || bytes % sizeof(void*)) return false;
    for (auto* cursor = first; cursor != end; ++cursor) {
        void* message = *cursor;
        if (isExplosionMessage(message) && field<unsigned>(message, 0x0c) == receiver) return true;
    }
    return false;
}

bool hasExplosionGroup(void* group, unsigned receiver) {
    auto* bytes = static_cast<Byte*>(group);
    return hasExplosionMessage(group, receiver) || hasExplosionMessage(bytes + 0x1c, receiver) ||
           searchMessageTree(bytes + 0x10, [receiver](void* node) {
               return hasExplosionMessage(static_cast<Byte*>(node) + 0x28, receiver);
           });
}

bool hasExplosionScript(void* object) {
    auto* bytes = static_cast<Byte*>(object);
    const unsigned receiver = field<unsigned short>(object, 0x1a0);
    if (hasExplosionGroup(bytes + 0x1fc, receiver)) return true;
    for (const unsigned offset : {0x228u, 0x234u, 0x240u}) {
        if (searchMessageTree(bytes + offset, [receiver](void* node) {
            void* group = field<void*>(node, 0x28);
            return group && hasExplosionGroup(group, receiver);
        })) return true;
    }
    return false;
}

std::string_view objectNamePart(std::string_view path) {
    const auto split = path.rfind("::");
    return split == std::string_view::npos ? path : path.substr(split + 2);
}

Icon explosiveIconFor(std::string_view path) {
    const auto name = objectNamePart(path);
    const auto prefab = path.rfind("::prefab::");
    const auto parent = prefab == std::string_view::npos ? std::string_view{} : objectNamePart(path.substr(0, prefab));
    const auto named = [&](std::string_view type) { return containsAscii(name, type) || containsAscii(parent, type); };
    if (containsAscii(name, "canister") || containsAscii(name, "jerry") ||
        containsAscii(name, "gascan") || containsAscii(name, "gas_can") ||
        containsAscii(name, "fuelcan") || containsAscii(name, "explosivecan") ||
        containsAscii(name, "explodingcan") ||
        (name.size() >= 3 && _strnicmp(name.data(), "can", 3) == 0)) return Icon::canister;
    if (named("barrel") || named("drum")) return Icon::barrel;
    if (named("cylinder") || named("bottle") || named("propane") || named("gasbott")) return Icon::cylinder;
    if (named("crate") || named("box")) return Icon::crate;
    if (named("tank")) return Icon::tank;
    if (named("monitor")) return Icon::monitor;
    return Icon::blast;
}

void* dynamicObjectArray() {
    using GetArray = void*(__fastcall*)(int);
    const int classId = *at<int*>(gameObjects, 0x198594);
    return classId < 0 ? nullptr : at<GetArray>(messages, 0x10000)(classId);
}

void* mappedDynamicObject(void* objects, unsigned index) {
    using IsValid = bool(__thiscall*)(void*, int);
    using GetMapped = void**(__thiscall*)(void*, int);
    if (index >= field<unsigned>(objects, 4) || !at<IsValid>(messages, 0xc2b0)(objects, index)) return nullptr;
    return *at<GetMapped>(messages, 0xc2c0)(objects, index);
}

bool hasDetonated(const ExplosiveRef& ref) {
    return ref.object && detonatedExplosives[ref.slot] == ref.object;
}

void __fastcall createLevelProjectile(void* self, void*, void* message, void* room,
                                      const Vec* position, const Vec* target, const void* rotation) {
    if (isExplosionMessage(message)) {
        const unsigned slot = field<unsigned>(message, 0x0c);
        if (slot < detonatedExplosives.size()) {
            if (void* objects = dynamicObjectArray()) {
                void* object = mappedDynamicObject(objects, slot);
                if (object && field<void*>(object, 0x34) == room && hasExplosionScript(object)) {
                    detonatedExplosives[slot] = object;
                    if (diagnostic) {
                        char messageText[100];
                        sprintf_s(messageText, "Explosive ESP removed after detonation: object %u\r\n", slot);
                        log(messageText);
                    }
                }
            }
        }
    }
    originalLevelProjectile(self, message, room, position, target, rotation);
}

void __fastcall unloadLevelProjectiles(void* self, void*) {
    detonatedExplosives.fill(nullptr);
    originalProjectileUnload(self);
}

void __fastcall loadLevelProjectiles(void* self, void*, void* file, const void* rooms, const void* materials) {
    detonatedExplosives.fill(nullptr);
    originalProjectileLoad(self, file, rooms, materials);
}

void captureExplosives() {
    using GetName = const LegacyString*(__thiscall*)(void*);
    void* objects = dynamicObjectArray();
    if (!objects) return;
    const unsigned slots = field<unsigned>(objects, 4);
    if (slots > detonatedExplosives.size()) return;
    for (unsigned index = 0; index < slots && itemCount < items.size(); ++index) {
        void* object = mappedDynamicObject(objects, index);
        if (detonatedExplosives[index] != object) detonatedExplosives[index] = nullptr;
        const ExplosiveRef explosive{object, index};
        if (hasDetonated(explosive)) continue;
        if (!object || !at<GetBool>(gameObjects, 0xc5b20)(object) || field<Byte>(object, 0x295)) continue;
        // Stock blastable props lose their dynamic-object hit points on impact or chain damage.
        if (!(field<float>(object, 0x2c8) > 0.0f)) continue;
        auto* base = static_cast<Byte*>(object) + 0x30;
        if (!at<GetBool>(levelRuntime, 0x1430)(base)) continue;
        const auto& state = *at<GetName>(gameObjects, 0xa6930)(object);
        if (containsAscii({state.text(), state.length}, "exploded")) continue;
        if (!hasExplosionScript(object)) continue;
        const auto& name = *at<GetName>(levelRuntime, 0x1420)(base);
        Vec position = *at<GetVec>(levelRuntime, 0x1aa0)(base);
        position.y += 0.30f;
        items[itemCount++] = {position, ItemKind::explosive, explosiveIconFor({name.text(), name.length}), explosive};
    }
}

void __fastcall captureLevelItems(void* implementation, void*) {
    originalItemUpdate(implementation);
    itemCount = 0;
    itemsSeen = GetTickCount();
    if (!itemEspActive()) return;
    using Next = void(__thiscall*)(void**);
    void* sentinel = field<void*>(implementation, 0x10);
    if (sentinel && (settings.painkillers || settings.ammo || settings.weapons)) {
        void* node = field<void*>(sentinel, 0);
        while (node && node != sentinel && itemCount < items.size()) {
            void* item = field<void*>(node, 0x0c);
            if (item && at<GetBool>(levelRuntime, 0x1430)(item)) {
                void* definition = field<void*>(item, 0x98);
                const Pickup pickup = definition ? identifyPickup(definition) : Pickup{ItemKind::unknown, Icon::weapon};
                if (itemEnabled(pickup.kind)) {
                    Vec position = *at<GetVec>(levelRuntime, 0x1aa0)(item);
                    position.y += 0.12f;
                    items[itemCount++] = {position, pickup.kind, pickup.icon, {}};
                }
            }
            at<Next>(gameObjects, 0xf0500)(&node);
        }
    }
    if (settings.explosives) captureExplosives();
}

Vec transform(const Matrix& m, const Vec& v) {
    return {
        v.x * m.row[0].x + v.y * m.row[1].x + v.z * m.row[2].x + m.row[3].x,
        v.x * m.row[0].y + v.y * m.row[1].y + v.z * m.row[2].y + m.row[3].y,
        v.x * m.row[0].z + v.y * m.row[1].z + v.z * m.row[2].z + m.row[3].z,
    };
}

bool isPlayer(void* character) {
    return field<Byte>(character, 0x59c) != 0;
}

void captureBones(void* character, Target& target) {
    target.validBones = 0;
    void* animation = field<void*>(character, 0x404);
    if (!animation) return;
    void* skeleton = at<GetPointer>(kf2, 0x1ca90)(animation);
    if (!skeleton) return;
    auto findBone = vtable<FindBone>(skeleton, 0xf8 / 4);
    const auto& world = *at<GetTransform>(levelRuntime, 0x1ab0)(static_cast<Byte*>(character) + 0x30);
    for (size_t index = 0; index < boneCount; ++index) {
        const char* name = boneNames[index];
        LegacyString lookup{};
        lookup.length = static_cast<unsigned>(std::strlen(name));
        if (lookup.length < sizeof(lookup.inlineText)) {
            std::memcpy(lookup.inlineText, name, lookup.length + 1);
            lookup.capacity = 15;
        } else {
            lookup.heapText = name;
            lookup.capacity = lookup.length;
        }
        if (!findBone(skeleton, &lookup)) continue;
        Matrix offset{};
        at<BoneOffset>(gameObjects, 0x38920)(character, name, &offset);
        target.bones[index] = transform(world, offset.row[3]);
        target.validBones |= (1u << index);
    }
}

void captureHeadMesh(void* character, HeadMesh& mesh) {
    using GetInt = int(__thiscall*)(void*);
    using GeometryAt = void*(__thiscall*)(void*, int);
    using GeometryBone = const char*(__thiscall*)(void*);
    using WriteTransform = void(__thiscall*)(void*, Matrix*);
    using LocalTransform = void(__thiscall*)(void*, Matrix*, int);

    mesh.count = 0;
    // Same rigid body and validity checks as X_Character::visualizeShootingTargets.
    void* body = field<void*>(character, 0x1a8);
    if (!body || !vtable<GetBool>(body, 0x30 / 4)(body) || !vtable<GetBool>(body, 0x0c / 4)(body)) return;

    const int geometryCount = at<GetInt>(physics, 0x1090)(body);
    if (geometryCount <= 0 || geometryCount > 128) return;

    for (int index = 0; index < geometryCount; ++index) {
        void* geometry = at<GeometryAt>(physics, 0x1590)(body, index);
        if (!geometry) continue;
        // The projectile hit code uses this identifier to resolve the skin's shooting target.
        const char* bone = at<GeometryBone>(physics, 0x50e0)(geometry);
        if (!bone || _stricmp(bone, "Bip01 Head") != 0) continue;
        const int count = at<GetInt>(physics, 0x4d20)(geometry);
        const Vec* vertices = at<GetVec>(physics, 0x4d10)(geometry);
        if (!vertices || count < 3 || count > static_cast<int>(mesh.world.size())) return;

        Matrix bodyWorld{}, local{};
        vtable<WriteTransform>(body, 0x38 / 4)(body, &bodyWorld);
        at<LocalTransform>(physics, 0x1c20)(body, &local, index);
        for (int vertex = 0; vertex < count; ++vertex) {
            mesh.world[vertex] = transform(bodyWorld, transform(local, vertices[vertex]));
        }
        mesh.count = count;
        if (diagnostic) {
            static bool reported;
            if (!reported) {
                char message[128];
                sprintf_s(message, "Bip01 Head collision mesh: %d vertices\r\n", count);
                log(message);
                reported = true;
            }
        }
        return;
    }
}

bool isDying(void* character) {
    return at<GetBool>(gameObjects, 0x55960)(static_cast<Byte*>(character) + 0x1e2);
}

float currentHealth(void* character) {
    return at<GetFloat>(gameObjects, 0xde10)(character);
}

bool canDrawTarget(void* character) {
    return at<GetBool>(gameObjects, 0xd790)(character) && field<Byte>(character, 0x580) &&
        !field<Byte>(character, 0x15d) && !field<Byte>(character, 0x15c) &&
        !isDying(character) && currentHealth(character) > 0.0f;
}

void clearTarget(Target& target) {
    if (diagnostic && !field<Byte>(target.id, 0x15c) &&
        (isDying(target.id) || currentHealth(target.id) <= 0.0f)) {
        log("ESP removed before the final dead flag\r\n");
    }
    target.id = nullptr;
}

void __fastcall destructCharacter(void* character, void*) {
    for (auto& target : targets) {
        if (target.id == character) {
            target.id = nullptr;
            break;
        }
    }
    originalDestruct(character);
}

void __fastcall postPhysics(void* character, void*, const void* timeUpdate) {
    originalPost(character, timeUpdate);
    const DWORD now = GetTickCount();
    if (isPlayer(character)) {
        if (at<GetBool>(gameObjects, 0xd790)(character)) {
            playerSeen = now;
            playerPosition = *at<GetVec>(gameObjects, 0xf7a0)(character);
        }
        return;
    }

    Target* slot = nullptr;
    Target* expired = nullptr;
    for (auto& target : targets) {
        if (target.id == character) {
            slot = &target;
            break;
        }
        if (!expired && (!target.id || now - target.seen > 350)) expired = &target;
    }

    // Auto-aim eligibility is authored for each character skin and can be changed by the level.
    if (!enemyEspActive() || !canDrawTarget(character) || !field<void*>(character, 0x404)) {
        if (slot) clearTarget(*slot);
        return;
    }
    if (!slot) slot = expired;
    if (!slot) return;
    slot->id = character;
    slot->seen = now;
    slot->feet = *at<GetVec>(gameObjects, 0xf7a0)(character);
    if (settings.boxes || settings.health) {
        at<HeadPosition>(gameObjects, 0x38e80)(character, &slot->head);
    }
    if (settings.head) captureHeadMesh(character, slot->headMesh);
    else slot->headMesh.count = 0;
    if (settings.skeleton) captureBones(character, *slot);
}

void __fastcall updateWeapons(void* character, void*, const void* timeUpdate, const Matrix* camera) {
    const bool preceding = playerWeaponUpdate;
    playerWeaponUpdate = isPlayer(character);
    originalWeapons(character, timeUpdate, camera);
    playerWeaponUpdate = preceding;
}

float __fastcall projectileAccuracy(void* projectile, void*) {
    using GetName = const LegacyString*(__thiscall*)(void*);
    const auto& name = *at<GetName>(sharedDb, 0x22220)(projectile);
    const bool playerBullet = name.length >= 7 &&
        _stricmp(name.text() + name.length - 7, "_player") == 0;
    const float original = originalAccuracy(projectile);
    if (playerBullet && diagnostic && shotCount++ < 16) {
        char message[160];
        sprintf_s(message, "Projectile %s: %.3f -> %.3f\r\n", name.text(), original,
                  settings.noSpread ? 0.0 : static_cast<double>(original));
        log(message);
    }
    return playerBullet && settings.noSpread ? 0.0f : original;
}

template<size_t index>
float __fastcall weaponShake(void* weapon, void*) {
    return playerWeaponUpdate && settings.noSpread ? 0.0f : originalShake[index](weapon);
}

void addLine(Point a, Point b, DWORD color) {
    if (lineVertices + 2 > lines.size()) return;
    lines[lineVertices++] = {a.x, a.y, 0.0f, 1.0f, color};
    lines[lineVertices++] = {b.x, b.y, 0.0f, 1.0f, color};
}

// The projected silhouette of a convex mesh is the convex hull of its projected vertices.
template<class ProjectPoint>
bool drawHeadSilhouette(const HeadMesh& mesh, ProjectPoint project, DWORD color) {
    if (mesh.count < 3) return false;
    std::array<Point, maxHeadVertices> projected;
    size_t count = 0;
    for (int index = 0; index < mesh.count; ++index) {
        Point point;
        if (project(mesh.world[index], point)) projected[count++] = point;
    }
    if (count < 3) return false;

    const auto compare = [](const Point& a, const Point& b) {
        return a.x == b.x ? a.y < b.y : a.x < b.x;
    };
    std::sort(projected.begin(), projected.begin() + count, compare);
    if (projected[count - 1].x <= 0 || projected[0].x >= 1) return false;
    const auto cross = [](const Point& a, const Point& b, const Point& c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    std::array<Point, maxHeadVertices * 2> hull;
    size_t size = 0;
    const auto append = [&](const Point& point, size_t minimum) {
        while (size >= minimum && cross(hull[size - 2], hull[size - 1], point) <= 0) --size;
        hull[size++] = point;
    };
    for (size_t index = 0; index < count; ++index) append(projected[index], 2);
    const size_t upperMinimum = size + 1;
    for (size_t index = count - 1; index > 0; --index) append(projected[index - 1], upperMinimum);
    if (size < 4) return false;
    bool aboveBottom = false, belowTop = false;
    for (size_t index = 0; index < size; ++index) {
        aboveBottom |= hull[index].y < 1;
        belowTop |= hull[index].y > 0;
    }
    if (!aboveBottom || !belowTop) return false;
    for (size_t index = 1; index < size; ++index) addLine(hull[index - 1], hull[index], color);
    return true;
}

void addBarRect(float left, float top, float right, float bottom, DWORD color) {
    if (barVertices + 6 > healthVertices.size()) return;
    const Vertex a{left, top, 0, 1, color}, b{right, top, 0, 1, color};
    const Vertex c{left, bottom, 0, 1, color}, d{right, bottom, 0, 1, color};
    for (const auto& vertex : {a, b, c, c, b, d}) healthVertices[barVertices++] = vertex;
}

void drawHealthBar(float left, float top, float bottom, float aspectScale, float health) {
    const float barWidth = std::clamp((bottom - top) * 0.049f, 0.0025f, 0.0055f) * aspectScale;
    const float right = left - 0.003f * aspectScale;
    const float edge = right - barWidth;
    const float fillTop = bottom - (bottom - top) * health;
    const auto red = static_cast<DWORD>(235.0f - 170.0f * health);
    const auto green = static_cast<DWORD>(70.0f + 150.0f * health);
    const DWORD color = 0xff00005au | (red << 16) | (green << 8);
    addBarRect(edge, top, right, bottom, 0xff1b1b22);
    addBarRect(edge, fillTop, right, bottom, color);
    ++projectedHealth;
}

bool withinEspDistance(const Vec& player, const Vec& target, float limitSquared) {
    if (limitSquared == 0.0f) return true;
    const float x = target.x - player.x;
    const float y = target.y - player.y;
    const float z = target.z - player.z;
    return x * x + y * y + z * z <= limitSquared;
}

void projectTargets(void* camera) {
    lineVertices = barVertices = projectedTargets = projectedBones = projectedHeads = projectedHealth = 0;
    itemMarkerCount = 0;
    projectedItems.fill(0);
    const DWORD now = GetTickCount();
    if (!espActive() || !playerSeen || now - playerSeen > 350) return;

    auto project = at<Project>(graphics, 0x15bd0);
    const float width = field<float>(camera, 0x20c);
    const float height = field<float>(camera, 0x210);
    if (width <= 0 || height <= 0) return;
    auto screenPoint = [&](const Vec& world, Point& out) {
        const Vec p = *project(camera, &world);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
            p.z <= 0.0f || p.z > 16.0f) return false;
        out = {p.x / 640.0f, p.y / 480.0f};
        return std::abs(out.x) < 8 && std::abs(out.y) < 8;
    };

    if (itemEspActive() && itemsSeen && now - itemsSeen <= 350) {
        const float limit = static_cast<float>(espDistances[settings.itemDistance].meters);
        for (size_t index = 0; index < itemCount; ++index) {
            const auto& item = items[index];
            if (!itemEnabled(item.kind) || hasDetonated(item.explosive) ||
                !withinEspDistance(playerPosition, item.position, limit * limit)) continue;
            Point position;
            if (!screenPoint(item.position, position) || position.x <= 0 || position.x >= 1 ||
                position.y <= 0 || position.y >= 1) continue;
            itemMarkers[itemMarkerCount++] = {position, item.kind, item.icon, item.explosive};
            ++projectedItems[static_cast<size_t>(item.kind)];
        }
    }
    if (!enemyEspActive()) return;
    const DWORD color = boxColors[settings.color];
    const float limit = static_cast<float>(espDistances[settings.espDistance].meters);
    const float limitSquared = limit * limit;
    for (auto& target : targets) {
        if (!target.id || now - target.seen > 350) continue;
        // Projectiles can finish a target after the post-physics snapshot in the same frame.
        if (!canDrawTarget(target.id)) {
            clearTarget(target);
            continue;
        }
        if (!withinEspDistance(playerPosition, target.feet, limitSquared)) continue;
        if (settings.head && drawHeadSilhouette(target.headMesh, screenPoint, color)) ++projectedHeads;
        Point head{}, feet{};
        if ((settings.boxes || settings.health) && screenPoint(target.head, head) && screenPoint(target.feet, feet)) {
            const float bodyHeight = feet.y - head.y;
            const float halfWidth = bodyHeight * 0.23f * height / width;
            const float left = (head.x + feet.x) / 2 - halfWidth;
            const float right = left + 2 * halfWidth;
            const float top = head.y - bodyHeight * 0.11f;
            if (bodyHeight >= 0.004f && right > 0 && left < 1 && feet.y > 0 && top < 1) {
                if (settings.boxes) {
                    ++projectedTargets;
                    addLine({left, top}, {right, top}, color);
                    addLine({right, top}, {right, feet.y}, color);
                    addLine({right, feet.y}, {left, feet.y}, color);
                    addLine({left, feet.y}, {left, top}, color);
                }
                if (settings.health) {
                    const float health = at<GetFloat>(gameObjects, 0xde20)(target.id);
                    if (std::isfinite(health) && health > 0.0f) {
                        drawHealthBar(left, top, feet.y, height / width, std::clamp(health, 0.0f, 1.0f));
                    }
                }
            }
        }
        if (!settings.skeleton) continue;
        for (size_t index = 1; index < boneCount; ++index) {
            const int parent = parents[index];
            const unsigned mask = (1u << index) | (1u << parent);
            if ((target.validBones & mask) != mask) continue;
            Point a, b;
            if (screenPoint(target.bones[index], a) && screenPoint(target.bones[parent], b)) {
                ++projectedBones;
                addLine(a, b, boneColors[settings.color]);
            }
        }
    }
}

struct IconEdge { int x1, y1, x2, y2; };

constexpr IconEdge healthIcon[] = {
    {-2,-7,2,-7}, {2,-7,2,-2}, {2,-2,7,-2}, {7,-2,7,2}, {7,2,2,2}, {2,2,2,7},
    {2,7,-2,7}, {-2,7,-2,2}, {-2,2,-7,2}, {-7,2,-7,-2}, {-7,-2,-2,-2}, {-2,-2,-2,-7},
};
constexpr IconEdge ammoIcon[] = {
    {-4,7,4,7}, {-4,7,-4,-2}, {4,7,4,-2}, {-4,-2,0,-8}, {0,-8,4,-2},
    {-4,-1,4,-1}, {-4,4,4,4},
};
constexpr IconEdge weaponIcon[] = {
    {-8,-5,8,-5}, {8,-5,8,0}, {8,0,1,0}, {1,0,-1,5}, {-1,5,-6,5},
    {-6,5,-4,0}, {-4,0,-8,0}, {-8,0,-8,-5}, {4,-5,4,-3}, {-4,0,1,0},
};
constexpr IconEdge explosionIcon[] = {
    {0,-8,2,-4}, {2,-4,6,-6}, {6,-6,5,-2}, {5,-2,8,0}, {8,0,5,2}, {5,2,6,6},
    {6,6,2,4}, {2,4,0,8}, {0,8,-2,4}, {-2,4,-6,6}, {-6,6,-5,2}, {-5,2,-8,0},
    {-8,0,-5,-2}, {-5,-2,-6,-6}, {-6,-6,-2,-4}, {-2,-4,0,-8},
};
constexpr IconEdge canisterIcon[] = {
    {-6,-4,-3,-4}, {-3,-4,-3,-8}, {-3,-8,4,-8}, {4,-8,4,-5}, {4,-5,7,-2}, {7,-2,7,7},
    {7,7,-6,7}, {-6,7,-6,-4}, {0,-5,3,-5}, {-3,-1,4,4}, {4,-1,-3,4},
};
constexpr IconEdge barrelIcon[] = {
    {-6,-6,6,-6}, {6,-6,7,6}, {7,6,5,8}, {5,8,-5,8}, {-5,8,-7,6}, {-7,6,-6,-6},
    {-6,-6,-4,-8}, {-4,-8,4,-8}, {4,-8,6,-6}, {-6,-2,6,-2}, {-7,4,7,4},
};
constexpr IconEdge cylinderIcon[] = {
    {-2,-8,2,-8}, {2,-8,2,-5}, {2,-5,5,-3}, {5,-3,5,6}, {5,6,3,8}, {3,8,-3,8},
    {-3,8,-5,6}, {-5,6,-5,-3}, {-5,-3,-2,-5}, {-2,-5,-2,-8}, {-5,2,5,2},
};
constexpr IconEdge crateIcon[] = {
    {-8,-6,8,-6}, {8,-6,8,6}, {8,6,-8,6}, {-8,6,-8,-6}, {-7,-5,7,5}, {7,-5,-7,5},
};
constexpr IconEdge tankIcon[] = {
    {-7,-5,7,-5}, {7,-5,9,-3}, {9,-3,9,3}, {9,3,7,5}, {7,5,-7,5}, {-7,5,-9,3},
    {-9,3,-9,-3}, {-9,-3,-7,-5}, {-4,5,-4,8}, {4,5,4,8}, {1,-5,1,-8}, {1,-8,5,-8},
};
constexpr IconEdge monitorIcon[] = {
    {-8,-7,8,-7}, {8,-7,8,4}, {8,4,-8,4}, {-8,4,-8,-7}, {0,4,0,7}, {-4,7,4,7},
    {-5,0,-2,-3}, {-2,-3,1,1}, {1,1,5,-2},
};
struct ItemIcon { const IconEdge* edges; size_t count; DWORD color; };
constexpr ItemIcon itemIcons[] = {
    {healthIcon, std::size(healthIcon), 0xff79f6a5},
    {ammoIcon, std::size(ammoIcon), 0xfff9dc85},
    {weaponIcon, std::size(weaponIcon), 0xff77d8ff},
    {explosionIcon, std::size(explosionIcon), 0xffff8b63},
    {canisterIcon, std::size(canisterIcon), 0xffff8b63},
    {barrelIcon, std::size(barrelIcon), 0xffffad68},
    {cylinderIcon, std::size(cylinderIcon), 0xffffb58b},
    {crateIcon, std::size(crateIcon), 0xffffca86},
    {tankIcon, std::size(tankIcon), 0xffff9c9a},
    {monitorIcon, std::size(monitorIcon), 0xffffc0a4},
};
static_assert(std::size(itemIcons) == static_cast<size_t>(Icon::grenade));

struct WeaponVertex { float x, y, z, rhw; DWORD color; float u, v; };
std::array<WeaponVertex, itemCapacity * 6> weaponVertices;

void* weaponTexture(void* device) {
    using GetMap = void*(__fastcall*)();
    using ValidateMap = void(__thiscall*)(void*);
    using Ref = ULONG(WINAPI*)(void*);
    static void* texture = nullptr;
    static void* lastDevice = nullptr;
    static DWORD lastSearch = 0;
    if (device != lastDevice) {
        if (texture) vtable<Ref>(texture, 2)(texture);
        texture = nullptr;
        lastDevice = device;
        lastSearch = 0;
    }
    if (texture) return texture;
    const DWORD now = GetTickCount();
    if (lastSearch && now - lastSearch < 1000) return nullptr;
    lastSearch = now;

    const auto next = at<GetMap>(graphics, 0x255d0);
    for (void* map = at<GetMap>(graphics, 0x25140)(); map; map = next()) {
        const auto& name = field<LegacyString>(map, 0xb0);
        if (_stricmp(name.text(), "HUD: Weapon inventory") != 0) continue;
        void* driverMap = field<void*>(map, 4);
        if (!driverMap || !field<void*>(driverMap, 0x30)) {
            at<ValidateMap>(graphics, 0x25ce0)(map);
            driverMap = field<void*>(map, 4);
        }
        texture = driverMap ? field<void*>(driverMap, 0x30) : nullptr;
        if (texture) {
            vtable<Ref>(texture, 1)(texture);
            log("Weapon ESP uses the game's HUD texture\r\n");
        }
        return texture;
    }
    return nullptr;
}

unsigned drawItemIcons(const Viewport& viewport, bool useWeaponTexture) {
    const float scale = std::clamp(static_cast<float>(viewport.height) / 1000.0f, 0.8f, 1.6f);
    unsigned weaponCount = 0;
    for (size_t index = 0; index < itemMarkerCount; ++index) {
        const auto& marker = itemMarkers[index];
        if (hasDetonated(marker.explosive)) continue;
        const float x = static_cast<float>(viewport.x) + marker.position.x * static_cast<float>(viewport.width);
        const float y = static_cast<float>(viewport.y) + marker.position.y * static_cast<float>(viewport.height);
        const bool atlasIcon = marker.icon >= Icon::grenade;
        if (atlasIcon && useWeaponTexture) {
            const float left = x - 20 * scale, right = x + 20 * scale;
            const float top = y - 10 * scale, bottom = y + 10 * scale;
            addBarRect(left, top, right, bottom, 0xff20242b);
            const unsigned row = static_cast<unsigned>(marker.icon) - static_cast<unsigned>(Icon::grenade);
            const float v1 = row / 18.0f, v2 = (row + 1) / 18.0f;
            constexpr DWORD tint = 0xffc7efff;
            const WeaponVertex a{left, top, 0, 1, tint, 0, v1}, b{right, top, 0, 1, tint, 1, v1};
            const WeaponVertex c{left, bottom, 0, 1, tint, 0, v2}, d{right, bottom, 0, 1, tint, 1, v2};
            const WeaponVertex quad[] = {a, b, c, c, b, d};
            std::copy(std::begin(quad), std::end(quad), weaponVertices.begin() + weaponCount);
            weaponCount += 6;
            continue;
        }
        const auto& glyph = itemIcons[static_cast<size_t>(atlasIcon ? Icon::weapon : marker.icon)];
        const float radius = 10 * scale;
        addBarRect(x - radius, y - radius, x + radius, y + radius, 0xff20242b);
        for (size_t edge = 0; edge < glyph.count; ++edge) {
            const auto& v = glyph.edges[edge];
            addLine({x + v.x1 * scale, y + v.y1 * scale}, {x + v.x2 * scale, y + v.y2 * scale}, glyph.color);
        }
    }
    return weaponCount;
}

void drawOverlay(void* device, unsigned width, unsigned height) {
    using Call = HRESULT(WINAPI*)(void*);
    using State = HRESULT(WINAPI*)(void*, DWORD);
    using StateValue = HRESULT(WINAPI*)(void*, DWORD, DWORD);
    using Stage = HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD);
    using SetTexture = HRESULT(WINAPI*)(void*, DWORD, void*);
    using GetViewport = HRESULT(WINAPI*)(void*, Viewport*);
    using SetViewport = HRESULT(WINAPI*)(void*, const Viewport*);
    using CreateState = HRESULT(WINAPI*)(void*, DWORD, DWORD*);
    using Draw = HRESULT(WINAPI*)(void*, DWORD, UINT, const void*, UINT);

    Viewport vp{};
    if (FAILED(vtable<GetViewport>(device, 41)(device, &vp))) return;
    const Viewport full{0, 0, width, height, 0.0f, 1.0f};
    DWORD saved = 0;
    if (FAILED(vtable<CreateState>(device, 57)(device, 1, &saved))) return; // D3DSBT_ALL
    if (SUCCEEDED(vtable<Call>(device, 34)(device))) {
        const auto overlayViewport = menuOpen || itemMarkerCount ? full : vp;
        vtable<SetViewport>(device, 40)(device, &overlayViewport);
        auto renderState = vtable<StateValue>(device, 50);
        auto stage = vtable<Stage>(device, 63);
        // Draw transformed diffuse lines in screen space, after the room meshes.
        renderState(device, 7, 0);   // D3DRS_ZENABLE
        renderState(device, 14, 0);  // D3DRS_ZWRITEENABLE
        renderState(device, 15, 0);  // D3DRS_ALPHATESTENABLE
        renderState(device, 27, 0);  // D3DRS_ALPHABLENDENABLE
        renderState(device, 28, 0);  // D3DRS_FOGENABLE
        renderState(device, 22, 1);  // D3DRS_CULLMODE = D3DCULL_NONE
        renderState(device, 137, 0); // D3DRS_LIGHTING
        renderState(device, 168, 15);// D3DRS_COLORWRITEENABLE
        vtable<State>(device, 76)(device, 0x44); // D3DFVF_XYZRHW | D3DFVF_DIFFUSE
        vtable<State>(device, 88)(device, 0);
        vtable<SetTexture>(device, 61)(device, 0, nullptr);
        stage(device, 0, 1, 2); // COLOROP = SELECTARG1
        stage(device, 0, 2, 0); // COLORARG1 = DIFFUSE
        stage(device, 0, 4, 2); // ALPHAOP = SELECTARG1
        stage(device, 0, 5, 0); // ALPHAARG1 = DIFFUSE
        stage(device, 1, 1, 1); // COLOROP = DISABLE
        const auto toViewport = [&vp](Vertex* vertices, unsigned count) {
            for (unsigned index = 0; index < count; ++index) {
                vertices[index].x = static_cast<float>(vp.x) + vertices[index].x * static_cast<float>(vp.width);
                vertices[index].y = static_cast<float>(vp.y) + vertices[index].y * static_cast<float>(vp.height);
            }
        };
        toViewport(lines.data(), lineVertices);
        toViewport(healthVertices.data(), barVertices);
        void* atlas = projectedItems[static_cast<size_t>(ItemKind::weapon)] ? weaponTexture(device) : nullptr;
        const unsigned weaponCount = drawItemIcons(full, atlas != nullptr);
        const auto barStatus = barVertices ? vtable<Draw>(device, 72)(
            device, 4, barVertices / 3, healthVertices.data(), sizeof(Vertex)) : S_OK;
        const auto status = lineVertices ?
            vtable<Draw>(device, 72)(device, 2, lineVertices / 2, lines.data(), sizeof(Vertex)) : S_OK;
        if (weaponCount) {
            vtable<State>(device, 76)(device, 0x144); // D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1
            vtable<SetTexture>(device, 61)(device, 0, atlas);
            stage(device, 0, 1, 4); // COLOROP = MODULATE
            stage(device, 0, 2, 2); // COLORARG1 = TEXTURE
            stage(device, 0, 3, 0); // COLORARG2 = DIFFUSE
            stage(device, 0, 5, 2); // ALPHAARG1 = TEXTURE
            stage(device, 0, 11, 0); // TEXCOORDINDEX
            stage(device, 0, 13, 3); // ADDRESSU = CLAMP
            stage(device, 0, 14, 3); // ADDRESSV = CLAMP
            stage(device, 0, 16, 2); // MAGFILTER = LINEAR
            stage(device, 0, 17, 2); // MINFILTER = LINEAR
            stage(device, 0, 24, 0); // TEXTURETRANSFORMFLAGS = DISABLE
            renderState(device, 19, 5); // D3DBLEND_SRCALPHA
            renderState(device, 20, 6); // D3DBLEND_INVSRCALPHA
            renderState(device, 27, 1);
            vtable<Draw>(device, 72)(device, 4, weaponCount / 3, weaponVertices.data(), sizeof(WeaponVertex));
            renderState(device, 27, 0);
            stage(device, 0, 1, 2); // COLOROP = SELECTARG1
        }
        if (menuOpen) {
            const float scale = std::clamp(static_cast<float>(height) / 750.0f, 0.65f, 1.4f);
            const float x = (static_cast<float>(width) - AssistPanel::width * scale) * 0.5f;
            const float y = (static_cast<float>(height) - AssistPanel::height * scale) * 0.30f;
            const auto menuResult = panel.draw(device, x, y, scale, menuTab, menuRow, settings);
            const float cx = x + menuCursor.x * scale, cy = y + menuCursor.y * scale;
            const auto cursorTriangle = [&](float size, DWORD color) {
                const Vertex vertices[] = {{cx, cy, 0, 1, color}, {cx + 6 * size, cy + 18 * size, 0, 1, color},
                    {cx + 14 * size, cy + 11 * size, 0, 1, color}};
                vtable<Draw>(device, 72)(device, 4, 1, vertices, sizeof(Vertex));
            };
            vtable<State>(device, 76)(device, 0x44);
            vtable<SetTexture>(device, 61)(device, 0, nullptr);
            stage(device, 0, 2, 0);
            stage(device, 0, 5, 0);
            cursorTriangle(scale * 1.18f, 0xff16191c);
            cursorTriangle(scale, 0xffe9fcfa);
            if (diagnostic) {
                static HRESULT last = E_PENDING;
                if (menuResult != last) {
                    char message[140];
                    sprintf_s(message, "Menu draw: hr=%08lX, drawable %ux%u\r\n", menuResult, width, height);
                    log(message);
                    last = menuResult;
                }
            }
        }
        if (diagnostic) {
            static std::array<unsigned, 4> lastItems{};
            static unsigned reports;
            if (projectedItems != lastItems && reports++ < 40) {
                char message[160];
                sprintf_s(message, "Item ESP: %u health, %u ammo, %u weapons, %u explosives\r\n",
                          projectedItems[0], projectedItems[1], projectedItems[2], projectedItems[3]);
                log(message);
                lastItems = projectedItems;
            }
        }
        if (diagnostic) {
            static unsigned lastCount, lastBones, lastHeads, lastHealth, reports;
            if ((lastCount != projectedTargets || lastBones != projectedBones || lastHeads != projectedHeads ||
                 lastHealth != projectedHealth) &&
                reports++ < 30) {
                char message[180];
                sprintf_s(message, "ESP: %u boxes, %u bone segments, %u heads, %u health bars, %ux%u, hr=%08lX/%08lX\r\n",
                          projectedTargets, projectedBones, projectedHeads, projectedHealth, vp.width, vp.height,
                          status, barStatus);
                log(message);
                lastCount = projectedTargets;
                lastBones = projectedBones;
                lastHeads = projectedHeads;
                lastHealth = projectedHealth;
            }
        }
        vtable<Call>(device, 35)(device);
        vtable<State>(device, 54)(device, saved);
        vtable<SetViewport>(device, 40)(device, &vp);
    }
    vtable<State>(device, 56)(device, saved);
}

int distanceForRow(int row) {
    return row == AssistPanel::enemyRangeRow ? settings.espDistance : settings.itemDistance;
}

void setEspDistance(int row, int step) {
    const bool enemies = row == AssistPanel::enemyRangeRow;
    int& current = enemies ? settings.espDistance : settings.itemDistance;
    step = std::clamp(step, 0, lastEspDistance);
    if (step == current) return;
    current = step;
    writeSetting(profilePath, enemies ? L"ESPDistance" : L"ESPItemDistance", step);
    panel.invalidate();
    if (diagnostic) {
        char message[90];
        sprintf_s(message, "%s ESP distance: %d m (0 = whole map)\r\n",
                  enemies ? "Enemy" : "Item", espDistances[step].meters);
        log(message);
    }
}

void changeMenuRow(int row) {
    const auto toggleItem = [](bool& enabled, const wchar_t* name) {
        enabled = !enabled;
        writeSetting(profilePath, name, enabled);
    };
    switch (row) {
    case AssistPanel::painkillerRow: toggleItem(settings.painkillers, L"ESPPainkillers"); break;
    case AssistPanel::ammoRow: toggleItem(settings.ammo, L"ESPAmmo"); break;
    case AssistPanel::weaponRow: toggleItem(settings.weapons, L"ESPWeapons"); break;
    case AssistPanel::explosiveRow: toggleItem(settings.explosives, L"ESPExplosives"); break;
    case 0:
        settings.boxes = !settings.boxes;
        writeSetting(profilePath, L"ESPBoxes", settings.boxes);
        log(settings.boxes ? "Boxes on\r\n" : "Boxes off\r\n");
        break;
    case 1:
        settings.skeleton = !settings.skeleton;
        writeSetting(profilePath, L"ESPSkeleton", settings.skeleton);
        log(settings.skeleton ? "Skeleton on\r\n" : "Skeleton off\r\n");
        break;
    case 2:
        settings.head = !settings.head;
        writeSetting(profilePath, L"ESPHead", settings.head);
        log(settings.head ? "Head marker on\r\n" : "Head marker off\r\n");
        break;
    case 3:
        settings.noSpread = !settings.noSpread;
        writeSetting(profilePath, L"NoSpread", settings.noSpread);
        log(settings.noSpread ? "No spread on\r\n" : "No spread off\r\n");
        break;
    case 4:
        settings.color = (settings.color + 1) % 3;
        writeSetting(profilePath, L"Color", settings.color);
        log("ESP color changed\r\n");
        break;
    case 5:
        settings.health = !settings.health;
        writeSetting(profilePath, L"ESPHealth", settings.health);
        break;
    }
    panel.invalidate();
}

void __fastcall updateMouse(void* mouse, void*, float delta) {
    if (!menuOpen) {
        originalMouseUpdate(mouse, delta);
        return;
    }

    // Read DirectInput normally, then keep its movement and clicks out of the gameplay state.
    auto* memory = static_cast<Byte*>(mouse);
    Byte position[40];
    std::memcpy(position, memory + 91, sizeof(position));
    originalMouseUpdate(mouse, delta);
    const LONG x = field<LONG>(mouse, 9), y = field<LONG>(mouse, 13);
    const bool button = (memory[21] & 0x80) != 0;
    if (x || y) {
        menuCursor.x = std::clamp(menuCursor.x + static_cast<float>(x), 0.0f, float(AssistPanel::width - 1));
        menuCursor.y = std::clamp(menuCursor.y + static_cast<float>(y), 0.0f, float(AssistPanel::height - 1));
        const int hover = AssistPanel::hitRow(menuTab, menuCursor.x, menuCursor.y);
        if (draggedRangeRow < 0 && menuRow != hover) {
            menuRow = hover;
            panel.invalidate();
        }
    }
    mouseClicked |= button && !mouseDown;
    mouseDown = button;

    std::memset(memory + 9, 0, 20);
    std::memcpy(memory + 91, position, sizeof(position));
    // The input code derives relative motion from pairs of current and previous coordinates.
    std::memcpy(memory + 99, memory + 91, 8);
    std::memcpy(memory + 115, memory + 107, 8);
    std::memcpy(memory + 127, memory + 123, 4);
    auto* buttons = field<Byte*>(mouse, 73);
    const int count = std::clamp(field<int>(mouse, 45), 0, 8);
    if (buttons) {
        for (int index = 0; index < count; ++index) {
            std::memset(buttons + index * 15, 0, 4);
            std::memset(buttons + index * 15 + 8, 0, 2);
        }
    }
}

void updateMenu() {
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    if (foregroundPid != GetCurrentProcessId()) {
        insertWasDown = false;
        mouseClicked = false;
        draggedRangeRow = -1;
        return;
    }
    const bool insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    const bool toggle = insertDown && !insertWasDown;
    insertWasDown = insertDown;
    if (toggle) {
        menuOpen = !menuOpen;
        mouseClicked = false;
        draggedRangeRow = -1;
        mouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (AssistPanel::isRangeRow(menuRow)) {
            menuCursor = {float(AssistPanel::rangePosition(distanceForRow(menuRow))),
                          float(AssistPanel::rangeTrack(menuRow))};
        } else {
            menuCursor = {float(AssistPanel::width / 2),
                          float(AssistPanel::rowTop(AssistPanel::firstRow(menuTab)) + 16)};
        }
        if (!menuOpen) panel.clear();
        log(menuOpen ? "Menu open\r\n" : "Menu closed\r\n");
    }
    if (!menuOpen) return;
    if (mouseClicked) {
        mouseClicked = false;
        if (AssistPanel::overTabs(menuCursor.x, menuCursor.y)) {
            menuTab = menuCursor.x < AssistPanel::tabDivider ? AssistPanel::Tab::enemies : AssistPanel::Tab::items;
            menuRow = -1;
            draggedRangeRow = -1;
            panel.invalidate();
            log(menuTab == AssistPanel::Tab::enemies ? "Enemy tab selected\r\n" : "Item tab selected\r\n");
            return;
        }
        const int row = AssistPanel::hitRow(menuTab, menuCursor.x, menuCursor.y);
        if (AssistPanel::isRangeRow(row)) {
            menuRow = row;
            panel.invalidate();
            if (menuCursor.y >= AssistPanel::rangeTrack(row) - 15) {
                draggedRangeRow = row;
                setEspDistance(row, AssistPanel::rangeStepAt(menuCursor.x));
            }
        } else if (row >= 0) {
            changeMenuRow(row);
        }
    }
    if (draggedRangeRow >= 0) {
        if (mouseDown) setEspDistance(draggedRangeRow, AssistPanel::rangeStepAt(menuCursor.x));
        else draggedRangeRow = -1;
    }
}

void __fastcall swapBuffer(void* driver, void*, const RECT* rect) {
    const HMODULE driverModule = GetModuleHandleW(L"e2_d3d8_driver_mfc.dll");
    if (driverModule && vtable<SwapBuffer>(driver, 0x58 / 4) == at<SwapBuffer>(driverModule, 0x1ff60)) {
        updateMenu();
        if (void* device = field<void*>(driver, 0x100)) {
            const unsigned width = field<unsigned>(driver, 4);
            const unsigned height = field<unsigned>(driver, 8);
            if (diagnostic) {
                static unsigned lastWidth, lastHeight;
                if (width != lastWidth || height != lastHeight) {
                    char message[120];
                    sprintf_s(message, "Direct3D drawable: %ux%u\r\n", width, height);
                    log(message);
                    lastWidth = width;
                    lastHeight = height;
                }
            }
            if (width && height && (menuOpen || (espActive() && (lineVertices || barVertices || itemMarkerCount)))) {
                if (!espActive()) {
                    lineVertices = barVertices = 0;
                    itemMarkerCount = 0;
                }
                drawOverlay(device, width, height);
            }
        }
    }
    lineVertices = barVertices = 0;
    itemMarkerCount = 0;
    originalSwap(driver, rect);
}

bool writeMemory(void* address, const void* bytes, SIZE_T size) {
    DWORD previous;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(address, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    DWORD unused;
    VirtualProtect(address, size, previous, &unused);
    return true;
}

template<size_t Size, class Function, class Hook>
bool installJump(Byte* entry, const Byte (&prefix)[Size], Function& original, Hook replace) {
    static_assert(Size >= 5);
    if (std::memcmp(entry, prefix, Size)) return false;
    constexpr size_t trampolineSize = Size + 5;
    auto* trampoline = static_cast<Byte*>(VirtualAlloc(nullptr, trampolineSize, MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    auto jump = [](Byte* from, std::uintptr_t target, Byte* bytes) {
        bytes[0] = 0xe9;
        const auto relative = static_cast<DWORD>(target - reinterpret_cast<std::uintptr_t>(from + 5));
        std::memcpy(bytes + 1, &relative, sizeof(relative));
    };
    std::memcpy(trampoline, prefix, Size);
    jump(trampoline + Size, reinterpret_cast<std::uintptr_t>(entry + Size), trampoline + Size);
    DWORD prior;
    if (!VirtualProtect(trampoline, trampolineSize, PAGE_EXECUTE_READ, &prior)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), trampoline, trampolineSize);
    Byte patch[Size];
    std::memset(patch, 0x90, Size);
    jump(entry, reinterpret_cast<std::uintptr_t>(replace), patch);
    original = reinterpret_cast<Function>(trampoline);
    if (writeMemory(entry, patch, Size)) return true;
    original = nullptr;
    VirtualFree(trampoline, 0, MEM_RELEASE);
    return false;
}

bool installRuntimeHooks() {
    auto* destruct = at<Byte*>(gameObjects, 0x40e00);
    constexpr Byte destructPrefix[] = {0x55, 0x8b, 0xe9, 0x8a, 0x85, 0x08, 0x01, 0, 0};
    auto* updateItems = at<Byte*>(gameObjects, 0xeb5d0);
    constexpr Byte itemPrefix[] = {0x51, 0x56, 0x57, 0x8b, 0xf9};
    constexpr Byte swapPrefix[] = {0x55, 0x8b, 0xec, 0x6a, 0xff};
    if (!installJump(destruct, destructPrefix, originalDestruct, &destructCharacter)) return false;
    if (installJump(updateItems, itemPrefix, originalItemUpdate, &captureLevelItems)) {
        if (installJump(at<Byte*>(graphics, 0x2ce00), swapPrefix, originalSwap, &swapBuffer)) {
            log("Direct3D overlay installed\r\n");
            return true;
        }
        writeMemory(updateItems, itemPrefix, sizeof(itemPrefix));
    }
    writeMemory(destruct, destructPrefix, sizeof(destructPrefix));
    return false;
}

void* __fastcall firstScene(void* self, void*) {
    void* scene = originalFirstScene(self);
    if (originalSwap && scene) {
        void* camera = at<GetPointer>(gameObjects, 0x4260)(self);
        if (camera) projectTargets(camera);
    }
    return scene;
}

struct ImportChange {
    void** slot;
    void* original;
    void* replacement;
};
template<class Function, class Hook>
ImportChange change(HMODULE module, size_t offset, Function& original, Hook replacement) {
    auto** slot = at<void**>(module, offset);
    original = reinterpret_cast<Function>(*slot);
    return {slot, *slot, reinterpret_cast<void*>(replacement)};
}
}

extern "C" DWORD WINAPI Initialize(void* options) {
    static bool initialized;
    if (initialized) return 1;
    const HMODULE exe = GetModuleHandleW(nullptr);
    gameObjects = GetModuleHandleW(L"X_GameObjectsMFC.dll");
    graphics = GetModuleHandleW(L"e2mfc.dll");
    levelRuntime = GetModuleHandleW(L"X_LevelRuntimeMFC.dll");
    sharedDb = GetModuleHandleW(L"X_SharedDBMFC.dll");
    kf2 = GetModuleHandleW(L"KF2MFC.dll");
    physics = GetModuleHandleW(L"X_PhysicalSimulationMFC.dll");
    messages = GetModuleHandleW(L"X_MessageMFC.dll");
    if (!gameObjects || !graphics || !levelRuntime || !sharedDb || !kf2 || !physics || !messages) return 0;
    diagnostic = options != nullptr;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&Initialize), &module) ||
        !siblingPath(module, L"MP2Assist.ini", profilePath)) return 0;
    if (diagnostic) siblingPath(module, L"MP2Assist.log", logFile);
    settings = readSettings(profilePath);

    const auto input = GetModuleHandleW(L"X_Inputmfc.dll");
    if (!input || *at<void**>(input, 0xa94c) != at<void*>(input, 0x6cf0) ||
        *at<void**>(gameObjects, 0x1218b0) != at<void*>(gameObjects, 0x98a00) ||
        *at<void**>(gameObjects, 0x1218bc) != at<void*>(gameObjects, 0x992f0) ||
        *at<void**>(gameObjects, 0x1218d0) != at<void*>(gameObjects, 0x98680)) return 0;
    const ImportChange changes[] = {
        change(input, 0xa94c, originalMouseUpdate, &updateMouse),
        change(exe, 0xe4a30, originalPost, &postPhysics),
        change(exe, 0xe4a34, originalWeapons, &updateWeapons),
        change(exe, 0xe4ab8, originalFirstScene, &firstScene),
        change(gameObjects, 0x11ade0, originalAccuracy, &projectileAccuracy),
        change(gameObjects, 0x11acf4, originalShake[0], &weaponShake<0>),
        change(gameObjects, 0x11acf8, originalShake[1], &weaponShake<1>),
        change(gameObjects, 0x11ad00, originalShake[2], &weaponShake<2>),
        change(gameObjects, 0x11ad04, originalShake[3], &weaponShake<3>),
        change(gameObjects, 0x1218b0, originalProjectileUnload, &unloadLevelProjectiles),
        change(gameObjects, 0x1218bc, originalProjectileLoad, &loadLevelProjectiles),
        change(gameObjects, 0x1218d0, originalLevelProjectile, &createLevelProjectile),
    };
    size_t applied = 0;
    while (applied < std::size(changes)) {
        const auto& patch = changes[applied];
        if (!writeMemory(patch.slot, &patch.replacement, sizeof(void*))) break;
        ++applied;
    }
    if (applied != std::size(changes) || !installRuntimeHooks()) {
        while (applied) {
            const auto& patch = changes[--applied];
            writeMemory(patch.slot, &patch.original, sizeof(void*));
        }
        return 0;
    }
    initialized = true;
    log("Max Payne 2 Assist initialized\r\n");
    return 1;
}
