#pragma once

//#include "xr_collide_form.h"
#include "xr_collide_defs.h"

#include "../xrCore/intrusive_ptr.h"

// refs
class ISpatial;
using ISpatialShared = intrusive_ptr<ISpatial>;

class ICollisionForm;
class CObject;

#include "../Include/xrRender/FactoryPtr.h"
#include "../Include/xrRender/ObjectSpaceRender.h"
#include "xrXRC.h"

#include "xrcdb.h"

//-----------------------------------------------------------------------------------------------------------
//Space Area
//-----------------------------------------------------------------------------------------------------------
struct hdrCFORM;

// Seamless world experiment (kt): the collision mesh of one more level to merge into the static model
// Seamless world experiment (kt): grows every time the static collision model is replaced on the
// move. Whoever keeps numbers or pointers of its triangles between frames compares and asks anew.
XRCDB_API extern u32 g_kt_static_generation;

struct KtCform
{
	string_path file;
	Fvector offset;
	u32 tri_base, tri_count; // filled in: where its triangles ended up
	u32 piece; // its number among the pieces; the cut is asked with piece + 1 (0 is the level)
};

class XRCDB_API CObjectSpace
{
private:
	CDB::MODEL Static;
	Fbox m_BoundingVolume;
public:
#ifdef DEBUG
	FactoryPtr<IObjectSpaceRender> *m_pRender;
#endif
private:
	BOOL _RayTest(const Fvector& start, const Fvector& dir, float range, collide::rq_target tgt,
	              collide::ray_cache* cache, CObject* ignore_object);
	BOOL _RayPick(const Fvector& start, const Fvector& dir, float range, collide::rq_target tgt, collide::rq_result& R,
	              CObject* ignore_object);
	BOOL _RayPick(const Fvector& start, const Fvector& dir, float range, collide::rq_target tgt, collide::rq_result& R,
				  xr_vector<CObject*>& ignore_objects);
	BOOL _RayQuery(collide::rq_results& dest, const collide::ray_defs& rq, collide::rq_callback* cb, LPVOID user_data,
	               collide::test_callback* tb, CObject* ignore_object);
	BOOL _RayQuery2(collide::rq_results& dest, const collide::ray_defs& rq, collide::rq_callback* cb, LPVOID user_data,
	                collide::test_callback* tb, CObject* ignore_object);
	BOOL _RayQuery3(collide::rq_results& dest, const collide::ray_defs& rq, collide::rq_callback* cb, LPVOID user_data,
	                collide::test_callback* tb, CObject* ignore_object);
public:
	CObjectSpace();
	~CObjectSpace();

	void Load(CDB::build_callback build_callback);
	void Load(LPCSTR path, LPCSTR fname, CDB::build_callback build_callback);
	void Load(IReader* R, CDB::build_callback build_callback);
	// Seamless world experiment (kt): the level's collision mesh and, in the same static model, the
	// mesh of one more level shifted by `offset`. Tells where that level's triangles lie in the model.
	void LoadWithChunks(CDB::build_callback build_callback, xr_vector<KtCform>& chunks, bool (*cut)(u32 piece, u32 material, const Fvector& a, const Fvector& b, const Fvector& c));
	void kt_BuildModel(CDB::MODEL& target, bool base, CDB::build_callback build_callback, xr_vector<KtCform>& chunks, bool (*cut)(u32 piece, u32 material, const Fvector& a, const Fvector& b, const Fvector& c));
	void kt_SwapStatic(CDB::MODEL& fresh);
	void Create(Fvector* verts, CDB::TRI* tris, const hdrCFORM& H, CDB::build_callback build_callback, bool init_bounds = true);
	// Occluded/No
	BOOL RayTest(const Fvector& start, const Fvector& dir, float range, collide::rq_target tgt,
	             collide::ray_cache* cache, CObject* ignore_object);

	// Game raypick (nearest) - returns object and addititional params
	BOOL RayPick(const Fvector& start, const Fvector& dir, float range, collide::rq_target tgt, collide::rq_result& R,
	             CObject* ignore_object);
	BOOL RayPick(const Fvector& start, const Fvector& dir, float range, collide::rq_target tgt, collide::rq_result& R,
				 xr_vector<CObject*>& ignore_objects);

	// General collision query
	BOOL RayQuery(collide::rq_results& dest, const collide::ray_defs& rq, collide::rq_callback* cb, LPVOID user_data,
	              collide::test_callback* tb, CObject* ignore_object);
	BOOL RayQuery(collide::rq_results& dest, ICollisionForm* target, const collide::ray_defs& rq);

	bool BoxQuery(Fvector const& box_center,
	              Fvector const& box_z_axis,
	              Fvector const& box_y_axis,
	              Fvector const& box_sizes,
	              xr_vector<Fvector>* out_tris);

	int GetNearest(xr_vector<CObject*>& q_nearest, ICollisionForm* obj, float range);
	int GetNearest(xr_vector<CObject*>& q_nearest, const Fvector& point, float range, CObject* ignore_object);
	int GetNearest(xr_vector<ISpatialShared>& q_spatial, xr_vector<CObject*>& q_nearest, const Fvector& point, float range,
	               CObject* ignore_object);

	CDB::TRI* GetStaticTris() { return Static.get_tris(); }
	Fvector* GetStaticVerts() { return Static.get_verts(); }
	CDB::MODEL* GetStaticModel() { return &Static; }

	const Fbox& GetBoundingVolume() { return m_BoundingVolume; }

	// Debugging
#ifdef DEBUG
	void dbgRender();
#endif
};
