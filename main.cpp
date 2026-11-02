// GTA4 Ragdoll Android - Fase 1 (AML, GTA SA 2.00, armeabi-v7a)
// Simbolos verificados contra el libGTASA.so de tu APK (GTASA 2.00 AML1.3.0 FLA7.6).
//
// Que hace esta fase:
//   1) Engancha la creacion de CEventDamage para saber con que arma/vehiculo fue golpeado el ped.
//   2) Engancha CEventDamage::ComputeDamageAnim (aqui sabemos a que ped le toca reaccionar).
//   3) Calcula una "fuerza" segun el arma y empuja al ped hacia atras varios frames (tambaleo).
//   4) Escribe un log para poder ajustar todo (ver README).
// NO incluye aun: huesos/ragdoll con Bullet (Fase 2).

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

MYMODCFG(net.gta4ragdoll.sa, GTA4 Ragdoll Android, 0.1, GTA4RagdollProject)
NEEDGAME(com.rockstargames.gtasa)
BEGIN_DEPLIST()
END_DEPLIST()

struct CVector { float x, y, z; };

// ---------------------------------------------------------------- simbolos
typedef void  (*ApplyMoveForce_t)(void* self, CVector force);
typedef void* (*FindPlayerPed_t)(int);
static ApplyMoveForce_t ApplyMoveForce = NULL;
static FindPlayerPed_t  FindPlayerPed  = NULL;
static unsigned int*    pTimeMs        = NULL;
struct RwV3d { float x, y, z; };
typedef void* (*GetAnimHierarchyFromFrame_t)(void* frame);
typedef void  (*GetBonePosition_t)(void* ped, RwV3d& out, unsigned int bone, bool b);
static GetAnimHierarchyFromFrame_t GetAnimHierarchyFromFrame = NULL;
static GetBonePosition_t           GetBonePosition           = NULL;
typedef float (*FindGroundZ3D_t)(float x, float y, float z, bool* found, void** ent);
static FindGroundZ3D_t             FindGroundZFor3DCoord     = NULL;

static uintptr_t pGTASA = 0;
static void*     hGTASA = NULL;

// ---------------------------------------------------------------- config
static float cfgLight   = 6.0f;    // fuerza minima para reaccionar
static float cfgHeavy   = 22.0f;   // fuerza "fuerte" (tambaleo largo)
static float cfgPush    = 0.55f;   // empuje por punto de fuerza
static float cfgDecay   = 0.90f;   // cuanto baja el empuje cada frame
static float cfgWeaponM = 1.0f;    // multiplicador armas
static float cfgCarM    = 1.0f;    // multiplicador autos
static float cfgExplM   = 1.0f;    // multiplicador explosiones
static bool  cfgPlayer  = false;   // tambien tambalear al jugador
static bool  cfgLog     = true;
static bool  cfgRagdoll = true;     // ragdoll (Verlet) al morir
static float cfgRdGravity = 14.0f;  // m/s^2
static float cfgRdImpulse = 0.12f;  // m/s por punto de fuerza
static float cfgRdRadius  = 0.10f;  // radio de cada articulacion contra el suelo
static float cfgRdDamp    = 0.992f; // amortiguacion por paso
static bool  cfgBoneTest = false;   // prueba visible: el ped "flota" 2.5 s tras un golpe

// ---------------------------------------------------------------- log a archivo
static int g_logLines = 0;
static void FLog(const char* fmt, ...)
{
    if(!cfgLog || g_logLines > 400) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/GTA4Ragdoll.log", aml->GetAndroidDataPath());
    FILE* f = fopen(path, "a");
    if(!f) return;
    va_list va; va_start(va, fmt);
    vfprintf(f, fmt, va);
    va_end(va);
    fputc('\n', f);
    fclose(f);
    ++g_logLines;
}

// ---------------------------------------------------------------- utilidades
static inline bool ValidPtr(void* p) { return (uintptr_t)p > 0x10000; }

// CPlaceable: +0x04 posicion simple, +0x14 puntero a CMatrix (pos en +0x30). (igual que PC en 32 bits)
static bool GetPos(void* ent, CVector& out)
{
    if(!ValidPtr(ent)) return false;
    void* mtx = *(void**)((char*)ent + 0x14);
    if(ValidPtr(mtx)) out = *(CVector*)((char*)mtx + 0x30);
    else              out = *(CVector*)((char*)ent + 0x04);
    return true;
}

// Fuerza base por eWeaponType
static float BaseForce(int w)
{
    switch(w)
    {
        case 0:  return 3.0f;   // puños
        case 1:  return 5.0f;   // manopla
        case 2: case 3: case 4: case 6: case 7: case 15: return 7.0f;
        case 5: case 8:  return 9.0f;   // bate, katana
        case 9:  return 10.0f;  // motosierra
        case 22: return 10.0f;  // pistola
        case 23: return 8.0f;
        case 24: return 18.0f;  // desert eagle
        case 25: case 26: case 27: return 30.0f; // escopetas
        case 28: case 32: case 52: return 6.0f;  // uzi, tec9, driveby
        case 29: return 8.0f;   // mp5
        case 30: case 31: return 14.0f; // ak, m4
        case 33: return 22.0f;  // rifle
        case 34: return 40.0f;  // sniper
        case 35: case 36: case 19: return 80.0f;
        case 38: return 10.0f;  // minigun
        case 49: return 35.0f;  // chocado por auto
        case 50: return 45.0f;  // atropellado
        case 51: return 60.0f;  // explosion
        default: return 0.0f;
    }
}

static float WeaponMult(int w)
{
    if(w == 49 || w == 50) return cfgCarM;
    if(w == 51 || w == 16 || w == 18 || w == 35 || w == 36 || w == 19) return cfgExplM;
    return cfgWeaponM;
}

// ---------------------------------------------------------------- registro de eventos recientes
struct DmgRec { void* self; void* attacker; int weapon; int piece; unsigned int time; };
static DmgRec g_rec[16];
static int    g_recN = 0;

// ---------------------------------------------------------------- tambaleo por ped
struct Stagger { void* ped; float dx, dy, power; int frames; };
static Stagger g_st[32];

static void StartStagger(void* ped, float dx, float dy, float force)
{
    int slot = -1;
    for(int i = 0; i < 32; ++i) if(g_st[i].ped == ped) { slot = i; break; }
    if(slot < 0) for(int i = 0; i < 32; ++i) if(g_st[i].frames <= 0) { slot = i; break; }
    if(slot < 0) slot = 0;
    g_st[slot].ped    = ped;
    g_st[slot].dx     = dx;
    g_st[slot].dy     = dy;
    g_st[slot].power  = force * cfgPush;
    g_st[slot].frames = (force >= cfgHeavy) ? 45 : 28;
}

// ---------------------------------------------------------------- hooks
static int g_ctorCalls = 0, g_copyCalls = 0;

static void RecordEvent(void* self, void* attacker, int weapon, int piece)
{
    DmgRec& r = g_rec[g_recN++ & 15];
    r.self = self; r.attacker = attacker; r.weapon = weapon; r.piece = piece;
    r.time = pTimeMs ? *pTimeMs : 0;
    if(g_ctorCalls++ < 12) FLog("[ctor] evento=%p attacker=%p weapon=%d piece=%d", self, attacker, weapon, piece);
}

DECL_HOOKv(CEventDamage_ctor, void* self, void* attacker, unsigned int t, int weapon, int piece, unsigned char dir, bool b1, bool b2)
{
    CEventDamage_ctor(self, attacker, t, weapon, piece, dir, b1, b2);
    RecordEvent(self, attacker, weapon, piece);
}
DECL_HOOKv(CEventDamage_ctor2, void* self, void* attacker, unsigned int t, int weapon, int piece, unsigned char dir, bool b1, bool b2)
{
    CEventDamage_ctor2(self, attacker, t, weapon, piece, dir, b1, b2);
    RecordEvent(self, attacker, weapon, piece);
}

// el juego copia el evento: pasamos el registro al objeto nuevo
static void CopyRecord(void* self, void* src)
{
    for(int i = 0; i < 16; ++i)
        if(g_rec[i].self == src)
        {
            DmgRec n = g_rec[i]; n.self = self;
            g_rec[g_recN++ & 15] = n;
            if(g_copyCalls++ < 12) FLog("[copy] %p <- %p weapon=%d", self, src, n.weapon);
            return;
        }
}
DECL_HOOKv(CEventDamage_copy, void* self, void* src)
{
    CEventDamage_copy(self, src);
    CopyRecord(self, src);
}
DECL_HOOKv(CEventDamage_copy2, void* self, void* src)
{
    CEventDamage_copy2(self, src);
    CopyRecord(self, src);
}

// Layout de CEventDamage verificado con el volcado de tu log:
//   +0x10 = atacante (CEntity*), +0x18 = eWeaponType, +0x1C = pieza del cuerpo
static bool ReadEvent(void* ev, int& weapon, void*& attacker)
{
    if(!ValidPtr(ev)) return false;
    weapon   = *(int*)((char*)ev + 0x18);
    attacker = *(void**)((char*)ev + 0x10);
    return weapon >= 0 && weapon < 60;
}

struct HitMark { void* ped; unsigned int time; };
static HitMark g_hit[32];
static int     g_hitN = 0;
static int     g_hitLogs = 0;

// ---- FASE 2.1: prueba de escritura de huesos ----
static void* GetPedHierarchy(void* ped)
{
    void* clump = *(void**)((char*)ped + 0x18);
    if(!ValidPtr(clump)) return NULL;
    void* frame = *(void**)((char*)clump + 4);
    if(!ValidPtr(frame) || !GetAnimHierarchyFromFrame) return NULL;
    void* hier = GetAnimHierarchyFromFrame(frame);
    return ValidPtr(hier) ? hier : NULL;
}

struct BoneTestRec { void* hier; unsigned int start; };
static BoneTestRec g_bt[8];
static int g_btN = 0;
static int g_btLogs = 0;

static void StartBoneTest(void* ped)
{
    void* h = GetPedHierarchy(ped);
    if(!h) return;
    unsigned int now = pTimeMs ? *pTimeMs : 0;
    g_bt[g_btN++ & 7] = { h, now };
    if(g_btLogs++ < 5) FLog("[bonetest] iniciado ped=%p hier=%p", ped, h);
}

// ======================= FASE 2.2: RAGDOLL (Verlet) =======================
// Una particula por hueso (posicion de la articulacion), restricciones de distancia
// padre-hijo, gravedad y suelo. Despues se reescriben las matrices de los huesos.
struct Quat { float x, y, z, w; };
static inline float Dot3(const float* a, const float* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static inline void  Cross3(const float* a, const float* b, float* o)
{ o[0] = a[1]*b[2]-a[2]*b[1]; o[1] = a[2]*b[0]-a[0]*b[2]; o[2] = a[0]*b[1]-a[1]*b[0]; }

static Quat QFromTo(const float* u, const float* v)
{
    float ul = sqrtf(Dot3(u,u)), vl = sqrtf(Dot3(v,v));
    Quat id = {0,0,0,1};
    if(ul < 1e-4f || vl < 1e-4f) return id;
    float a[3] = { u[0]/ul, u[1]/ul, u[2]/ul }, b[3] = { v[0]/vl, v[1]/vl, v[2]/vl };
    float d = Dot3(a,b);
    float c[3]; Cross3(a, b, c);
    if(d < -0.9999f)
    {
        float t[3] = { fabsf(a[0]) < 0.9f ? 1.f : 0.f, fabsf(a[0]) < 0.9f ? 0.f : 1.f, 0.f };
        Cross3(a, t, c);
        float cl = sqrtf(Dot3(c,c)); if(cl < 1e-6f) return id;
        Quat q = { c[0]/cl, c[1]/cl, c[2]/cl, 0.f }; return q;
    }
    Quat q = { c[0], c[1], c[2], 1.f + d };
    float l = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    q.x /= l; q.y /= l; q.z /= l; q.w /= l;
    return q;
}
static void QRot(const Quat& q, const float* w, float* out)
{
    float qv[3] = { q.x, q.y, q.z }, t[3], c[3];
    Cross3(qv, w, t); t[0] *= 2; t[1] *= 2; t[2] *= 2;
    Cross3(qv, t, c);
    out[0] = w[0] + q.w*t[0] + c[0];
    out[1] = w[1] + q.w*t[1] + c[1];
    out[2] = w[2] + q.w*t[2] + c[2];
}

#define RD_MAXN 32
struct Ragdoll
{
    bool  used, inited;
    void* ped; void* hier;
    unsigned int n, startTime, lastTime, lastCheck;
    float dirx, diry, force;
    float p[RD_MAXN][3], pv[RD_MAXN][3], p0[RD_MAXN][3], R0[RD_MAXN][9], gz[RD_MAXN];
    int   parent[RD_MAXN], child[RD_MAXN], grand[RD_MAXN];
    float len[RD_MAXN], glen[RD_MAXN];
    int   stillFrames;
};
static Ragdoll g_rd[4];

static void StartRagdoll(void* ped, float dx, float dy, float force)
{
    void* h = GetPedHierarchy(ped);
    if(!h) return;
    for(int i = 0; i < 4; ++i) if(g_rd[i].used && g_rd[i].hier == h) return;   // ya activo
    int slot = -1;
    for(int i = 0; i < 4; ++i) if(!g_rd[i].used) { slot = i; break; }
    if(slot < 0) return;                                                         // maximo 4 a la vez
    Ragdoll& r = g_rd[slot];
    memset(&r, 0, sizeof(r));
    r.used = true; r.ped = ped; r.hier = h;
    r.dirx = dx; r.diry = dy; r.force = force;
    FLog("[ragdoll] solicitado ped=%p hier=%p dir=(%.2f,%.2f) force=%.1f slot=%d", ped, h, dx, dy, force, slot);
}

static void RagdollInit(Ragdoll& r)
{
    unsigned int* h = (unsigned int*)r.hier;
    unsigned int n = h[1];
    float* m = *(float**)(h + 2);
    unsigned char* ni = *(unsigned char**)(h + 4);          // RpHAnimNodeInfo[] (16 bytes c/u)
    if(!ValidPtr(m) || !ValidPtr(ni) || n == 0 || n > RD_MAXN) { r.used = false; FLog("[ragdoll] jerarquia no valida n=%u", n); return; }
    r.n = n;
    int stack[RD_MAXN]; int sp = 0; int cur = -1;
    for(unsigned i = 0; i < n; ++i)
    {
        int flags = *(int*)(ni + i*16 + 8);
        r.parent[i] = cur; r.child[i] = -1;
        cur = (int)i;
        if(flags & 2) { if(sp < RD_MAXN) stack[sp++] = (int)i; }   // PUSH
        if(flags & 1) { if(sp > 0) cur = stack[--sp]; }            // POP
    }
    for(unsigned i = 0; i < n; ++i)
    {
        float* mm = m + i*16;
        r.p[i][0] = r.pv[i][0] = r.p0[i][0] = mm[12];
        r.p[i][1] = r.pv[i][1] = r.p0[i][1] = mm[13];
        r.p[i][2] = r.pv[i][2] = r.p0[i][2] = mm[14];
        for(int k = 0; k < 3; ++k) { r.R0[i][k] = mm[k]; r.R0[i][3+k] = mm[4+k]; r.R0[i][6+k] = mm[8+k]; }
        r.gz[i] = mm[14] - 1.0f;
    }
    for(unsigned i = 0; i < n; ++i)
    {
        int pa = r.parent[i];
        r.grand[i] = (pa >= 0) ? r.parent[pa] : -1;
        if(pa >= 0 && r.child[pa] < 0) r.child[pa] = (int)i;
        r.len[i] = 0; r.glen[i] = 0;
        if(pa >= 0)
        {
            float d[3] = { r.p0[i][0]-r.p0[pa][0], r.p0[i][1]-r.p0[pa][1], r.p0[i][2]-r.p0[pa][2] };
            r.len[i] = sqrtf(Dot3(d,d));
        }
        int g = r.grand[i];
        if(g >= 0)
        {
            float d[3] = { r.p0[i][0]-r.p0[g][0], r.p0[i][1]-r.p0[g][1], r.p0[i][2]-r.p0[g][2] };
            r.glen[i] = sqrtf(Dot3(d,d));
        }
    }
    // velocidad inicial: empuje del impacto, mas fuerte arriba para que el torso se incline
    float pelvisZ = r.p0[0][2];
    float speed = r.force * cfgRdImpulse * 0.02f;           // m/s -> desplazamiento por paso de 20 ms
    for(unsigned i = 0; i < n; ++i)
    {
        float k = (r.p0[i][2] > pelvisZ + 0.25f) ? 1.4f : 1.0f;
        r.pv[i][0] -= r.dirx * speed * k;
        r.pv[i][1] -= r.diry * speed * k;
        r.pv[i][2] -= 0.004f;
    }
    r.inited = true;
    r.startTime = r.lastTime = pTimeMs ? *pTimeMs : 0;
    FLog("[ragdoll] init n=%u", n);
    for(unsigned i = 0; i < n; ++i)
        FLog("[ragdoll]  nodo %u flags=%d padre=%d len=%.2f", i, *(int*)(ni + i*16 + 8), r.parent[i], r.len[i]);
}

static void RagdollStep(Ragdoll& r, float dt)
{
    unsigned n = r.n;
    float g = cfgRdGravity * dt * dt;
    float damp = cfgRdDamp;
    for(unsigned i = 0; i < n; ++i)
    {
        for(int k = 0; k < 3; ++k)
        {
            float v = (r.p[i][k] - r.pv[i][k]) * damp;
            r.pv[i][k] = r.p[i][k];
            r.p[i][k] += v;
        }
        r.p[i][2] -= g;
    }
    // altura del suelo bajo cada articulacion
    if(FindGroundZFor3DCoord)
        for(unsigned i = 0; i < n; ++i)
        {
            bool found = false; void* ent = NULL;
            float z = FindGroundZFor3DCoord(r.p[i][0], r.p[i][1], r.p[i][2] + 0.6f, &found, &ent);
            if(found) r.gz[i] = z;
        }
    for(int it = 0; it < 6; ++it)
    {
        for(unsigned i = 0; i < n; ++i)
        {
            int pa = r.parent[i];
            if(pa >= 0 && r.len[i] > 1e-4f)
            {
                float d[3] = { r.p[i][0]-r.p[pa][0], r.p[i][1]-r.p[pa][1], r.p[i][2]-r.p[pa][2] };
                float l = sqrtf(Dot3(d,d)); if(l < 1e-5f) continue;
                float f = (l - r.len[i]) / l * 0.5f;
                for(int k = 0; k < 3; ++k) { r.p[i][k] -= d[k]*f; r.p[pa][k] += d[k]*f; }
            }
            int gr = r.grand[i];
            if(gr >= 0 && r.glen[i] > 1e-4f)                  // evita que las extremidades se plieguen del todo
            {
                float minL = r.glen[i] * 0.55f;
                float d[3] = { r.p[i][0]-r.p[gr][0], r.p[i][1]-r.p[gr][1], r.p[i][2]-r.p[gr][2] };
                float l = sqrtf(Dot3(d,d));
                if(l < minL && l > 1e-5f)
                {
                    float f = (l - minL) / l * 0.5f;
                    for(int k = 0; k < 3; ++k) { r.p[i][k] -= d[k]*f; r.p[gr][k] += d[k]*f; }
                }
            }
        }
        for(unsigned i = 0; i < n; ++i)                        // suelo + friccion
        {
            float floorZ = r.gz[i] + cfgRdRadius;
            if(r.p[i][2] < floorZ)
            {
                r.p[i][2] = floorZ;
                r.pv[i][0] += (r.p[i][0] - r.pv[i][0]) * 0.25f;
                r.pv[i][1] += (r.p[i][1] - r.pv[i][1]) * 0.25f;
            }
        }
    }
}

static void RagdollWrite(Ragdoll& r)
{
    unsigned int* h = (unsigned int*)r.hier;
    float* m = *(float**)(h + 2);
    if(!ValidPtr(m)) return;
    Quat q[RD_MAXN];
    for(unsigned i = 0; i < r.n; ++i)
    {
        int c = r.child[i];
        if(c >= 0)
        {
            float u[3] = { r.p0[c][0]-r.p0[i][0], r.p0[c][1]-r.p0[i][1], r.p0[c][2]-r.p0[i][2] };
            float v[3] = { r.p[c][0]-r.p[i][0],   r.p[c][1]-r.p[i][1],   r.p[c][2]-r.p[i][2] };
            q[i] = QFromTo(u, v);
        }
        else if(r.parent[i] >= 0) q[i] = q[r.parent[i]];
        else { Quat id = {0,0,0,1}; q[i] = id; }
        float* mm = m + i*16;
        float o[3];
        QRot(q[i], &r.R0[i][0], o); mm[0] = o[0]; mm[1] = o[1]; mm[2] = o[2];
        QRot(q[i], &r.R0[i][3], o); mm[4] = o[0]; mm[5] = o[1]; mm[6] = o[2];
        QRot(q[i], &r.R0[i][6], o); mm[8] = o[0]; mm[9] = o[1]; mm[10] = o[2];
        mm[12] = r.p[i][0]; mm[13] = r.p[i][1]; mm[14] = r.p[i][2];
    }
}

static void RagdollOnHierarchy(void* hier)
{
    unsigned int now = *pTimeMs;
    for(int i = 0; i < 4; ++i)
    {
        Ragdoll& r = g_rd[i];
        if(!r.used || r.hier != hier) continue;
        if(!r.inited) { RagdollInit(r); if(!r.used) continue; }
        if(now - r.lastCheck > 1000)                             // el ped sigue existiendo con esta misma jerarquia?
        {
            r.lastCheck = now;
            if(GetPedHierarchy(r.ped) != r.hier) { FLog("[ragdoll] fin (ped liberado) slot=%d", i); r.used = false; continue; }
        }
        if(now != r.lastTime && now - r.lastTime < 200)
        {
            float dt = (now - r.lastTime) * 0.001f;
            if(dt > 0.04f) dt = 0.04f;
            RagdollStep(r, dt);
            static int sl = 0;
            if(sl++ < 8) FLog("[ragdoll] paso dt=%.3f pelvis=(%.2f,%.2f,%.2f) suelo=%.2f", dt, r.p[0][0], r.p[0][1], r.p[0][2], r.gz[0]);
        }
        r.lastTime = now;
        RagdollWrite(r);
    }
}

typedef void* (*UpdMat_t)(void*);
DECL_HOOK(void*, RpHAnimHierarchyUpdateMatrices_hook, void* hier)
{
    void* r = RpHAnimHierarchyUpdateMatrices_hook(hier);
    if(cfgRagdoll && pTimeMs && ValidPtr(hier)) RagdollOnHierarchy(hier);
    if(!cfgBoneTest || !pTimeMs || !ValidPtr(hier)) return r;
    unsigned int now = *pTimeMs;
    for(int i = 0; i < 8; ++i)
    {
        if(g_bt[i].hier != hier) continue;
        unsigned int dt = now - g_bt[i].start;
        if(dt > 2500) { g_bt[i].hier = NULL; continue; }
        unsigned int* h = (unsigned int*)hier;
        unsigned int n = h[1];
        float* m = *(float**)(h + 2);
        if(!ValidPtr(m) || n == 0 || n > 64) continue;
        float dz = 0.35f * sinf(dt * 0.008f);          // sube y baja ~0.35 m
        for(unsigned int b = 0; b < n; ++b) m[b * 16 + 14] += dz;
        static int wl = 0;
        if(wl++ < 3) FLog("[bonetest] escribiendo %u huesos dz=%.2f", n, dz);
    }
    return r;
}

// ---- FASE 2.0: sonda del esqueleto (solo LEE, no modifica nada) ----
static void ProbeSkeleton(void* ped)
{
    static int n = 0;
    if(n++ >= 3) return;
    void* clump = *(void**)((char*)ped + 0x18);               // CEntity::m_pRwObject
    FLog("[skel] ped=%p clump=%p", ped, clump);
    if(!ValidPtr(clump)) return;
    void* frame = *(void**)((char*)clump + 4);                // RpClump.object.parent = RwFrame*
    FLog("[skel] rwtype=%d frame=%p", (int)*(unsigned char*)clump, frame);
    if(!ValidPtr(frame) || !GetAnimHierarchyFromFrame) return;
    void* hier = GetAnimHierarchyFromFrame(frame);
    FLog("[skel] hierarchy=%p", hier);
    if(!ValidPtr(hier)) return;
    unsigned int* h = (unsigned int*)hier;
    FLog("[skel] hier: %08X %08X %08X %08X %08X %08X %08X %08X", h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
    float* m = *(float**)(h + 2);                                // pMatrixArray (RW 3.7)
    if(ValidPtr(m))
        FLog("[skel] m0: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
             m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7], m[8],m[9],m[10],m[11], m[12],m[13],m[14],m[15]);
    if(GetBonePosition)
    {
        static const unsigned bones[] = {1,2,3,4,5,8,22,32,41,51,43,53};
        for(unsigned i = 0; i < sizeof(bones)/sizeof(bones[0]); ++i)
        {
            RwV3d v = {0,0,0};
            GetBonePosition(ped, v, bones[i], false);
            FLog("[skel] bone %u = (%.2f, %.2f, %.2f)", bones[i], v.x, v.y, v.z);
        }
    }
}

// Llamada desde los 3 puntos donde el juego procesa un golpe
static void OnHit(const char* tag, void* ped, void* ev)
{
    if(!ValidPtr(ped) || !ApplyMoveForce) return;
    if(!cfgPlayer && FindPlayerPed && ped == FindPlayerPed(0)) return;

    unsigned int now = pTimeMs ? *pTimeMs : 0;
    bool isDeath = (tag[0] == 'd' && tag[1] == 'e' && tag[2] == 'a');
    if(!isDeath)
        for(int i = 0; i < 32; ++i)                   // evitar procesar 2 veces el mismo golpe
            if(g_hit[i].ped == ped && now - g_hit[i].time < 200) return;
    g_hit[g_hitN++ & 31] = { ped, now };

    int weapon = -1; void* attacker = NULL;
    bool direct = ReadEvent(ev, weapon, attacker);
    if(!direct)                                       // plan B: registro del constructor
    {
        for(int k = 1; k <= 16; ++k)
        {
            DmgRec& c = g_rec[(g_recN - k) & 15];
            if(c.self && now - c.time < 1000) { weapon = c.weapon; attacker = c.attacker; direct = true; break; }
        }
    }
    if(!direct) return;

    float force = BaseForce(weapon) * WeaponMult(weapon);
    CVector pp = {0,0,0}, ap = {0,0,0};
    bool okP = GetPos(ped, pp);
    bool okA = GetPos(attacker, ap);
    if(g_hitLogs++ < 60)
        FLog("[%s] ped=%p weapon=%d force=%.1f pos=%d/%d atk(%.1f,%.1f) ped(%.1f,%.1f)",
             tag, ped, weapon, force, okP, okA, ap.x, ap.y, pp.x, pp.y);

    if(isDeath && cfgRagdoll)
    {
        float rx = 0.f, ry = 0.f;
        if(okP && okA) { float dx = pp.x - ap.x, dy = pp.y - ap.y, l = sqrtf(dx*dx + dy*dy); if(l > 0.01f) { rx = dx / l; ry = dy / l; } }
        StartRagdoll(ped, rx, ry, force > 8.0f ? force : 8.0f);
    }
    if(force >= cfgLight) ProbeSkeleton(ped);
    if(force >= cfgLight && cfgBoneTest) StartBoneTest(ped);
    if(force < cfgLight || !okP || !okA) return;
    float dx = pp.x - ap.x, dy = pp.y - ap.y;
    float len = sqrtf(dx*dx + dy*dy);
    if(len < 0.01f) return;
    StartStagger(ped, dx / len, dy / len, force);
}

// CEventHandler::m_pPed esta en +0x00
DECL_HOOK(int, CEH_ComputeDamageResponse, void* self, void* ev, void* a, void* b, void* c)
{
    int r = CEH_ComputeDamageResponse(self, ev, a, b, c);
    if(ValidPtr(self)) OnHit("damage", *(void**)self, ev);
    return r;
}
DECL_HOOK(int, CEH_ComputeDeathResponse, void* self, void* ev, void* a, void* b)
{
    int r = CEH_ComputeDeathResponse(self, ev, a, b);
    if(ValidPtr(self)) OnHit("death", *(void**)self, ev);
    return r;
}
DECL_HOOK(int, CEventDamage_ComputeDamageAnim, void* self, void* ped, bool b)
{
    int r = CEventDamage_ComputeDamageAnim(self, ped, b);
    OnHit("anim", ped, self);
    return r;
}

DECL_HOOKv(CPed_ProcessControl, void* self)
{
    CPed_ProcessControl(self);
    if(!ApplyMoveForce) return;
    for(int i = 0; i < 32; ++i)
    {
        Stagger& s = g_st[i];
        if(s.ped != self || s.frames <= 0) continue;
        CVector f = { s.dx * s.power, s.dy * s.power, 0.0f };
        ApplyMoveForce(self, f);
        s.power *= cfgDecay;
        --s.frames;
    }
}

// ---------------------------------------------------------------- entrada del mod
ON_MOD_PRELOAD()
{
    logger->SetTag("GTA4 Ragdoll");
}

ON_MOD_LOAD()
{
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");
    if(!pGTASA || !hGTASA) { logger->Error("libGTASA.so no encontrada"); return; }

    cfgLight   = cfg->GetFloat("LightForce",  6.0f,  "Settings");
    cfgHeavy   = cfg->GetFloat("HeavyForce",  22.0f, "Settings");
    cfgPush    = cfg->GetFloat("PushPerForce", 0.55f, "Settings");
    cfgDecay   = cfg->GetFloat("Decay",       0.90f, "Settings");
    cfgWeaponM = cfg->GetFloat("WeaponMult",  1.0f,  "Settings");
    cfgCarM    = cfg->GetFloat("VehicleMult", 1.0f,  "Settings");
    cfgExplM   = cfg->GetFloat("ExplosionMult", 1.0f, "Settings");
    cfgPlayer  = cfg->GetBool ("AffectPlayer", false, "Settings");
    cfgLog     = cfg->GetBool ("WriteLog",    true,  "Settings");
    cfgBoneTest = cfg->GetBool("BoneTest",    false, "Settings");
    cfgRagdoll  = cfg->GetBool ("Ragdoll",    true,  "Settings");
    cfgRdGravity = cfg->GetFloat("RagdollGravity", 14.0f, "Settings");
    cfgRdImpulse = cfg->GetFloat("RagdollImpulse", 0.12f, "Settings");
    cfgRdRadius  = cfg->GetFloat("RagdollRadius",  0.10f, "Settings");
    cfgRdDamp    = cfg->GetFloat("RagdollDamping", 0.992f, "Settings");
    cfg->Save();

    SETSYM_TO(ApplyMoveForce, hGTASA, "_ZN9CPhysical14ApplyMoveForceE7CVector");
    SETSYM_TO(FindPlayerPed,  hGTASA, "_Z13FindPlayerPedi");
    SETSYM_TO(GetAnimHierarchyFromFrame, hGTASA, "_Z25GetAnimHierarchyFromFrameP7RwFrame");
    SETSYM_TO(GetBonePosition,           hGTASA, "_ZN4CPed15GetBonePositionER5RwV3djb");
    SETSYM_TO(FindGroundZFor3DCoord,     hGTASA, "_ZN6CWorld21FindGroundZFor3DCoordEfffPbPP7CEntity");
    SET_TO(pTimeMs, aml->GetSym(hGTASA, "_ZN6CTimer22m_snTimeInMillisecondsE"));

    const char* sC1 = "_ZN12CEventDamageC1EP7CEntityj11eWeaponType14ePedPieceTypeshbb";
    const char* sC2 = "_ZN12CEventDamageC2EP7CEntityj11eWeaponType14ePedPieceTypeshbb";
    const char* sK1 = "_ZN12CEventDamageC1ERKS_";
    const char* sK2 = "_ZN12CEventDamageC2ERKS_";
    uintptr_t aC1 = aml->GetSym(hGTASA, sC1), aC2 = aml->GetSym(hGTASA, sC2);
    uintptr_t aK1 = aml->GetSym(hGTASA, sK1), aK2 = aml->GetSym(hGTASA, sK2);
    FLog("[sym] ctor C1=%p C2=%p copy C1=%p C2=%p", (void*)aC1, (void*)aC2, (void*)aK1, (void*)aK2);
    HOOKSYM(CEventDamage_ctor, hGTASA, sC1);
    if(aC2 && aC2 != aC1) { HOOKSYM(CEventDamage_ctor2, hGTASA, sC2); }
    HOOKSYM(CEventDamage_copy, hGTASA, sK1);
    if(aK2 && aK2 != aK1) { HOOKSYM(CEventDamage_copy2, hGTASA, sK2); }
    HOOKSYM(CEventDamage_ComputeDamageAnim, hGTASA, "_ZN12CEventDamage17ComputeDamageAnimEP4CPedb");
    HOOKSYM(CEH_ComputeDamageResponse, hGTASA, "_ZN13CEventHandler21ComputeDamageResponseEP6CEventP5CTaskS3_S3_");
    HOOKSYM(CEH_ComputeDeathResponse,  hGTASA, "_ZN13CEventHandler20ComputeDeathResponseEP6CEventP5CTaskS3_");
    HOOKSYM(CPed_ProcessControl,            hGTASA, "_ZN4CPed14ProcessControlEv");
    HOOKSYM(RpHAnimHierarchyUpdateMatrices_hook, hGTASA, "_Z30RpHAnimHierarchyUpdateMatricesP16RpHAnimHierarchy");

    FLog("=== GTA4 Ragdoll Android 0.1 cargado. base=0x%X ApplyMoveForce=%p FindPlayerPed=%p timer=%p",
         (unsigned)pGTASA, (void*)ApplyMoveForce, (void*)FindPlayerPed, (void*)pTimeMs);
    logger->Info("Cargado. ApplyMoveForce=%p", (void*)ApplyMoveForce);
}
