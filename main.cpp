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
static float cfgLegDamp      = 0.25f;  // amortiguacion extra de rodillas/caderas
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
    void* mtx = *(void**)((char*)ent + 0x14);
    if(ValidPtr(mtx) && CanRead(mtx, 0x40)) out = *(CVector*)((char*)mtx + 0x30);
    else                                    out = *(CVector*)((char*)ent + 0x04);
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
    if(!ValidPtr(ev) || (((uintptr_t)ev) & 3) != 0 || !CanRead(ev, 0x30)) return false;
    weapon   = *(int*)((char*)ev + 0x18);
    attacker = *(void**)((char*)ev + 0x10);
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
    void* clump = *(void**)((char*)ped + 0x18);
    if(!ValidPtr(clump) || !CanRead(clump, 0x10)) return NULL;
    void* frame = *(void**)((char*)clump + 4);
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
    {24,26,0.62f,1.00f}, {28,30,0.62f,1.00f},                                           // flexion de la rodilla (max ~100 grados)
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
};
static Ragdoll g_rd[RD_SLOTS];

// Devuelve: 1 = iniciado/ya activo, 0 = reintentar en el siguiente frame, -1 = no se puede (se descarta)
static int StartRagdoll(void* ped, float dx, float dy, float force)
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
    r.used = true; r.ped = ped; r.hier
