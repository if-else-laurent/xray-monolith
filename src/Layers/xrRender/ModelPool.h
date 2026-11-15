// ModelPool.h: interface for the CModelPool class.
//////////////////////////////////////////////////////////////////////
#pragma once

// refs
class dxRender_Visual;

namespace PS
{
	struct SEmitter;
};

// defs
class ECORE_API CModelPool
{
private:
	friend class CRender;

	struct str_pred
	{
		IC bool operator()(const shared_str& x, const shared_str& y) const
		{
			return xr_strcmp(x, y) < 0;
		}
	};

	struct ModelDef
	{
		shared_str name;
		dxRender_Visual* model;
		u32 refs;
		u32 kt_zero_since; // kt (#467): Device.dwTimeContinual when refs fell to 0, 0 while in use

		ModelDef()
		{
			refs = 0;
			model = 0;
			kt_zero_since = 0;
		}

		// Order by pointer
		bool operator<(const ModelDef& other) const { return model < other.model; }
		bool operator<(const dxRender_Visual* other) const { return model < other; }
		friend bool operator<(dxRender_Visual* ptr, const ModelDef& other) { return ptr < other.model; }
	};

	typedef xr_multimap<shared_str, dxRender_Visual*, str_pred> POOL;
	typedef POOL::iterator POOL_IT;
	typedef xr_map<dxRender_Visual*, shared_str> REGISTRY;
	typedef REGISTRY::iterator REGISTRY_IT;
private:
	xr_vector<ModelDef> Models; // Reference / Base
	xr_vector<dxRender_Visual*> ModelsToDelete; // 
    xr_vector<dxRender_Visual*> ModelsToDeleteDeffer; // 
	REGISTRY Registry; // Just pairing of pointer / Name
	POOL Pool; // Unused / Inactive
	BOOL bLogging;
	BOOL bForceDiscard;
	BOOL bAllowChildrenDuplicate;
	xrCriticalSection deffered_del_lock;
	xrSRWLock ModelsLock;

	// kt (#467, Anomaly): idle cleanup of the pool. Pool/Registry/Models and the motions container were
	// touched by the prefetch thread (Prefetch_One) and the main thread without a common lock; kt_lock
	// (recursive) now covers create, delete, discard, prefetch and the sweep.
	xrCriticalSection kt_lock;
	xr_map<dxRender_Visual*, u32> kt_pool_time; // pooled instance -> when it went into Pool
	u32 kt_last_sweep;
	u32 kt_swept_instances;
	u32 kt_swept_bases;
	void kt_pool_put(const shared_str& name, dxRender_Visual* V);
	void kt_delete_base(xr_vector<ModelDef>::iterator I);
	void kt_Sweep();

	void Destroy();
public:
	CModelPool();
	virtual ~CModelPool();
	dxRender_Visual* Instance_Create(u32 Type);
	dxRender_Visual* Instance_Duplicate(dxRender_Visual* V);
	dxRender_Visual* Instance_Load(LPCSTR N, BOOL allow_register, bool assert = true);
	dxRender_Visual* Instance_Load(LPCSTR N, IReader* data, BOOL allow_register);
	dxRender_Visual* Instance_Register(LPCSTR N, dxRender_Visual* V);
	dxRender_Visual* Instance_Find(LPCSTR N);

	dxRender_Visual* CreatePE(PS::CPEDef* source);
	dxRender_Visual* CreatePG(PS::CPGDef* source);
	dxRender_Visual* Create(LPCSTR name, IReader* data = 0, bool assert = true);
	dxRender_Visual* CreateChild(LPCSTR name, IReader* data);
	void DeleteDeffered(dxRender_Visual* &V);
	void Delete(dxRender_Visual* & V, BOOL bDiscard = FALSE);
	void Discard(dxRender_Visual* & V, BOOL b_complete);
	void DeleteInternal(dxRender_Visual* & V, BOOL bDiscard = FALSE);
	void DeleteQueue();
	void DeleteQueuedDeffer();

	void Logging(BOOL bEnable) { bLogging = bEnable; }

	void Prefetch();
	void Prefetch_One(LPCSTR N, bool assert = true);
	bool Exists(LPCSTR N);
	bool Exists_kt(LPCSTR N); // kt (#467): in the pool or loaded, nothing read
	void ClearPool(BOOL b_complete);

	void dump();

	void memory_stats(u32& vb_mem_video, u32& vb_mem_system, u32& ib_mem_video, u32& ib_mem_system);
#ifdef _EDITOR
	void					OnDeviceDestroy		();
	void 					Render				(dxRender_Visual* m_pVisual, const Fmatrix& mTransform, int priority, bool strictB2F, float m_fLOD);
	void 					RenderSingle		(dxRender_Visual* m_pVisual, const Fmatrix& mTransform, float m_fLOD);
#endif
};
