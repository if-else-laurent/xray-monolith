#include "stdafx.h"
#pragma hdrstop
#include "../../xrEngine/IGame_Level.h"

#include "ModelPool.h"
#include "xrRender_console.h" // kt (#467): r__kt_model_idle

#ifndef _EDITOR
#include "../../xrEngine/IGame_Persistent.h"
#include "../../xrEngine/fmesh.h"
#include "fhierrarhyvisual.h"
#include "SkeletonAnimated.h"
#include "fvisual.h"
#include "fprogressive.h"
#include "fskinned.h"
#include "flod.h"
#include "ftreevisual.h"
#include "ParticleGroup.h"
#include "ParticleEffect.h"
#else
    #include "fmesh.h"
    #include "fvisual.h"
    #include "fprogressive.h"
    #include "ParticleEffect.h"
    #include "ParticleGroup.h"
	#include "fskinned.h"
    #include "fhierrarhyvisual.h"
    #include "SkeletonAnimated.h"
	#include "IGame_Persistent.h"
#endif

// kt (#467): "children of the model being loaded are cloned" - was the member bAllowChildrenDuplicate,
// shared by the main and the prefetch thread, which now load at the same time
static thread_local bool kt_children_duplicate = true;

dxRender_Visual* CModelPool::Instance_Create(u32 type)
{
	dxRender_Visual* V = NULL;

	// Check types
	switch (type)
	{
	case MT_NORMAL: // our base visual
		V = xr_new<Fvisual>();
		break;
	case MT_HIERRARHY:
		V = xr_new<FHierrarhyVisual>();
		break;
	case MT_PROGRESSIVE: // dynamic-resolution visual
		V = xr_new<FProgressive>();
		break;
	case MT_SKELETON_ANIM:
		V = xr_new<CKinematicsAnimated>();
		break;
	case MT_SKELETON_RIGID:
		V = xr_new<CKinematics>();
		break;
	case MT_SKELETON_GEOMDEF_PM:
		V = xr_new<CSkeletonX_PM>();
		break;
	case MT_SKELETON_GEOMDEF_ST:
		V = xr_new<CSkeletonX_ST>();
		break;
	case MT_PARTICLE_EFFECT:
		V = xr_new<PS::CParticleEffect>();
		break;
	case MT_PARTICLE_GROUP:
		V = xr_new<PS::CParticleGroup>();
		break;
#ifndef _EDITOR
	case MT_LOD:
		V = xr_new<FLOD>();
		break;
	case MT_TREE_ST:
		V = xr_new<FTreeVisual_ST>();
		break;
	case MT_TREE_PM:
		V = xr_new<FTreeVisual_PM>();
		break;
#endif
	default:
		FATAL("Unknown visual type");
		break;
	}
	R_ASSERT(V);
	V->Type = type;
	return V;
}

dxRender_Visual* CModelPool::Instance_Duplicate(dxRender_Visual* V)
{
	R_ASSERT(V);
	dxRender_Visual* N = Instance_Create(V->Type);
	N->Copy(V);
	N->Spawn();
	{
		// inc ref counter
		xrSRWLockGuard g(ModelsLock, true);
		auto it = std::lower_bound(Models.begin(), Models.end(), V, std::less<>{});
		if (it != Models.end() && it->model == V)
		{
			it->refs++;
			it->kt_zero_since = 0;
		}
		return N;
	}
}

dxRender_Visual* CModelPool::Instance_Load(const char* N, BOOL allow_register, bool assert)
{
	KtFrameScope kt_cost(KtFrameCost::models); // seamless world experiment (kt): a model read on the main thread
	dxRender_Visual* V;
	string_path fn;
	string_path name;

	// Add default ext if no ext at all
	if (0 == strext(N)) strconcat(sizeof(name), name, N, ".ogf");
	else xr_strcpy(name, sizeof(name), N);

	// Load data from MESHES or LEVEL
	if (!FS.exist(N))
	{
		if (!FS.exist(fn, "$level$", name))
			if (!FS.exist(fn, "$game_meshes$", name))
			{
#ifdef _EDITOR
				Msg("!Can't find model file '%s'.",name);
                return 0;
#else
				if (assert)	
					Debug.fatal(DEBUG_INFO, "Can't find model file '%s'.", name);
				else
					return nullptr;
#endif
			}
	}
	else
	{
		xr_strcpy(fn, N);
	}

	// Actual loading
#ifdef DEBUG
	if (bLogging)		Msg		("- Uncached model loading: %s",fn);
#endif // DEBUG

	IReader* data = FS.r_open(fn);
	ogf_header H;
	data->r_chunk_safe(OGF_HEADER, &H, sizeof(H));
	V = Instance_Create(H.type);
	V->Load(N, data, 0);
	FS.r_close(data);
	g_pGamePersistent->RegisterModel(V);

	// Registration
	if (allow_register) 
		V = Instance_Register(N, V);

	return V;
}

dxRender_Visual* CModelPool::Instance_Load(LPCSTR name, IReader* data, BOOL allow_register)
{
	dxRender_Visual* V;

	ogf_header H;
	data->r_chunk_safe(OGF_HEADER, &H, sizeof(H));
	V = Instance_Create(H.type);
	V->Load(name, data, 0);

	// Registration
	if (allow_register) 
		V = Instance_Register(name, V);
	return V;
}

dxRender_Visual* CModelPool::Instance_Register(LPCSTR N, dxRender_Visual* V)
{
	// Registration
	shared_str S(N);
	xrSRWLockGuard g(ModelsLock);

	// Double-check for duplicate model
	for (auto& M : Models)
	{
		if (M.name == S)
		{
			// Increment the reference count
			M.refs++;

			// Destroy the redundant one we just loaded
			V->Release();
			xr_delete(V);

			// Return the existing model
			return M.model;
		}
	}

	ModelDef M;
	M.name = S;
	M.model = V;

	auto it = std::lower_bound(Models.begin(), Models.end(), V, std::less<>{});
	Models.insert(it, std::move(M));

	return V;
}


void CModelPool::Destroy()
{
	// Pool
	xrCriticalSectionGuard kt_guard(&kt_lock);
	Pool.clear();
	kt_pool_time.clear();

	// Registry
	while (!Registry.empty())
	{
		REGISTRY_IT it = Registry.begin();
		dxRender_Visual* V = (dxRender_Visual*)it->first;
		if (!V)
		{
			Registry.erase(it);
			continue;
		}
#ifdef _DEBUG
		Msg				("ModelPool: Destroy object: '%s'",*V->dbg_name);
#endif
		DeleteInternal(V,TRUE);
	}

	// Base/Reference
	{
		xrSRWLockGuard g(ModelsLock);
		for (auto& M : Models)
		{
			M.model->Release();
			xr_delete(M.model);
		}
		Models.clear();
	}

	// cleanup motions container
	g_pMotionsContainer->clean(false);
}

CModelPool::CModelPool()
{
	bLogging = TRUE;
	bForceDiscard = FALSE;
	bAllowChildrenDuplicate = TRUE;
	kt_last_sweep = 0;
	kt_swept_instances = 0;
	kt_swept_bases = 0;
	g_pMotionsContainer = xr_new<motions_container>();
}

CModelPool::~CModelPool()
{
	Destroy();
	xr_delete(g_pMotionsContainer);
}

dxRender_Visual* CModelPool::Instance_Find(LPCSTR N)
{
	shared_str S(N);
	dxRender_Visual* Model = 0;
	xrSRWLockGuard g(ModelsLock, true);

	for (auto& M : Models)
	{
		if (S == M.name)
		{
			Model = M.model;
			break;
		}
	}
	return Model;
}

dxRender_Visual* CModelPool::Create(const char* name, IReader* data, bool assert)
{
#ifdef _EDITOR
	if (!name||!name[0])	return 0;
#endif
	string_path low_name;
	VERIFY(xr_strlen(name)<sizeof(low_name));
	xr_strcpy(low_name, name);
	strlwr(low_name);
	if (strext(low_name)) *strext(low_name) = 0;

	// kt (#467): Pool and Registry under kt_lock (the prefetch thread creates too); the base is read
	// without it, so that the main thread does not wait for a model the prefetch thread is reading
	{
		xrCriticalSectionGuard kt_guard(&kt_lock);

		// 0. Search POOL
		POOL_IT it = Pool.find(low_name);
		if (it != Pool.end())
		{
			// 1. Instance found
			dxRender_Visual* Model = it->second;
			Model->Spawn();
			Pool.erase(it);
			kt_pool_time.erase(Model);
			return Model;
		}

		// 1. Search for already loaded model (reference, base model); if found - return (cloned) reference
		if (dxRender_Visual* Base = Instance_Find(low_name))
		{
			dxRender_Visual* Model = Instance_Duplicate(Base);
			Registry.insert(mk_pair(Model, low_name));
			return Model;
		}
	}

	// 2. If not found - load and register it (Instance_Register keeps the first of two loaded at once)
	const bool children_duplicate = kt_children_duplicate;
	kt_children_duplicate = false;
	dxRender_Visual* Loaded = data ? Instance_Load(low_name, data, TRUE) : Instance_Load(low_name, TRUE, assert);
	kt_children_duplicate = children_duplicate;
	if (!Loaded)
	{
		// If not found and assert is false, return nullptr
		return nullptr;
	}

	// 3. Return (cloned) reference; the base was registered just now, a sweep lets it go only when idle
	xrCriticalSectionGuard kt_guard(&kt_lock);
	dxRender_Visual* Base = Instance_Find(low_name);
	if (!Base)
		return nullptr;
	dxRender_Visual* Model = Instance_Duplicate(Base);
	Registry.insert(mk_pair(Model, low_name));
	return Model;
}

dxRender_Visual* CModelPool::CreateChild(LPCSTR name, IReader* data)
{
	string256 low_name;
	VERIFY(xr_strlen(name)<256);
	xr_strcpy(low_name, name);
	strlwr(low_name);
	if (strext(low_name)) *strext(low_name) = 0;

	// 1. Search for already loaded model
	{
		// kt (#467, Anomaly): a registered base is always cloned (counted); given itself to a parent being
		// loaded, it was freed with the parent while Models still named it
		xrCriticalSectionGuard kt_guard(&kt_lock);
		if (dxRender_Visual* Base = Instance_Find(low_name))
			return Instance_Duplicate(Base);
	}
	dxRender_Visual* Base;
	//.	if (0==Base) Base	 	= Instance_Load(name,data,FALSE);
	if (data) Base = Instance_Load(low_name, data,FALSE);
	else Base = Instance_Load(low_name,FALSE);

	dxRender_Visual* Model = kt_children_duplicate ? Instance_Duplicate(Base) : Base;
	return Model;
}

extern  xr_atomic_bool ENGINE_API g_bRendering; 

void CModelPool::DeleteInternal(dxRender_Visual* & V, BOOL bDiscard)
{
	VERIFY(!g_bRendering);
	if (!V) return;
	xrCriticalSectionGuard kt_guard(&kt_lock);
	V->Depart();
	if (bDiscard || bForceDiscard)
	{
		Discard(V, TRUE);
	}
	else
	{
		//
		REGISTRY_IT it = Registry.find(V);
		if (it != Registry.end())
		{
			// Registry entry found - move it to pool and reset changed shader/texture if necessary
			xr_vector<IRenderVisual*>* children = V->get_children();
			if (children)
				for (auto* child : *children)
					child->ResetShaderTexture();
			else
				V->ResetShaderTexture();

			kt_pool_put(it->second, V);
		}
		else
		{
			// Registry entry not-found - just special type of visual / particles / etc.
			xr_delete(V);
		}
	}
	V = NULL;
}

void CModelPool::DeleteDeffered(dxRender_Visual* &V)
{
	if (nullptr==V)
		return;

	xrCriticalSectionGuard guard(&deffered_del_lock);

    if (std::find(ModelsToDeleteDeffer.begin(), ModelsToDeleteDeffer.end(), V) == ModelsToDeleteDeffer.end())
	    ModelsToDeleteDeffer.push_back(V);
	V = nullptr;
}

void CModelPool::Delete(dxRender_Visual* &V, BOOL bDiscard)
{
	if (nullptr==V)
		return;
	if (g_bRendering)
	{
		VERIFY(!bDiscard);
		ModelsToDelete.push_back(V);
	}
	else
	{
		DeleteInternal(V,bDiscard);
	}
	V =	nullptr;
}

void CModelPool::DeleteQueue()
{
	for (u32 it = 0; it < ModelsToDelete.size(); it++)
		DeleteInternal(ModelsToDelete[it]);
	ModelsToDelete.clear();

	kt_Sweep(); // kt (#467): called each frame by every renderer's OnFrame, outside rendering
}

void CModelPool::DeleteQueuedDeffer()
{
	xrCriticalSectionGuard guard(&deffered_del_lock);

    for (dxRender_Visual* Vis : ModelsToDeleteDeffer)
    {
        if (Vis)
            DeleteInternal(Vis);
    }	

	ModelsToDeleteDeffer.clear();
}

void CModelPool::Discard(dxRender_Visual* & V, BOOL b_complete)
{
	xrCriticalSectionGuard kt_guard(&kt_lock);
	kt_pool_time.erase(V);
	//
	REGISTRY_IT it = Registry.find(V);
	if (it != Registry.end())
	{
		// Base
		const shared_str& name = it->second;
		xrSRWLockGuard g(ModelsLock);

		for (u32 i = 0; i < Models.size(); i++)
		{
			auto I = Models.begin() + i;
			if (I->name == name)
			{
				if (b_complete || strchr(*name, '#'))
				{
					VERIFY(I->refs>0);
					I->refs--;
					if (0 == I->refs)
					{
						bForceDiscard = TRUE;
						I->model->Release();
						xr_delete(I->model);
						Models.erase(I);
						bForceDiscard = FALSE;
					}
					break;
				}
				else
				{
					if (I->refs > 0)
						I->refs--;
					if (0 == I->refs)
						I->kt_zero_since = Device.dwTimeContinual; // kt (#467): kept for reuse, swept when idle
					break;
				}
			}
		}
		// Registry
		xr_delete(V);
		Registry.erase(it);
	}
	else
	{
		// Registry entry not-found - just special type of visual / particles / etc.
		xr_delete(V);
	}
	V = NULL;
}

void CModelPool::Prefetch()
{
	Logging(FALSE);
	// prefetch visuals
	string256 section;
	strconcat(sizeof(section), section, "prefetch_visuals_", g_pGamePersistent->m_game_params.m_game_type);
	CInifile::Sect& sect = pSettings->r_section(section);
	for (CInifile::SectCIt I = sect.Data.begin(); I != sect.Data.end(); I++)
	{
		const CInifile::Item& item = *I;
		dxRender_Visual* V = Create(item.first.c_str());
		Delete(V,FALSE);
	}
	Logging(TRUE);
}

void CModelPool::Prefetch_One(LPCSTR N, bool assert)
{
	// kt (#467): runs on the prefetch thread. A model still in the pool is left as it is (the level's set of
	// prefetched names no longer skips a model the sweep let go). The instance goes back straight into the
	// pool: Delete would queue it into ModelsToDelete while rendering, unlocked against the main thread.
	if (Exists_kt(N))
		return;
	dxRender_Visual* V = Create(N, 0, assert);
	if (V)
		DeleteInternal(V, FALSE);
}

bool CModelPool::Exists_kt(LPCSTR N)
{
	string_path low_name;
	VERIFY(xr_strlen(N) < sizeof(low_name));
	xr_strcpy(low_name, N);
	strlwr(low_name);
	if (strext(low_name)) *strext(low_name) = 0;
	xrCriticalSectionGuard kt_guard(&kt_lock);
	return Pool.find(low_name) != Pool.end() || Instance_Find(low_name);
}

bool CModelPool::Exists(LPCSTR N)
{
	string_path low_name;
	VERIFY(xr_strlen(N) < sizeof(low_name));
	xr_strcpy(low_name, N);
	strlwr(low_name);
	if (strext(low_name)) *strext(low_name) = 0;

	// Search pool and loaded models (reference, base model) and return early if exists
	if (Exists_kt(low_name))
		return true;

	// Prefetch model
	dxRender_Visual* V = Create(N, 0, false);
	if (V) 
	{
		Delete(V, FALSE);
		return true;
	}

	return false;
}

dxRender_Visual* CModelPool::CreatePE(PS::CPEDef* source)
{
	PS::CParticleEffect* V = (PS::CParticleEffect*)Instance_Create(MT_PARTICLE_EFFECT);
	V->Compile(source);
	return V;
}

dxRender_Visual* CModelPool::CreatePG(PS::CPGDef* source)
{
	PS::CParticleGroup* V = (PS::CParticleGroup*)Instance_Create(MT_PARTICLE_GROUP);
	V->Compile(source);
	return V;
}

void CModelPool::ClearPool(BOOL b_complete)
{
	xrCriticalSectionGuard kt_guard(&kt_lock);
	POOL_IT _I = Pool.begin();
	POOL_IT _E = Pool.end();
	for (; _I != _E; _I++)
	{
		Discard(_I->second, b_complete);
	}
	Pool.clear();
	kt_pool_time.clear();
}

// kt (#467, Anomaly): the pool used to keep every model ever created. An instance given back went into
// Pool for reuse and stayed there until a level change; its base model (vertices, bones, motions - the
// motions and skinned vertices in g_pSharedMemoryContainer) stayed for the whole session even after the
// level change (ClearPool(FALSE) only drops the count), and the shared blocks themselves were never freed
// in a release build (Memory.mem_compact cleans them only under DEBUG_MEMORY_MANAGER). On one level and
// in the seamless world this grows with every new kind of item, body and monster met (night soak:
// bases 304 -> 582, pooled instances 669 -> 1312 in 4 h).
// The sweep: every 2 s, outside rendering, under kt_lock: of the pooled instances of one name the extra
// ones idle for idle/4 go, the last one after idle; a base without instances for idle goes; then the
// unreferenced motions and shared blocks are freed. A model needed again is loaded again (as after a
// level change). r__kt_model_idle 0 turns it off.
void CModelPool::kt_pool_put(const shared_str& name, dxRender_Visual* V)
{
	Pool.insert(mk_pair(name, V));
	kt_pool_time[V] = Device.dwTimeContinual;
}

void CModelPool::kt_delete_base(xr_vector<ModelDef>::iterator I)
{
	// the same as the b_complete branch of Discard, ModelsLock taken by the caller
	bForceDiscard = TRUE;
	I->model->Release();
	xr_delete(I->model);
	Models.erase(I);
	bForceDiscard = FALSE;
}

void CModelPool::kt_Sweep()
{
	if (ps_r__kt_model_idle <= 0)
		return;
	const u32 now = Device.dwTimeContinual;
	if (now - kt_last_sweep < 2000)
		return;
	kt_last_sweep = now;

	xrCriticalSectionGuard kt_guard(&kt_lock);
	const u32 idle_last = u32(ps_r__kt_model_idle) * 1000;
	const u32 idle_extra = idle_last / 4;
	u32 budget = 32; // a frame does not pay for more
	bool freed = false;

	// 1. pooled instances
	for (POOL_IT it = Pool.begin(); it != Pool.end() && budget;)
	{
		POOL_IT group_end = Pool.upper_bound(it->first);
		auto idle_of = [&](dxRender_Visual* V) -> u32
		{
			auto t = kt_pool_time.find(V);
			return (t != kt_pool_time.end()) ? now - t->second : 0;
		};
		u32 least_idle = u32(-1);
		for (POOL_IT j = it; j != group_end; ++j)
			least_idle = _min(least_idle, idle_of(j->second));
		bool kept_newest = false;
		for (POOL_IT j = it; j != group_end && budget;)
		{
			u32 idle = idle_of(j->second);
			bool is_newest = !kept_newest && idle == least_idle;
			if (is_newest)
				kept_newest = true;
			if (idle >= (is_newest ? idle_last : idle_extra))
			{
				dxRender_Visual* V = j->second;
				j = Pool.erase(j);
				Discard(V, TRUE); // drops the base too when this was its last instance
				++kt_swept_instances;
				--budget;
				freed = true;
			}
			else
				++j;
		}
		it = group_end;
	}

	// 2. bases left without instances (ClearPool(FALSE) on a level change keeps them)
	{
		xrSRWLockGuard g(ModelsLock);
		for (u32 i = 0; i < Models.size() && budget;)
		{
			auto I = Models.begin() + i;
			if (0 != I->refs)
			{
				I->kt_zero_since = 0;
				++i;
				continue;
			}
			if (0 == I->kt_zero_since)
			{
				I->kt_zero_since = now ? now : 1;
				++i;
				continue;
			}
			if (now - I->kt_zero_since < idle_last)
			{
				++i;
				continue;
			}
			kt_delete_base(I);
			++kt_swept_bases;
			--budget;
			freed = true;
		}
	}

	// 3. what nobody references any more
	if (freed)
	{
		g_pMotionsContainer->clean(false);
		g_pSharedMemoryContainer->clean();
	}
}

void CModelPool::dump()
{
	xrCriticalSectionGuard kt_guard(&kt_lock);
	Log("--- model pool --- begin:");
	u32 sz = 0;
	u32 k = 0;
	xrSRWLockGuard g(ModelsLock, true);
	for (xr_vector<ModelDef>::iterator I = Models.begin(); I != Models.end(); I++)
	{
		CKinematics* K = PCKinematics(I->model);
		if (K)
		{
			u32 cur = K->mem_usage(false);
			sz += cur;
			Msg("#%3d: [%3d/%5d Kb] - %s", k++, I->refs, cur / 1024, I->name.c_str());
		}
	}
	Msg("--- models: %d, mem usage: %d Kb ", k, sz / 1024);
	sz = 0;
	k = 0;
	int free_cnt = 0;
	for (REGISTRY_IT it = Registry.begin(); it != Registry.end(); it++)
	{
		CKinematics* K = PCKinematics((dxRender_Visual*)it->first);
		VERIFY(K);
		if (K)
		{
			u32 cur = K->mem_usage(true);
			sz += cur;
			bool b_free = (Pool.find(it->second) != Pool.end());
			if (b_free) ++free_cnt;
			Msg("#%3d: [%s] [%5d Kb] - %s", k++, (b_free) ? "free" : "used", cur / 1024, it->second.c_str());
		}
	}
	Msg("--- instances: %d, free %d, mem usage: %d Kb ", k, free_cnt, sz / 1024);
	{
		// kt (#467): what the pool really holds - base models of every kind (the list above shows only
		// skeletons), the pooled (unused) instances, bases without any instance, and what the sweep let go
		u32 bases = 0, bases_idle = 0;
		for (auto& M : Models)
		{
			++bases;
			if (0 == M.refs) ++bases_idle;
		}
		u32 m_items = 0, m_unused = 0, m_kb = 0, m_unused_kb = 0;
		g_pMotionsContainer->kt_stats(m_items, m_unused, m_kb, m_unused_kb);
		u32 s_blocks = 0, s_unused = 0;
		u64 s_bytes = 0, s_unused_bytes = 0;
		g_pSharedMemoryContainer->kt_stats(s_blocks, s_bytes, s_unused, s_unused_bytes);
		Msg("--- kt pool: bases %u (no instance %u), pooled %u, swept instances %u, swept bases %u, idle %d s",
			bases, bases_idle, (u32)Pool.size(), kt_swept_instances, kt_swept_bases, ps_r__kt_model_idle);
		Msg("--- kt pool: motions %u (%u Kb), unreferenced %u (%u Kb); smem blocks %u (%u Kb), unreferenced %u (%u Kb)",
			m_items, m_kb, m_unused, m_unused_kb, s_blocks, u32(s_bytes / 1024), s_unused, u32(s_unused_bytes / 1024));
	}
	Log("--- model pool --- end.");
}

void CModelPool::memory_stats(u32& vb_mem_video, u32& vb_mem_system, u32& ib_mem_video, u32& ib_mem_system)
{
	vb_mem_video = 0;
	vb_mem_system = 0;
	ib_mem_video = 0;
	ib_mem_system = 0;

	xr_vector<ModelDef>::iterator it = Models.begin();
	xr_vector<ModelDef>::const_iterator en = Models.end();
	xrSRWLockGuard g(ModelsLock, true);

	for (; it != en; ++it)
	{
		dxRender_Visual* ptr = it->model;
		Fvisual* vis_ptr = fast_dynamic_cast<Fvisual*>(ptr);

		if (vis_ptr == NULL)
			continue;
#if !defined(USE_DX10) && !defined(USE_DX11)
		D3DINDEXBUFFER_DESC IB_desc;
		D3DVERTEXBUFFER_DESC VB_desc;

		vis_ptr->m_fast->p_rm_Indices->GetDesc(&IB_desc);

		if (IB_desc.Pool == D3DPOOL_DEFAULT ||
			IB_desc.Pool == D3DPOOL_MANAGED)
			ib_mem_video += IB_desc.Size;

		if (IB_desc.Pool == D3DPOOL_MANAGED ||
			IB_desc.Pool == D3DPOOL_SCRATCH)
			ib_mem_system += IB_desc.Size;

		vis_ptr->m_fast->p_rm_Vertices->GetDesc(&VB_desc);

		if (VB_desc.Pool == D3DPOOL_DEFAULT ||
			VB_desc.Pool == D3DPOOL_MANAGED)
			vb_mem_video += IB_desc.Size;

		if (VB_desc.Pool == D3DPOOL_MANAGED ||
			VB_desc.Pool == D3DPOOL_SCRATCH)
			vb_mem_system += IB_desc.Size;

#else
		D3D_BUFFER_DESC IB_desc;
		D3D_BUFFER_DESC VB_desc;

		vis_ptr->m_fast->p_rm_Indices->GetDesc(&IB_desc);

		ib_mem_video += IB_desc.ByteWidth;
		ib_mem_system += IB_desc.ByteWidth;

		vis_ptr->m_fast->p_rm_Vertices->GetDesc(&VB_desc);

		vb_mem_video += IB_desc.ByteWidth;
		vb_mem_system += IB_desc.ByteWidth;

#endif
	}
}

#ifdef _EDITOR
IC bool	_IsBoxVisible(dxRender_Visual* visual, const Fmatrix& transform)
{
    Fbox 		bb; 
    bb.xform	(visual->vis.box,transform);
    return 		::Render->occ_visible(bb);
}
IC bool	_IsValidShader(dxRender_Visual* visual, u32 priority, bool strictB2F)
{
	if (visual->shader)
        return (priority==visual->shader->E[0]->flags.iPriority)&&(strictB2F==visual->shader->E[0]->flags.bStrictB2F);
    return false;
}

void 	CModelPool::Render(dxRender_Visual* m_pVisual, const Fmatrix& mTransform, int priority, bool strictB2F, float m_fLOD)
{
    // render visual
    xr_vector<dxRender_Visual*>::iterator I,E;
    switch (m_pVisual->Type){
    case MT_SKELETON_ANIM:
    case MT_SKELETON_RIGID:{
        if (_IsBoxVisible(m_pVisual,mTransform)){
            CKinematics* pV		= fast_dynamic_cast<CKinematics*>(m_pVisual); VERIFY(pV);
            if (fis_zero(m_fLOD,EPS)&&pV->m_lod){
		        if (_IsValidShader(pV->m_lod,priority,strictB2F)){
	                RCache.set_Shader		(pV->m_lod->shader?pV->m_lod->shader:EDevice.m_WireShader);
    	            RCache.set_xform_world	(mTransform);
        	        pV->m_lod->Render		(1.f);
                }
            }else{
                I = pV->children.begin		();
                E = pV->children.end		();
                for (; I!=E; I++){
                    if (_IsValidShader(*I,priority,strictB2F)){
                        RCache.set_Shader		((*I)->shader?(*I)->shader:EDevice.m_WireShader);
                        RCache.set_xform_world	(mTransform);
                        (*I)->Render		 	(m_fLOD);
                    }
                }
            }
        }
    }break;
    case MT_HIERRARHY:{
        if (_IsBoxVisible(m_pVisual,mTransform)){
            FHierrarhyVisual* pV		= fast_dynamic_cast<FHierrarhyVisual*>(m_pVisual); VERIFY(pV);
            I = pV->children.begin		();
            E = pV->children.end		();
            for (; I!=E; I++){
		        if (_IsValidShader(*I,priority,strictB2F)){
	                RCache.set_Shader		((*I)->shader?(*I)->shader:EDevice.m_WireShader);
    	            RCache.set_xform_world	(mTransform);
        	        (*I)->Render		 	(m_fLOD);
                }
            }
        }
    }break;
    case MT_PARTICLE_GROUP:{
        PS::CParticleGroup* pG			= fast_dynamic_cast<PS::CParticleGroup*>(m_pVisual); VERIFY(pG);
//		if (_IsBoxVisible(m_pVisual,mTransform))
        {
            RCache.set_xform_world	  		(mTransform);
            for (PS::CParticleGroup::SItemVecIt i_it=pG->items.begin(); i_it!=pG->items.end(); i_it++){
                xr_vector<dxRender_Visual*>	visuals;
                i_it->GetVisuals			(visuals);
                for (xr_vector<dxRender_Visual*>::iterator it=visuals.begin(); it!=visuals.end(); it++)
                    Render					(*it,Fidentity,priority,strictB2F,m_fLOD);
            }
        }
    }break;
    case MT_PARTICLE_EFFECT:{
//		if (_IsBoxVisible(m_pVisual,mTransform))
        {
            if (_IsValidShader(m_pVisual,priority,strictB2F)){
                RCache.set_Shader			(m_pVisual->shader?m_pVisual->shader:EDevice.m_WireShader);
                RCache.set_xform_world		(mTransform);
                m_pVisual->Render		 	(m_fLOD);
            }
        }
    }break;
    default:
        if (_IsBoxVisible(m_pVisual,mTransform)){
            if (_IsValidShader(m_pVisual,priority,strictB2F)){
                RCache.set_Shader			(m_pVisual->shader?m_pVisual->shader:EDevice.m_WireShader);
                RCache.set_xform_world		(mTransform);
                m_pVisual->Render		 	(m_fLOD);
            }
        }
        break;
    }
}

void 	CModelPool::RenderSingle(dxRender_Visual* m_pVisual, const Fmatrix& mTransform, float m_fLOD)
{
	for (int p=0; p<4; p++){
    	Render(m_pVisual,mTransform,p,false,m_fLOD);
    	Render(m_pVisual,mTransform,p,true,m_fLOD);
    }
}
void CModelPool::OnDeviceDestroy()
{
	Destroy();
}
#endif
