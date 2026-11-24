#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>

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

HMODULE gameObjects, graphics, levelRuntime, sharedDb, kf2, physics;
CharacterTick originalPost;
CharacterDestruct originalDestruct;
CharacterWeapons originalWeapons;
GetPointer originalFirstScene;
GetFloat originalAccuracy;
std::array<GetFloat, 4> originalShake;
SwapBuffer originalSwap;
MouseUpdate originalMouseUpdate;
bool playerWeaponUpdate;
AssistSettings settings;
bool espVisible = true;
bool diagnostic, menuOpen, mouseDown, mouseClicked, rangeDragging;
int menuRow;
Point menuCursor{};
bool espActive() { return espVisible && (settings.boxes || settings.skeleton || settings.head || settings.health); }
wchar_t profilePath[MAX_PATH];
AssistPanel::Panel panel;
constexpr int keys[] = {VK_F6, VK_F7, VK_INSERT, VK_PRIOR, VK_NEXT, VK_HOME, VK_END};
std::array<bool, std::size(keys)> oldKeys{};
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
std::array<Vertex, targets.size() * (boneCount + 4 + maxHeadVertices) * 2> lines;
std::array<Vertex, targets.size() * 12> healthVertices;
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
    if (!espActive() || !canDrawTarget(character) || !field<void*>(character, 0x404)) {
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
        const auto overlayViewport = menuOpen ? full : vp;
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
        const auto barStatus = barVertices ? vtable<Draw>(device, 72)(
            device, 4, barVertices / 3, healthVertices.data(), sizeof(Vertex)) : S_OK;
        const auto status = lineVertices ?
            vtable<Draw>(device, 72)(device, 2, lineVertices / 2, lines.data(), sizeof(Vertex)) : S_OK;
        if (menuOpen) {
            const float scale = std::clamp(static_cast<float>(height) / 750.0f, 0.65f, 1.4f);
            const float x = (static_cast<float>(width) - AssistPanel::width * scale) * 0.5f;
            const float y = (static_cast<float>(height) - AssistPanel::height * scale) * 0.30f;
            const auto menuResult = panel.draw(device, x, y, scale, menuRow, settings, espVisible);
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

void setEspDistance(int step) {
    step = std::clamp(step, 0, lastEspDistance);
    if (step == settings.espDistance) return;
    settings.espDistance = step;
    writeSetting(profilePath, L"ESPDistance", step);
    panel.invalidate();
    if (diagnostic) {
        char message[80];
        sprintf_s(message, "ESP distance: %d m (0 = whole map)\r\n", espDistances[step].meters);
        log(message);
    }
}

void changeMenuRow(int row, int direction) {
    switch (row) {
    case AssistPanel::rangeRow:
        setEspDistance(settings.espDistance + direction);
        return;
    case 0:
        settings.boxes = !settings.boxes;
        espVisible = true;
        writeSetting(profilePath, L"ESPBoxes", settings.boxes);
        log(settings.boxes ? "Boxes on\r\n" : "Boxes off\r\n");
        break;
    case 1:
        settings.skeleton = !settings.skeleton;
        espVisible = true;
        writeSetting(profilePath, L"ESPSkeleton", settings.skeleton);
        log(settings.skeleton ? "Skeleton on\r\n" : "Skeleton off\r\n");
        break;
    case 2:
        settings.head = !settings.head;
        espVisible = true;
        writeSetting(profilePath, L"ESPHead", settings.head);
        log(settings.head ? "Head marker on\r\n" : "Head marker off\r\n");
        break;
    case 3:
        settings.noSpread = !settings.noSpread;
        writeSetting(profilePath, L"NoSpread", settings.noSpread);
        log(settings.noSpread ? "No spread on\r\n" : "No spread off\r\n");
        break;
    case 4:
        settings.color = (settings.color + direction + 3) % 3;
        writeSetting(profilePath, L"Color", settings.color);
        log("ESP color changed\r\n");
        break;
    case 5:
        settings.health = !settings.health;
        espVisible = true;
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
        const int hover = AssistPanel::hitRow(menuCursor.x, menuCursor.y);
        if (hover >= 0 && menuRow != hover) {
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
        oldKeys.fill(false);
        mouseClicked = false;
        rangeDragging = false;
        return;
    }
    std::array<bool, std::size(keys)> press{};
    for (size_t index = 0; index < press.size(); ++index) {
        const bool down = (GetAsyncKeyState(keys[index]) & 0x8000) != 0;
        press[index] = down && !oldKeys[index];
        oldKeys[index] = down;
    }
    if (press[2]) {
        menuOpen = !menuOpen;
        mouseClicked = false;
        rangeDragging = false;
        mouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (menuRow == AssistPanel::rangeRow) {
            menuCursor = {float(AssistPanel::rangePosition(settings.espDistance)), float(AssistPanel::rangeTrack)};
        } else {
            menuCursor = {float(AssistPanel::width / 2),
                          float(AssistPanel::firstRowTop + AssistPanel::rowStep * menuRow + 16)};
        }
        if (!menuOpen) panel.clear();
        log(menuOpen ? "Menu open\r\n" : "Menu closed\r\n");
    }
    if (press[0]) {
        espVisible = !espVisible;
        panel.invalidate();
        log(espVisible ? "ESP resumed\r\n" : "ESP paused\r\n");
    }
    if (press[1]) changeMenuRow(3, 1);
    if (!menuOpen) return;
    if (press[3] || press[4]) {
        menuRow = (menuRow + (press[4] ? 1 : AssistPanel::rowCount - 1)) % AssistPanel::rowCount;
        panel.invalidate();
    }
    if (press[5] || press[6]) changeMenuRow(menuRow, press[6] ? 1 : -1);
    if (mouseClicked) {
        mouseClicked = false;
        const int row = AssistPanel::hitRow(menuCursor.x, menuCursor.y);
        if (row == AssistPanel::rangeRow) {
            menuRow = row;
            panel.invalidate();
            rangeDragging = menuCursor.y >= AssistPanel::rangeTrack - 15;
            if (rangeDragging) setEspDistance(AssistPanel::rangeStepAt(menuCursor.x));
        } else if (row >= 0) {
            changeMenuRow(row, 1);
        }
    }
    if (rangeDragging) {
        if (mouseDown) setEspDistance(AssistPanel::rangeStepAt(menuCursor.x));
        else rangeDragging = false;
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
            if (width && height && (menuOpen || (espActive() && (lineVertices || barVertices)))) {
                if (!espActive()) lineVertices = barVertices = 0;
                drawOverlay(device, width, height);
            }
        }
    }
    lineVertices = barVertices = 0;
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
    constexpr Byte swapPrefix[] = {0x55, 0x8b, 0xec, 0x6a, 0xff};
    if (!installJump(destruct, destructPrefix, originalDestruct, &destructCharacter)) return false;
    if (installJump(at<Byte*>(graphics, 0x2ce00), swapPrefix, originalSwap, &swapBuffer)) {
        log("Direct3D overlay installed\r\n");
        return true;
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
    if (!gameObjects || !graphics || !levelRuntime || !sharedDb || !kf2 || !physics) return 0;
    diagnostic = options != nullptr;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&Initialize), &module) ||
        !siblingPath(module, L"MP2Assist.ini", profilePath)) return 0;
    if (diagnostic) siblingPath(module, L"MP2Assist.log", logFile);
    settings = readSettings(profilePath);

    const auto input = GetModuleHandleW(L"X_Inputmfc.dll");
    if (!input || *at<void**>(input, 0xa94c) != at<void*>(input, 0x6cf0)) return 0;
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
