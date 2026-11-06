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
static float cfgPush    = 0.08f;   // empuje por punto de fuerza (antes 0.55: lanzaba a los NPC muy lejos)
static float cfgDecay   = 0.90f;   // cuanto baja el empuje cada frame
static float cfgWeaponM = 1.0f;    // multiplicador armas
static float cfgCarM    = 1.0f;    // multiplicador autos
static float cfgExplM   = 1.0f;    // multiplicador explosiones
static bool  cfgPlayer  = false;   // tambien tambalear al jugador
static bool  cfgLog     = true;
static bool  cfgRagdoll = true;     // ragdoll (Verlet) al morir
static float cfgRdGravity = 14.0f;  // m/s^2
static float cfgRdImpulse = 0.10f;  // empuje inicial por punto de fuerza
static float cfgRdRadius  = 0.10f;  // radio de cada articulacion contra el suelo
static float cfgRdDamp    = 0.992f; // amortiguacion por paso
static float cfgRdBlendTime = 0.0f;  // segundos que sigue la animacion del juego (0 = ragdoll instantaneo)
static float cfgRdBlend     = 0.0f;  // fuerza de esa mezcla (0 = ragdoll puro)
static int   cfgRdIter      = 10;    // iteraciones del solver (mas = mas rigido, mas costo)
static float cfgRdStiff     = 1.0f;  // rigidez de las articulaciones (0..1)
static float cfgRdJointDamp = 0.12f; // amortiguacion de velocidad relativa entre huesos conectados
static float cfgRdSelfColl  = 1.0f;  // 0 = sin colision entre partes del cuerpo
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
    float pw = force * cfgPush;
    if(!(pw == pw) || pw > 3.0f) pw = 3.0f;          // sin NaN y con tope (evita lanzar al NPC fuera del mapa)
    g_st[slot].power  = pw;
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

// ======================= FASE 2.2: RAGDOLL (Verlet con articulaciones) =======================
// Esqueleto SA (32 nodos). Solo se simulan las articulaciones importantes (torso, cuello, cabeza,
// brazos, piernas, pies). Dedos, cejas, mandibula y accesorios siguen rigidamente a su ancla.
struct Quat { float x, y, z, w; };
static inline float Dot3(const float* a, const float* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static inline void  Cross3(const float* a, const float* b, float* o)
{ o[0] = a[1]*b[2]-a[2]*b[1]; o[1] = a[2]*b[0]-a[0]*b[2]; o[2] = a[0]*b[1]-a[1]*b[0]; }
static inline void  Norm3(float* v) { float l = sqrtf(Dot3(v,v)); if(l > 1e-6f) { v[0]/=l; v[1]/=l; v[2]/=l; } }

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
#define RD_MAXC 96

// Tablas del esqueleto de ped de SA (verificadas con las banderas de tu log)
static const int kParent[32] = {-1,0,1,2,3,4,5,5,5,4,9,10,11,12,13,4,15,16,17,18,19,3,3,2,1,24,25,26,1,28,29,30};
static const int kAnchor[32] = {1,-1,-1,-1,-1,-1,5,5,5,-1,-1,-1,-1,12,12,-1,-1,-1,-1,18,18,3,3,2,-1,-1,-1,-1,-1,-1,-1,-1};
// 0 = gira con el marco del torso, 1 = giro por segmento (swing), 2 = cabeza (torso + swing)
static const int kKind[32]   = {0,0,0,0,0,2,0,0,0,0,1,1,1,0,0,0,1,1,1,0,0,0,0,0,1,1,1,1,1,1,1,1};
static const int kPC[32]     = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,11,12,-1,-1,-1,-1,17,18,-1,-1,-1,-1,-1,-1,25,26,27,-1,29,30,31,-1};
static const int kTorso[8]   = {1,2,3,4,9,15,24,28};   // cluster rigido del torso (+caderas)

struct CDef { unsigned char a, b; float mn, mx; };
static const CDef kC[] = {
    {4,5,0.97f,1.03f}, {3,5,0.85f,1.15f}, {9,5,0.80f,1.20f}, {15,5,0.80f,1.20f},        // cuello / cabeza
    {9,10,0.99f,1.01f}, {10,11,0.99f,1.01f}, {11,12,0.99f,1.01f},                       // brazo
    {15,16,0.99f,1.01f}, {16,17,0.99f,1.01f}, {17,18,0.99f,1.01f},
    {24,25,0.99f,1.01f}, {25,26,0.99f,1.01f}, {26,27,0.99f,1.01f},                      // pierna
    {28,29,0.99f,1.01f}, {29,30,0.99f,1.01f}, {30,31,0.99f,1.01f},
    {10,12,0.42f,1.00f}, {16,18,0.42f,1.00f},                                           // flexion del codo
    {24,26,0.55f,1.00f}, {28,30,0.55f,1.00f},                                           // flexion de la rodilla
    {25,27,0.72f,1.00f}, {29,31,0.72f,1.00f},                                           // tobillo
    {1,25,0.79f,1.19f}, {1,29,0.79f,1.19f}                                              // cadera: las piernas no se cruzan por debajo del cuerpo
};

static inline bool IsActive(int i) { return kAnchor[i] < 0; }
static inline bool InTorso(int i) { for(int k = 0; k < 8; ++k) if(kTorso[k] == i) return true; return false; }
static float NodeRadius(int i)
{
    if(i >= 1 && i <= 4) return 0.13f;
    if(i == 5) return 0.11f;
    if(i == 9 || i == 15 || i == 24 || i == 28) return 0.09f;
    if(i == 12 || i == 18) return 0.05f;
    return 0.06f;
}
static float NodeInvMass(int i)
{
    if(i >= 1 && i <= 4) return 0.35f;
    if(i == 9 || i == 15 || i == 24 || i == 28) return 0.5f;
    if(i == 5) return 0.8f;
    if(i == 12 || i == 18 || i == 27 || i == 31) return 1.4f;
    return 1.0f;
}

struct Ragdoll
{
    bool  used, inited;
    void* ped; void* hier;
    unsigned int n, startTime, lastTime, lastCheck;
    float dirx, diry, force;
    float p[RD_MAXN][3], pv[RD_MAXN][3], p0[RD_MAXN][3], R0[RD_MAXN][9], T[RD_MAXN][3], gz[RD_MAXN];
    float rad[RD_MAXN], im[RD_MAXN];
    int   nc; unsigned char ca[RD_MAXC], cb[RD_MAXC]; float cmn[RD_MAXC], cmx[RD_MAXC], cd0[RD_MAXC];
    float F0[9];       // marco del torso al inicio: lateral, arriba, adelante
    float fwd0[3];     // "adelante" del cuerpo al inicio
    Quat  q[RD_MAXN];
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

// marco del torso a partir de pelvis(1), columna alta(3) y caderas(24,28)
static void TorsoFrame(const float P[][3], float* F)
{
    float u[3] = { P[3][0]-P[1][0], P[3][1]-P[1][1], P[3][2]-P[1][2] }; Norm3(u);
    float l[3] = { P[28][0]-P[24][0], P[28][1]-P[24][1], P[28][2]-P[24][2] };
    float d = Dot3(l, u); l[0] -= d*u[0]; l[1] -= d*u[1]; l[2] -= d*u[2]; Norm3(l);
    float f[3]; Cross3(l, u, f); Norm3(f);
    F[0]=l[0]; F[1]=l[1]; F[2]=l[2]; F[3]=u[0]; F[4]=u[1]; F[5]=u[2]; F[6]=f[0]; F[7]=f[1]; F[8]=f[2];
}
// rotacion del torso (marco inicial -> marco actual) aplicada a un vector
static void ApplyTorso(const float* F0, const float* Fn, const float* v, float* o)
{
    o[0] = o[1] = o[2] = 0.f;
    for(int a = 0; a < 3; ++a)
    {
        float k = Dot3(F0 + a*3, v);
        o[0] += Fn[a*3+0]*k; o[1] += Fn[a*3+1]*k; o[2] += Fn[a*3+2]*k;
    }
}

static void RagdollInit(Ragdoll& r)
{
    unsigned int* h = (unsigned int*)r.hier;
    unsigned int n = h[1];
    float* m = *(float**)(h + 2);
    unsigned char* ni = *(unsigned char**)(h + 4);          // RpHAnimNodeInfo[] (16 bytes c/u)
    if(!ValidPtr(m) || !ValidPtr(ni) || n != 32) { r.used = false; FLog("[ragdoll] esqueleto inesperado n=%u", n); return; }
    // comprobar que la jerarquia coincide con la tabla (banderas PUSH=2 / POP=1)
    int stack[RD_MAXN]; int sp = 0; int cur = -1; bool ok = true;
    for(unsigned i = 0; i < n; ++i)
    {
        int flags = *(int*)(ni + i*16 + 8);
        if(flags & 2) { if(sp < RD_MAXN) stack[sp++] = cur; }
        if(cur != kParent[i]) ok = false;
        cur = (int)i;
        if(flags & 1) { if(sp > 0) cur = stack[--sp]; }
    }
    if(!ok) { r.used = false; FLog("[ragdoll] la jerarquia no coincide con la tabla de SA"); return; }
    r.n = n;
    for(unsigned i = 0; i < n; ++i)
    {
        float* mm = m + i*16;
        for(int k = 0; k < 3; ++k) { r.p[i][k] = r.pv[i][k] = r.p0[i][k] = mm[12+k]; r.T[i][k] = mm[12+k]; }
        for(int k = 0; k < 3; ++k) { r.R0[i][k] = mm[k]; r.R0[i][3+k] = mm[4+k]; r.R0[i][6+k] = mm[8+k]; }
        r.gz[i] = mm[14] - 1.0f;
        r.rad[i] = NodeRadius((int)i); r.im[i] = NodeInvMass((int)i);
    }
    // restricciones: cluster rigido del torso + tabla de articulaciones
    r.nc = 0;
    for(int x = 0; x < 8; ++x) for(int y = x + 1; y < 8; ++y)
    {
        int a = kTorso[x], b = kTorso[y];
        float d[3] = { r.p0[b][0]-r.p0[a][0], r.p0[b][1]-r.p0[a][1], r.p0[b][2]-r.p0[a][2] };
        r.ca[r.nc] = a; r.cb[r.nc] = b; r.cmn[r.nc] = 0.98f; r.cmx[r.nc] = 1.02f; r.cd0[r.nc] = sqrtf(Dot3(d,d)); ++r.nc;
    }
    for(unsigned k = 0; k < sizeof(kC)/sizeof(kC[0]) && r.nc < RD_MAXC; ++k)
    {
        int a = kC[k].a, b = kC[k].b;
        float d[3] = { r.p0[b][0]-r.p0[a][0], r.p0[b][1]-r.p0[a][1], r.p0[b][2]-r.p0[a][2] };
        r.ca[r.nc] = a; r.cb[r.nc] = b; r.cmn[r.nc] = kC[k].mn; r.cmx[r.nc] = kC[k].mx; r.cd0[r.nc] = sqrtf(Dot3(d,d)); ++r.nc;
    }
    TorsoFrame(r.p0, r.F0);
    // "adelante" del cuerpo: hacia donde apuntan los pies
    r.fwd0[0] = (r.p0[27][0]-r.p0[26][0]) + (r.p0[31][0]-r.p0[30][0]);
    r.fwd0[1] = (r.p0[27][1]-r.p0[26][1]) + (r.p0[31][1]-r.p0[30][1]);
    r.fwd0[2] = 0.f;
    if(Dot3(r.fwd0, r.fwd0) < 1e-6f) { r.fwd0[0] = r.F0[6]; r.fwd0[1] = r.F0[7]; r.fwd0[2] = r.F0[8]; }
    Norm3(r.fwd0);
    // velocidad inicial: empuje del impacto, mas fuerte arriba para que el torso se incline
    float pelvisZ = r.p0[1][2];
    float speed = r.force * cfgRdImpulse * 0.02f;
    if(speed > 0.04f) speed = 0.04f;
    for(unsigned i = 0; i < n; ++i)
    {
        if(!IsActive((int)i)) continue;
        float k = (r.p0[i][2] > pelvisZ + 0.25f) ? 1.4f : 1.0f;
        r.pv[i][0] -= r.dirx * speed * k;
        r.pv[i][1] -= r.diry * speed * k;
    }
    r.inited = true;
    r.startTime = r.lastTime = pTimeMs ? *pTimeMs : 0;
    FLog("[ragdoll] init n=%u restricciones=%d", n, r.nc);
}

static void SolvePair(Ragdoll& r, int a, int b, float mn, float mx, float d0)
{
    float d[3] = { r.p[b][0]-r.p[a][0], r.p[b][1]-r.p[a][1], r.p[b][2]-r.p[a][2] };
    float l = sqrtf(Dot3(d,d)); if(l < 1e-6f) return;
    float tgt;
    if(l < mn*d0) tgt = mn*d0; else if(l > mx*d0) tgt = mx*d0; else return;
    float diff = (l - tgt) / l * cfgRdStiff;
    float wa = r.im[a], wb = r.im[b], s = wa + wb;
    for(int k = 0; k < 3; ++k) { r.p[a][k] += d[k]*diff*wa/s; r.p[b][k] -= d[k]*diff*wb/s; }
}


// ---- capsulas de los huesos largos: evitan que un miembro atraviese el cuerpo u otro miembro ----
struct SegDef { unsigned char a, b; float r; };
static const SegDef kSeg[] = {
    {10,11,0.06f},{11,12,0.05f},{16,17,0.06f},{17,18,0.05f},          // brazos
    {24,25,0.08f},{25,26,0.06f},{26,27,0.04f},{28,29,0.08f},{29,30,0.06f},{30,31,0.04f},   // piernas
    {1,3,0.13f},{3,4,0.12f},{4,5,0.09f}                              // torso, cuello
};
#define NSEG ((int)(sizeof(kSeg)/sizeof(kSeg[0])))
static bool SegAdj(const SegDef& s, const SegDef& t)
{
    int xs[2] = { s.a, s.b }, ys[2] = { t.a, t.b };
    for(int i = 0; i < 2; ++i) for(int j = 0; j < 2; ++j)
    {
        int x = xs[i], y = ys[j];
        if(x == y || kParent[x] == y || kParent[y] == x) return true;
    }
    return false;
}
static inline float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
static void ClosestSegSeg(const float* p1, const float* q1, const float* p2, const float* q2, float& s, float& t)
{
    float d1[3] = { q1[0]-p1[0], q1[1]-p1[1], q1[2]-p1[2] };
    float d2[3] = { q2[0]-p2[0], q2[1]-p2[1], q2[2]-p2[2] };
    float r[3]  = { p1[0]-p2[0], p1[1]-p2[1], p1[2]-p2[2] };
    float a = Dot3(d1,d1), e = Dot3(d2,d2), f = Dot3(d2,r);
    const float EPS = 1e-8f;
    if(a <= EPS && e <= EPS) { s = t = 0.f; return; }
    if(a <= EPS) { s = 0.f; t = Clamp01(f / e); return; }
    float c = Dot3(d1,r);
    if(e <= EPS) { t = 0.f; s = Clamp01(-c / a); return; }
    float b = Dot3(d1,d2), denom = a*e - b*b;
    s = (denom > EPS) ? Clamp01((b*f - c*e) / denom) : 0.f;
    t = (b*s + f) / e;
    if(t < 0.f)      { t = 0.f; s = Clamp01(-c / a); }
    else if(t > 1.f) { t = 1.f; s = Clamp01((b - c) / a); }
}
static void SolveSegSeg(Ragdoll& r, const SegDef& A, const SegDef& B)
{
    float* pa = r.p[A.a]; float* pb = r.p[A.b]; float* pc = r.p[B.a]; float* pd = r.p[B.b];
    float s, t; ClosestSegSeg(pa, pb, pc, pd, s, t);
    float c1[3], c2[3];
    for(int k = 0; k < 3; ++k) { c1[k] = pa[k] + (pb[k]-pa[k])*s; c2[k] = pc[k] + (pd[k]-pc[k])*t; }
    float n[3] = { c1[0]-c2[0], c1[1]-c2[1], c1[2]-c2[2] };
    float dist = sqrtf(Dot3(n,n));
    float req = (A.r + B.r) * 0.88f;
    if(dist >= req) return;
    if(dist < 1e-5f) { n[0] = 0.f; n[1] = 0.f; n[2] = 1.f; dist = 0.f; }
    else { n[0] /= dist; n[1] /= dist; n[2] /= dist; }
    float ia = r.im[A.a], ib = r.im[A.b], ic = r.im[B.a], id = r.im[B.b];
    float den = (1-s)*(1-s)*ia + s*s*ib + (1-t)*(1-t)*ic + t*t*id;
    if(den < 1e-6f) return;
    float lam = (req - dist) / den * 0.9f;
    for(int k = 0; k < 3; ++k)
    {
        pa[k] += n[k]*lam*(1-s)*ia; pb[k] += n[k]*lam*s*ib;
        pc[k] -= n[k]*lam*(1-t)*ic; pd[k] -= n[k]*lam*t*id;
    }
}

static void RagdollStep(Ragdoll& r, float dt, unsigned int now)
{
    float g = cfgRdGravity * dt * dt;
    float t = (now - r.startTime) * 0.001f;
    float w = (cfgRdBlendTime > 0.001f) ? (1.0f - t / cfgRdBlendTime) : 0.f;
    if(w < 0.f) w = 0.f;
    w *= cfgRdBlend;
    for(unsigned i = 0; i < r.n; ++i)
    {
        if(!IsActive((int)i)) continue;
        for(int k = 0; k < 3; ++k)
        {
            float v = (r.p[i][k] - r.pv[i][k]) * cfgRdDamp;
            r.pv[i][k] = r.p[i][k];
            r.p[i][k] += v;
        }
        r.p[i][2] -= g;
        if(w > 0.f)                                   // al principio sigue la animacion del juego (intenta mantenerse)
            for(int k = 0; k < 3; ++k) r.p[i][k] += (r.T[i][k] - r.p[i][k]) * 0.35f * w;
    }
    // amortiguacion articular: frena la velocidad relativa entre huesos conectados (evita el efecto gelatina)
    if(cfgRdJointDamp > 0.f)
        for(int c = 0; c < r.nc; ++c)
        {
            if(r.cmn[c] < 0.97f) continue;                       // solo segmentos rigidos
            int a = r.ca[c], b = r.cb[c];
            for(int k = 0; k < 3; ++k)
            {
                float va = r.p[a][k] - r.pv[a][k], vb = r.p[b][k] - r.pv[b][k];
                float rel = (vb - va) * cfgRdJointDamp * 0.5f;
                r.pv[a][k] = r.p[a][k] - (va + rel);
                r.pv[b][k] = r.p[b][k] - (vb - rel);
            }
        }
    if(FindGroundZFor3DCoord)
        for(unsigned i = 0; i < r.n; ++i)
        {
            if(!IsActive((int)i)) continue;
            bool found = false; void* ent = NULL;
            float z = FindGroundZFor3DCoord(r.p[i][0], r.p[i][1], r.p[i][2] + 0.6f, &found, &ent);
            if(found) r.gz[i] = z;
        }
    for(int it = 0; it < cfgRdIter; ++it)
    {
        for(int c = 0; c < r.nc; ++c) SolvePair(r, r.ca[c], r.cb[c], r.cmn[c], r.cmx[c], r.cd0[c]);

        // las rodillas solo se doblan hacia delante y los codos hacia atras
        float Fn[9]; TorsoFrame(r.p, Fn);
        float F[3]; ApplyTorso(r.F0, Fn, r.fwd0, F);
        static const int legs[2][3] = { {24,25,26}, {28,29,30} };
        static const int arms[2][3] = { {10,11,12}, {16,17,18} };
        for(int s = 0; s < 2; ++s)
        {
            int H = legs[s][0], K = legs[s][1], A = legs[s][2];
            float mid[3] = { (r.p[H][0]+r.p[A][0])*0.5f, (r.p[H][1]+r.p[A][1])*0.5f, (r.p[H][2]+r.p[A][2])*0.5f };
            float dk[3] = { r.p[K][0]-mid[0], r.p[K][1]-mid[1], r.p[K][2]-mid[2] };
            float off = Dot3(dk, F);
            if(off < 0.f) for(int k = 0; k < 3; ++k) { float c = -off * F[k]; r.p[K][k] += c*0.5f; r.p[H][k] -= c*0.25f; r.p[A][k] -= c*0.25f; }
            int S = arms[s][0], E = arms[s][1], Hd = arms[s][2];
            float mid2[3] = { (r.p[S][0]+r.p[Hd][0])*0.5f, (r.p[S][1]+r.p[Hd][1])*0.5f, (r.p[S][2]+r.p[Hd][2])*0.5f };
            float de[3] = { r.p[E][0]-mid2[0], r.p[E][1]-mid2[1], r.p[E][2]-mid2[2] };
            float offE = Dot3(de, F) - 0.06f;
            if(offE > 0.f) for(int k = 0; k < 3; ++k) { float c = offE * F[k]; r.p[E][k] -= c*0.5f; r.p[S][k] += c*0.25f; r.p[Hd][k] += c*0.25f; }
        }

        // capsulas: huesos largos contra el resto del cuerpo (brazo/pierna no atraviesan torso ni entre si)
        if(cfgRdSelfColl > 0.f)
            for(int a = 0; a < NSEG; ++a)
                for(int b = a + 1; b < NSEG; ++b)
                {
                    if(SegAdj(kSeg[a], kSeg[b])) continue;
                    SolveSegSeg(r, kSeg[a], kSeg[b]);
                }

        // volumen: las partes del cuerpo no se atraviesan entre si
        for(int i = 1; i < 32 && cfgRdSelfColl > 0.f; ++i)
        {
            if(!IsActive(i)) continue;
            for(int j = i + 1; j < 32; ++j)
            {
                if(!IsActive(j)) continue;
                if(InTorso(i) && InTorso(j)) continue;
                if(kParent[j] == i || kParent[i] == j) continue;
                if((i == 5 && (InTorso(j))) || (j == 5 && InTorso(i))) continue;
                if(kParent[kParent[j] < 0 ? 0 : kParent[j]] == i) continue;
                float minD = (r.rad[i] + r.rad[j]) * 0.9f;
                float d[3] = { r.p[j][0]-r.p[i][0], r.p[j][1]-r.p[i][1], r.p[j][2]-r.p[i][2] };
                float l2 = Dot3(d,d);
                if(l2 >= minD*minD || l2 < 1e-8f) continue;
                float l = sqrtf(l2), diff = (l - minD) / l;
                float wa = r.im[i], wb = r.im[j], s2 = wa + wb;
                for(int k = 0; k < 3; ++k) { r.p[i][k] += d[k]*diff*wa/s2; r.p[j][k] -= d[k]*diff*wb/s2; }
            }
        }

        // suelo + friccion (cada articulacion con su radio)
        for(unsigned i = 0; i < r.n; ++i)
        {
            if(!IsActive((int)i)) continue;
            float floorZ = r.gz[i] + r.rad[i];
            if(r.p[i][2] < floorZ)
            {
                r.p[i][2] = floorZ;
                r.pv[i][0] += (r.p[i][0] - r.pv[i][0]) * 0.35f;
                r.pv[i][1] += (r.p[i][1] - r.pv[i][1]) * 0.35f;
            }
        }
    }
}

// aplica al vector v la rotacion del nodo i (segun su tipo)
static void NodeRotate(const Ragdoll& r, const float* Fn, int i, const float* v, float* o)
{
    int kind = kKind[i];
    if(kind == 0) { ApplyTorso(r.F0, Fn, v, o); return; }
    if(kind == 2) { float t[3]; ApplyTorso(r.F0, Fn, v, t); QRot(r.q[i], t, o); return; }
    QRot(r.q[i], v, o);
}

static void RagdollWrite(Ragdoll& r)
{
    unsigned int* h = (unsigned int*)r.hier;
    float* m = *(float**)(h + 2);
    if(!ValidPtr(m)) return;
    float Fn[9]; TorsoFrame(r.p, Fn);
    // giros por segmento
    for(int i = 0; i < 32; ++i)
    {
        Quat id = {0,0,0,1};
        r.q[i] = id;
        if(!IsActive(i) || kKind[i] == 0) continue;
        if(kKind[i] == 2)                                  // cabeza: direccion cuello->cabeza (ya con el giro del torso)
        {
            float u0[3] = { r.p0[5][0]-r.p0[4][0], r.p0[5][1]-r.p0[4][1], r.p0[5][2]-r.p0[4][2] }, u1[3];
            ApplyTorso(r.F0, Fn, u0, u1);
            float v[3] = { r.p[5][0]-r.p[4][0], r.p[5][1]-r.p[4][1], r.p[5][2]-r.p[4][2] };
            r.q[i] = QFromTo(u1, v);
            continue;
        }
        int c = kPC[i];
        if(c >= 0)
        {
            float u[3] = { r.p0[c][0]-r.p0[i][0], r.p0[c][1]-r.p0[i][1], r.p0[c][2]-r.p0[i][2] };
            float v[3] = { r.p[c][0]-r.p[i][0],   r.p[c][1]-r.p[i][1],   r.p[c][2]-r.p[i][2] };
            r.q[i] = QFromTo(u, v);
        }
        else if(kParent[i] >= 0) r.q[i] = r.q[kParent[i]];  // extremos (manos, puntas del pie)
    }
    for(int i = 0; i < 32; ++i)
    {
        float* mm = m + i*16;
        float o[3];
        if(IsActive(i))
        {
            NodeRotate(r, Fn, i, &r.R0[i][0], o); mm[0] = o[0]; mm[1] = o[1]; mm[2] = o[2];
            NodeRotate(r, Fn, i, &r.R0[i][3], o); mm[4] = o[0]; mm[5] = o[1]; mm[6] = o[2];
            NodeRotate(r, Fn, i, &r.R0[i][6], o); mm[8] = o[0]; mm[9] = o[1]; mm[10] = o[2];
            mm[12] = r.p[i][0]; mm[13] = r.p[i][1]; mm[14] = r.p[i][2];
        }
        else                                                   // sigue rigidamente a su ancla
        {
            int a = kAnchor[i];
            float rel[3] = { r.p0[i][0]-r.p0[a][0], r.p0[i][1]-r.p0[a][1], r.p0[i][2]-r.p0[a][2] };
            NodeRotate(r, Fn, a, rel, o);
            mm[12] = r.p[a][0] + o[0]; mm[13] = r.p[a][1] + o[1]; mm[14] = r.p[a][2] + o[2];
            NodeRotate(r, Fn, a, &r.R0[i][0], o); mm[0] = o[0]; mm[1] = o[1]; mm[2] = o[2];
            NodeRotate(r, Fn, a, &r.R0[i][3], o); mm[4] = o[0]; mm[5] = o[1]; mm[6] = o[2];
            NodeRotate(r, Fn, a, &r.R0[i][6], o); mm[8] = o[0]; mm[9] = o[1]; mm[10] = o[2];
        }
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
        float* m = *(float**)((unsigned int*)hier + 2);          // pose animada del juego = objetivo del inicio
        if(ValidPtr(m)) for(unsigned k = 0; k < r.n; ++k) { r.T[k][0] = m[k*16+12]; r.T[k][1] = m[k*16+13]; r.T[k][2] = m[k*16+14]; }
        if(now != r.lastTime && now - r.lastTime < 200)
        {
            float dt = (now - r.lastTime) * 0.001f;
            if(dt > 0.04f) dt = 0.04f;
            RagdollStep(r, dt, now);
            static int sl = 0;
            if(sl++ < 12) FLog("[ragdoll] paso dt=%.3f pelvis=(%.2f,%.2f,%.2f) pie=%.2f suelo=%.2f", dt, r.p[1][0], r.p[1][1], r.p[1][2], r.p[26][2], r.gz[1]);
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

struct HitInfo { void* ped; float dx, dy, force; unsigned int time; };
static HitInfo g_info[32];
static int     g_infoN = 0;
static void*   g_rdSeen[32];
static int     g_rdSeenN = 0;
static int     g_dieLogs = 0;

// Se llama cada frame mientras el ped muere (CTaskSimpleDie) o esta muerto (CTaskSimpleDead)
static void OnDying(const char* tag, void* ped, bool isDieTask)
{
    if(!cfgRagdoll || !pTimeMs || !ValidPtr(ped)) return;
    for(int i = 0; i < 32; ++i) if(g_rdSeen[i] == ped) return;       // ya procesado
    unsigned int now = *pTimeMs;
    HitInfo* hi = NULL;
    for(int i = 0; i < 32; ++i) if(g_info[i].ped == ped && now - g_info[i].time < 4000) hi = &g_info[i];
    if(!hi && !isDieTask) return;                                    // cadaver viejo: no tocar
    g_rdSeen[g_rdSeenN++ & 31] = ped;
    float dx = hi ? hi->dx : 0.f, dy = hi ? hi->dy : 0.f, f = hi ? hi->force : 8.f;
    if(f < 8.f) f = 8.f;
    if(g_dieLogs++ < 30) FLog("[die] %s ped=%p dir=(%.2f,%.2f) force=%.1f info=%d", tag, ped, dx, dy, f, hi ? 1 : 0);
    StartRagdoll(ped, dx, dy, f);
}

DECL_HOOK(int, CTaskSimpleDie_ProcessPed, void* self, void* ped)
{
    OnDying("die", ped, true);
    return CTaskSimpleDie_ProcessPed(self, ped);
}
DECL_HOOK(int, CTaskSimpleDead_ProcessPed, void* self, void* ped)
{
    OnDying("dead", ped, false);
    return CTaskSimpleDead_ProcessPed(self, ped);
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

    {
        float rx = 0.f, ry = 0.f;
        if(okP && okA) { float dx = pp.x - ap.x, dy = pp.y - ap.y, l = sqrtf(dx*dx + dy*dy); if(l > 0.01f) { rx = dx / l; ry = dy / l; } }
        g_info[g_infoN++ & 31] = { ped, rx, ry, force, now };
    }
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
    cfgPush    = cfg->GetFloat("PushForce", 0.08f, "Settings");
    cfgDecay   = cfg->GetFloat("Decay",       0.90f, "Settings");
    cfgWeaponM = cfg->GetFloat("WeaponMult",  1.0f,  "Settings");
    cfgCarM    = cfg->GetFloat("VehicleMult", 1.0f,  "Settings");
    cfgExplM   = cfg->GetFloat("ExplosionMult", 1.0f, "Settings");
    cfgPlayer  = cfg->GetBool ("AffectPlayer", false, "Settings");
    cfgLog     = cfg->GetBool ("WriteLog",    true,  "Settings");
    cfgBoneTest = cfg->GetBool("BoneTest",    false, "Settings");
    cfgRagdoll  = cfg->GetBool ("Ragdoll",    true,  "Settings");
    cfgRdGravity = cfg->GetFloat("RagdollGravity", 14.0f, "Settings");
    cfgRdImpulse = cfg->GetFloat("RagdollPush", 0.10f, "Settings");
    cfgRdRadius  = cfg->GetFloat("RagdollRadius",  0.10f, "Settings");
    cfgRdDamp    = cfg->GetFloat("RagdollDamping", 0.992f, "Settings");
    cfgRdBlendTime = cfg->GetFloat("RagdollBlendTime2", 0.0f, "Settings");
    cfgRdBlend     = cfg->GetFloat("RagdollBlend2", 0.0f, "Settings");
    cfgRdIter      = (int)cfg->GetFloat("RagdollIterations", 10.0f, "Settings");
    cfgRdStiff     = cfg->GetFloat("RagdollStiffness", 1.0f, "Settings");
    cfgRdJointDamp = cfg->GetFloat("RagdollJointDamping", 0.12f, "Settings");
    cfgRdSelfColl  = cfg->GetFloat("RagdollSelfCollision", 1.0f, "Settings");
    if(cfgRdIter < 2) cfgRdIter = 2; if(cfgRdIter > 24) cfgRdIter = 24;
    if(cfgRdStiff < 0.05f) cfgRdStiff = 0.05f; if(cfgRdStiff > 1.0f) cfgRdStiff = 1.0f;
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
    HOOKSYM(CTaskSimpleDie_ProcessPed,  hGTASA, "_ZN14CTaskSimpleDie10ProcessPedEP4CPed");
    HOOKSYM(CTaskSimpleDead_ProcessPed, hGTASA, "_ZN15CTaskSimpleDead10ProcessPedEP4CPed");
    HOOKSYM(CPed_ProcessControl,            hGTASA, "_ZN4CPed14ProcessControlEv");
    HOOKSYM(RpHAnimHierarchyUpdateMatrices_hook, hGTASA, "_Z30RpHAnimHierarchyUpdateMatricesP16RpHAnimHierarchy");

    FLog("=== GTA4 Ragdoll Android 0.1 cargado. base=0x%X ApplyMoveForce=%p FindPlayerPed=%p timer=%p",
         (unsigned)pGTASA, (void*)ApplyMoveForce, (void*)FindPlayerPed, (void*)pTimeMs);
    logger->Info("Cargado. ApplyMoveForce=%p", (void*)ApplyMoveForce);
}
