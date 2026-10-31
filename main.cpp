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
    for(int i = 0; i < 32; ++i)                       // evitar procesar 2 veces el mismo golpe
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

    if(force >= cfgLight) ProbeSkeleton(ped);
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
    cfg->Save();

    SETSYM_TO(ApplyMoveForce, hGTASA, "_ZN9CPhysical14ApplyMoveForceE7CVector");
    SETSYM_TO(FindPlayerPed,  hGTASA, "_Z13FindPlayerPedi");
    SETSYM_TO(GetAnimHierarchyFromFrame, hGTASA, "_Z25GetAnimHierarchyFromFrameP7RwFrame");
    SETSYM_TO(GetBonePosition,           hGTASA, "_ZN4CPed15GetBonePositionER5RwV3djb");
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

    FLog("=== GTA4 Ragdoll Android 0.1 cargado. base=0x%X ApplyMoveForce=%p FindPlayerPed=%p timer=%p",
         (unsigned)pGTASA, (void*)ApplyMoveForce, (void*)FindPlayerPed, (void*)pTimeMs);
    logger->Info("Cargado. ApplyMoveForce=%p", (void*)ApplyMoveForce);
}
