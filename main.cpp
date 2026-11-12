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
#include <unistd.h>
#include <sys/mman.h>

MYMODCFG(net.gta4ragdoll.sa, GTA4 Ragdoll Android, 0.1, GTA4RagdollProject)
NEEDGAME(com.rockstargames.gtasa)
BEGIN_DEPLIST()
END_DEPLIST()

struct CVector { float x, y, z; };

// ---- Desplazamientos que dependen del tamano del puntero ----
//  32 bits (GTA SA 2.00): verificados con los logs de tu juego.
//  64 bits (GTA SA 2.10): derivados de las reglas de alineacion de C++ (vtable de 8 bytes, punteros de 8) y de RenderWare 3.7;
//                         el mod los valida al arrancar y escribe en el log "[arch]" lo que encuentra.
struct ArchOff
{
    int matrix;        // CPlaceable::m_matrix (puntero a CMatrix)
    int placePos;      // CPlaceable::m_placement.pos
    int rwObj;         // CEntity::m_pRwObject (RpClump*)
    int clumpParent;   // RpClump.object.parent (RwFrame*)
    int hierNodeInfo;  // RpHAnimHierarchy::pNodeInfo
    int nodeStride;    // sizeof(RpHAnimNodeInfo)
};
static const ArchOff A = (sizeof(void*) == 8) ? ArchOff{ 0x18, 0x08, 0x20, 8, 24, 24 } : ArchOff{ 0x14, 0x04, 0x18, 4, 16, 16 };
// CEventDamage: atacante y arma. En 32 bits son fijos; en 64 bits se calibran con los argumentos del constructor.
static int g_evAtk = (sizeof(void*) == 8) ? -1 : 0x10;
static int g_evWpn = (sizeof(void*) == 8) ? -1 : 0x18;

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
typedef void (*FindObjectsInRange_t)(const void* center, float radius, bool b2D, short* n, short maxN, void** list, bool bld, bool veh, bool ped, bool obj, bool dum);
static FindObjectsInRange_t        FindObjectsInRange        = NULL;
static void**                      pModelInfoPtrs            = NULL;

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
static bool  cfgPlayer  = true;    // el ragdoll tambien se aplica a CJ
static bool  cfgLog     = true;
static bool  cfgRagdoll = true;     // ragdoll (Verlet) al morir
static float cfgRdGravity = 14.0f;  // m/s^2
static float cfgRdImpulse = 0.06f;  // empuje inicial por punto de fuerza
static float cfgRdRadius  = 0.10f;  // radio de cada articulacion contra el suelo
static float cfgRdDamp    = 0.985f; // amortiguacion por paso
static float cfgRdBlendTime = 0.0f;  // segundos que sigue la animacion del juego (0 = ragdoll instantaneo)
static float cfgRdBlend     = 0.0f;  // fuerza de esa mezcla (0 = ragdoll puro)
static int   cfgRdIter      = 10;    // iteraciones del solver (mas = mas rigido, mas costo)
static float cfgRdStiff     = 1.0f;  // rigidez de las articulaciones (0..1)
static float cfgRdJointDamp = 0.30f; // amortiguacion de velocidad relativa entre huesos conectados
static float cfgRdSelfColl  = 1.0f;  // 0 = sin colision entre partes del cuerpo
static float cfgMaxImpulse   = 2.0f;   // tope de fuerza por frame sobre un NPC (MaxImpulseLimit)
static int   cfgMaxExpTicks  = 2;      // maximo de explosiones procesadas por NPC cada 100 ms
static float cfgRdMaxStep    = 0.10f;  // tope de desplazamiento por paso de cada articulacion (m)
static bool  cfgLenLock      = true;   // bloqueo estricto de longitud de huesos
static int   cfgLegPasses    = 4;      // pasadas extra de rigidez en piernas/caderas
static float cfgLegDamp      = 0.35f;  // amortiguacion extra de rodillas/caderas
static float cfgKneeSpring   = 0.004f; // resistencia de la rodilla: tiende a una flexion natural
static bool  cfgRunOver      = true;   // atropello: ragdoll aunque el NPC no muera
static float cfgRunOverMin   = 2.5f;   // velocidad minima del vehiculo (m/s) para activar el ragdoll
static float cfgRunOverXfer  = 0.60f;  // fraccion de la velocidad del vehiculo que se transfiere al cuerpo
static float cfgAliveTime    = 3.5f;   // segundos que dura el ragdoll de un NPC vivo
static float cfgReleaseTime  = 0.35f;  // segundos de mezcla al devolver el control a la animacion
static bool  cfgSyncEntity   = true;   // el NPC vivo sigue a su cuerpo ragdoll
static bool  cfgVehCollision = true;   // el cuerpo ragdoll choca con vehiculos
static bool  cfgSafeReads    = true;
static float cfgRdBodyScale  = 1.0f;   // escala del volumen del cuerpo (radios)
static float cfgSpineStiff   = 0.9f;   // 0..1: cuanto se mantiene recta la columna   // comprobar con mincore() que un puntero es legible antes de leerlo
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

// Lectura segura: mincore() falla si la pagina no esta mapeada. Evita cerrar el juego por punteros
// colgantes (por ejemplo, una mochila explosiva que ya exploto y fue borrada).
static bool CanRead(const void* p, size_t n)
{
    uintptr_t a = (uintptr_t)p;
    if(a < 0x10000) return false;
    if(!cfgSafeReads) return true;
    uintptr_t pg = (uintptr_t)getpagesize();
    uintptr_t start = a & ~(pg - 1);
    uintptr_t end = (a + n + pg - 1) & ~(pg - 1);
    size_t len = end - start;
    if(len == 0 || len > pg * 4) return false;
    unsigned char vec[4];
    return mincore((void*)start, len, vec) == 0;
}
static inline bool FiniteF(float v) { return v == v && v > -1e7f && v < 1e7f; }
static inline bool InWorld(float x, float y, float z)
{ return FiniteF(x) && FiniteF(y) && FiniteF(z) && fabsf(x) < 8000.f && fabsf(y) < 8000.f && z > -500.f && z < 3000.f; }
static bool IsValidPed(void* ped) { return ValidPtr(ped) && (((uintptr_t)ped) & 3) == 0 && CanRead(ped, 0x600); }

// CPlaceable: +0x04 posicion simple, +0x14 puntero a CMatrix (pos en +0x30). (igual que PC en 32 bits)
static bool GetPos(void* ent, CVector& out)
{
    if(!ValidPtr(ent) || (((uintptr_t)ent) & 3) != 0 || !CanRead(ent, 0x20)) return false;
    void* mtx = *(void**)((char*)ent + A.matrix);
    if(ValidPtr(mtx) && CanRead(mtx, 0x40)) out = *(CVector*)((char*)mtx + 0x30);
    else                                    out = *(CVector*)((char*)ent + A.placePos);
    return InWorld(out.x, out.y, out.z);
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
    if(!(pw == pw) || pw > cfgMaxImpulse) pw = cfgMaxImpulse;   // sin NaN y con tope (MaxImpulseLimit)
    g_st[slot].power  = pw;
    g_st[slot].frames = (force >= cfgHeavy) ? 45 : 28;
}

// ---------------------------------------------------------------- hooks
static int g_ctorCalls = 0, g_copyCalls = 0;
static int g_evVotesA[48], g_evVotesW[48], g_evNA = 0, g_evNW = 0, g_evLastW = -999, g_evDistinctW = 0;
static void CalibrateEvent(void* self, void* attacker, int weapon)
{
    if(g_evAtk >= 0 && g_evWpn >= 0) return;
    if(!ValidPtr(self) || !CanRead(self, 0xC0)) return;
    const int ps = (int)sizeof(void*);
    if(ValidPtr(attacker)) { ++g_evNA; for(int off = 8; off < 0xC0; off += ps) if(*(void**)((char*)self + off) == attacker) ++g_evVotesA[off / 4]; }
    if(weapon >= 0 && weapon < 60)
    {
        ++g_evNW; if(weapon != g_evLastW) { ++g_evDistinctW; g_evLastW = weapon; }
        for(int off = 8; off < 0xC0; off += 4) if(*(int*)((char*)self + off) == weapon) ++g_evVotesW[off / 4];
    }
    if(g_evNA >= 4 && g_evNW >= 8 && g_evDistinctW >= 3)
    {
        int bestA = -1, bestW = -1, maxA = 0, maxW = 0, tiesA = 0, tiesW = 0;
        for(int i = 0; i < 48; ++i)
        {
            if(g_evVotesA[i] > maxA) { maxA = g_evVotesA[i]; bestA = i * 4; tiesA = 0; } else if(g_evVotesA[i] == maxA && maxA > 0) ++tiesA;
            if(g_evVotesW[i] > maxW) { maxW = g_evVotesW[i]; bestW = i * 4; tiesW = 0; } else if(g_evVotesW[i] == maxW && maxW > 0) ++tiesW;
        }
        if(bestA >= 0 && maxA >= g_evNA && bestW >= 0 && maxW >= g_evNW - 1 && tiesA == 0 && tiesW == 0)
        {
            g_evAtk = bestA; g_evWpn = bestW;
            FLog("[arch] CEventDamage calibrado: atacante +0x%X (%d/%d) arma +0x%X (%d/%d)", g_evAtk, maxA, g_evNA, g_evWpn, maxW, g_evNW);
        }
        else if(g_evNW > 40) { g_evNW = 0; memset(g_evVotesA, 0, sizeof(g_evVotesA)); memset(g_evVotesW, 0, sizeof(g_evVotesW)); g_evNA = 0; g_evDistinctW = 0; }
    }
}

static void RecordEvent(void* self, void* attacker, int weapon, int piece)
{
    DmgRec& r = g_rec[g_recN++ & 15];
    r.self = self; r.attacker = attacker; r.weapon = weapon; r.piece = piece;
    r.time = pTimeMs ? *pTimeMs : 0;
    if(g_ctorCalls++ < 12) FLog("[ctor] evento=%p attacker=%p weapon=%d piece=%d", self, attacker, weapon, piece);
    CalibrateEvent(self, attacker, weapon);
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
    if(g_evAtk < 0 || g_evWpn < 0) return false;                       // 64 bits: aun sin calibrar (se usa el plan B)
    if(!ValidPtr(ev) || (((uintptr_t)ev) & 3) != 0 || !CanRead(ev, 0x60)) return false;
    weapon   = *(int*)((char*)ev + g_evWpn);
    attacker = *(void**)((char*)ev + g_evAtk);
    return weapon >= 0 && weapon < 60;
}

struct HitMark { void* ped; unsigned int time; };
static HitMark g_hit[32];
static int     g_hitN = 0;
static int     g_hitLogs = 0;

// ---- FASE 2.1: prueba de escritura de huesos ----
static void* GetPedHierarchyEx(void* ped, void** outClump, void** outFrame)
{
    if(!IsValidPed(ped)) return NULL;
    void* clump = *(void**)((char*)ped + A.rwObj);
    if(!ValidPtr(clump) || !CanRead(clump, 0x10)) return NULL;
    if(*(unsigned char*)clump != 2) return NULL;                       // RwObject.type de un RpClump = 2 (verificado en tus logs)
    void* frame = *(void**)((char*)clump + A.clumpParent);
    if(!ValidPtr(frame) || !CanRead(frame, 0x20) || !GetAnimHierarchyFromFrame) return NULL;
    void* hier = GetAnimHierarchyFromFrame(frame);
    if(!ValidPtr(hier) || !CanRead(hier, 0x24)) return NULL;
    if(outClump) *outClump = clump;
    if(outFrame) *outFrame = frame;
    return hier;
}
static void* GetPedHierarchy(void* ped) { return GetPedHierarchyEx(ped, NULL, NULL); }

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
#define RD_SLOTS 6

// Tablas del esqueleto de ped de SA (verificadas con las banderas de tu log)
static const int kParent[32] = {-1,0,1,2,3,4,5,5,5,4,9,10,11,12,13,4,15,16,17,18,19,3,3,2,1,24,25,26,1,28,29,30};
static const int kAnchor[32] = {1,-1,1,-1,-1,-1,5,5,5,3,-1,-1,-1,12,12,3,-1,-1,-1,18,18,3,3,1,-1,-1,-1,26,-1,-1,-1,30};
// Huesos SIMULADOS (16): pelvis 1, columna 3, cuello 4, cabeza 5, hombros 10/16, codos 11/17, munecas 12/18,
//                        caderas 24/28, rodillas 25/29, tobillos 26/30.  El resto sigue rigidamente a su ancla.
// 0 = gira con el marco del torso, 1 = giro por segmento (swing), 2 = cuello (torso + swing), 3 = hereda el giro del padre
static const int kKind[32]   = {0,0,0,0,2,3,0,0,0,0,1,1,1,0,0,0,1,1,1,0,0,0,0,0,1,1,1,0,1,1,1,0};
static const int kPC[32]     = {-1,-1,-1,-1,5,-1,-1,-1,-1,-1,11,12,-1,-1,-1,-1,17,18,-1,-1,-1,-1,-1,-1,25,26,-1,-1,29,30,-1,-1};
#define RD_NTORSO 7
static const int kTorso[RD_NTORSO] = {1,3,4,10,16,24,28};   // torso = cuerpo rigido (pelvis, columna, cuello, hombros, caderas)

struct CDef { unsigned char a, b; float mn, mx; };
static const CDef kC[] = {
    {4,5,0.985f,1.015f}, {3,5,0.94f,1.06f}, {10,5,0.90f,1.10f}, {16,5,0.90f,1.10f},    // cuello / cabeza (limite ~45 grados)
    {10,11,0.99f,1.01f}, {11,12,0.99f,1.01f},                                           // brazo derecho
    {16,17,0.99f,1.01f}, {17,18,0.99f,1.01f},                                           // brazo izquierdo
    {24,25,0.99f,1.01f}, {25,26,0.99f,1.01f},                                           // pierna derecha
    {28,29,0.99f,1.01f}, {29,30,0.99f,1.01f},                                           // pierna izquierda
    {10,12,0.50f,1.00f}, {16,18,0.50f,1.00f},                                           // flexion del codo (max ~115 grados)
    {24,26,0.74f,1.00f}, {28,30,0.74f,1.00f},                                           // flexion de la rodilla (max ~85 grados)
    {1,25,0.82f,1.15f},  {1,29,0.82f,1.15f},                                            // cadera: la pierna no se cruza bajo el cuerpo
    {25,29,0.62f,4.00f}, {26,30,0.62f,4.00f}                                            // separacion minima entre rodillas y entre tobillos
};

static inline bool IsActive(int i) { return kAnchor[i] < 0; }
static inline bool InTorso(int i) { for(int k = 0; k < RD_NTORSO; ++k) if(kTorso[k] == i) return true; return false; }
// Radio de cada articulacion = grosor de la "piel" alrededor del hueso (evita cuerpos aplastados)
static float NodeRadius(int i)
{
    float r = 0.06f;
    if(i == 1 || i == 3) r = 0.16f;               // pelvis y pecho
    else if(i == 4) r = 0.12f;                    // cuello / base de la cabeza
    else if(i == 5) r = 0.12f;                    // cabeza
    else if(i == 10 || i == 16) r = 0.09f;        // hombros
    else if(i == 24 || i == 28) r = 0.11f;        // caderas
    else if(i == 25 || i == 29) r = 0.08f;        // rodillas
    else if(i == 11 || i == 17) r = 0.06f;        // codos
    else if(i == 12 || i == 18) r = 0.05f;        // munecas
    else if(i == 26 || i == 30) r = 0.06f;        // tobillos
    return r * cfgRdBodyScale;
}
// Masa aproximada (kg) de la parte del cuerpo que representa cada articulacion
static float NodeMass(int i)
{
    switch(i)
    {
        case 1:  return 12.f;  case 3:  return 10.f;                 // pelvis, pecho
        case 4:  return 3.f;   case 5:  return 5.f;                  // cuello, cabeza
        case 10: case 16: return 4.f;                                // hombro + brazo
        case 11: case 17: return 2.2f;  case 12: case 18: return 1.2f;   // antebrazo, mano
        case 24: case 28: return 9.f;                                // muslo
        case 25: case 29: return 4.5f;  case 26: case 30: return 1.6f;   // pantorrilla, pie
        default: return 2.f;
    }
}
static float NodeInvMass(int i) { return 1.0f / NodeMass(i); }
// amortiguacion por parte: lo liviano (manos, pies) se frena mas; lo pesado conserva inercia
static float NodeDamp(int i)
{
    float m = NodeMass(i);
    float base = cfgRdDamp;
    float extra = (m < 2.0f) ? 0.02f : (m < 5.0f ? 0.01f : 0.0f);
    return base - extra;
}

// ======================= VEHICULOS: seguimiento y cajas de colision =======================
// Cada tick se consultan los vehiculos cercanos al jugador con CWorld::FindObjectsInRange (solo devuelve
// entidades vivas) y se guarda una COPIA de su estado: nunca se vuelve a leer un puntero de vehiculo despues.
struct VehTrack
{
    bool  used, bbox; int bboxTry;
    void* v; unsigned int seen, tPrev;
    float pos[3], prev[3], vel[3];          // posicion y velocidad (m/s) por diferencias entre frames
    float ax[3], ay[3], az[3];              // ejes: derecha, adelante, arriba
    float bmn[3], bmx[3];                   // caja local del modelo
};
#define NVEH 32
static VehTrack g_veh[NVEH];
static unsigned int g_vehTick = 0xFFFFFFFFu;
static int g_miOff = -1, g_colOff = -1;
static int g_vehLogs = 0;

static bool PlausibleBox(const float* b)
{   // {min.x,min.y,min.z,max.x,max.y,max.z}: dimensiones de un vehiculo
    for(int i = 0; i < 6; ++i) if(!FiniteF(b[i])) return false;
    return b[0] < -0.2f && b[3] > 0.2f && b[1] < -0.5f && b[4] > 0.5f && b[5] > b[2] + 0.5f
        && b[0] > -4.5f && b[3] < 4.5f && b[1] > -14.f && b[4] < 14.f && b[2] > -3.5f && b[5] < 6.f;
}
// La caja se busca en la col. del modelo; los desplazamientos de este juego se calibran solos en la primera lectura valida
static bool ReadVehBox(void* veh, float* mn, float* mx)
{
    if(!pModelInfoPtrs) return false;
    static const int entOffs[] = { 0x22, 0x26, 0x24, 0x20, 0x28, 0x2A, 0x2E, 0x32, 0x30, 0x2C, 0x34, 0x36 };   // 32 bits: 0x22 / 64 bits: se calibra
    for(unsigned a = 0; a < sizeof(entOffs)/sizeof(entOffs[0]); ++a)
    {
        int eo = entOffs[a];
        if(g_miOff >= 0 && eo != g_miOff) continue;
        unsigned short mi = *(unsigned short*)((char*)veh + eo);
        if(mi == 0 || mi >= 20000) continue;
        void* info = pModelInfoPtrs[mi];
        if(!ValidPtr(info) || (((uintptr_t)info) & 3) || !CanRead(info, 0x60)) continue;
        for(int co = 0x08; co <= 0x60; co += (int)sizeof(void*))
        {
            if(g_colOff >= 0 && co != g_colOff) continue;
            void* col = *(void**)((char*)info + co);
            if(!ValidPtr(col) || (((uintptr_t)col) & 3) || !CanRead(col, 0x20)) continue;
            const float* b = (const float*)col;
            if(!PlausibleBox(b)) continue;
            if(g_miOff < 0) { g_miOff = eo; g_colOff = co; FLog("[veh] calibrado: modelo en +0x%X, colision en +0x%X, caja (%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)", eo, co, b[0], b[1], b[2], b[3], b[4], b[5]); }
            for(int k = 0; k < 3; ++k) { mn[k] = b[k]; mx[k] = b[3+k]; }
            return true;
        }
    }
    return false;
}

static VehTrack* FindTrack(void* v)
{
    for(int i = 0; i < NVEH; ++i) if(g_veh[i].used && g_veh[i].v == v) return &g_veh[i];
    return NULL;
}

static void UpdateVehicles(unsigned int now)
{
    if(now == g_vehTick) return;
    g_vehTick = now;
    if(!FindObjectsInRange || !FindPlayerPed) return;
    void* pl = FindPlayerPed(0);
    CVector c;
    if(!GetPos(pl, c)) return;
    short n = 0; void* list[48];
    FindObjectsInRange(&c, 45.0f, false, &n, 48, list, false, true, false, false, false);
    if(n <= 0 || n > 48) return;
    for(int i = 0; i < n; ++i)
    {
        void* e = list[i];
        if(!ValidPtr(e) || (((uintptr_t)e) & 3) || !CanRead(e, 0x60)) continue;
        void* m = *(void**)((char*)e + A.matrix);
        if(!ValidPtr(m) || (((uintptr_t)m) & 3) || !CanRead(m, 0x40)) continue;
        const float* f = (const float*)m;
        if(!InWorld(f[12], f[13], f[14])) continue;
        VehTrack* t = FindTrack(e);
        if(!t)
        {
            int slot = -1; unsigned int oldest = 0xFFFFFFFFu;
            for(int k = 0; k < NVEH; ++k)
            {
                if(!g_veh[k].used || now - g_veh[k].seen > 2000) { slot = k; break; }
                if(g_veh[k].seen < oldest) { oldest = g_veh[k].seen; slot = k; }
            }
            t = &g_veh[slot];
            memset(t, 0, sizeof(*t));
            t->used = true; t->v = e; t->tPrev = now;
            t->prev[0] = f[12]; t->prev[1] = f[13]; t->prev[2] = f[14];
        }
        float dt = (now - t->tPrev) * 0.001f;
        if(now != t->tPrev)
        {
            if(dt > 0.004f && dt < 0.25f)
            {
                for(int k = 0; k < 3; ++k)
                {
                    float nv = (f[12+k] - t->prev[k]) / dt;
                    if(!FiniteF(nv) || fabsf(nv) > 120.f) nv = 0.f;
                    t->vel[k] = t->vel[k] * 0.5f + nv * 0.5f;
                }
            }
            else if(dt >= 0.25f) { t->vel[0] = t->vel[1] = t->vel[2] = 0.f; }
            t->prev[0] = f[12]; t->prev[1] = f[13]; t->prev[2] = f[14]; t->tPrev = now;
        }
        for(int k = 0; k < 3; ++k) { t->pos[k] = f[12+k]; t->ax[k] = f[k]; t->ay[k] = f[4+k]; t->az[k] = f[8+k]; }
        t->seen = now;
        if(!t->bbox && t->bboxTry < 6)
        {
            ++t->bboxTry;
            float mn[3], mx[3];
            if(ReadVehBox(e, mn, mx)) { for(int k = 0; k < 3; ++k) { t->bmn[k] = mn[k]; t->bmx[k] = mx[k]; } t->bbox = true; }
            else if(t->bboxTry >= 6)    // caja por defecto de un coche
            {
                t->bmn[0] = -0.95f; t->bmn[1] = -2.25f; t->bmn[2] = -0.70f; t->bmx[0] = 0.95f; t->bmx[1] = 2.25f; t->bmx[2] = 0.90f; t->bbox = true;
                if(g_vehLogs++ < 5) FLog("[veh] sin caja de colision para el vehiculo %p, se usa una caja por defecto", e);
            }
        }
    }
}

struct VBox { float c[3], ax[3], ay[3], az[3], h[3], vs[3]; void* id; };
static void BuildBox(const VehTrack& t, float dt, const float* mn, const float* mx, VBox& b)
{
    for(int k = 0; k < 3; ++k)
    {
        b.h[k] = (mx[k] - mn[k]) * 0.5f;
        b.ax[k] = t.ax[k]; b.ay[k] = t.ay[k]; b.az[k] = t.az[k];
        b.vs[k] = t.vel[k] * dt;
    }
    float cl[3] = { (mx[0]+mn[0])*0.5f, (mx[1]+mn[1])*0.5f, (mx[2]+mn[2])*0.5f };
    for(int k = 0; k < 3; ++k) b.c[k] = t.pos[k] + t.ax[k]*cl[0] + t.ay[k]*cl[1] + t.az[k]*cl[2];
    b.id = t.v;
}
// Caja completa del vehiculo (para comprobar si algo esta dentro / se acerca)
static bool MakeVBox(const VehTrack& t, float dt, VBox& b)
{
    if(!t.bbox) return false;
    BuildBox(t, dt, t.bmn, t.bmx, b);
    return true;
}
// Forma para colisionar: base (paragolpes y bajos) + cabina/capo mas corta. Asi las piernas chocan con el paragolpes y el
// torso puede caer sobre el capo. Los vehiculos largos o bajos (camiones, motos) usan una sola caja.
static int MakeVShape(const VehTrack& t, float dt, VBox* out)
{
    if(!t.bbox) return 0;
    float len = t.bmx[1] - t.bmn[1], hgt = t.bmx[2] - t.bmn[2];
    if(len < 3.2f || len > 6.2f || hgt < 1.1f)
    {
        BuildBox(t, dt, t.bmn, t.bmx, out[0]);
        return 1;
    }
    float zs = t.bmn[2] + hgt * 0.55f;                   // altura del capo
    float lo_mn[3] = { t.bmn[0], t.bmn[1], t.bmn[2] }, lo_mx[3] = { t.bmx[0], t.bmx[1], zs };
    float up_mn[3] = { t.bmn[0], t.bmn[1] + len * 0.30f, zs }, up_mx[3] = { t.bmx[0], t.bmx[1] - len * 0.12f, t.bmx[2] };
    BuildBox(t, dt, lo_mn, lo_mx, out[0]);
    BuildBox(t, dt, up_mn, up_mx, out[1]);
    return 2;
}
// La articulacion no puede entrar en la caja del vehiculo: sale por la cara mas cercana y adopta su velocidad
// (el vehiculo "empuja", y el cuerpo puede quedar apoyado sobre el capo o el techo)
static bool CollideParticleBox(float* p, float* pv, float rad, const VBox& b)
{
    float d[3] = { p[0]-b.c[0], p[1]-b.c[1], p[2]-b.c[2] };
    float lx = Dot3(d, b.ax), ly = Dot3(d, b.ay), lz = Dot3(d, b.az);
    float ex = b.h[0] + rad - fabsf(lx), ey = b.h[1] + rad - fabsf(ly), ez = b.h[2] + rad - fabsf(lz);
    if(ex <= 0.f || ey <= 0.f || ez <= 0.f) return false;
    const float* ax; float pen, sg;
    if(ex <= ey && ex <= ez) { ax = b.ax; pen = ex; sg = lx >= 0.f ? 1.f : -1.f; }
    else if(ey <= ez)        { ax = b.ay; pen = ey; sg = ly >= 0.f ? 1.f : -1.f; }
    else                     { ax = b.az; pen = ez; sg = lz >= 0.f ? 1.f : -1.f; }
    float n[3] = { ax[0]*sg, ax[1]*sg, ax[2]*sg };
    float vrel[3] = { p[0]-pv[0]-b.vs[0], p[1]-pv[1]-b.vs[1], p[2]-pv[2]-b.vs[2] };   // velocidad antes de corregir, relativa al vehiculo
    for(int k = 0; k < 3; ++k) p[k] += n[k] * pen;
    float vn = Dot3(vrel, n);
    float vt[3] = { vrel[0]-n[0]*vn, vrel[1]-n[1]*vn, vrel[2]-n[2]*vn };
    float keep = vn > 0.f ? vn : 0.f;                         // sin rebote: no sigue entrando
    for(int k = 0; k < 3; ++k) pv[k] = p[k] - (n[k]*keep + vt[k]*0.80f + b.vs[k]);   // 20% de friccion: el vehiculo lo arrastra
    return true;
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
    float sp[RD_MAXN][3], spv[RD_MAXN][3];   // ultimo estado valido (para volver atras si algo se rompe)
    bool  hasSnap, leg[RD_MAXC];
    int   fails;
    float c0[3];
    float spT[2], spDev[2][3];   // columna: posicion de los nodos 2 y 3 respecto a la recta pelvis->cuello
    bool  asleep; int calm;      // reposo: el cuerpo ya se detuvo en el suelo
    void* clump; void* frame;    // cadena ped->clump->frame guardada al empezar (validacion sin llamar al juego)
    float dk[RD_MAXN];           // amortiguacion de cada particula
    int   dbg;                   // pasos registrados en el log
    float dtPrev;                // duracion del paso anterior (integracion de Verlet con dt variable)
    bool  alive, releasing, hasVel; unsigned int aliveUntil, relStart, sleepT;
    float vel0[3];               // velocidad inicial (m/s) transferida por un atropello
    float maxStep;               // tope de desplazamiento por paso de este ragdoll
    float entOff[3]; bool entOk; // desplazamiento entre la entidad del ped y su pelvis
    void* ignoreV[4]; int nIgn;  // vehiculos que se ignoran (el del propio ocupante)
};
static Ragdoll g_rd[RD_SLOTS];

// Devuelve: 1 = iniciado/ya activo, 0 = reintentar en el siguiente frame, -1 = no se puede (se descarta)
static int StartRagdollEx(void* ped, float dx, float dy, float force, bool alive, const float* vel)
{
    void *clump = NULL, *frame = NULL;
    void* h = GetPedHierarchyEx(ped, &clump, &frame);
    if(!h) return -1;
    for(int i = 0; i < RD_SLOTS; ++i) if(g_rd[i].used && g_rd[i].hier == h) return 1;   // ya activo
    unsigned int now = pTimeMs ? *pTimeMs : 0;
    static unsigned int lastStart = 0xFFFFFFFFu;
    if(now == lastStart) return 0;                          // tope de explosion: un ragdoll nuevo por tick (los demas esperan)
    int slot = -1;
    for(int i = 0; i < RD_SLOTS; ++i) if(!g_rd[i].used) { slot = i; break; }
    if(slot < 0) return -1;                                 // sin slots libres: el juego usa su animacion normal
    lastStart = now;
    Ragdoll& r = g_rd[slot];
    memset(&r, 0, sizeof(r));
    r.used = true; r.ped = ped; r.hier = h; r.clump = clump; r.frame = frame;
    r.dirx = dx; r.diry = dy; r.force = force;
    r.maxStep = cfgRdMaxStep;
    r.alive = alive; if(alive) r.aliveUntil = now + (unsigned int)(cfgAliveTime * 1000.f);
    if(vel)
    {
        r.hasVel = true; r.vel0[0] = vel[0]; r.vel0[1] = vel[1]; r.vel0[2] = vel[2];
        if(r.maxStep < 0.20f) r.maxStep = 0.20f;               // un atropello puede lanzar mas rapido que un disparo
    }
    FLog("[ragdoll] solicitado ped=%p hier=%p dir=(%.2f,%.2f) force=%.1f slot=%d vivo=%d vel=%d", ped, h, dx, dy, force, slot, alive ? 1 : 0, vel ? 1 : 0);
    return 1;
}
static int StartRagdoll(void* ped, float dx, float dy, float force) { return StartRagdollEx(ped, dx, dy, force, false, NULL); }

// marco del torso a partir de pelvis(1), cuello(4) y caderas(24,28)
static void TorsoFrame(const float P[][3], float* F)
{
    float u[3] = { P[4][0]-P[1][0], P[4][1]-P[1][1], P[4][2]-P[1][2] }; Norm3(u);
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
    unsigned char* ni = *(unsigned char**)((char*)r.hier + A.hierNodeInfo);   // RpHAnimNodeInfo[] (16 bytes en 32 bits, 24 en 64)
    if(!ValidPtr(m) || !ValidPtr(ni) || n != 32) { r.used = false; FLog("[ragdoll] esqueleto inesperado n=%u", n); return; }
    // comprobar que la jerarquia coincide con la tabla (banderas PUSH=2 / POP=1)
    int stack[RD_MAXN]; int sp = 0; int cur = -1; bool ok = true;
    for(unsigned i = 0; i < n; ++i)
    {
        int flags = *(int*)(ni + i*A.nodeStride + 8);
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
        r.rad[i] = NodeRadius((int)i); r.im[i] = NodeInvMass((int)i); r.dk[i] = NodeDamp((int)i);
    }
    // pose inicial coherente? (modelo raro o pose de explosion con huesos fuera de rango: no se usa ragdoll)
    {
        static const struct { int a, b; float mn, mx; } chk[] = {
            {24,25,0.25f,0.70f}, {25,26,0.22f,0.65f}, {10,11,0.12f,0.50f}, {11,12,0.12f,0.50f}, {1,4,0.35f,1.00f}, {4,5,0.05f,0.35f} };
        bool sane = true;
        for(unsigned k = 0; k < sizeof(chk)/sizeof(chk[0]); ++k)
        {
            float d[3] = { r.p0[chk[k].b][0]-r.p0[chk[k].a][0], r.p0[chk[k].b][1]-r.p0[chk[k].a][1], r.p0[chk[k].b][2]-r.p0[chk[k].a][2] };
            float l = sqrtf(Dot3(d,d));
            if(!(l >= chk[k].mn && l <= chk[k].mx)) { sane = false; FLog("[ragdoll] pose rara: hueso %d-%d mide %.2f m", chk[k].a, chk[k].b, l); }
        }
        for(unsigned i = 0; i < n && sane; ++i) if(!InWorld(r.p0[i][0], r.p0[i][1], r.p0[i][2])) sane = false;
        if(!sane) { r.used = false; return; }
    }
    // restricciones: cluster rigido del torso + tabla de articulaciones
    r.nc = 0;
    for(int x = 0; x < RD_NTORSO; ++x) for(int y = x + 1; y < RD_NTORSO; ++y)
    {
        int a = kTorso[x], b = kTorso[y];
        float d[3] = { r.p0[b][0]-r.p0[a][0], r.p0[b][1]-r.p0[a][1], r.p0[b][2]-r.p0[a][2] };
        r.ca[r.nc] = a; r.cb[r.nc] = b; r.cmn[r.nc] = 0.99f; r.cmx[r.nc] = 1.01f; r.cd0[r.nc] = sqrtf(Dot3(d,d)); ++r.nc;
    }
    for(unsigned k = 0; k < sizeof(kC)/sizeof(kC[0]) && r.nc < RD_MAXC; ++k)
    {
        int a = kC[k].a, b = kC[k].b;
        float d[3] = { r.p0[b][0]-r.p0[a][0], r.p0[b][1]-r.p0[a][1], r.p0[b][2]-r.p0[a][2] };
        r.ca[r.nc] = a; r.cb[r.nc] = b; r.cmn[r.nc] = kC[k].mn; r.cmx[r.nc] = kC[k].mx; r.cd0[r.nc] = sqrtf(Dot3(d,d)); ++r.nc;
    }
    for(int c = 0; c < r.nc; ++c) { int a = r.ca[c], b = r.cb[c]; r.leg[c] = (a >= 24 && b >= 24) || (a == 1 && b >= 24); }
    r.c0[0] = r.p0[1][0]; r.c0[1] = r.p0[1][1]; r.c0[2] = r.p0[1][2];
    TorsoFrame(r.p0, r.F0);
    {   // los nodos 2 y 3 de la columna quedan "pegados" a la recta pelvis(1) -> cuello(4), conservando su curvatura de reposo
        float chord[3] = { r.p0[4][0]-r.p0[1][0], r.p0[4][1]-r.p0[1][1], r.p0[4][2]-r.p0[1][2] };
        float cl2 = Dot3(chord, chord); if(cl2 < 1e-8f) cl2 = 1e-8f;
        for(int q2 = 0; q2 < 2; ++q2)
        {
            int nn = 2 + q2;
            if(!IsActive(nn)) { r.spT[q2] = 0.f; continue; }
            float rel[3] = { r.p0[nn][0]-r.p0[1][0], r.p0[nn][1]-r.p0[1][1], r.p0[nn][2]-r.p0[1][2] };
            float t = Dot3(rel, chord) / cl2; r.spT[q2] = t;
            float dev[3] = { rel[0]-chord[0]*t, rel[1]-chord[1]*t, rel[2]-chord[2]*t };
            for(int a = 0; a < 3; ++a) r.spDev[q2][a] = Dot3(r.F0 + a*3, dev);
        }
    }
    // "adelante" del cuerpo: hacia donde apuntan los pies
    r.fwd0[0] = (r.p0[27][0]-r.p0[26][0]) + (r.p0[31][0]-r.p0[30][0]);
    r.fwd0[1] = (r.p0[27][1]-r.p0[26][1]) + (r.p0[31][1]-r.p0[30][1]);
    r.fwd0[2] = 0.f;
    if(Dot3(r.fwd0, r.fwd0) < 1e-6f) { r.fwd0[0] = r.F0[6]; r.fwd0[1] = r.F0[7]; r.fwd0[2] = r.F0[8]; }
    Norm3(r.fwd0);
    // velocidad inicial: empuje del impacto, mas fuerte arriba para que el torso se incline
    float pelvisZ = r.p0[1][2];
    if(r.hasVel)                                      // atropello: el cuerpo recibe la velocidad del vehiculo
    {
        for(unsigned i = 0; i < n; ++i)
        {
            if(!IsActive((int)i)) continue;
            float k = (r.p0[i][2] > pelvisZ + 0.25f) ? 1.15f : 1.0f;
            float d[3] = { r.vel0[0]*0.016f*k, r.vel0[1]*0.016f*k, r.vel0[2]*0.016f };
            float dl = sqrtf(Dot3(d,d));
            if(dl > r.maxStep) { float kk = r.maxStep / dl; d[0] *= kk; d[1] *= kk; d[2] *= kk; }
            r.pv[i][0] -= d[0]; r.pv[i][1] -= d[1]; r.pv[i][2] -= d[2];
        }
    }
    else
    {
        float speed = r.force * cfgRdImpulse * 0.02f;
        if(speed > cfgMaxImpulse * 0.02f) speed = cfgMaxImpulse * 0.02f;
        for(unsigned i = 0; i < n; ++i)
        {
            if(!IsActive((int)i)) continue;
            float k = (r.p0[i][2] > pelvisZ + 0.25f) ? 1.4f : 1.0f;
            r.pv[i][0] -= r.dirx * speed * k;
            r.pv[i][1] -= r.diry * speed * k;
        }
    }
    {   // posicion de la entidad respecto a la pelvis (para que el NPC vivo siga a su cuerpo)
        CVector ep;
        if(GetPos(r.ped, ep)) { r.entOff[0] = ep.x - r.p0[1][0]; r.entOff[1] = ep.y - r.p0[1][1]; r.entOff[2] = ep.z - r.p0[1][2]; r.entOk = true; }
        // vehiculos que contienen la pelvis al empezar (el propio coche del ocupante): se ignoran para este ragdoll
        unsigned int tn = pTimeMs ? *pTimeMs : 0;
        UpdateVehicles(tn);
        r.nIgn = 0;
        for(int k = 0; k < NVEH && r.nIgn < 4; ++k)
        {
            VehTrack& t = g_veh[k];
            if(!t.used || tn - t.seen > 300) continue;
            VBox b; if(!MakeVBox(t, 0.016f, b)) continue;
            float d[3] = { r.p0[1][0]-b.c[0], r.p0[1][1]-b.c[1], r.p0[1][2]-b.c[2] };
            if(fabsf(Dot3(d,b.ax)) < b.h[0] + 0.30f && fabsf(Dot3(d,b.ay)) < b.h[1] + 0.30f && fabsf(Dot3(d,b.az)) < b.h[2] + 0.30f && !r.hasVel)
            { r.ignoreV[r.nIgn++] = t.v; FLog("[ragdoll] vehiculo %p ignorado (el ped esta dentro)", t.v); }
        }
    }
    memcpy(r.sp, r.p, sizeof(r.sp)); memcpy(r.spv, r.pv, sizeof(r.spv)); r.hasSnap = true; r.fails = 0;
    r.inited = true;
    r.startTime = r.lastTime = pTimeMs ? *pTimeMs : 0;
    FLog("[ragdoll] init n=%u restricciones=%d", n, r.nc);
}

static void SolvePair(Ragdoll& r, int a, int b, float mn, float mx, float d0, float stiff)
{
    float d[3] = { r.p[b][0]-r.p[a][0], r.p[b][1]-r.p[a][1], r.p[b][2]-r.p[a][2] };
    float l = sqrtf(Dot3(d,d)); if(l < 1e-6f) return;
    float tgt;
    if(l < mn*d0) tgt = mn*d0; else if(l > mx*d0) tgt = mx*d0; else return;
    float diff = (l - tgt) / l * stiff;
    float wa = r.im[a], wb = r.im[b], s = wa + wb;
    for(int k = 0; k < 3; ++k) { r.p[a][k] += d[k]*diff*wa/s; r.p[b][k] -= d[k]*diff*wb/s; }
}


// ---- capsulas de los huesos largos: evitan que un miembro atraviese el cuerpo u otro miembro ----
struct SegDef { unsigned char a, b; float r; };
static const SegDef kSeg[] = {
    {10,11,0.065f},{11,12,0.055f},{16,17,0.065f},{17,18,0.055f},      // brazos y antebrazos
    {24,25,0.100f},{25,26,0.075f},{28,29,0.100f},{29,30,0.075f},      // muslos y pantorrillas
    {1,3,0.160f},{3,4,0.140f},{4,5,0.100f}                            // torso, pecho, cuello
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
    float req = (A.r + B.r) * 0.85f * cfgRdBodyScale;
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
        float vel[3];
        float kdt = dt / (r.dtPrev > 0.001f ? r.dtPrev : 0.016f);       // corrige la velocidad si el paso cambio de duracion
        if(kdt < 0.25f) kdt = 0.25f; if(kdt > 4.0f) kdt = 4.0f;
        for(int k = 0; k < 3; ++k) vel[k] = (r.p[i][k] - r.pv[i][k]) * r.dk[i] * kdt;
        float vl = sqrtf(Dot3(vel, vel));                       // tope de velocidad (explosiones, impulsos enormes)
        if(!(vl == vl)) { vel[0] = vel[1] = vel[2] = 0.f; vl = 0.f; }
        float mxs = r.maxStep > 0.f ? r.maxStep : cfgRdMaxStep;
        if(vl > mxs) { float k2 = mxs / vl; vel[0] *= k2; vel[1] *= k2; vel[2] *= k2; }
        for(int k = 0; k < 3; ++k) { r.pv[i][k] = r.p[i][k]; r.p[i][k] += vel[k]; }
        r.p[i][2] -= g;
        if(w > 0.f)                                   // al principio sigue la animacion del juego (intenta mantenerse)
            for(int k = 0; k < 3; ++k) r.p[i][k] += (r.T[i][k] - r.p[i][k]) * 0.35f * w;
    }
    r.dtPrev = dt;
    // amortiguacion articular: frena la velocidad relativa entre huesos conectados (evita el efecto gelatina)
    if(cfgRdJointDamp > 0.f)
        for(int c = 0; c < r.nc; ++c)
        {
            if(r.cmn[c] < 0.97f) continue;                       // solo segmentos rigidos
            int a = r.ca[c], b = r.cb[c];
            float jd = cfgRdJointDamp + (r.leg[c] ? cfgLegDamp * 0.5f : 0.f);
            if(jd > 0.9f) jd = 0.9f;
            for(int k = 0; k < 3; ++k)
            {
                float va = r.p[a][k] - r.pv[a][k], vb = r.p[b][k] - r.pv[b][k];
                float rel = (vb - va) * jd * 0.5f;
                r.pv[a][k] = r.p[a][k] - (va + rel);
                r.pv[b][k] = r.p[b][k] - (vb - rel);
            }
        }
    // amortiguacion angular de rodillas: frena la flexion rapida (la pierna conserva resistencia natural al caer)
    if(cfgLegDamp > 0.f)
    {
        static const int lg[2][3] = { {24,25,26}, {28,29,30} };
        for(int s2 = 0; s2 < 2; ++s2)
        {
            int H = lg[s2][0], K = lg[s2][1], A = lg[s2][2];
            for(int k = 0; k < 3; ++k)
            {
                float vh = r.p[H][k]-r.pv[H][k], vk = r.p[K][k]-r.pv[K][k], va2 = r.p[A][k]-r.pv[A][k];
                float rel = (vk - (vh + va2) * 0.5f) * cfgLegDamp;
                r.pv[K][k] = r.p[K][k] - (vk - rel);
            }
        }
    }
    static unsigned int gzTick = 0; static int gzCount = 0;
    if(now != gzTick) { gzTick = now; gzCount = 0; }
    if(FindGroundZFor3DCoord)
        for(unsigned i = 0; i < r.n; ++i)
        {
            if(!IsActive((int)i)) continue;
            if(gzCount >= 64) break;                                   // tope de consultas al mundo por frame
            if(!InWorld(r.p[i][0], r.p[i][1], r.p[i][2])) continue;   // nunca consultar coordenadas invalidas
            ++gzCount;
            bool found = false; void* ent = NULL;
            float z = FindGroundZFor3DCoord(r.p[i][0], r.p[i][1], r.p[i][2] + 0.6f, &found, &ent);
            if(found && FiniteF(z)) r.gz[i] = z;
        }
    VBox vb[8]; int nvb = 0;                                      // vehiculos cercanos como cajas (copias, sin leer punteros)
    if(cfgVehCollision)
    {
        for(int k = 0; k < NVEH && nvb < 6; ++k)
        {
            VehTrack& t = g_veh[k];
            if(!t.used || now - t.seen > 300) continue;
            float d[3] = { t.pos[0]-r.p[1][0], t.pos[1]-r.p[1][1], t.pos[2]-r.p[1][2] };
            if(Dot3(d,d) > 10.f*10.f) continue;
            bool ign = false; for(int q = 0; q < r.nIgn; ++q) if(r.ignoreV[q] == t.v) ign = true;
            if(ign) continue;
            nvb += MakeVShape(t, dt, vb + nvb);
        }
    }
    for(int it = 0; it < cfgRdIter; ++it)
    {
        for(int c = 0; c < r.nc; ++c) SolvePair(r, r.ca[c], r.cb[c], r.cmn[c], r.cmx[c], r.cd0[c], cfgRdStiff);

        float Fn[9]; TorsoFrame(r.p, Fn);
        for(int q2 = 0; q2 < 2; ++q2)                    // columna rigida: sin pliegues ni "codo" en la espalda
        {
            int nn = 2 + q2;
            if(!IsActive(nn)) continue;
            for(int k = 0; k < 3; ++k)
            {
                float tg = r.p[1][k] + (r.p[4][k] - r.p[1][k]) * r.spT[q2]
                         + Fn[k]*r.spDev[q2][0] + Fn[3+k]*r.spDev[q2][1] + Fn[6+k]*r.spDev[q2][2];
                r.p[nn][k] += (tg - r.p[nn][k]) * cfgSpineStiff;
            }
        }

        // las rodillas solo se doblan hacia delante y los codos hacia atras
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
            {   // bisagra: la rodilla casi no se desvia hacia los lados (eje lateral del torso)
                float lat = Dot3(dk, Fn);                                  // Fn[0..2] = eje lateral
                float lim = 0.06f;
                if(fabsf(lat) > lim)
                {
                    float ex = lat - (lat > 0.f ? lim : -lim);
                    for(int k = 0; k < 3; ++k) { r.p[K][k] -= Fn[k]*ex*0.6f; r.p[H][k] += Fn[k]*ex*0.2f; r.p[A][k] += Fn[k]*ex*0.2f; }
                }
                if(cfgKneeSpring > 0.f)                                    // resistencia: tiende a una flexion natural pequena hacia delante
                {
                    float full = sqrtf((r.p0[K][0]-r.p0[H][0])*(r.p0[K][0]-r.p0[H][0]) + (r.p0[K][1]-r.p0[H][1])*(r.p0[K][1]-r.p0[H][1]) + (r.p0[K][2]-r.p0[H][2])*(r.p0[K][2]-r.p0[H][2]))
                               + sqrtf((r.p0[A][0]-r.p0[K][0])*(r.p0[A][0]-r.p0[K][0]) + (r.p0[A][1]-r.p0[K][1])*(r.p0[A][1]-r.p0[K][1]) + (r.p0[A][2]-r.p0[K][2])*(r.p0[A][2]-r.p0[K][2]));
                    for(int k = 0; k < 3; ++k) { float tg = mid[k] + F[k] * 0.10f * full; r.p[K][k] += (tg - r.p[K][k]) * cfgKneeSpring; }
                }
            }
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

        // vehiculos: las articulaciones no entran en la caja del coche (el coche empuja; el cuerpo puede quedar encima)
        for(int b = 0; b < nvb; ++b)
            for(unsigned i = 0; i < r.n; ++i)
                if(IsActive((int)i)) CollideParticleBox(r.p[i], r.pv[i], r.rad[i] * 0.85f, vb[b]);

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

    // pasadas extra de rigidez en caderas y piernas (resistencia natural al llegar al suelo)
    for(int ps = 0; ps < cfgLegPasses; ++ps)
        for(int c = 0; c < r.nc; ++c)
            if(r.leg[c]) SolvePair(r, r.ca[c], r.cb[c], r.cmn[c], r.cmx[c], r.cd0[c], 1.0f);

    // bloqueo estricto de longitud: los segmentos rigidos se proyectan a su longitud exacta
    if(cfgLenLock)
    {
        for(int ps = 0; ps < 6; ++ps)
            for(int c = 0; c < r.nc; ++c)
                if(r.cmn[c] >= 0.97f) SolvePair(r, r.ca[c], r.cb[c], r.cmn[c], r.cmx[c], r.cd0[c], 1.0f);
        for(int b = 0; b < nvb; ++b)                                // los vehiculos tienen la ultima palabra: nada queda dentro de un coche
            for(unsigned i = 0; i < r.n; ++i)
                if(IsActive((int)i)) CollideParticleBox(r.p[i], r.pv[i], r.rad[i] * 0.85f, vb[b]);
        for(unsigned i = 0; i < r.n; ++i)                           // el suelo manda sobre todo
        {
            if(!IsActive((int)i)) continue;
            float floorZ = r.gz[i] + r.rad[i];
            if(r.p[i][2] < floorZ) r.p[i][2] = floorZ;
        }
    }

    // friccion por contacto: las articulaciones apoyadas en el suelo pierden velocidad horizontal cada paso
    for(unsigned i = 0; i < r.n; ++i)
    {
        if(!IsActive((int)i)) continue;
        if(r.p[i][2] <= r.gz[i] + r.rad[i] + 0.02f)
        {
            r.pv[i][0] += (r.p[i][0] - r.pv[i][0]) * 0.45f;
            r.pv[i][1] += (r.p[i][1] - r.pv[i][1]) * 0.45f;
        }
    }

    // validacion: sin NaN, dentro del mapa, cerca del punto inicial y sin huesos estirados
    bool ok = true; int badNode = -1, badKind = 0;
    for(unsigned i = 0; i < r.n && ok; ++i)
    {
        if(!IsActive((int)i)) continue;
        if(!InWorld(r.p[i][0], r.p[i][1], r.p[i][2])) { ok = false; badNode = (int)i; badKind = 1; }
        float d[3] = { r.p[i][0]-r.c0[0], r.p[i][1]-r.c0[1], r.p[i][2]-r.c0[2] };
        if(ok && Dot3(d,d) > 30.f*30.f) { ok = false; badNode = (int)i; badKind = 2; }
    }
    for(int c = 0; c < r.nc && ok; ++c)
    {
        if(r.cmn[c] < 0.97f) continue;
        float d[3] = { r.p[r.cb[c]][0]-r.p[r.ca[c]][0], r.p[r.cb[c]][1]-r.p[r.ca[c]][1], r.p[r.cb[c]][2]-r.p[r.ca[c]][2] };
        float l = sqrtf(Dot3(d,d));
        if(!(l > r.cd0[c] * 0.80f && l < r.cd0[c] * 1.20f)) { ok = false; badNode = r.ca[c] * 100 + r.cb[c]; badKind = 3; }   // mas de 20% de estiramiento = algo se rompio
    }
    if(ok)
    {
        memcpy(r.sp, r.p, sizeof(r.sp)); memcpy(r.spv, r.pv, sizeof(r.spv)); r.fails = 0;
        // reposo: si casi no se mueve y esta apoyado en el suelo, se queda quieto (evita que el cuerpo "repte")
        float sum = 0.f; int na = 0, contact = 0;
        for(unsigned i = 0; i < r.n; ++i)
        {
            if(!IsActive((int)i)) continue;
            float d[3] = { r.p[i][0]-r.pv[i][0], r.p[i][1]-r.pv[i][1], r.p[i][2]-r.pv[i][2] };
            sum += sqrtf(Dot3(d,d)); ++na;
            if(r.p[i][2] <= r.gz[i] + r.rad[i] + 0.03f) ++contact;
        }
        float tsec = (now - r.startTime) * 0.001f;
        if(tsec > 0.6f && na > 0 && sum / na < 0.006f && contact * 10 >= na * 6) ++r.calm; else r.calm = 0;
        if(r.calm > 20) { r.asleep = true; r.sleepT = now; memcpy(r.pv, r.p, sizeof(r.pv)); }
    }
    else
    {
        ++r.fails;
        memcpy(r.p, r.sp, sizeof(r.p)); memcpy(r.pv, r.sp, sizeof(r.pv));      // vuelve al ultimo estado bueno, sin velocidad
        static int fl = 0; if(fl++ < 20) FLog("[ragdoll] estado invalido tipo=%d nodo=%d (fallos=%d)", badKind, badNode, r.fails);
    }
}

// aplica al vector v la rotacion del nodo i (segun su tipo)
static void NodeRotate(const Ragdoll& r, const float* Fn, int i, const float* v, float* o)
{
    int kind = kKind[i];
    if(kind == 0) { ApplyTorso(r.F0, Fn, v, o); return; }
    if(kind == 3) { NodeRotate(r, Fn, kParent[i], v, o); return; }
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
        if(kKind[i] == 3) continue;                        // cabeza: hereda el giro del cuello
        if(kKind[i] == 2)                                  // cuello y claviculas: giro del torso + swing hacia su hijo
        {
            int c2 = kPC[i];
            float u0[3] = { r.p0[c2][0]-r.p0[i][0], r.p0[c2][1]-r.p0[i][1], r.p0[c2][2]-r.p0[i][2] }, u1[3];
            ApplyTorso(r.F0, Fn, u0, u1);
            float v[3] = { r.p[c2][0]-r.p[i][0], r.p[c2][1]-r.p[i][1], r.p[c2][2]-r.p[i][2] };
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

// un vehiculo en movimiento toca un cuerpo que estaba en reposo?  -> hay que despertarlo
static bool VehicleWakes(Ragdoll& r, unsigned int now)
{
    if(!cfgVehCollision) return false;
    for(int k = 0; k < NVEH; ++k)
    {
        VehTrack& t = g_veh[k];
        if(!t.used || now - t.seen > 300) continue;
        float sp2 = t.vel[0]*t.vel[0] + t.vel[1]*t.vel[1] + t.vel[2]*t.vel[2];
        if(sp2 < 0.5f*0.5f) continue;
        bool ign = false; for(int q = 0; q < r.nIgn; ++q) if(r.ignoreV[q] == t.v) ign = true;
        if(ign) continue;
        VBox b; if(!MakeVBox(t, 0.016f, b)) continue;
        for(unsigned i = 0; i < r.n; ++i)
        {
            if(!IsActive((int)i)) continue;
            float d[3] = { r.p[i][0]-b.c[0], r.p[i][1]-b.c[1], r.p[i][2]-b.c[2] };
            float rr = r.rad[i] + 0.25f;
            if(fabsf(Dot3(d,b.ax)) < b.h[0] + rr && fabsf(Dot3(d,b.ay)) < b.h[1] + rr && fabsf(Dot3(d,b.az)) < b.h[2] + rr) return true;
        }
    }
    return false;
}

// el NPC vivo sigue a su cuerpo (solo horizontal; la altura la sigue decidiendo el juego)
static void SyncEntity(Ragdoll& r)
{
    if(!r.entOk || !r.inited || !IsValidPed(r.ped)) return;
    void* mtx = *(void**)((char*)r.ped + A.matrix);
    float* dst = NULL;
    if(ValidPtr(mtx) && (((uintptr_t)mtx) & 3) == 0 && CanRead(mtx, 0x40)) dst = (float*)((char*)mtx + 0x30);
    else if(CanRead(r.ped, 0x30)) dst = (float*)((char*)r.ped + A.placePos);
    if(!dst) return;
    float nx = r.p[1][0] + r.entOff[0], ny = r.p[1][1] + r.entOff[1];
    if(!InWorld(nx, ny, dst[2])) return;
    dst[0] = nx; dst[1] = ny;
}

static void RagdollOnHierarchy(void* hier)
{
    unsigned int now = *pTimeMs;
    for(int i = 0; i < RD_SLOTS; ++i)
    {
        Ragdoll& r = g_rd[i];
        if(!r.used || r.hier != hier) continue;
        if(!IsValidPed(r.ped)) { FLog("[ragdoll] fin (ped invalido) slot=%d", i); r.used = false; continue; }
        {   // el ped sigue siendo el mismo? ped -> clump -> frame se comparan con lo guardado (solo lecturas, sin llamar al juego)
            void* cl = *(void**)((char*)r.ped + A.rwObj);
            bool same = (cl == r.clump) && ValidPtr(cl) && CanRead(cl, 16) && (*(void**)((char*)cl + A.clumpParent) == r.frame);
            if(!same) { FLog("[ragdoll] fin (ped liberado) slot=%d", i); r.used = false; continue; }
        }
        if(!r.inited) { RagdollInit(r); if(!r.used) continue; }
        if(r.fails >= 4) { FLog("[ragdoll] demasiados fallos, se cede el control al juego slot=%d", i); r.used = false; continue; }
        float* m = *(float**)((unsigned int*)hier + 2);          // pose animada del juego = objetivo del inicio
        if(!ValidPtr(m) || !CanRead(m, r.n * 64)) { r.used = false; continue; }
        for(unsigned k = 0; k < r.n; ++k) { r.T[k][0] = m[k*16+12]; r.T[k][1] = m[k*16+13]; r.T[k][2] = m[k*16+14]; }
        if(r.asleep && VehicleWakes(r, now)) { r.asleep = false; r.calm = 0; }           // un vehiculo vuelve a empujar el cuerpo
        if(r.alive && !r.releasing)                                                       // NPC vivo: el ragdoll dura unos segundos
        {
            bool timeUp = now >= r.aliveUntil;
            bool rested = r.asleep && r.sleepT && (now - r.sleepT) > 700;
            if(timeUp || rested) { r.releasing = true; r.relStart = now; FLog("[ragdoll] liberando NPC vivo slot=%d", i); }
        }
        if(now != r.lastTime && now - r.lastTime < 200)
        {
            float dt = (now - r.lastTime) * 0.001f;
            if(dt > 0.05f) dt = 0.05f;
            // sub-pasos: siempre al menos uno por frame (el coche se mueve cada frame) y ninguno mayor de 20 ms.
            // La integracion corrige el cambio de dt entre pasos, asi que es estable a 30, 60 o 120 FPS.
            int nsteps = (int)ceilf(dt / 0.02f);
            if(nsteps < 1) nsteps = 1; if(nsteps > 3) nsteps = 3;
            dt = dt / (float)nsteps;
            if(r.dbg < 4) FLog("[ragdoll] slot=%d paso %d inicio pasos=%d", i, r.dbg, nsteps);     // migas de pan: si el juego se cierra, se ve donde
            for(int st = 0; st < nsteps && !r.asleep; ++st) RagdollStep(r, dt, now);
            if(r.dbg < 4) { FLog("[ragdoll] slot=%d paso %d fin pelvis=(%.2f,%.2f,%.2f) suelo=%.2f", i, r.dbg, r.p[1][0], r.p[1][1], r.p[1][2], r.gz[1]); ++r.dbg; }
        }
        r.lastTime = now;
        bool finite = true;                                       // nunca escribir huesos con valores invalidos
        for(unsigned k = 0; k < r.n && finite; ++k) if(IsActive((int)k) && !InWorld(r.p[k][0], r.p[k][1], r.p[k][2])) finite = false;
        if(!finite)
        {
            ++r.fails; memcpy(r.p, r.sp, sizeof(r.p)); memcpy(r.pv, r.sp, sizeof(r.pv)); r.asleep = false;
            FLog("[ragdoll] posiciones invalidas, se restaura el ultimo estado bueno (fallos=%d)", r.fails);
            if(r.fails >= 4) { r.used = false; continue; }
        }
        float G[RD_MAXN * 16];                                    // pose del juego, para volver a ella suavemente
        bool rel = r.releasing;
        if(rel) memcpy(G, m, r.n * 64);
        RagdollWrite(r);
        if(rel)
        {
            float tt = cfgReleaseTime > 0.01f ? cfgReleaseTime : 0.01f;
            float w = (now - r.relStart) / (tt * 1000.f);
            if(w >= 1.f) { memcpy(m, G, r.n * 64); FLog("[ragdoll] NPC vivo liberado slot=%d", i); r.used = false; continue; }
            static const int idx[12] = { 0,1,2,4,5,6,8,9,10,12,13,14 };
            for(unsigned k = 0; k < r.n; ++k)
                for(int j = 0; j < 12; ++j) m[k*16 + idx[j]] = m[k*16 + idx[j]] * (1.f - w) + G[k*16 + idx[j]] * w;
        }
    }
}

// un NPC vivo con ragdoll que muere pasa a ragdoll permanente
static void PromoteRagdoll(void* ped)
{
    for(int i = 0; i < RD_SLOTS; ++i)
        if(g_rd[i].used && g_rd[i].ped == ped && g_rd[i].alive) { g_rd[i].alive = false; g_rd[i].releasing = false; FLog("[ragdoll] el NPC murio: ragdoll permanente slot=%d", i); }
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

struct HitInfo { void* ped; float dx, dy, force; unsigned int time; float vel[3]; bool hasVel; };
static bool IsRagdollActive(void* ped);
static HitInfo g_info[32];
static int     g_infoN = 0;
static void*   g_rdSeen[32];
static int     g_rdSeenN = 0;
static int     g_dieLogs = 0;

// Se llama cada frame mientras el ped muere (CTaskSimpleDie) o esta muerto (CTaskSimpleDead)
static void OnDying(const char* tag, void* ped, bool isDieTask)
{
    if(!cfgRagdoll || !pTimeMs || !IsValidPed(ped)) return;
    for(int i = 0; i < 32; ++i) if(g_rdSeen[i] == ped) return;       // ya procesado
    unsigned int now = *pTimeMs;
    HitInfo* hi = NULL;
    for(int i = 0; i < 32; ++i) if(g_info[i].ped == ped && now - g_info[i].time < 4000) hi = &g_info[i];
    if(!hi && !isDieTask) return;                                    // cadaver viejo: no tocar
    float dx = hi ? hi->dx : 0.f, dy = hi ? hi->dy : 0.f, f = hi ? hi->force : 8.f;
    if(f < 8.f) f = 8.f;
    if(IsRagdollActive(ped))                                         // NPC vivo atropellado que murio: el ragdoll pasa a ser permanente
    {
        PromoteRagdoll(ped);
        g_rdSeen[g_rdSeenN++ & 31] = ped;
        return;
    }
    int res = (hi && hi->hasVel) ? StartRagdollEx(ped, dx, dy, f, false, hi->vel) : StartRagdoll(ped, dx, dy, f);
    if(res == 0) return;                                             // otro ragdoll empezo en este tick: reintenta el siguiente frame
    g_rdSeen[g_rdSeenN++ & 31] = ped;
    if(g_dieLogs++ < 30) FLog("[die] %s ped=%p dir=(%.2f,%.2f) force=%.1f info=%d res=%d", tag, ped, dx, dy, f, hi ? 1 : 0, res);
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
    void* clump = *(void**)((char*)ped + A.rwObj);            // CEntity::m_pRwObject
    FLog("[skel] ped=%p clump=%p", ped, clump);
    if(!ValidPtr(clump) || !CanRead(clump, 16)) return;
    void* frame = *(void**)((char*)clump + A.clumpParent);    // RpClump.object.parent = RwFrame*
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
static bool IsExplosionWeapon(int w) { return w == 51 || w == 16 || w == 17 || w == 18 || w == 19 || w == 20 || w == 35 || w == 36 || w == 39; }
struct ExpTick { void* ped; unsigned int time; int count; };
static ExpTick g_exp[16];
static unsigned int g_tickT = 0; static int g_tickN = 0;
static bool IsRagdollActive(void* ped) { for(int i = 0; i < RD_SLOTS; ++i) if(g_rd[i].used && g_rd[i].ped == ped) return true; return false; }
static void WakeRagdollVel(void* ped, const float* tv)
{
    for(int i = 0; i < RD_SLOTS; ++i)
    {
        Ragdoll& r = g_rd[i];
        if(!r.used || r.ped != ped || !r.inited) continue;
        for(unsigned k = 0; k < r.n; ++k)
        {
            if(!IsActive((int)k)) continue;
            float d[3] = { tv[0]*0.016f, tv[1]*0.016f, tv[2]*0.016f };
            float dl = sqrtf(Dot3(d,d)); if(dl > r.maxStep) { float kk = r.maxStep / dl; d[0]*=kk; d[1]*=kk; d[2]*=kk; }
            r.pv[k][0] -= d[0]; r.pv[k][1] -= d[1]; r.pv[k][2] -= d[2];
        }
        r.asleep = false; r.calm = 0;
        if(r.alive) { unsigned int tn = pTimeMs ? *pTimeMs : 0; r.aliveUntil = tn + (unsigned int)(cfgAliveTime * 1000.f); r.releasing = false; }
    }
}
static void WakeRagdoll(void* ped, float dx, float dy, float force)
{
    for(int i = 0; i < RD_SLOTS; ++i)
    {
        Ragdoll& r = g_rd[i];
        if(!r.used || r.ped != ped || !r.inited) continue;
        float speed = force * cfgRdImpulse * 0.02f; if(speed > cfgMaxImpulse * 0.02f) speed = cfgMaxImpulse * 0.02f;
        for(unsigned k = 0; k < r.n; ++k) { if(!IsActive((int)k)) continue; r.pv[k][0] -= dx * speed; r.pv[k][1] -= dy * speed; r.pv[k][2] -= speed * 0.4f; }
        r.asleep = false; r.calm = 0;
    }
}

static void OnHit(const char* tag, void* ped, void* ev)
{
    if(!IsValidPed(ped) || !ApplyMoveForce) return;                 // IsValidPed: puntero legible y alineado
    if(!cfgPlayer && FindPlayerPed && ped == FindPlayerPed(0)) return;

    unsigned int now = pTimeMs ? *pTimeMs : 0;
    if(now != g_tickT) { g_tickT = now; g_tickN = 0; }
    if(++g_tickN > 24) return;                                      // tope global de golpes procesados por tick

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

    // limite de explosiones por NPC (satchels/mochilas: decenas de eventos en el mismo frame)
    bool capped = false;
    if(IsExplosionWeapon(weapon))
    {
        ExpTick* e = NULL;
        for(int i = 0; i < 16; ++i) if(g_exp[i].ped == ped) { e = &g_exp[i]; break; }
        if(!e) { static int rr = 0; e = &g_exp[rr++ & 15]; e->ped = ped; e->time = now; e->count = 0; }
        if(now - e->time > 100) { e->time = now; e->count = 0; }
        if(++e->count > cfgMaxExpTicks) capped = true;
    }

    float force = BaseForce(weapon) * WeaponMult(weapon);
    CVector pp = {0,0,0}, ap = {0,0,0};
    bool okP = GetPos(ped, pp);
    bool okA = GetPos(attacker, ap);                  // atacante colgante (mochila ya explotada) => okA=false, sin acceso a memoria invalida
    if(capped) { if(isDeath && cfgRagdoll && okP) StartRagdoll(ped, 0.f, 0.f, 8.0f); return; }
    if(g_hitLogs++ < 60)
        FLog("[%s] ped=%p weapon=%d force=%.1f pos=%d/%d atk(%.1f,%.1f) ped(%.1f,%.1f)",
             tag, ped, weapon, force, okP, okA, ap.x, ap.y, pp.x, pp.y);

    float tv[3] = { 0.f, 0.f, 0.f }; bool hasTv = false;
    float rx = 0.f, ry = 0.f;
    if(okP && okA) { float dx = pp.x - ap.x, dy = pp.y - ap.y, l = sqrtf(dx*dx + dy*dy); if(l > 0.01f) { rx = dx / l; ry = dy / l; } }

    // ---- ATROPELLO / CHOQUE CON VEHICULO: ragdoll aunque el NPC no muera; la fuerza sale de la velocidad real del vehiculo
    if((weapon == 49 || weapon == 50) && cfgRunOver && cfgRagdoll)
    {
        UpdateVehicles(now);
        VehTrack* t = FindTrack(attacker);
        float vel[3] = { 0.f, 0.f, 0.f }; bool haveV = false;
        if(t && now - t->seen < 400) { vel[0] = t->vel[0]; vel[1] = t->vel[1]; vel[2] = t->vel[2]; haveV = true; }
        float spd = sqrtf(vel[0]*vel[0] + vel[1]*vel[1]);
        if(!haveV && okP && okA) { vel[0] = rx * 6.0f; vel[1] = ry * 6.0f; spd = 6.0f; }   // sin seguimiento: velocidad estimada
        if(g_hitLogs < 80) FLog("[atropello] ped=%p vehiculo=%p seguimiento=%d velocidad=%.1f m/s vel=(%.1f,%.1f,%.1f)", ped, attacker, haveV ? 1 : 0, spd, vel[0], vel[1], vel[2]);
        if(spd >= cfgRunOverMin)
        {
            tv[0] = vel[0] * cfgRunOverXfer; tv[1] = vel[1] * cfgRunOverXfer; tv[2] = vel[2] * cfgRunOverXfer;
            float up = spd * 0.10f; if(up > 3.0f) up = 3.0f; tv[2] += up;
            float tl = sqrtf(Dot3(tv, tv)); if(tl > 14.0f) { float kk = 14.0f / tl; tv[0] *= kk; tv[1] *= kk; tv[2] *= kk; }
            if(spd > 0.5f) { rx = vel[0] / spd; ry = vel[1] / spd; }            // el empuje sigue la direccion del vehiculo
            hasTv = true;
            g_info[g_infoN++ & 31] = { ped, rx, ry, force, now, { tv[0], tv[1], tv[2] }, true };
            if(IsRagdollActive(ped)) WakeRagdollVel(ped, tv);
            else StartRagdollEx(ped, rx, ry, spd * cfgCarM, true, tv);          // NPC vivo: ragdoll temporal
            return;
        }
    }
    g_info[g_infoN++ & 31] = { ped, rx, ry, force, now, { 0.f, 0.f, 0.f }, false };
    if(isDeath && cfgRagdoll) StartRagdoll(ped, rx, ry, force > 8.0f ? force : 8.0f);
    if(force >= cfgLight && !IsExplosionWeapon(weapon)) ProbeSkeleton(ped);
    if(force >= cfgLight && cfgBoneTest) StartBoneTest(ped);
    if(force < cfgLight || !okP || !okA) return;
    float dx = pp.x - ap.x, dy = pp.y - ap.y;
    float len = sqrtf(dx*dx + dy*dy);
    if(len < 0.01f) return;
    if(IsRagdollActive(ped)) { WakeRagdoll(ped, dx / len, dy / len, force); return; }   // el cadaver reacciona a nuevos golpes
    StartStagger(ped, dx / len, dy / len, force);
}

// CEventHandler::m_pPed esta en +0x00
DECL_HOOK(int, CEH_ComputeDamageResponse, void* self, void* ev, void* a, void* b, void* c)
{
    int r = CEH_ComputeDamageResponse(self, ev, a, b, c);
    if(ValidPtr(self) && CanRead(self, 4)) OnHit("damage", *(void**)self, ev);
    return r;
}
DECL_HOOK(int, CEH_ComputeDeathResponse, void* self, void* ev, void* a, void* b)
{
    int r = CEH_ComputeDeathResponse(self, ev, a, b);
    if(ValidPtr(self) && CanRead(self, 4)) OnHit("death", *(void**)self, ev);
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
    if(pTimeMs && FindPlayerPed && self == FindPlayerPed(0)) UpdateVehicles(*pTimeMs);
    if(cfgSyncEntity)
        for(int q = 0; q < RD_SLOTS; ++q)
            if(g_rd[q].used && g_rd[q].alive && g_rd[q].ped == self) SyncEntity(g_rd[q]);
    if(!ApplyMoveForce) return;
    for(int i = 0; i < 32; ++i)
    {
        Stagger& s = g_st[i];
        if(s.ped != self || s.frames <= 0) continue;
        if(!IsValidPed(self) || IsRagdollActive(self)) { s.frames = 0; continue; }   // sin tambaleo si ya hay ragdoll o el ped no es valido
        float pw = s.power;
        if(!(pw == pw) || pw > cfgMaxImpulse) pw = cfgMaxImpulse;                      // MaxImpulseLimit por frame
        CVector f = { s.dx * pw, s.dy * pw, 0.0f };
        if(!FiniteF(f.x) || !FiniteF(f.y)) { s.frames = 0; continue; }
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
    cfgPlayer  = cfg->GetBool ("AffectPlayer2", true, "Settings");
    cfgRunOver      = cfg->GetBool("RunOverRagdoll", true, "Vehicles");
    cfgRunOverMin   = cfg->GetFloat("RunOverMinSpeed", 2.5f, "Vehicles");
    cfgRunOverXfer  = cfg->GetFloat("RunOverTransfer", 0.60f, "Vehicles");
    cfgAliveTime    = cfg->GetFloat("AliveRagdollTime", 3.5f, "Vehicles");
    cfgReleaseTime  = cfg->GetFloat("ReleaseBlendTime", 0.35f, "Vehicles");
    cfgSyncEntity   = cfg->GetBool("SyncEntityToBody", true, "Vehicles");
    cfgVehCollision = cfg->GetBool("VehicleCollision", true, "Vehicles");
    cfgKneeSpring   = cfg->GetFloat("RagdollKneeSpring", 0.004f, "Settings");
    if(cfgRunOverXfer < 0.1f) cfgRunOverXfer = 0.1f; if(cfgRunOverXfer > 1.0f) cfgRunOverXfer = 1.0f;
    if(cfgAliveTime < 1.0f) cfgAliveTime = 1.0f; if(cfgAliveTime > 10.0f) cfgAliveTime = 10.0f;
    if(cfgKneeSpring < 0.f) cfgKneeSpring = 0.f; if(cfgKneeSpring > 0.03f) cfgKneeSpring = 0.03f;
    cfgLog     = cfg->GetBool ("WriteLog",    true,  "Settings");
    cfgBoneTest = cfg->GetBool("BoneTest",    false, "Settings");
    cfgMaxImpulse  = cfg->GetFloat("MaxImpulseLimit", 2.0f, "Safety");
    cfgMaxExpTicks = (int)cfg->GetFloat("MaxExplosionTicksPerPed", 2.0f, "Safety");
    cfgRdMaxStep   = cfg->GetFloat("RagdollMaxSpeed", 0.10f, "Safety");
    cfgSafeReads   = cfg->GetBool("SafeMemoryReads", true, "Safety");
    cfgLenLock     = cfg->GetBool("RagdollLengthLock", true, "Settings");
    cfgLegPasses   = (int)cfg->GetFloat("RagdollLegPasses", 4.0f, "Settings");
    cfgLegDamp     = cfg->GetFloat("RagdollLegDamp2", 0.35f, "Settings");
    cfgSpineStiff  = cfg->GetFloat("RagdollSpineStiffness", 0.9f, "Settings");
    cfgRdBodyScale = cfg->GetFloat("RagdollBodyScale", 1.0f, "Settings");
    if(cfgRdBodyScale < 0.5f) cfgRdBodyScale = 0.5f; if(cfgRdBodyScale > 1.5f) cfgRdBodyScale = 1.5f;
    if(cfgSpineStiff < 0.f) cfgSpineStiff = 0.f; if(cfgSpineStiff > 1.f) cfgSpineStiff = 1.f;
    if(cfgMaxImpulse < 0.2f) cfgMaxImpulse = 0.2f; if(cfgMaxImpulse > 6.0f) cfgMaxImpulse = 6.0f;
    if(cfgMaxExpTicks < 1) cfgMaxExpTicks = 1;
    if(cfgRdMaxStep < 0.05f) cfgRdMaxStep = 0.05f; if(cfgRdMaxStep > 0.5f) cfgRdMaxStep = 0.5f;
    if(cfgLegPasses < 0) cfgLegPasses = 0; if(cfgLegPasses > 12) cfgLegPasses = 12;
    cfgRagdoll  = cfg->GetBool ("Ragdoll",    true,  "Settings");
    cfgRdGravity = cfg->GetFloat("RagdollGravity", 14.0f, "Settings");
    cfgRdImpulse = cfg->GetFloat("RagdollPush2", 0.06f, "Settings");
    cfgRdRadius  = cfg->GetFloat("RagdollRadius",  0.10f, "Settings");
    cfgRdDamp    = cfg->GetFloat("RagdollDamp2", 0.985f, "Settings");
    cfgRdBlendTime = cfg->GetFloat("RagdollBlendTime2", 0.0f, "Settings");
    cfgRdBlend     = cfg->GetFloat("RagdollBlend2", 0.0f, "Settings");
    cfgRdIter      = (int)cfg->GetFloat("RagdollIterations", 10.0f, "Settings");
    cfgRdStiff     = cfg->GetFloat("RagdollStiffness", 1.0f, "Settings");
    cfgRdJointDamp = cfg->GetFloat("RagdollJointDamp2", 0.30f, "Settings");
    cfgRdSelfColl  = cfg->GetFloat("RagdollSelfCollision", 1.0f, "Settings");
    if(cfgRdIter < 2) cfgRdIter = 2; if(cfgRdIter > 24) cfgRdIter = 24;
    if(cfgRdStiff < 0.05f) cfgRdStiff = 0.05f; if(cfgRdStiff > 1.0f) cfgRdStiff = 1.0f;
    cfg->Save();

    SETSYM_TO(ApplyMoveForce, hGTASA, "_ZN9CPhysical14ApplyMoveForceE7CVector");
    SETSYM_TO(FindPlayerPed,  hGTASA, "_Z13FindPlayerPedi");
    SETSYM_TO(GetAnimHierarchyFromFrame, hGTASA, "_Z25GetAnimHierarchyFromFrameP7RwFrame");
    SETSYM_TO(GetBonePosition,           hGTASA, "_ZN4CPed15GetBonePositionER5RwV3djb");
    SETSYM_TO(FindGroundZFor3DCoord,     hGTASA, "_ZN6CWorld21FindGroundZFor3DCoordEfffPbPP7CEntity");
    SETSYM_TO(FindObjectsInRange,        hGTASA, "_ZN6CWorld18FindObjectsInRangeERK7CVectorfbPssPP7CEntitybbbbb");
    pModelInfoPtrs = (void**)aml->GetSym(hGTASA, "_ZN10CModelInfo16ms_modelInfoPtrsE");
    SET_TO(pTimeMs, aml->GetSym(hGTASA, "_ZN6CTimer22m_snTimeInMillisecondsE"));

    const char* sC1 = "_ZN12CEventDamageC1EP7CEntityj11eWeaponType14ePedPieceTypeshbb";
    const char* sC2 = "_ZN12CEventDamageC2EP7CEntityj11eWeaponType14ePedPieceTypeshbb";
    const char* sK1 = "_ZN12CEventDamageC1ERKS_";
    const char* sK2 = "_ZN12CEventDamageC2ERKS_";
    uintptr_t aC1 = aml->GetSym(hGTASA, sC1), aC2 = aml->GetSym(hGTASA, sC2);
    uintptr_t aK1 = aml->GetSym(hGTASA, sK1), aK2 = aml->GetSym(hGTASA, sK2);
    FLog("[sym] ctor C1=%p C2=%p copy C1=%p C2=%p", (void*)aC1, (void*)aC2, (void*)aK1, (void*)aK2);
    if(aC1) { HOOKSYM(CEventDamage_ctor, hGTASA, sC1); }
    if(aC2 && aC2 != aC1) { HOOKSYM(CEventDamage_ctor2, hGTASA, sC2); }
    if(aK1) { HOOKSYM(CEventDamage_copy, hGTASA, sK1); }
    if(aK2 && aK2 != aK1) { HOOKSYM(CEventDamage_copy2, hGTASA, sK2); }
    if(aml->GetSym(hGTASA, "_ZN12CEventDamage17ComputeDamageAnimEP4CPedb")) { HOOKSYM(CEventDamage_ComputeDamageAnim, hGTASA, "_ZN12CEventDamage17ComputeDamageAnimEP4CPedb"); } else FLog("[sym] NO ENCONTRADO: _ZN12CEventDamage17ComputeDamageAnimEP4CPedb");
    if(aml->GetSym(hGTASA, "_ZN13CEventHandler21ComputeDamageResponseEP6CEventP5CTaskS3_S3_")) { HOOKSYM(CEH_ComputeDamageResponse, hGTASA, "_ZN13CEventHandler21ComputeDamageResponseEP6CEventP5CTaskS3_S3_"); } else FLog("[sym] NO ENCONTRADO: _ZN13CEventHandler21ComputeDamageResponseEP6CEventP5CTaskS3_S3_");
    if(aml->GetSym(hGTASA, "_ZN13CEventHandler20ComputeDeathResponseEP6CEventP5CTaskS3_")) { HOOKSYM(CEH_ComputeDeathResponse, hGTASA, "_ZN13CEventHandler20ComputeDeathResponseEP6CEventP5CTaskS3_"); } else FLog("[sym] NO ENCONTRADO: _ZN13CEventHandler20ComputeDeathResponseEP6CEventP5CTaskS3_");
    if(aml->GetSym(hGTASA, "_ZN14CTaskSimpleDie10ProcessPedEP4CPed")) { HOOKSYM(CTaskSimpleDie_ProcessPed, hGTASA, "_ZN14CTaskSimpleDie10ProcessPedEP4CPed"); } else FLog("[sym] NO ENCONTRADO: _ZN14CTaskSimpleDie10ProcessPedEP4CPed");
    if(aml->GetSym(hGTASA, "_ZN15CTaskSimpleDead10ProcessPedEP4CPed")) { HOOKSYM(CTaskSimpleDead_ProcessPed, hGTASA, "_ZN15CTaskSimpleDead10ProcessPedEP4CPed"); } else FLog("[sym] NO ENCONTRADO: _ZN15CTaskSimpleDead10ProcessPedEP4CPed");
    if(aml->GetSym(hGTASA, "_ZN4CPed14ProcessControlEv")) { HOOKSYM(CPed_ProcessControl, hGTASA, "_ZN4CPed14ProcessControlEv"); } else FLog("[sym] NO ENCONTRADO: _ZN4CPed14ProcessControlEv");
    if(aml->GetSym(hGTASA, "_Z30RpHAnimHierarchyUpdateMatricesP16RpHAnimHierarchy")) { HOOKSYM(RpHAnimHierarchyUpdateMatrices_hook, hGTASA, "_Z30RpHAnimHierarchyUpdateMatricesP16RpHAnimHierarchy"); } else FLog("[sym] NO ENCONTRADO: _Z30RpHAnimHierarchyUpdateMatricesP16RpHAnimHierarchy");

    FLog("=== GTA4 Ragdoll Android cargado (%d bits). base=%p ApplyMoveForce=%p FindPlayerPed=%p timer=%p",
         (int)(sizeof(void*) * 8), (void*)pGTASA, (void*)ApplyMoveForce, (void*)FindPlayerPed, (void*)pTimeMs);
    {   // lista de lo que NO se encontro en esta version del juego (cada funcion que falte desactiva su parte del mod)
        struct { const char* n; const void* p; } need[] = {
            {"ApplyMoveForce", (const void*)ApplyMoveForce}, {"FindPlayerPed", (const void*)FindPlayerPed}, {"CTimer::m_snTimeInMilliseconds", (const void*)pTimeMs},
            {"GetAnimHierarchyFromFrame", (const void*)GetAnimHierarchyFromFrame}, {"GetBonePosition", (const void*)GetBonePosition},
            {"FindGroundZFor3DCoord", (const void*)FindGroundZFor3DCoord}, {"FindObjectsInRange", (const void*)FindObjectsInRange}, {"ms_modelInfoPtrs", (const void*)pModelInfoPtrs} };
        for(unsigned i = 0; i < sizeof(need)/sizeof(need[0]); ++i) if(!need[i].p) FLog("[sym] NO ENCONTRADO: %s", need[i].n);
    }
    logger->Info("Cargado. ApplyMoveForce=%p", (void*)ApplyMoveForce);
}
