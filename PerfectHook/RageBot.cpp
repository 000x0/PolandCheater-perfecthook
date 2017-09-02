
#include "RageBot.h"
#include "RenderManager.h"
#include "Autowall.h"
#include <iostream>
#include "MathFunctions.h"
#include "SDK.h"
#include "EnginePrediction.h"
#include "LagComp.h"

using namespace std;

#define TICK_INTERVAL			(I::Globals->interval_per_tick)
#define TIME_TO_TICKS( dt )		( (int)( 0.5f + (float)(dt) / TICK_INTERVAL ) )

void IRage::Init()
{
	IsAimStepping = false;
	IsLocked = false;
	TargetID = -1;
	pTarget = nullptr;
}

void IRage::PaintTraverse()
{

}

Vector MultipointFull(IClientEntity* entity, int hitbox, Vector pos)
{
    int health = entity->GetHealth();
    float highest_damage = fminf(menu.Ragebot.MinimumDamage, (float)health);



    model_t* model = (model_t*)entity->GetModel();
    if (!model)
        return pos;

    studiohdr_t* hdr = I::ModelInfo->GetStudiomodel(model);
    if (!hdr)
        return pos;

    matrix3x4 matrix[128];
    if (!entity->SetupBones(matrix, 128, 0x100, 0.f))
        return pos;

    mstudiohitboxset_t* set = hdr->GetHitboxSet(entity->GetHitboxSet());
    if (!set)
        return pos;

    mstudiobbox_t* box = set->GetHitbox(hitbox);
    if (!box)
        return pos;

    Vector bbmin = box->bbmin;
    Vector bbmax = box->bbmax;

    float radius = box->m_flRadius; //should scale it down a bit properly, e.g. 0.8f or so
    bbmin -= Vector(radius, radius, radius);
    bbmax += Vector(radius, radius, radius);

    Vector points[9] = {
        ((bbmin + bbmax) * .5f),
        Vector(bbmin.x, bbmin.y, bbmin.z),
        Vector(bbmin.x, bbmax.y, bbmin.z),
        Vector(bbmax.x, bbmax.y, bbmin.z),
        Vector(bbmax.x, bbmin.y, bbmin.z),
        Vector(bbmax.x, bbmax.y, bbmax.z),
        Vector(bbmin.x, bbmax.y, bbmax.z),
        Vector(bbmin.x, bbmin.y, bbmax.z),
        Vector(bbmax.x, bbmin.y, bbmax.z)
    };

    int best_point = -1; // -1 means headscaled point
    for (int i = 0; i < 9; i++) {
        if (i != 0)
            points[i] = ((((points[i] + points[0]) * .5f) + points[i]) * .5f);

        VectorTransform(points[i], matrix[box->bone], points[i]);

        float temp_dmg;
        if (CanHit(points[i], &temp_dmg))
        {
            if (temp_dmg > highest_damage + 1.f)
            {
                best_point = i;
                highest_damage = temp_dmg;
            }

            if (temp_dmg > health)
                return points[i];
        }
    }

    if (best_point == -1)
        return pos;

    return points[best_point];
}

bool IRage::hit_chance(IClientEntity* local, CInput::CUserCmd* cmd, CBaseCombatWeapon* weapon, IClientEntity* target)
{
	Vector forward, right, up;

	constexpr auto max_traces = 256;

	AngleVectors(cmd->viewangles, &forward, &right, &up);

	int total_hits = 0;
	int needed_hits = static_cast<int>(max_traces * (menu.Ragebot.HitchanceAmount / 100.f));

	weapon->UpdateAccuracyPenalty(weapon);

	auto eyes = local->GetEyePosition();
	auto flRange = weapon->GetCSWpnData()->m_fRange;

	for (int i = 0; i < max_traces; i++) {
		RandomSeed(i + 1);

		float fRand1 = RandomFloat(0.f, 1.f);
		float fRandPi1 = RandomFloat(0.f, XM_2PI);
		float fRand2 = RandomFloat(0.f, 1.f);
		float fRandPi2 = RandomFloat(0.f, XM_2PI);

		float fRandInaccuracy = fRand1 * weapon->GetInaccuracy();
		float fRandSpread = fRand2 * weapon->GetSpread();

		float fSpreadX = cos(fRandPi1) * fRandInaccuracy + cos(fRandPi2) * fRandSpread;
		float fSpreadY = sin(fRandPi1) * fRandInaccuracy + sin(fRandPi2) * fRandSpread;

		auto viewSpreadForward = (forward + fSpreadX * right + fSpreadY * up).Normalized();

		Vector viewAnglesSpread;
		VectorAngles(viewSpreadForward, viewAnglesSpread);
		MiscFunctions::NormaliseViewAngle(viewAnglesSpread);

		Vector viewForward;
		AngleVectors(viewAnglesSpread, &viewForward);
		viewForward.NormalizeInPlace();

		viewForward = eyes + (viewForward * flRange);

		trace_t tr;
		Ray_t ray;
		ray.Init(eyes, viewForward);

		I::Trace->ClipRayToEntity(ray, MASK_SHOT | CONTENTS_GRATE, target, &tr);


		if (tr.m_pEnt == target)
			total_hits++;

		if (total_hits >= needed_hits)
			return true;

		if ((max_traces - i + total_hits) < needed_hits)
			return false;
	}

	return false;
}

void IRage::CreateMove(CInput::CUserCmd *pCmd, bool& bSendPacket)
{
	if (!menu.Ragebot.b1g)
		return;

	IClientEntity *pLocal = I::EntityList->GetClientEntity(I::Engine->GetLocalPlayer());
	if (pLocal != nullptr && pLocal->IsAlive())
	{

		
		if (menu.Ragebot.Enabled)
			DoAimbot(pCmd, bSendPacket);

		if (menu.Ragebot.AntiRecoil)
			DoNoRecoil(pCmd);

		

		if (menu.Ragebot.EnabledAntiAim)
			DoAntiAim(pCmd, bSendPacket);


	}
	LastAngle = pCmd->viewangles;
}

template<class T, class U>
T clamp(T in, U low, U high)
{
	if (in <= low)
		return low;

	if (in >= high)
		return high;

	return in;
}
float LagFix()
{
	float updaterate = I::CVar->FindVar("cl_updaterate")->fValue;
	ConVar* minupdate = I::CVar->FindVar("sv_minupdaterate");
	ConVar* maxupdate = I::CVar->FindVar("sv_maxupdaterate");

	if (minupdate && maxupdate)
		updaterate = maxupdate->fValue;

	float ratio = I::CVar->FindVar("cl_interp_ratio")->fValue;

	if (ratio == 0)
		ratio = 1.0f;

	float lerp = I::CVar->FindVar("cl_interp")->fValue;
	ConVar* cmin = I::CVar->FindVar("sv_client_min_interp_ratio");
	ConVar* cmax = I::CVar->FindVar("sv_client_max_interp_ratio");

	if (cmin && cmax && cmin->fValue != 1)
		ratio = clamp(ratio, cmin->fValue, cmax->fValue);


	return max(lerp, ratio / updaterate);
}

// Functionality
void IRage::DoAimbot(CInput::CUserCmd *pCmd, bool& bSendPacket)
{

	IClientEntity* pLocal = I::EntityList->GetClientEntity(I::Engine->GetLocalPlayer());
	bool FindNewTarget = true;
	//IsLocked = false;

	// Don't aimbot with the knife..
	CBaseCombatWeapon* pWeapon = (CBaseCombatWeapon*)I::EntityList->GetClientEntityFromHandle(pLocal->GetActiveWeaponHandle());

	if (pWeapon != nullptr)
	{

		if (pWeapon->GetAmmoInClip() == 0 || MiscFunctions::IsKnife(pWeapon) || MiscFunctions::IsGrenade(pWeapon))
		{
			//TargetID = 0;
			//pTarget = nullptr;
			//HitBox = -1;
			return;
		}
	}
	else
		return;

	// Make sure we have a good target
	if (IsLocked && TargetID >= 0 && HitBox >= 0)
	{
		pTarget = I::EntityList->GetClientEntity(TargetID);
		if (pTarget  && TargetMeetsRequirements(pTarget))
		{
			HitBox = HitScan(pTarget);
			if (HitBox >= 0)
			{
				Vector ViewOffset = pLocal->GetOrigin() + pLocal->GetViewOffset();
				Vector View; I::Engine->GetViewAngles(View);
				float FoV = FovToPlayer(ViewOffset, View, pTarget, HitBox);
				if (FoV < menu.Ragebot.FOV)
					FindNewTarget = false;
			}
		}
	}



	// Find a new target, apparently we need to
	if (FindNewTarget)
	{
		TargetID = 0;
		pTarget = nullptr;
		HitBox = -1;


		TargetID = GetTargetCrosshair();


		// Memesj
		if (TargetID >= 0)
		{
			pTarget = I::EntityList->GetClientEntity(TargetID);
		}
	}

	if (TargetID >= 0 && pTarget)
	{
		HitBox = HitScan(pTarget);

		// Key
		if (menu.Ragebot.KeyPress)
		{
			if (menu.Ragebot.KeyPress > 0 && !G::PressedKeys[menu.Ragebot.KeyPress])
			{
				TargetID = -1;
				pTarget = nullptr;
				HitBox = -1;
				return;
			}
		}


		Vector AimPoint = GetHitboxPosition(pTarget, HitBox);






        if (AimAtPoint(pLocal, AimPoint, pCmd))
        {
            if (menu.Ragebot.AutoFire && CanAttack() && MiscFunctions::IsSniper(pWeapon) && menu.Ragebot.AutoScope)
            {
                if (pLocal->IsScoped()) if (!menu.Ragebot.Hitchance || hit_chance(pLocal, pCmd, pWeapon, pTarget)) pCmd->buttons |= IN_ATTACK;
                if (!pLocal->IsScoped()) pCmd->buttons |= IN_ATTACK2;
            }
            if (menu.Ragebot.AutoFire && CanAttack() && !(MiscFunctions::IsSniper(pWeapon)))
            {
                if (!menu.Ragebot.Hitchance || hit_chance(pLocal, pCmd, pWeapon, pTarget)) pCmd->buttons |= IN_ATTACK;
            }
            if (menu.Ragebot.AutoFire && CanAttack() && (MiscFunctions::IsSniper(pWeapon)) && !menu.Ragebot.AutoScope)
            {
                if (!menu.Ragebot.Hitchance || hit_chance(pLocal, pCmd, pWeapon, pTarget)) if (pLocal->IsScoped()) pCmd->buttons |= IN_ATTACK;
            }
        }




		if (menu.Ragebot.AutoStop)
		{
			pCmd->forwardmove = 0.f;
			pCmd->sidemove = 0.f;
		}



		if (menu.Ragebot.AutoCrouch)
		{
			pCmd->buttons |= IN_DUCK;
		}

	}

	// Auto Pistol
	static bool WasFiring = false;
	if (pWeapon != nullptr)
	{
		CSWeaponInfo* WeaponInfo = pWeapon->GetCSWpnData();
		if (MiscFunctions::IsPistol(pWeapon) && menu.Ragebot.AutoPistol && pWeapon->m_AttributeManager()->m_Item()->GetItemDefinitionIndex() != 64)
		{
			if (pCmd->buttons & IN_ATTACK && !MiscFunctions::IsKnife(pWeapon) && !MiscFunctions::IsGrenade(pWeapon))
			{
				if (WasFiring)
				{
					pCmd->buttons &= ~IN_ATTACK;
				}
			}

			WasFiring = pCmd->buttons & IN_ATTACK ? true : false;
		}
	}


}



bool IRage::TargetMeetsRequirements(IClientEntity* pEntity)
{
	// Is a valid player
	if (pEntity && pEntity->IsDormant() == false && pEntity->IsAlive() && pEntity->GetIndex() != hack.pLocal()->GetIndex())
	{
		// Entity Type checks
		ClientClass *pClientClass = pEntity->GetClientClass();
		player_info_t pinfo;
		if (pClientClass->m_ClassID == (int)ClassID::CCSPlayer && I::Engine->GetPlayerInfo(pEntity->GetIndex(), &pinfo))
		{
			// Team Check
			if (pEntity->GetTeamNum() != hack.pLocal()->GetTeamNum() || menu.Ragebot.FriendlyFire)
			{
				// Spawn Check
				if (!pEntity->HasGunGameImmunity())
				{
					return true;
				}
			}
		}
	}

	// They must have failed a requirement
	return false;
}
bool IRage::IsValidTARGET(int iEnt, IClientEntity* pLocal)
{
	IClientEntity* pEnt = nullptr;

	if ((pEnt = I::EntityList->GetClientEntity(iEnt)))
		if (!(pEnt == pLocal))
		{
			if (pEnt->GetTeamNum() != pLocal->GetTeamNum())
				if (!pEnt->IsDormant())
					if (pEnt->GetHealth() > 0)
						return true;
		}
	return false;
}
int IRage::AATARGE(CInput::CUserCmd *pCmd, IClientEntity* pLocal, CBaseCombatWeapon* pWeapon)
{
	int target = -1;
	int minDist = 99999;


	Vector ViewOffset = pLocal->GetEyePosition();
	Vector View; I::Engine->GetViewAngles(View);

	for (int i = 0; i < I::EntityList->GetHighestEntityIndex(); i++)
	{
		IClientEntity* pEntity = I::EntityList->GetClientEntity(i);
		if (IsValidTARGET(i, pLocal))
		{
			//ValveVector Difference = pLocalEntity->GetAbsOrigin() - pEntity->GetAbsOrigin();
			target = i;
		}
	}
	return target;

}



float IRage::FovToPlayer(Vector ViewOffSet, Vector View, IClientEntity* pEntity, int aHitBox)
{
	// Anything past 180 degrees is just going to wrap around
	CONST FLOAT MaxDegrees = 180.0f;

	// Get local angles
	Vector Angles = View;

	// Get local view / eye position
	Vector Origin = ViewOffSet;

	// Create and intiialize vectors for calculations below
	Vector Delta(0, 0, 0);
	//Vector Origin(0, 0, 0);
	Vector Forward(0, 0, 0);

	// Convert angles to normalized directional forward vector
	AngleVectors(Angles, &Forward);
	Vector AimPos = GetHitboxPosition(pEntity, aHitBox); //pvs fix disabled
																// Get delta vector between our local eye position and passed vector
	VectorSubtract(AimPos, Origin, Delta);
	//Delta = AimPos - Origin;

	// Normalize our delta vector
	Normalize(Delta, Delta);

	// Get dot product between delta position and directional forward vectors
	FLOAT DotProduct = Forward.Dot(Delta);

	// Time to calculate the field of view
	return (acos(DotProduct) * (MaxDegrees / PI));
}

int IRage::GetTargetCrosshair()
{
	// Target selection
	int target = -1;
	float minFoV = menu.Ragebot.FOV;

	IClientEntity* pLocal = I::EntityList->GetClientEntity(I::Engine->GetLocalPlayer());
	Vector ViewOffset = pLocal->GetOrigin() + pLocal->GetViewOffset();
	Vector View; I::Engine->GetViewAngles(View);

	for (int i = 0; i < I::EntityList->GetHighestEntityIndex(); i++)
	{
		IClientEntity *pEntity = I::EntityList->GetClientEntity(i);
		if (TargetMeetsRequirements(pEntity))
		{
			int NewHitBox = HitScan(pEntity);
			if (NewHitBox >= 0)
			{
				float fov = FovToPlayer(ViewOffset, View, pEntity, 0);
				if (fov < minFoV)
				{
					minFoV = fov;
					target = i;
				}
			}
		}
	}

	return target;
}

int IRage::HitScan(IClientEntity* pEntity)
{
	std::vector<int> HitBoxesToScan;
	bool AWall = menu.Ragebot.AutoWall;

	// Get the hitboxes to scan
#pragma region GetHitboxesToScan
	int HitScanMode = menu.Ragebot.Hitscan;
	if (HitScanMode == 0)
	{
		// No Hitscan, just a single hitbox	
		switch (menu.Ragebot.Hitbox)
		{
		case 0:
			HitBoxesToScan.push_back((int)CSGOHitboxID::Head);
			break;
		case 1:
			HitBoxesToScan.push_back((int)CSGOHitboxID::Neck);
			break;
		case 2:
			HitBoxesToScan.push_back((int)CSGOHitboxID::Chest);
			break;
		case 3:
			HitBoxesToScan.push_back((int)CSGOHitboxID::Stomach);
			break;
		}
	}
	else
	{
		switch (HitScanMode)
		{
		case 1:
			// low
			HitBoxesToScan.push_back((int)CSGOHitboxID::Head);
			HitBoxesToScan.push_back((int)CSGOHitboxID::Neck);
			HitBoxesToScan.push_back((int)CSGOHitboxID::UpperChest);
			HitBoxesToScan.push_back((int)CSGOHitboxID::Chest);
			HitBoxesToScan.push_back((int)CSGOHitboxID::Stomach);
			HitBoxesToScan.push_back((int)CSGOHitboxID::Pelvis);
			break;
		case 2:
			// medium
			HitBoxesToScan.push_back((int)CSGOHitboxID::Head);
			HitBoxesToScan.push_back((int)CSGOHitboxID::Neck);
			HitBoxesToScan.push_back((int)CSGOHitboxID::UpperChest);
			HitBoxesToScan.push_back((int)CSGOHitboxID::Chest);
			HitBoxesToScan.push_back((int)CSGOHitboxID::Stomach);
			HitBoxesToScan.push_back((int)CSGOHitboxID::Pelvis);
			HitBoxesToScan.push_back((int)CSGOHitboxID::LeftUpperArm);
			HitBoxesToScan.push_back((int)CSGOHitboxID::RightUpperArm);
			HitBoxesToScan.push_back((int)CSGOHitboxID::LeftThigh);
			HitBoxesToScan.push_back((int)CSGOHitboxID::RightThigh);
			break;
		case 3:
			// high
			HitBoxesToScan.push_back((int)CSGOHitboxID::Head);
			HitBoxesToScan.push_back((int)CSGOHitboxID::Neck);
			HitBoxesToScan.push_back((int)CSGOHitboxID::UpperChest);
			HitBoxesToScan.push_back((int)CSGOHitboxID::Chest);
			HitBoxesToScan.push_back((int)CSGOHitboxID::Stomach);
			HitBoxesToScan.push_back((int)CSGOHitboxID::Pelvis);
			HitBoxesToScan.push_back((int)CSGOHitboxID::LeftUpperArm);
			HitBoxesToScan.push_back((int)CSGOHitboxID::RightUpperArm);
			HitBoxesToScan.push_back((int)CSGOHitboxID::LeftThigh);
			HitBoxesToScan.push_back((int)CSGOHitboxID::RightThigh);
			HitBoxesToScan.push_back((int)CSGOHitboxID::LeftHand);
			HitBoxesToScan.push_back((int)CSGOHitboxID::RightHand);
			HitBoxesToScan.push_back((int)CSGOHitboxID::LeftFoot);
			HitBoxesToScan.push_back((int)CSGOHitboxID::RightFoot);
			HitBoxesToScan.push_back((int)CSGOHitboxID::LeftShin);
			HitBoxesToScan.push_back((int)CSGOHitboxID::RightShin);
			HitBoxesToScan.push_back((int)CSGOHitboxID::LeftLowerArm);
			HitBoxesToScan.push_back((int)CSGOHitboxID::RightLowerArm);
			break;
        case 4:
            // baim
            HitBoxesToScan.push_back((int)CSGOHitboxID::UpperChest);
            HitBoxesToScan.push_back((int)CSGOHitboxID::Chest);
            HitBoxesToScan.push_back((int)CSGOHitboxID::Stomach);
            HitBoxesToScan.push_back((int)CSGOHitboxID::Pelvis);
            HitBoxesToScan.push_back((int)CSGOHitboxID::LeftUpperArm);
            HitBoxesToScan.push_back((int)CSGOHitboxID::RightUpperArm);
            HitBoxesToScan.push_back((int)CSGOHitboxID::LeftThigh);
            HitBoxesToScan.push_back((int)CSGOHitboxID::RightThigh);
            HitBoxesToScan.push_back((int)CSGOHitboxID::LeftHand);
            HitBoxesToScan.push_back((int)CSGOHitboxID::RightHand);
            HitBoxesToScan.push_back((int)CSGOHitboxID::LeftFoot);
            HitBoxesToScan.push_back((int)CSGOHitboxID::RightFoot);
            HitBoxesToScan.push_back((int)CSGOHitboxID::LeftShin);
            HitBoxesToScan.push_back((int)CSGOHitboxID::RightShin);
            HitBoxesToScan.push_back((int)CSGOHitboxID::LeftLowerArm);
            HitBoxesToScan.push_back((int)CSGOHitboxID::RightLowerArm);
            break;
		}
	}
#pragma endregion Get the list of shit to scan

	// check hits
	/*for (auto HitBoxID : HitBoxesToScan)
	{
	if (AWall)
	{
	Vector Point = GetHitboxPosition(pEntity, HitBoxID);
	float Damage = 0.f;
	Color c = Color(255, 255, 255, 255);
	if (CanHit(Point, &Damage))
	{
	c = Color(0, 255, 0, 255);
	if (Damage >= Menu::Window.Rage.AccuracyMinimumDamage.GetValue())
	{
	return HitBoxID;
	}
	}
	}
	else
	{
	if (GameUtils::IsVisible(hackManager.pLocal(), pEntity, HitBoxID))
	return HitBoxID;
	}
	}

	return -1;*/
	int bestHitbox = -1;
	float highestDamage = static_cast<float>(menu.Ragebot.MinimumDamage);
	for (auto HitBoxID : HitBoxesToScan)
	{
        
		Vector Point = GetHitboxPosition(pEntity, HitBoxID); //pvs fix disabled

		float damage = 0.0f;
		if (CanHit(Point, &damage))
		{
			if (damage > highestDamage)
			{
				bestHitbox = HitBoxID;
				highestDamage = damage;
			}
		}
	}
	return bestHitbox;

}



void IRage::DoNoRecoil(CInput::CUserCmd *pCmd)
{
	// Ghetto rcs shit, implement properly later
	IClientEntity* pLocal = I::EntityList->GetClientEntity(I::Engine->GetLocalPlayer());
	if (pLocal != nullptr)
	{
		Vector AimPunch = pLocal->localPlayerExclusive()->GetAimPunchAngle();
		if (AimPunch.Length2D() > 0 && AimPunch.Length2D() < 150)
		{
			pCmd->viewangles -= AimPunch * 2;
			MiscFunctions::NormaliseViewAngle(pCmd->viewangles);
		}
	}
}

float FovToPoint(Vector ViewOffSet, Vector View, Vector Point)
{
	// Get local view / eye position
	Vector Origin = ViewOffSet;

	// Create and intiialize vectors for calculations below
	Vector Delta(0, 0, 0);
	Vector Forward(0, 0, 0);

	// Convert angles to normalized directional forward vector
	AngleVectors(View, &Forward);
	Vector AimPos = Point;

	// Get delta vector between our local eye position and passed vector
	Delta = AimPos - Origin;
	//Delta = AimPos - Origin;

	// Normalize our delta vector
	Normalize(Delta, Delta);

	// Get dot product between delta position and directional forward vectors
	FLOAT DotProduct = Forward.Dot(Delta);

	// Time to calculate the field of view
	return (acos(DotProduct) * (180.f / PI));
}
bool me123 = false;
bool IRage::AimAtPoint(IClientEntity* pLocal, Vector point, CInput::CUserCmd *pCmd)
{
	bool ReturnValue = false;

	if (point.Length() == 0) return ReturnValue;

	Vector angles;

	Vector src = pLocal->GetOrigin() + pLocal->GetViewOffset();

	//AngleVectors(angles, &src);
	VectorAngles(point - src, angles);
	//CalcAngle(src, point, angles);
	MiscFunctions::NormaliseViewAngle(angles);

	CBaseCombatWeapon* pWeapon = (CBaseCombatWeapon*)I::EntityList->GetClientEntityFromHandle(pLocal->GetActiveWeaponHandle());
	bool can_shoot = true;
	float server_time = pLocal->GetTickBase() * I::Globals->interval_per_tick;

	if (pWeapon != nullptr)
	{
		float next_shot = pWeapon->GetNextPrimaryAttack() - server_time;
		if (next_shot > 0) {
			can_shoot = false;
		}
	}




	IsLocked = true;
	Vector ViewOffset = pLocal->GetOrigin() + pLocal->GetViewOffset();
	if (!IsAimStepping)
		LastAimstepAngle = LastAngle; // Don't just use the viewangs because you need to consider aa

	float fovLeft = FovToPlayer(ViewOffset, LastAimstepAngle, I::EntityList->GetClientEntity(TargetID), 0);

	if (fovLeft > 25.0f && me123)
	{
		Vector AddAngs = angles - LastAimstepAngle;
		Normalize(AddAngs, AddAngs);
		AddAngs *= 25;
		LastAimstepAngle += AddAngs;
		MiscFunctions::NormaliseViewAngle(LastAimstepAngle);
		angles = LastAimstepAngle;
	}
	else
	{
		ReturnValue = true;
	}



	if (menu.Ragebot.Silent)
	{
		if (can_shoot) {
			pCmd->viewangles = angles;
		}
	}

    if (!menu.Ragebot.Silent)
    {
        pCmd->viewangles = angles;
        I::Engine->SetViewAngles(pCmd->viewangles);
    }
	if (menu.Ragebot.FakeLagFix)
	{
		pCmd->tick_count = TIME_TO_TICKS(LagFix());
	}
	return ReturnValue;
}




void NormalizeVector(Vector& vec) {
	for (int i = 0; i < 3; ++i) {
		while (vec[i] > 180.f)
			vec[i] -= 360.f;

		while (vec[i] < -180.f)
			vec[i] += 360.f;
	}
	vec[2] = 0.f;
}


void VectorAngles2(const Vector &vecForward, Vector &vecAngles)
{
	Vector vecView;
	if (vecForward[1] == 0.f && vecForward[0] == 0.f)
	{
		vecView[0] = 0.f;
		vecView[1] = 0.f;
	}
	else
	{
		vecView[1] = vec_t(atan2(vecForward[1], vecForward[0]) * 180.f / M_PI);

		if (vecView[1] < 0.f)
			vecView[1] += 360.f;

		vecView[2] = sqrt(vecForward[0] * vecForward[0] + vecForward[1] * vecForward[1]);

		vecView[0] = vec_t(atan2(vecForward[2], vecView[2]) * 180.f / M_PI);
	}

	vecAngles[0] = -vecView[0];
	vecAngles[1] = vecView[1];
	vecAngles[2] = 0.f;
}


void AtTarget(IClientEntity *Target, CInput::CUserCmd *pCmd) {
	if (!Target)
		return;

	if ((Target->GetTeamNum() == hack.pLocal()->GetTeamNum()) || Target->IsDormant() || !Target->IsAlive() || Target->GetHealth() <= 0)
		return;

	Vector TargetPosition = Target->GetEyePosition();
	CalcAngle(hack.pLocal()->GetEyePosition(), TargetPosition, pCmd->viewangles);
}

bool EdgeAntiAim(IClientEntity* pLocalBaseEntity, CInput::CUserCmd* cmd, float flWall, float flCornor)
{
	Ray_t ray;
	trace_t tr;

	CTraceFilter traceFilter;
	traceFilter.pSkip = pLocalBaseEntity;

	auto bRetVal = false;
	auto vecCurPos = pLocalBaseEntity->GetEyePosition();

	for (float i = 0; i < 360; i++)
	{
		Vector vecDummy(10.f, cmd->viewangles.y, 0.f);
		vecDummy.y += i;

		NormalizeVector(vecDummy);

		Vector vecForward;
		AngleVectors2(vecDummy, vecForward);

		auto flLength = ((16.f + 3.f) + ((16.f + 3.f) * sin(DEG2RAD(10.f)))) + 7.f;
		vecForward *= flLength;

		ray.Init(vecCurPos, (vecCurPos + vecForward));
		I::Trace->TraceRay(ray, MASK_SHOT, (CTraceFilter *)&traceFilter, &tr);

		if (tr.fraction != 1.0f)
		{
			Vector qAngles;
			auto vecNegate = tr.plane.normal;

			vecNegate *= -1.f;
			VectorAngles2(vecNegate, qAngles);

			vecDummy.y = qAngles.y;

			NormalizeVector(vecDummy);
			trace_t leftTrace, rightTrace;

			Vector vecLeft;
			AngleVectors2(vecDummy + Vector(0.f, 30.f, 0.f), vecLeft);

			Vector vecRight;
			AngleVectors2(vecDummy - Vector(0.f, 30.f, 0.f), vecRight);

			vecLeft *= (flLength + (flLength * sin(DEG2RAD(30.f))));
			vecRight *= (flLength + (flLength * sin(DEG2RAD(30.f))));

			ray.Init(vecCurPos, (vecCurPos + vecLeft));
			I::Trace->TraceRay(ray, MASK_SHOT, (CTraceFilter*)&traceFilter, &leftTrace);

			ray.Init(vecCurPos, (vecCurPos + vecRight));
			I::Trace->TraceRay(ray, MASK_SHOT, (CTraceFilter*)&traceFilter, &rightTrace);

			if ((leftTrace.fraction == 1.f) && (rightTrace.fraction != 1.f))
				vecDummy.y -= flCornor; // left
			else if ((leftTrace.fraction != 1.f) && (rightTrace.fraction == 1.f))
				vecDummy.y += flCornor; // right			

			cmd->viewangles.y = vecDummy.y;
			cmd->viewangles.y -= flWall;
			cmd->viewangles.x = 89.0f;
			bRetVal = true;
		}
	}
	return bRetVal;
}

// AntiAim
void IRage::DoAntiAim(CInput::CUserCmd *pCmd, bool& bSendPacket)
{
	IClientEntity* pLocal = I::EntityList->GetClientEntity(I::Engine->GetLocalPlayer());
	CBaseCombatWeapon* pWeapon = (CBaseCombatWeapon*)I::EntityList->GetClientEntityFromHandle(pLocal->GetActiveWeaponHandle());




	// If the aimbot is doing something don't do anything
	if (pCmd->buttons & IN_ATTACK && CanAttack())
		return;
	if ((pCmd->buttons & IN_USE))
		return;
	if (pLocal->GetMoveType() == MOVETYPE_LADDER)
		return;
	// Weapon shit

	if (pWeapon)
	{
		CSWeaponInfo* pWeaponInfo = pWeapon->GetCSWpnData();
		CCSGrenade* csGrenade = (CCSGrenade*)pWeapon;


		if (MiscFunctions::IsKnife(pWeapon) && !menu.Ragebot.KnifeAA)
			return;

		if (csGrenade->GetThrowTime() > 0.f)
			return;
	}

	// Don't do antiaim
	// if (DoExit) return;

	if (menu.Ragebot.Edge) {
		auto bEdge = EdgeAntiAim(hack.pLocal(), pCmd, 360.f, 89.f);
		if (bEdge)
			return;
	}

	if (menu.Ragebot.AtTarget) {
		IClientEntity *Target = I::EntityList->GetClientEntity(TargetID);
		AtTarget(Target, pCmd);
	}

	

	// Anti-Aim Pitch


	//Anti-Aim Yaw
/*	switch (Menu::Window.Rage.AntiAimYaw.GetIndex())
	{
	case 0:
		// No Yaw AA
		break;
	case 1:
		// Fake sideways
		AntiAims::FakeSideways(pCmd, bSendPacket);
		break;
	case 2:
		// Slow Spin
		AntiAims::SlowSpin(pCmd);
		break;
	case 3:
		// Fast Spin
		AntiAims::FastSpin(pCmd);
		break;
	case 4:
		//backwards
		pCmd->viewangles.y -= 180;
		break;
	}*/

	static bool ySwitch;

	if (menu.Ragebot.YawFake != 0)
		ySwitch = !ySwitch;
	else
		ySwitch = true;

	bSendPacket = ySwitch;

	Vector SpinAngles;
	Vector FakeAngles;
    float server_time = pLocal->GetTickBase() * I::Globals->interval_per_tick;
	static int ticks;
	static bool flip;
	if (ticks < 15 + rand() % 20)
		ticks++;
	else
	{
		flip = !flip;
		ticks = 0;
	}
	Vector StartAngles;
	double rate = 360.0 / 1.618033988749895;
	double yaw = fmod(static_cast<double>(server_time)*rate, 360.0);
	double factor = 360.0 / M_PI;
	factor *= 25;
	switch (menu.Ragebot.YawTrue)
	{
	case 1: //sideways
	{
		I::Engine->GetViewAngles(StartAngles);
		SpinAngles.y = flip ? StartAngles.y - 90.f : StartAngles.y + 90.f;
	}
		break;
	case 2://slowspin
		SpinAngles.y += static_cast<float>(yaw);
		break;
	case 3://fastspin
	{
		SpinAngles.y = (float)(fmod(server_time / 0.05f * 360.0f, 360.0f));
	}
		break;
	case 4://backwards
	{
		I::Engine->GetViewAngles(StartAngles);
		StartAngles.y -= 180.f;
		SpinAngles = StartAngles;
	}
		break;
	case 5:
	{
		SpinAngles.y = pLocal->GetLowerBodyYaw();
	}
		break;
	}



	switch (menu.Ragebot.YawFake)
	{
	case 1://sideways
	{
		I::Engine->GetViewAngles(StartAngles);
		FakeAngles.y = flip ? StartAngles.y + 90.f : StartAngles.y - 90.f;
	}
		break;
	case 2://slowspin
		FakeAngles.y += static_cast<float>(yaw);
		break;
	case 3://fastspin
		FakeAngles.y = (float)(fmod(server_time / 0.05f * 360.0f, 360.0f));
	break;
	case 4://backwards
	{
		I::Engine->GetViewAngles(StartAngles);
		StartAngles -= 180.f;
		FakeAngles = StartAngles;
	}
	break;
	case 5: //lby antiaim
		{
			I::Engine->GetViewAngles(StartAngles);
			static bool llamaflip;
			static float oldLBY = 0.0f;
			float LBY = pLocal->GetLowerBodyYaw();
			if (LBY != oldLBY) // did lowerbody update?
			{
				llamaflip = !llamaflip;
				oldLBY = LBY;
			}
			FakeAngles.y = llamaflip ? StartAngles.y + 90.f : StartAngles.y - 90.f;
		}
		break;
	}


	{
		if (ySwitch && menu.Ragebot.YawTrue != 0)
			pCmd->viewangles = SpinAngles;
		else if (!ySwitch && menu.Ragebot.YawFake != 0)
			pCmd->viewangles = FakeAngles;
	}

	switch (menu.Ragebot.Pitch)
	{
	case 0:
		// No Pitch AA
		break;
	case 1:
		// Down
		pCmd->viewangles.x = 89;
		break;
	case 2:
		pCmd->viewangles.x = -89;
		break;
	}

}

